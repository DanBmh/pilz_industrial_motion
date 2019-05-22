/*
 * Copyright (c) 2018 Pilz GmbH & Co. KG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <ros/ros.h>
#include <ros/time.h>
#include <functional>

#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit_msgs/MotionPlanResponse.h>
#include <moveit_msgs/DisplayTrajectory.h>
#include <moveit/planning_scene/planning_scene.h>

#include <tf2_eigen/tf2_eigen.h>

#include <pilz_industrial_motion_testutils/xml_testdata_loader.h>
#include <pilz_industrial_motion_testutils/sequence.h>
#include <pilz_industrial_motion_testutils/lin.h>
#include <pilz_industrial_motion_testutils/gripper.h>

#include "test_utils.h"

#include "pilz_trajectory_generation/command_list_manager.h"
#include "pilz_trajectory_generation/tip_frame_getter.h"

const std::string ROBOT_DESCRIPTION_STR {"robot_description"};
const std::string EMPTY_VALUE {""};

const std::string TEST_DATA_FILE_NAME("testdata_file_name");

using testutils::hasStrictlyIncreasingTime;
using namespace pilz_trajectory_generation;
using namespace pilz_industrial_motion_testutils;

static std::string createManipulatorJointName(const size_t& joint_number)
{
  return std::string("prbt_joint_") + std::to_string(joint_number + 1);
}

static std::string createGripperJointName(const size_t& joint_number)
{
  switch (joint_number)
  {
  case 0:
    return "prbt_gripper_finger_left_joint";
  default:
    break;
  }
  throw std::runtime_error("Could not create gripper joint name");
}

class IntegrationTestCommandListManager : public testing::Test
{
protected:
  virtual void SetUp();

protected:
  // ros stuff
  ros::NodeHandle ph_ {"~"};
  robot_model::RobotModelConstPtr robot_model_ {
    robot_model_loader::RobotModelLoader(ROBOT_DESCRIPTION_STR).getModel() };
  std::shared_ptr<pilz_trajectory_generation::CommandListManager> manager_;
  planning_scene::PlanningScenePtr scene_;

  std::unique_ptr<pilz_industrial_motion_testutils::TestdataLoader> data_loader_;
};

void IntegrationTestCommandListManager::SetUp()
{
  // get necessary parameters
  if(!robot_model_)
  {
    FAIL() << "Robot model could not be loaded.";
  }

  std::string test_data_file_name;
  ASSERT_TRUE(ph_.getParam(TEST_DATA_FILE_NAME, test_data_file_name));

  // load the test data provider
  data_loader_.reset(new pilz_industrial_motion_testutils::XmlTestdataLoader{test_data_file_name, robot_model_});
  ASSERT_NE(nullptr, data_loader_) << "Failed to load test data by provider.";

  // Define and set the current scene and manager test object
  scene_ = std::make_shared<planning_scene::PlanningScene>(robot_model_);
  manager_ = std::make_shared<pilz_trajectory_generation::CommandListManager>(ph_, robot_model_);
}

/**
 * @brief Checks that each derived MoveItErrorCodeException contains the correct
 * error code.
 *
 */
TEST_F(IntegrationTestCommandListManager, TestExceptionErrorCodeMapping)
{
  std::shared_ptr<NegativeBlendRadiusException> nbr_ex {new NegativeBlendRadiusException("")};
  EXPECT_EQ(nbr_ex->getErrorCode(), moveit_msgs::MoveItErrorCodes::INVALID_MOTION_PLAN);

  std::shared_ptr<LastBlendRadiusNotZeroException> lbrnz_ex {new LastBlendRadiusNotZeroException ("")};
  EXPECT_EQ(lbrnz_ex->getErrorCode(), moveit_msgs::MoveItErrorCodes::INVALID_MOTION_PLAN);

  std::shared_ptr<StartStateSetException> sss_ex {new StartStateSetException ("")};
  EXPECT_EQ(sss_ex->getErrorCode(), moveit_msgs::MoveItErrorCodes::INVALID_ROBOT_STATE);

  std::shared_ptr<OverlappingBlendRadiiException> obr_ex {new OverlappingBlendRadiiException("")};
  EXPECT_EQ(obr_ex->getErrorCode(), moveit_msgs::MoveItErrorCodes::INVALID_MOTION_PLAN);

  std::shared_ptr<PlanningPipelineException> pp_ex {new PlanningPipelineException("")};
  EXPECT_EQ(pp_ex->getErrorCode(), moveit_msgs::MoveItErrorCodes::FAILURE);
}

/**
 * @brief Tests the concatenation of three motion commands.
 *
 * Test Sequence:
 *    1. Generate request with three trajectories and zero blend radius.
 *    2. Generate request with first trajectory and zero blend radius.
 *    3. Generate request with second trajectory and zero blend radius.
 *    4. Generate request with third trajectory and zero blend radius.
 *
 * Expected Results:
 *    1. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive
 *    2. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive
 *    3. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive
 *    4. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive
 *       resulting duration in step1 is approximately step2 + step3 + step4
 */
TEST_F(IntegrationTestCommandListManager, concatThreeSegments)
{
  Sequence seq {data_loader_->getSequence("ComplexSequence")};
  ASSERT_GE(seq.size(), 3u);
  seq.erase(3, seq.size());
  seq.setAllBlendRadiiToZero();

  RobotTrajCont res123_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res123_vec.size(), 1u);
  EXPECT_GT(res123_vec.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res123_vec.front())) << "Time steps not strictly positively increasing";

  ROS_INFO("step 2: only first segment");
  pilz_msgs::MotionSequenceRequest req_1;
  req_1.items.resize(1);
  req_1.items.at(0).req = seq.getCmd(0).toRequest();
  req_1.items.at(0).blend_radius = 0.;
  RobotTrajCont res1_vec {manager_->solve(scene_, req_1)};
  EXPECT_EQ(res1_vec.size(), 1u);
  EXPECT_GT(res1_vec.front()->getWayPointCount(), 0u);
  EXPECT_EQ(res1_vec.front()->getFirstWayPoint().getVariableCount(),
            req_1.items.at(0).req.start_state.joint_state.name.size());

  ROS_INFO("step 3: only second segment");
  pilz_msgs::MotionSequenceRequest req_2;
  req_2.items.resize(1);
  req_2.items.at(0).req = seq.getCmd(1).toRequest();
  req_2.items.at(0).blend_radius = 0.;
  RobotTrajCont res2_vec {manager_->solve(scene_, req_2)};
  EXPECT_EQ(res2_vec.size(), 1u);
  EXPECT_GT(res2_vec.front()->getWayPointCount(), 0u);
  EXPECT_EQ(res2_vec.front()->getFirstWayPoint().getVariableCount(),
            req_2.items.at(0).req.start_state.joint_state.name.size());


  ROS_INFO("step 4: only third segment");
  pilz_msgs::MotionSequenceRequest req_3;
  req_3.items.resize(1);
  req_3.items.at(0).req = seq.getCmd(2).toRequest();
  req_3.items.at(0).blend_radius = 0.;
  RobotTrajCont res3_vec {manager_->solve(scene_, req_3)};
  EXPECT_EQ(res3_vec.size(), 1u);
  EXPECT_GT(res3_vec.front()->getWayPointCount(), 0u);
  EXPECT_EQ(res3_vec.front()->getFirstWayPoint().getVariableCount(),
            req_3.items.at(0).req.start_state.joint_state.name.size());


  // durations for the different segments
  auto t1_2_3 = res123_vec.front()->getWayPointDurationFromStart(res123_vec.front()->getWayPointCount()-1);
  auto t1     = res1_vec.front()->getWayPointDurationFromStart(res123_vec.front()->getWayPointCount()-1);
  auto t2     = res2_vec.front()->getWayPointDurationFromStart(res123_vec.front()->getWayPointCount()-1);
  auto t3     = res3_vec.front()->getWayPointDurationFromStart(res123_vec.front()->getWayPointCount()-1);
  ROS_DEBUG_STREAM("total time: "<< t1_2_3 << " t1:" << t1 << " t2:" << t2 << " t3:" << t3);
  EXPECT_LT(fabs((t1_2_3-t1-t2-t3)), 0.4);
}


/**
 * @brief Tests if times are strictly increasing with selective blending
 *
 * Test Sequence:
 *    1. Generate request with three trajectories where only the first has a blend radius.
 *    1. Generate request with three trajectories where only the second has a blend radius.
 *
 * Expected Results:
 *    1. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly increasing in time
 *    2. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly increasing in time
 */
TEST_F(IntegrationTestCommandListManager, concatSegmentsSelectiveBlending)
{
  Sequence seq {data_loader_->getSequence("ComplexSequence")};
  ASSERT_GE(seq.size(), 3u);
  seq.erase(3, seq.size());
  seq.setAllBlendRadiiToZero();
  seq.setBlendRadius(0, 0.1);
  RobotTrajCont res1 {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res1.size(), 1u);
  EXPECT_GT(res1.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res1.front())) << "Time steps not strictly positively increasing";

  seq.setAllBlendRadiiToZero();
  seq.setBlendRadius(1, 0.1);
  RobotTrajCont res2 {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res2.size(), 1u);
  EXPECT_GT(res2.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res2.front())) << "Time steps not strictly positively increasing";
}

/**
 * @brief Tests the concatenation of two ptp commands
 *
 * Test Sequence:
 *    1. Generate request with two PTP trajectories and zero blend radius.
 *
 * Expected Results:
 *    1. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive.
 */
TEST_F(IntegrationTestCommandListManager, concatTwoPtpSegments)
{
  Sequence seq {data_loader_->getSequence("PtpPtpSequence")};
  ASSERT_GE(seq.size(), 2u);
  seq.setAllBlendRadiiToZero();

  RobotTrajCont res_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_vec.size(), 1u);
  EXPECT_GT(res_vec.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res_vec.front()));
}

/**
 * @brief Tests the concatenation of ptp and a lin command
 *
 * Test Sequence:
 *    1. Generate request with PTP and LIN trajectory and zero blend radius.
 *
 * Expected Results:
 *    1. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive.
 */
TEST_F(IntegrationTestCommandListManager, concatPtpAndLinSegments)
{
  Sequence seq {data_loader_->getSequence("PtpLinSequence")};
  ASSERT_GE(seq.size(), 2u);
  seq.setAllBlendRadiiToZero();

  RobotTrajCont res_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_vec.size(), 1u);
  EXPECT_GT(res_vec.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res_vec.front()));
}

/**
 * @brief Tests the concatenation of a lin and a ptp command
 *
 * Test Sequence:
 *    1. Generate request with LIN and PTP trajectory and zero blend radius.
 *
 * Expected Results:
 *    1. Generation of concatenated trajectory is successful.
 *       All time steps of resulting trajectory are strictly positive.
 */
TEST_F(IntegrationTestCommandListManager, concatLinAndPtpSegments)
{
  Sequence seq {data_loader_->getSequence("LinPtpSequence")};
  ASSERT_GE(seq.size(), 2u);
  seq.setAllBlendRadiiToZero();

  RobotTrajCont res_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_vec.size(), 1u);
  EXPECT_GT(res_vec.front()->getWayPointCount(), 0u);
  EXPECT_TRUE(hasStrictlyIncreasingTime(res_vec.front()));
}

/**
 * @brief Tests the blending of motion commands
 *
 *  - Test Sequence:
 *    1. Generate request with two trajectories and request blending.
 *
 *  - Expected Results:
 *    1. blending is successful, result trajectory is not empty
 */
TEST_F(IntegrationTestCommandListManager, blendTwoSegments)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_EQ(seq.size(), 2u);
  pilz_msgs::MotionSequenceRequest req {seq.toRequest()};
  RobotTrajCont res_vec {manager_->solve(scene_, req)};
  EXPECT_EQ(res_vec.size(), 1u);
  EXPECT_GT(res_vec.front()->getWayPointCount(), 0u);
  EXPECT_EQ(res_vec.front()->getFirstWayPoint().getVariableCount(),
            req.items.at(0).req.start_state.joint_state.name.size());


  ros::NodeHandle nh;
  ros::Publisher pub = nh.advertise<moveit_msgs::DisplayTrajectory>("my_planned_path", 1);
  ros::Duration duration(1.0); // wait to notify possible subscribers
  duration.sleep();

  moveit_msgs::DisplayTrajectory displayTrajectory;
  moveit_msgs::RobotTrajectory rob_traj_msg;
  res_vec.front()->getRobotTrajectoryMsg(rob_traj_msg);
  displayTrajectory.trajectory.push_back(rob_traj_msg);
  pub.publish(displayTrajectory);
}

// ------------------
// FAILURE cases
// ------------------

/**
 * @brief Tests sending an empty blending request.
 *
 * Test Sequence:
 *    1. Generate empty request and request blending.
 *
 * Expected Results:
 *    1. blending is successful, result trajectory container is empty
 */
TEST_F(IntegrationTestCommandListManager, emptyList)
{
  pilz_msgs::MotionSequenceRequest empty_list;
  RobotTrajCont res_vec {manager_->solve(scene_, empty_list)};
  EXPECT_TRUE(res_vec.empty());
}

/**
 * @brief Checks that exception is thrown if first goal is not reachable.
 *
 * Test Sequence:
 *    1. Generate request with first goal out of workspace.
 *
 * Expected Results:
 *    1. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, firstGoalNotReachable)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_GE(seq.size(), 2u);
  LinCart& lin {seq.getCmd<LinCart>(0)};
  lin.getGoalConfiguration().getPose().position.y = 2700;
  EXPECT_THROW(manager_->solve(scene_, seq.toRequest()), PlanningPipelineException);
}

/**
 * @brief Checks that exception is thrown if second goal has a start state.
 *
 * Test Sequence:
 *    1. Generate request, second goal has an invalid start state set.
 *
 * Expected Results:
 *    1. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, startStateNotFirstGoal)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_GE(seq.size(), 2u);
  const LinCart& lin {seq.getCmd<LinCart>(0)};
  pilz_msgs::MotionSequenceRequest req {seq.toRequest()};
  req.items.at(1).req.start_state = lin.getGoalConfiguration().toMoveitMsgsRobotState();
  EXPECT_THROW(manager_->solve(scene_, req), StartStateSetException);
}

/**
 * @brief Checks that exception is thrown in case of blending request with
 * negative blend_radius.
 *
 *  Test Sequence:
 *    1. Generate request, first goal has negative blend_radius.
 *
 *  Expected Results:
 *    1. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, blendingRadiusNegative)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_GE(seq.size(), 2u);
  seq.setBlendRadius(0,-0.3);
  EXPECT_THROW(manager_->solve(scene_, seq.toRequest()), NegativeBlendRadiusException);
}

/**
 * @brief Checks that exception is thrown if last blend radius is not zero.
 *
 *
 * Test Sequence:
 *    1. Generate request, second goal has non-zero blend_radius.
 *
 * Expected Results:
 *    1. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, lastBlendingRadiusNonZero)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_EQ(seq.size(), 2u);
  seq.setBlendRadius(1, 0.03);
  EXPECT_THROW(manager_->solve(scene_, seq.toRequest()), LastBlendRadiusNotZeroException);
}

/**
 * @brief Checks that exception is thrown if blend radius is greater than the
 * segment.
 *
 * Test Sequence:
 *    1. Generate request with huge blending radius, so that trajectories are
 *       completely inside
 *
 * Expected Results:
 *    2. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, blendRadiusGreaterThanSegment)
{
  Sequence seq {data_loader_->getSequence("SimpleSequence")};
  ASSERT_GE(seq.size(), 2u);
  seq.setBlendRadius(0, 42.0);
  EXPECT_THROW(manager_->solve(scene_, seq.toRequest()), BlendingFailedException);
}

/**
 * @brief Checks that exception is thrown if two consecutive blend radii
 * overlap.
 *
 * Test Sequence:
 *    1. Generate request with three trajectories
 *    2. Increase second blend radius, so that the radii overlap
 *
 * Expected Results:
 *    1. blending succeeds, result trajectory is not empty
 *    2. Exception is thrown.
 */
TEST_F(IntegrationTestCommandListManager, blendingRadiusOverlapping)
{
  Sequence seq {data_loader_->getSequence("ComplexSequence")};
  ASSERT_GE(seq.size(), 3u);
  seq.erase(3, seq.size());

  RobotTrajCont res_valid_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_valid_vec.size(), 1u);
  EXPECT_GT(res_valid_vec.front()->getWayPointCount(), 0u);

  // calculate distance from first to second goal
  const PtpJointCart& ptp {seq.getCmd<PtpJointCart>(0)};
  const CircInterimCart& circ {seq.getCmd<CircInterimCart>(1)};
  Eigen::Isometry3d p1, p2;
  tf2::fromMsg(ptp.getGoalConfiguration().getPose(), p1);
  tf2::fromMsg(circ.getGoalConfiguration().getPose(), p2);
  auto distance = (p2.translation()-p1.translation()).norm();

  seq.setBlendRadius(1, (distance - seq.getBlendRadius(0) + 0.01) ); // overlapping radii
  EXPECT_THROW(manager_->solve(scene_, seq.toRequest()), OverlappingBlendRadiiException);
}

/**
 * @brief Tests if the planned execution time scales correctly with the number
 * of repetitions.
 *
 * Test Sequence:
 *    1. Generate trajectory and save calculated execution time.
 *    2. Generate request with repeated path along the points from Test Step 1
 *      (repeated two times).
 *
 * Expected Results:
 *    1. Blending succeeds, result trajectory is not empty.
 *    2. Blending succeeds, planned execution time should be approx N times
 *       the single planned execution time.
 */
TEST_F(IntegrationTestCommandListManager, TestExecutionTime)
{
  Sequence seq {data_loader_->getSequence("ComplexSequence")};
  ASSERT_GE(seq.size(), 2u);
  RobotTrajCont res_single_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_single_vec.size(), 1u);
  EXPECT_GT(res_single_vec.front()->getWayPointCount(), 0u);

  pilz_msgs::MotionSequenceRequest req {seq.toRequest()};
  // Create large request by making copies of the original sequence commands
  // and adding them to the end of the original sequence.
  const size_t N {req.items.size()};
  for(size_t i = 0; i<N; ++i)
  {
    pilz_msgs::MotionSequenceItem item {req.items.at(i)};
    if (i == 0)
    {
      // Remove start state because only the first request
      // is allowed to have a start state in a sequence.
      item.req.start_state = moveit_msgs::RobotState();
    }
    req.items.push_back(item);
  }

  RobotTrajCont res_n_vec {manager_->solve(scene_, req)};
  EXPECT_EQ(res_n_vec.size(), 1u);
  EXPECT_GT(res_n_vec.front()->getWayPointCount(), 0u);

  const double trajectory_time_1 = res_single_vec.front()->getWayPointDurationFromStart(
        res_single_vec.front()->getWayPointCount()-1);
  const double trajectory_time_n = res_n_vec.front()->getWayPointDurationFromStart(
        res_n_vec.front()->getWayPointCount()-1);
  double multiplicator = req.items.size() / N;
  EXPECT_LE(trajectory_time_n, trajectory_time_1*multiplicator);
  EXPECT_GE(trajectory_time_n, trajectory_time_1 * multiplicator * 0.5);
}

/**
 * @brief Tests if it possible to send requests which contain more than
 * one group.
 *
 * Please note: This test is still quite trivial. It does not check the
 * "correctness" of a calculated trajectory. It only checks that for each
 * group and group change there exists a calculated trajectory.
 *
 */
TEST_F(IntegrationTestCommandListManager, TestDifferentGroups)
{
  Sequence seq {data_loader_->getSequence("ComplexSequenceWithGripper")};
  ASSERT_GE(seq.size(), 1u);
  // Count the number of group changes in the given sequence
  unsigned int num_groups {1};
  std::string last_group_name {seq.getCmd(0).getPlanningGroup()};
  for (size_t i = 1; i < seq.size(); ++i)
  {
    if (seq.getCmd(i).getPlanningGroup() != last_group_name)
    {
      ++num_groups;
      last_group_name = seq.getCmd(i).getPlanningGroup();
    }
  }

  RobotTrajCont res_single_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_single_vec.size(), num_groups);
  for(RobotTrajCont::size_type i = 0; i<res_single_vec.size(); ++i)
  {
    EXPECT_GT(res_single_vec.at(i)->getWayPointCount(), 0u);
  }
}

/**
 * @brief Checks that no exception is thrown if two gripper commands are
 * blended.
 *
 */
TEST_F(IntegrationTestCommandListManager, TestGripperCmdBlending)
{
  Sequence seq {data_loader_->getSequence("PureGripperSequence")};
  ASSERT_GE(seq.size(), 2u);
  ASSERT_TRUE(seq.cmdIsOfType<Gripper>(0));
  ASSERT_TRUE(seq.cmdIsOfType<Gripper>(1));

  // Ensure that blending is requested for gripper commands.
  seq.setBlendRadius(0, 1.0);
  RobotTrajCont res_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_EQ(res_vec.size(), 1u);
}

/**
 * @brief Tests the execution of a sequence in which each group states a start
 * state only consisting of joints of the corresonding group.
 *
 * Test Sequence:
 *    1. Create sequence request for which each start state only consists of
 *        joints of the corresonding group
 *
 * Expected Results:
 *    1. Trajectory generation is successful, result trajectory is not empty.
 *
 *
 */
TEST_F(IntegrationTestCommandListManager, TestGroupSpecificStartState)
{
  using std::placeholders::_1;

  Sequence seq {data_loader_->getSequence("ComplexSequenceWithGripper")};
  ASSERT_GE(seq.size(), 4u);
  seq.erase(4, seq.size());

  Gripper& gripper {seq.getCmd<Gripper>(0)};
  gripper.getStartConfiguration().setCreateJointNameFunc(std::bind(&createGripperJointName, _1));
  // By deleting the model we guarantee that the start state only consists
  // of joints of the gripper group without the manipulator
  gripper.getStartConfiguration().clearModel();

  PtpJointCart& ptp {seq.getCmd<PtpJointCart>(1)};
  ptp.getStartConfiguration().setCreateJointNameFunc(std::bind(&createManipulatorJointName, _1));
  // By deleting the model we guarantee that the start state only consists
  // of joints of the manipulator group without the gripper
  ptp.getStartConfiguration().clearModel();

  RobotTrajCont res_vec {manager_->solve(scene_, seq.toRequest())};
  EXPECT_GE(res_vec.size(), 1u);
  EXPECT_GT(res_vec.front()->getWayPointCount(), 0u);
}

/**
 * @brief Checks that exception is thrown if Tip-Frame is requested for
 * an end-effector.
 */
TEST_F(IntegrationTestCommandListManager, TestTipFrameNoEndEffector)
{
  Gripper gripper_cmd {data_loader_->getGripper("open_gripper")};
  EXPECT_THROW(getTipFrame(robot_model_->getJointModelGroup(gripper_cmd.getPlanningGroup())), EndEffectorException);
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "integrationtest_command_list_manager");
  testing::InitGoogleTest(&argc, argv);

  ros::NodeHandle nh;

  return RUN_ALL_TESTS();
}
