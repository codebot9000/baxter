/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, University of Colorado, Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman
   Desc:   ros_control hardware interface for a Baxter arm
*/

#include <baxter_control/arm_hardware_interface.h>

namespace baxter_control
{

ArmHardwareInterface::ArmHardwareInterface(const std::string &arm_name)
  : ArmInterface(arm_name),
    state_msg_(new sensor_msgs::JointState()),
    cuff_squeezed_previous(false)
{
  // Populate joints in this arm
  joint_names_.push_back(arm_name_+"_e0");
  joint_names_.push_back(arm_name_+"_e1");
  joint_names_.push_back(arm_name_+"_s0");
  joint_names_.push_back(arm_name_+"_s1");
  joint_names_.push_back(arm_name_+"_w0");
  joint_names_.push_back(arm_name_+"_w1");
  joint_names_.push_back(arm_name_+"_w2");
  n_dof_ = joint_names_.size();

  // Resize vectors
  joint_position_.resize(n_dof_);
  joint_velocity_.resize(n_dof_);
  joint_effort_.resize(n_dof_);
  joint_position_command_.resize(n_dof_);
  joint_effort_command_.resize(n_dof_);
  joint_velocity_command_.resize(n_dof_);
  output_command_msg_.angles.resize(n_dof_);
  output_command_msg_.names.resize(n_dof_);
  trajectory_command_msg_.joint_names.resize(n_dof_);

  for (std::size_t i = 0; i < n_dof_; ++i)
  {
    joint_position_[i] = 0.0;
    joint_velocity_[i] = 0.0;
    joint_effort_[i] = 0.0;
    joint_position_command_[i] = 0.0;
    joint_effort_command_[i] = 0.0;
    joint_velocity_command_[i] = 0.0;
    trajectory_command_msg_.joint_names[i] = joint_names_[i];
  }

  // Set trajectory to have one point
  trajectory_msgs::JointTrajectoryPoint single_pt;
  single_pt.positions.resize(n_dof_);
  single_pt.time_from_start = ros::Duration(0);
  trajectory_command_msg_.points.push_back(single_pt);

  trajectory_msgs::JointTrajectoryPoint single_pt2;
  single_pt2.positions.resize(n_dof_);
  single_pt2.time_from_start = ros::Duration(0.5);
  trajectory_command_msg_.points.push_back(single_pt2);
}

ArmHardwareInterface::~ArmHardwareInterface()
{
}

bool ArmHardwareInterface::init(
  hardware_interface::JointStateInterface&    js_interface,
  hardware_interface::EffortJointInterface&   ej_interface,
  hardware_interface::VelocityJointInterface& vj_interface,
  hardware_interface::PositionJointInterface& pj_interface)
{
  for (std::size_t i = 0; i < n_dof_; ++i)
  {
    // Create joint state interface for all joints
    js_interface.registerHandle(hardware_interface::JointStateHandle(
        joint_names_[i], &joint_position_[i], &joint_velocity_[i], &joint_effort_[i]));

    // Create position joint interface
    pj_interface.registerHandle(hardware_interface::JointHandle(
        js_interface.getHandle(joint_names_[i]),&joint_position_command_[i]));
  }

  // Start publishers
  pub_position_command_ = nh_.advertise<baxter_msgs::JointPositions>("/robot/limb/"+arm_name_+
                          "/command_joint_angles",10);

  pub_trajectory_command_ = nh_.advertise<trajectory_msgs::JointTrajectory>("/robot/"+arm_name_+
                            "_joint_trajectory_controller/command",10);

  // Start subscribers
  sub_joint_state_ = nh_.subscribe<sensor_msgs::JointState>("/robot/limb/" + arm_name_ +
                     "/joint_states", 1, &ArmHardwareInterface::stateCallback, this);

  cuff_squeezed_sub_ = nh_.subscribe<baxter_msgs::DigitalIOState>("/sdk/robot/digital_io/" +
                       arm_name_ + "_lower_cuff/state",
                       1, &ArmHardwareInterface::cuffSqueezedCallback, this);

  // Wait for first state message to be recieved
  while(ros::ok() && state_msg_timestamp_.toSec() == 0)
  {
    ROS_INFO_STREAM_NAMED(arm_name_,"Waiting for first state message to be recieved");
    ros::spinOnce();
    ros::Duration(0.25).sleep();
  }

  // Make sure Rethink hasn't changed anything with the ordering in which joints are published in
  // joint state message
  if( state_msg_->name[0].compare("e0") != 0 || state_msg_->name[1].compare("e1") != 0 ||
    state_msg_->name[2].compare("s0") != 0 || state_msg_->name[3].compare("s1") != 0 ||
    state_msg_->name[4].compare("w0") != 0 || state_msg_->name[5].compare("w1") != 0 ||
    state_msg_->name[6].compare("w2") != 0)
  {
    ROS_ERROR_STREAM_NAMED(arm_name_,"It seems the joint state message for arm " << arm_name_ <<
      " has changed its ordering. The order has been hard coded into the arm_hardware_interface and "
      << "now it needs to be fixed");
    return false;
  }

  // Set the initial command values based on current state
  for (std::size_t i = 0; i < n_dof_; ++i)
  {
    joint_position_command_[i] = state_msg_->position[i];
    //ROS_DEBUG_STREAM_NAMED("temp","set joint " << joint_names_[i] << " to position " << joint_position_command_[i]);

    // Pre-load the joint names into the output messages just once
    output_command_msg_.names[i] = joint_names_[i];
  }

  ROS_INFO_NAMED(arm_name_, "Loaded baxter_hardware_interface.");
  return true;
}

void ArmHardwareInterface::stateCallback(const sensor_msgs::JointStateConstPtr& msg)
{
  // Copy the latest message into a buffer
  state_msg_ = msg;
  state_msg_timestamp_ = ros::Time::now();
}

/*
  void ArmHardwareInterface::loadJointStateNameMap()
  {
  ROS_DEBUG_STREAM_NAMED(arm_name_,"Loading joint state map using state msg:\n"
  << *state_msg_);

  int controlled_joints_count = 0;
  for (std::size_t i = 0; i < joint_names_.size(); ++i)
  {
  // Find the name in the state message that corresponds to the transmission index
  std::vector<std::string>::const_iterator name_it;
  name_it = std::find(state_msg_->name.begin(), state_msg_->name.end(), joint_names_[i]);

  // Error check
  if( name_it == state_msg_->name.end() )
  {
  ROS_DEBUG_STREAM_NAMED(arm_name_,"No mapping for '" <<
  joint_names_[i] << "' in Baxter state message");
  }ppp
  else
  {
  // We will use this in our published command message - keep track of size of vector
  controlled_joints_count++;
  }

  // Copy index of found joint name
  joint_interface_to_joint_state_[i] = name_it - state_msg_->name.begin() - 1;
  }

  std::copy(joint_interface_to_joint_state_.begin(), joint_interface_to_joint_state_.end(),
  std::ostream_iterator<double>(std::cout, "\n"));

  // Resize the output publisher message to match input message
  output_command_msg_.angles.resize(controlled_joints_count);
  output_command_msg_.names.resize(controlled_joints_count);

  ROS_DEBUG_STREAM_NAMED(arm_name_,"Matched " << controlled_joints_count <<
  " joints from state message and URDF transmissions");

  // \temp
  sleep(2);
  }
*/

bool ArmHardwareInterface::stateExpired()
{
  // Check that we have a non-expired state message
  // \todo lower the expiration duration
  if( ros::Time::now() > state_msg_timestamp_ + ros::Duration(STATE_EXPIRED_TIMEOUT)) // check that the message timestamp is no older than 1 second
  {

    ROS_WARN_STREAM_THROTTLE_NAMED(1,arm_name_,"State expired. Last recieved state " << (ros::Time::now() - state_msg_timestamp_).toSec() << " seconds ago." );
    return true;
  }
  return false;
}

void ArmHardwareInterface::read()
{
  if( stateExpired() )
    return;

  // Copy state message to our datastructures
  for (std::size_t i = 0; i < n_dof_; ++i)
  {
    //ROS_INFO_STREAM_NAMED("read","on index " << i << " which maps to " << i);
    //ROS_INFO_STREAM_NAMED("read","and the max message is " << state_msg_->position.size() );
    //ROS_INFO_STREAM_NAMED("read","joint_position size is " << joint_position_.size());
    //ROS_INFO_STREAM_NAMED("read","state_msg position size is " << state_msg_->position.size());
    //ROS_INFO_STREAM_NAMED("read","position is " << state_msg_->position[i]);

    joint_position_[i] = state_msg_->position[i];
    joint_velocity_[i] = state_msg_->velocity[i];
    joint_effort_[i] = state_msg_->effort[i];
  }
}

void ArmHardwareInterface::write()
{
  if( stateExpired() )
    return;


  for (std::size_t i = 0; i < n_dof_; ++i)
  {
    //ROS_INFO_STREAM_NAMED("write","id = "<<i);
    //ROS_INFO_STREAM_NAMED("write","name = " << joint_names_[i]);
    //ROS_INFO_STREAM_NAMED("write","mapping = " << i);

    output_command_msg_.angles[i] = joint_position_command_[i];
  }

  pub_position_command_.publish(output_command_msg_);
}

void ArmHardwareInterface::cuffSqueezedCallback(const baxter_msgs::DigitalIOStateConstPtr& msg)
{
  // Check if button is pressed
  if( msg->state == 1 )
  {
    cuff_squeezed_previous = true;
  }
  else  // button not pressed
  {
    if ( cuff_squeezed_previous )
    {
      // Publish this new trajectory just once, on cuff release
      //trajectory_command_msg_.header.stamp = ros::Time::now() + ros::Duration(1.0);

      // Update the trajectory message with the current positions
      for (std::size_t i = 0; i < n_dof_; ++i)
      {
        trajectory_command_msg_.points[0].positions[i] = joint_position_[i];
        trajectory_command_msg_.points[1].positions[i] = joint_position_[i];
      }

      // Send a trajectory
      pub_trajectory_command_.publish(trajectory_command_msg_);
    }

    cuff_squeezed_previous = false;
  }
}


} // namespace
