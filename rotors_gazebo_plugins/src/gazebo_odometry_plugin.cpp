/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 * Copyright 2016 Geoffrey Hunter <gbmhunter@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "rotors_gazebo_plugins/gazebo_odometry_plugin.h"

#include <chrono>
#include <iostream>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <rotors_gazebo_plugins/common.h>

#include "ConnectGazeboToRosTopic.pb.h"
#include "ConnectRosToGazeboTopic.pb.h"

#include "PoseStamped.pb.h"
#include "PoseWithCovarianceStamped.pb.h"
#include "PositionStamped.pb.h"
#include "TransformStamped.pb.h"

namespace gazebo {

GazeboOdometryPlugin::~GazeboOdometryPlugin() {
  event::Events::DisconnectWorldUpdateBegin(updateConnection_);
}

void GazeboOdometryPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {

  gzdbg << __FUNCTION__ << "() called." << std::endl;

  // Store the pointer to the model
  model_ = _model;
  world_ = model_->GetWorld();

  SdfVector3 noise_normal_position;
  SdfVector3 noise_normal_quaternion;
  SdfVector3 noise_normal_linear_velocity;
  SdfVector3 noise_normal_angular_velocity;
  SdfVector3 noise_uniform_position;
  SdfVector3 noise_uniform_quaternion;
  SdfVector3 noise_uniform_linear_velocity;
  SdfVector3 noise_uniform_angular_velocity;
  const SdfVector3 zeros3(0.0, 0.0, 0.0);

  odometry_queue_.clear();

  if (_sdf->HasElement("robotNamespace"))
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  else
    gzerr << "[gazebo_odometry_plugin] Please specify a robotNamespace.\n";

//  node_handle_ = new ros::NodeHandle(namespace_);
  gz_node_ptr_ = gazebo::transport::NodePtr(new transport::Node());
  gz_node_ptr_->Init(namespace_);

  if (_sdf->HasElement("linkName"))
    link_name_ = _sdf->GetElement("linkName")->Get<std::string>();
  else
    gzerr << "[gazebo_odometry_plugin] Please specify a linkName.\n";
  link_ = model_->GetLink(link_name_);
  if (link_ == NULL)
    gzthrow("[gazebo_odometry_plugin] Couldn't find specified link \"" << link_name_ << "\".");

  if (_sdf->HasElement("covarianceImage")) {
    std::string image_name = _sdf->GetElement("covarianceImage")->Get<std::string>();
    covariance_image_ = cv::imread(image_name, CV_LOAD_IMAGE_GRAYSCALE);
    if (covariance_image_.data == NULL)
      gzerr << "loading covariance image " << image_name << " failed" << std::endl;
    else
      gzlog << "loading covariance image " << image_name << " successful" << std::endl;
  }

  if (_sdf->HasElement("randomEngineSeed")) {
    random_generator_.seed(_sdf->GetElement("randomEngineSeed")->Get<unsigned int>());
  }
  else {
    random_generator_.seed(std::chrono::system_clock::now().time_since_epoch().count());
  }
  getSdfParam<std::string>(_sdf, "poseTopic", pose_pub_topic_, pose_pub_topic_);
  getSdfParam<std::string>(_sdf, "poseWithCovarianceTopic", pose_with_covariance_stamped_pub_topic_, pose_with_covariance_stamped_pub_topic_);
  getSdfParam<std::string>(_sdf, "positionTopic", position_stamped_pub_topic_, position_stamped_pub_topic_);
  getSdfParam<std::string>(_sdf, "transformTopic", transform_stamped_pub_topic_, transform_stamped_pub_topic_);
  getSdfParam<std::string>(_sdf, "odometryTopic", odometry_pub_topic_, odometry_pub_topic_);
  getSdfParam<std::string>(_sdf, "parentFrameId", parent_frame_id_, parent_frame_id_);
  getSdfParam<std::string>(_sdf, "childFrameId", child_frame_id_, child_frame_id_);
  getSdfParam<SdfVector3>(_sdf, "noiseNormalPosition", noise_normal_position, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseNormalQuaternion", noise_normal_quaternion, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseNormalLinearVelocity", noise_normal_linear_velocity, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseNormalAngularVelocity", noise_normal_angular_velocity, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseUniformPosition", noise_uniform_position, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseUniformQuaternion", noise_uniform_quaternion, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseUniformLinearVelocity", noise_uniform_linear_velocity, zeros3);
  getSdfParam<SdfVector3>(_sdf, "noiseUniformAngularVelocity", noise_uniform_angular_velocity, zeros3);
  getSdfParam<int>(_sdf, "measurementDelay", measurement_delay_, measurement_delay_);
  getSdfParam<int>(_sdf, "measurementDivisor", measurement_divisor_, measurement_divisor_);
  getSdfParam<double>(_sdf, "unknownDelay", unknown_delay_, unknown_delay_);
  getSdfParam<double>(_sdf, "covarianceImageScale", covariance_image_scale_, covariance_image_scale_);

  parent_link_ = world_->GetEntity(parent_frame_id_);
  if (parent_link_ == NULL && parent_frame_id_ != kDefaultParentFrameId) {
    gzthrow("[gazebo_odometry_plugin] Couldn't find specified parent link \"" << parent_frame_id_ << "\".");
  }

  position_n_[0] = NormalDistribution(0, noise_normal_position.X());
  position_n_[1] = NormalDistribution(0, noise_normal_position.Y());
  position_n_[2] = NormalDistribution(0, noise_normal_position.Z());

  attitude_n_[0] = NormalDistribution(0, noise_normal_quaternion.X());
  attitude_n_[1] = NormalDistribution(0, noise_normal_quaternion.Y());
  attitude_n_[2] = NormalDistribution(0, noise_normal_quaternion.Z());

  linear_velocity_n_[0] = NormalDistribution(0, noise_normal_linear_velocity.X());
  linear_velocity_n_[1] = NormalDistribution(0, noise_normal_linear_velocity.Y());
  linear_velocity_n_[2] = NormalDistribution(0, noise_normal_linear_velocity.Z());

  angular_velocity_n_[0] = NormalDistribution(0, noise_normal_angular_velocity.X());
  angular_velocity_n_[1] = NormalDistribution(0, noise_normal_angular_velocity.Y());
  angular_velocity_n_[2] = NormalDistribution(0, noise_normal_angular_velocity.Z());

  position_u_[0] = UniformDistribution(-noise_uniform_position.X(), noise_uniform_position.X());
  position_u_[1] = UniformDistribution(-noise_uniform_position.Y(), noise_uniform_position.Y());
  position_u_[2] = UniformDistribution(-noise_uniform_position.Z(), noise_uniform_position.Z());

  attitude_u_[0] = UniformDistribution(-noise_uniform_quaternion.X(), noise_uniform_quaternion.X());
  attitude_u_[1] = UniformDistribution(-noise_uniform_quaternion.Y(), noise_uniform_quaternion.Y());
  attitude_u_[2] = UniformDistribution(-noise_uniform_quaternion.Z(), noise_uniform_quaternion.Z());

  linear_velocity_u_[0] = UniformDistribution(-noise_uniform_linear_velocity.X(), noise_uniform_linear_velocity.X());
  linear_velocity_u_[1] = UniformDistribution(-noise_uniform_linear_velocity.Y(), noise_uniform_linear_velocity.Y());
  linear_velocity_u_[2] = UniformDistribution(-noise_uniform_linear_velocity.Z(), noise_uniform_linear_velocity.Z());

  angular_velocity_u_[0] = UniformDistribution(-noise_uniform_angular_velocity.X(), noise_uniform_angular_velocity.X());
  angular_velocity_u_[1] = UniformDistribution(-noise_uniform_angular_velocity.Y(), noise_uniform_angular_velocity.Y());
  angular_velocity_u_[2] = UniformDistribution(-noise_uniform_angular_velocity.Z(), noise_uniform_angular_velocity.Z());

  // Fill in covariance. We omit uniform noise here.
  Eigen::Map<Eigen::Matrix<double, 6, 6> > pose_covariance(pose_covariance_matrix_.data());
  Eigen::Matrix<double, 6, 1> pose_covd;

  pose_covd << noise_normal_position.X() * noise_normal_position.X(),
               noise_normal_position.Y() * noise_normal_position.Y(),
               noise_normal_position.Z() * noise_normal_position.Z(),
               noise_normal_quaternion.X() * noise_normal_quaternion.X(),
               noise_normal_quaternion.Y() * noise_normal_quaternion.Y(),
               noise_normal_quaternion.Z() * noise_normal_quaternion.Z();
  pose_covariance = pose_covd.asDiagonal();

  // Fill in covariance. We omit uniform noise here.
  Eigen::Map<Eigen::Matrix<double, 6, 6> > twist_covariance(twist_covariance_matrix_.data());
  Eigen::Matrix<double, 6, 1> twist_covd;

  twist_covd << noise_normal_linear_velocity.X() * noise_normal_linear_velocity.X(),
                noise_normal_linear_velocity.Y() * noise_normal_linear_velocity.Y(),
                noise_normal_linear_velocity.Z() * noise_normal_linear_velocity.Z(),
                noise_normal_angular_velocity.X() * noise_normal_angular_velocity.X(),
                noise_normal_angular_velocity.Y() * noise_normal_angular_velocity.Y(),
                noise_normal_angular_velocity.Z() * noise_normal_angular_velocity.Z();
  twist_covariance = twist_covd.asDiagonal();



  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  updateConnection_ = event::Events::ConnectWorldUpdateBegin(boost::bind(&GazeboOdometryPlugin::OnUpdate, this, _1));

}

// This gets called by the world update start event.
void GazeboOdometryPlugin::OnUpdate(const common::UpdateInfo& _info) {

  if(kPrintOnUpdates) {
    gzdbg << __FUNCTION__ << "() called." << std::endl;
  }

  if(!pubs_and_subs_created_) {
    CreatePubsAndSubs();
    pubs_and_subs_created_ = true;
  }

  // C denotes child frame, P parent frame, and W world frame.
  // Further C_pose_W_P denotes pose of P wrt. W expressed in C.
  math::Pose W_pose_W_C = link_->GetWorldCoGPose();
  math::Vector3 C_linear_velocity_W_C = link_->GetRelativeLinearVel();
  math::Vector3 C_angular_velocity_W_C = link_->GetRelativeAngularVel();

  math::Vector3 gazebo_linear_velocity = C_linear_velocity_W_C;
  math::Vector3 gazebo_angular_velocity = C_angular_velocity_W_C;
  math::Pose gazebo_pose = W_pose_W_C;

  if (parent_frame_id_ != kDefaultParentFrameId) {
    math::Pose W_pose_W_P = parent_link_->GetWorldPose();
    math::Vector3 P_linear_velocity_W_P = parent_link_->GetRelativeLinearVel();
    math::Vector3 P_angular_velocity_W_P = parent_link_->GetRelativeAngularVel();
    math::Pose C_pose_P_C_ = W_pose_W_C - W_pose_W_P;
    math::Vector3 C_linear_velocity_P_C;
    // \prescript{}{C}{\dot{r}}_{PC} = -R_{CP}
    //       \cdot \prescript{}{P}{\omega}_{WP} \cross \prescript{}{P}{r}_{PC}
    //       + \prescript{}{C}{v}_{WC}
    //                                 - R_{CP} \cdot \prescript{}{P}{v}_{WP}
    C_linear_velocity_P_C = - C_pose_P_C_.rot.GetInverse()
                            * P_angular_velocity_W_P.Cross(C_pose_P_C_.pos)
                            + C_linear_velocity_W_C
                            - C_pose_P_C_.rot.GetInverse() * P_linear_velocity_W_P;

    // \prescript{}{C}{\omega}_{PC} = \prescript{}{C}{\omega}_{WC}
    //       - R_{CP} \cdot \prescript{}{P}{\omega}_{WP}
    gazebo_angular_velocity = C_angular_velocity_W_C
                              - C_pose_P_C_.rot.GetInverse() * P_angular_velocity_W_P;
    gazebo_linear_velocity = C_linear_velocity_P_C;
    gazebo_pose = C_pose_P_C_;
  }

  // This flag could be set to false in the following code...
  bool publish_odometry = true;

  // First, determine whether we should publish a odometry.
  if (covariance_image_.data != NULL) {
    // We have an image.

    // Image is always centered around the origin:
    int width = covariance_image_.cols;
    int height = covariance_image_.rows;
    int x = static_cast<int>(std::floor(gazebo_pose.pos.x / covariance_image_scale_)) + width / 2;
    int y = static_cast<int>(std::floor(gazebo_pose.pos.y / covariance_image_scale_)) + height / 2;

    if (x >= 0 && x < width && y >= 0 && y < height) {
      uint8_t pixel_value = covariance_image_.at<uint8_t>(y, x);
      if (pixel_value == 0) {
        publish_odometry = false;
        // TODO: covariance scaling, according to the intensity values could be implemented here.
      }
    }
  }

  if (gazebo_sequence_ % measurement_divisor_ == 0) {
//    nav_msgs::Odometry odometry;
//    odometry.header.frame_id = parent_frame_id_;
//    odometry.header.seq = odometry_sequence_++;
//    odometry.header.stamp.sec = (world_->GetSimTime()).sec + ros::Duration(unknown_delay_).sec;
//    odometry.header.stamp.nsec = (world_->GetSimTime()).nsec + ros::Duration(unknown_delay_).nsec;
//    odometry.child_frame_id = child_frame_id_;
//    copyPosition(gazebo_pose.pos, &odometry.pose.pose.position);
//    odometry.pose.pose.orientation.w = gazebo_pose.rot.w;
//    odometry.pose.pose.orientation.x = gazebo_pose.rot.x;
//    odometry.pose.pose.orientation.y = gazebo_pose.rot.y;
//    odometry.pose.pose.orientation.z = gazebo_pose.rot.z;
//    odometry.twist.twist.linear.x = gazebo_linear_velocity.x;
//    odometry.twist.twist.linear.y = gazebo_linear_velocity.y;
//    odometry.twist.twist.linear.z = gazebo_linear_velocity.z;
//    odometry.twist.twist.angular.x = gazebo_angular_velocity.x;
//    odometry.twist.twist.angular.y = gazebo_angular_velocity.y;
//    odometry.twist.twist.angular.z = gazebo_angular_velocity.z;

    gz_geometry_msgs::Odometry odometry;
    odometry.mutable_header()->set_frame_id(parent_frame_id_);
    //odometry.mutable_header()->set_seq(odometry_sequence_++);
    //odometry.mutable_header()->mutable_stamp()->set_sec(
    //    (world_->GetSimTime()).sec + ros::Duration(unknown_delay_).sec);
    odometry.mutable_header()->mutable_stamp()->set_sec(
            (world_->GetSimTime()).sec + static_cast<int32_t>(unknown_delay_));
    //odometry.mutable_header()->mutable_stamp()->set_nsec(
    //    (world_->GetSimTime()).nsec + ros::Duration(unknown_delay_).nsec);
    odometry.mutable_header()->mutable_stamp()->set_nsec(
            (world_->GetSimTime()).nsec + static_cast<int32_t>(unknown_delay_));
    odometry.set_child_frame_id(child_frame_id_);


    odometry.mutable_pose()->mutable_pose()->mutable_position()->set_x(
        gazebo_pose.pos.x);
    odometry.mutable_pose()->mutable_pose()->mutable_position()->set_y(
        gazebo_pose.pos.y);
    odometry.mutable_pose()->mutable_pose()->mutable_position()->set_z(
        gazebo_pose.pos.z);

    odometry.mutable_pose()->mutable_pose()->mutable_orientation()->set_x(
        gazebo_pose.rot.x);
    odometry.mutable_pose()->mutable_pose()->mutable_orientation()->set_y(
        gazebo_pose.rot.y);
    odometry.mutable_pose()->mutable_pose()->mutable_orientation()->set_z(
        gazebo_pose.rot.z);
    odometry.mutable_pose()->mutable_pose()->mutable_orientation()->set_w(
        gazebo_pose.rot.w);

    odometry.mutable_twist()->mutable_twist()->mutable_linear()->set_x(
        gazebo_linear_velocity.x);
    odometry.mutable_twist()->mutable_twist()->mutable_linear()->set_y(
        gazebo_linear_velocity.y);
    odometry.mutable_twist()->mutable_twist()->mutable_linear()->set_z(
        gazebo_linear_velocity.z);

    odometry.mutable_twist()->mutable_twist()->mutable_angular()->set_x(
        gazebo_angular_velocity.x);
    odometry.mutable_twist()->mutable_twist()->mutable_angular()->set_y(
        gazebo_angular_velocity.y);
    odometry.mutable_twist()->mutable_twist()->mutable_angular()->set_z(
        gazebo_angular_velocity.z);

    if (publish_odometry)
      odometry_queue_.push_back(std::make_pair(gazebo_sequence_ + measurement_delay_, odometry));
  }

  // Is it time to publish the front element?
  if (gazebo_sequence_ == odometry_queue_.front().first) {
//    gzmsg << "Publishing first element..." << std::endl;

    //nav_msgs::OdometryPtr odometry(new nav_msgs::Odometry);

    // Copy the odometry message that is on the queue
    gz_geometry_msgs::Odometry odometry_msg(odometry_queue_.front().second);

//    odometry->header = odometry_queue_.front().second.header;
//    odometry->child_frame_id = odometry_queue_.front().second.child_frame_id;
//    odometry->pose.pose = odometry_queue_.front().second.pose.pose;
//    odometry->twist.twist.linear = odometry_queue_.front().second.twist.twist.linear;
//    odometry->twist.twist.angular = odometry_queue_.front().second.twist.twist.angular;

    // Now that we have copied the first element from the queue, remove it.
    odometry_queue_.pop_front();

    // Calculate position distortions.
    Eigen::Vector3d pos_n;
    pos_n << position_n_[0](random_generator_) + position_u_[0](random_generator_),
             position_n_[1](random_generator_) + position_u_[1](random_generator_),
             position_n_[2](random_generator_) + position_u_[2](random_generator_);
//    geometry_msgs::Point& p = odometry->pose.pose.position;
    gz_geometry_msgs::Position * p = odometry_msg.mutable_pose()->mutable_pose()->mutable_position();
    //p.x += pos_n[0];
    //p.y += pos_n[1];
    //p.z += pos_n[2];
    p->set_x(p->x() + pos_n[0]);
    p->set_y(p->y() + pos_n[1]);
    p->set_z(p->z() + pos_n[2]);


    // Calculate attitude distortions.
    Eigen::Vector3d theta;
    theta << attitude_n_[0](random_generator_) + attitude_u_[0](random_generator_),
             attitude_n_[1](random_generator_) + attitude_u_[1](random_generator_),
             attitude_n_[2](random_generator_) + attitude_u_[2](random_generator_);
    Eigen::Quaterniond q_n = QuaternionFromSmallAngle(theta);
    q_n.normalize();

    //geometry_msgs::Quaternion& q_W_L = odometry->pose.pose.orientation;
    gz_geometry_msgs::Quaternion* q_W_L =
        odometry_msg.mutable_pose()->mutable_pose()->mutable_orientation();

    Eigen::Quaterniond _q_W_L(q_W_L->w(), q_W_L->x(), q_W_L->y(), q_W_L->z());
    _q_W_L = _q_W_L * q_n;
    q_W_L->set_w(_q_W_L.w());
    q_W_L->set_x(_q_W_L.x());
    q_W_L->set_y(_q_W_L.y());
    q_W_L->set_z(_q_W_L.z());

    // Calculate linear velocity distortions.
    Eigen::Vector3d linear_velocity_n;
    linear_velocity_n << linear_velocity_n_[0](random_generator_) + linear_velocity_u_[0](random_generator_),
                linear_velocity_n_[1](random_generator_) + linear_velocity_u_[1](random_generator_),
                linear_velocity_n_[2](random_generator_) + linear_velocity_u_[2](random_generator_);
    //geometry_msgs::Vector3& linear_velocity = odometry->twist.twist.linear;
    gz_geometry_msgs::Vector3 * linear_velocity =
        odometry_msg.mutable_twist()->mutable_twist()->mutable_linear();

//    linear_velocity.x += linear_velocity_n[0];
//    linear_velocity.y += linear_velocity_n[1];
//    linear_velocity.z += linear_velocity_n[2];
    linear_velocity->set_x(linear_velocity->x() + linear_velocity_n[0]);
    linear_velocity->set_y(linear_velocity->y() + linear_velocity_n[1]);
    linear_velocity->set_z(linear_velocity->z() + linear_velocity_n[2]);

    // Calculate angular velocity distortions.
    Eigen::Vector3d angular_velocity_n;
    angular_velocity_n << angular_velocity_n_[0](random_generator_) + angular_velocity_u_[0](random_generator_),
                angular_velocity_n_[1](random_generator_) + angular_velocity_u_[1](random_generator_),
                angular_velocity_n_[2](random_generator_) + angular_velocity_u_[2](random_generator_);
//    geometry_msgs::Vector3& angular_velocity = odometry->twist.twist.angular;
    gz_geometry_msgs::Vector3 * angular_velocity =
        odometry_msg.mutable_twist()->mutable_twist()->mutable_angular();

//    angular_velocity.x += angular_velocity_n[0];
//    angular_velocity.y += angular_velocity_n[1];
//    angular_velocity.z += angular_velocity_n[2];
    angular_velocity->set_x(angular_velocity->x() + angular_velocity_n[0]);
    angular_velocity->set_y(angular_velocity->y() + angular_velocity_n[1]);
    angular_velocity->set_z(angular_velocity->z() + angular_velocity_n[2]);

//    odometry->pose.covariance = pose_covariance_matrix_;
    odometry_msg.mutable_pose()->mutable_covariance()->Clear();
    for(int i = 0; i < pose_covariance_matrix_.size(); i++) {
      odometry_msg.mutable_pose()->mutable_covariance()->Add(pose_covariance_matrix_[i]);
    }

//    odometry->twist.covariance = twist_covariance_matrix_;
    odometry_msg.mutable_twist()->mutable_covariance()->Clear();
    for(int i = 0; i < twist_covariance_matrix_.size(); i++) {
      odometry_msg.mutable_twist()->mutable_covariance()->Add(twist_covariance_matrix_[i]);
    }

    // Publish all the topics, for which the topic name is specified.
    if (pose_pub_->HasConnections()) {
//      gzmsg << "Publishing pose..." << std::endl;
      pose_pub_->Publish(odometry_msg.pose().pose());
    }

    if (pose_with_covariance_stamped_pub_->HasConnections()) {
      gz_geometry_msgs::PoseWithCovarianceStamped pose_with_covariance_stamped_msg;
//      pose_with_covariance_stamped_msg.set_allocated_header(odometry_msg.mutable_header());
      pose_with_covariance_stamped_msg.mutable_header()->CopyFrom(odometry_msg.header());
      pose_with_covariance_stamped_msg.mutable_pose_with_covariance()->CopyFrom(odometry_msg.pose());

//      gzmsg << "Publishing PoseWithCovarianceStamped message..." << std::endl;
      pose_with_covariance_stamped_pub_->Publish(pose_with_covariance_stamped_msg);
    }

    if (position_stamped_pub_->HasConnections()) {
      gz_geometry_msgs::PositionStamped position_stamped_msg;
      position_stamped_msg.mutable_header()->CopyFrom(odometry_msg.header());
      position_stamped_msg.mutable_position()->CopyFrom(odometry_msg.pose().pose().position());

//      gzmsg << "Publishing position..." << std::endl;
      position_stamped_pub_->Publish(position_stamped_msg);
    }

    if (transform_stamped_pub_->HasConnections()) {
//      geometry_msgs::TransformStampedPtr transform(new geometry_msgs::TransformStamped);
//      transform->header = odometry->header;
//      geometry_msgs::Vector3 translation;
//      translation.x = p.x;
//      translation.y = p.y;
//      translation.z = p.z;
//      transform->transform.translation = translation;
//      transform->transform.rotation = q_W_L;

      gz_geometry_msgs::TransformStamped transform_stamped_msg;

      transform_stamped_msg.mutable_header()->CopyFrom(odometry_msg.header());
      transform_stamped_msg.mutable_transform()->mutable_translation()->set_x(p->x());
      transform_stamped_msg.mutable_transform()->mutable_translation()->set_y(p->y());
      transform_stamped_msg.mutable_transform()->mutable_translation()->set_z(p->z());
      transform_stamped_msg.mutable_transform()->mutable_rotation()->CopyFrom(*q_W_L);

//      gzmsg << "Publishing transform..." << std::endl;

//      transform_stamped_pub_.publish(transform);
      transform_stamped_pub_->Publish(transform_stamped_msg);
    }

    if (odometry_pub_->HasConnections()) {
//      gzmsg << "Publishing odometry..." << std::endl;
      odometry_pub_->Publish(odometry_msg);
    }

    /// @todo ROS DEPENDENCY TO FIX
//    tf::Quaternion tf_q_W_L(q_W_L.x, q_W_L.y, q_W_L.z, q_W_L.w);
//    tf::Vector3 tf_p(p.x, p.y, p.z);
//    tf_ = tf::Transform(tf_q_W_L, tf_p);
//    transform_broadcaster_.sendTransform(
//        tf::StampedTransform(tf_, odometry->header().stamp, parent_frame_id_, child_frame_id_));

    //    std::cout << "published odometry with timestamp " << odometry->header.stamp << "at time t" << world_->GetSimTime().Double()
    //        << "delay should be " << measurement_delay_ << "sim cycles" << std::endl;
  } // if (gazebo_sequence_ == odometry_queue_.front().first) {

  ++gazebo_sequence_;
}

void GazeboOdometryPlugin::CreatePubsAndSubs() {

  // Create temporary "ConnectGazeboToRosTopic" publisher and message
  gazebo::transport::PublisherPtr connect_gazebo_to_ros_topic_pub =
        gz_node_ptr_->Advertise<gz_std_msgs::ConnectGazeboToRosTopic>("~/" + kConnectGazeboToRosSubtopic, 1);

  gz_std_msgs::ConnectGazeboToRosTopic connect_gazebo_to_ros_topic_msg;

  // ============================================ //
  // =============== POSE MSG SETUP ============= //
  // ============================================ //
//  gzmsg << "GazeboOdometryPlugin creating publisher on Gazebo topic \"" << pose_pub_topic_ << "\"." << std::endl;
  pose_pub_ = gz_node_ptr_->Advertise<gz_geometry_msgs::Pose>(
      "~/" + model_->GetName() + "/" + pose_pub_topic_,
      1);

  connect_gazebo_to_ros_topic_msg.set_gazebo_topic("~/" + model_->GetName() + "/" + pose_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_ros_topic(pose_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_msgtype(gz_std_msgs::ConnectGazeboToRosTopic::POSE);
  connect_gazebo_to_ros_topic_pub->Publish(connect_gazebo_to_ros_topic_msg, true);

  // ============================================ //
  // == POSE WITH COVARIANCE STAMPED MSG SETUP == //
  // ============================================ //
//  gzmsg << "GazeboOdometryPlugin creating publisher on Gazebo topic \"" << pose_with_covariance_stamped_pub_topic_ << "\"." << std::endl;
  pose_with_covariance_stamped_pub_ = gz_node_ptr_->Advertise<gz_geometry_msgs::PoseWithCovarianceStamped>(
      "~/" + model_->GetName() + "/" + pose_with_covariance_stamped_pub_topic_,
      1);

  connect_gazebo_to_ros_topic_msg.set_gazebo_topic("~/" + model_->GetName() + "/" + pose_with_covariance_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_ros_topic(pose_with_covariance_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_msgtype(gz_std_msgs::ConnectGazeboToRosTopic::POSE_WITH_COVARIANCE_STAMPED);
  connect_gazebo_to_ros_topic_pub->Publish(connect_gazebo_to_ros_topic_msg, true);

  // ============================================ //
  // ========= POSITION STAMPED MSG SETUP ======= //
  // ============================================ //
//  gzmsg << "GazeboOdometryPlugin creating publisher on Gazebo topic \"" << position_stamped_pub_topic_ << "\"." << std::endl;
  position_stamped_pub_ = gz_node_ptr_->Advertise<gz_geometry_msgs::PositionStamped>(
      "~/" + model_->GetName() + "/" + position_stamped_pub_topic_,
      1);

  connect_gazebo_to_ros_topic_msg.set_gazebo_topic("~/" + model_->GetName() + "/" + position_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_ros_topic(position_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_msgtype(gz_std_msgs::ConnectGazeboToRosTopic::POSITION_STAMPED);
  connect_gazebo_to_ros_topic_pub->Publish(connect_gazebo_to_ros_topic_msg, true);

  // ============================================ //
  // ============= ODOMETRY MSG SETUP =========== //
  // ============================================ //
//  gzmsg << "GazeboOdometryPlugin creating publisher on Gazebo topic \"" << odometry_pub_topic_ << "\"." << std::endl;
  odometry_pub_ = gz_node_ptr_->Advertise<gz_geometry_msgs::Odometry>(
      "~/" + model_->GetName() + "/" + odometry_pub_topic_,
      1);

  connect_gazebo_to_ros_topic_msg.set_gazebo_topic("~/" + model_->GetName() + "/" + odometry_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_ros_topic(odometry_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_msgtype(gz_std_msgs::ConnectGazeboToRosTopic::ODOMETRY);
  connect_gazebo_to_ros_topic_pub->Publish(connect_gazebo_to_ros_topic_msg, true);

  // ============================================ //
  // ======== TRANSFORM STAMPED MSG SETUP ======= //
  // ============================================ //
//  gzmsg << "GazeboOdometryPlugin creating publisher on Gazebo topic \"" << transform_stamped_pub_topic_ << "\"." << std::endl;
  transform_stamped_pub_ = gz_node_ptr_->Advertise<gz_geometry_msgs::TransformStamped>(
      "~/" + model_->GetName() + "/" + transform_stamped_pub_topic_,
      1);

  connect_gazebo_to_ros_topic_msg.set_gazebo_topic("~/" + model_->GetName() + "/" + transform_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_ros_topic(transform_stamped_pub_topic_);
  connect_gazebo_to_ros_topic_msg.set_msgtype(gz_std_msgs::ConnectGazeboToRosTopic::TRANSFORM_STAMPED);
  connect_gazebo_to_ros_topic_pub->Publish(connect_gazebo_to_ros_topic_msg, true);

}

GZ_REGISTER_MODEL_PLUGIN(GazeboOdometryPlugin);

} // namespace gazebo
