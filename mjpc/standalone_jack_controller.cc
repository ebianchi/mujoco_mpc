#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>
#include <vector>

#include <absl/flags/parse.h>
#include <lcm/lcm-cpp.hpp>

#include "dairlib/lcmt_c3_state.hpp"
#include "dairlib/lcmt_object_state.hpp"
#include "dairlib/lcmt_robot_output.hpp"
#include "dairlib/lcmt_timestamped_saved_traj.hpp"
#include "mjpc/agent.h"
#include "mjpc/array_safety.h"
#include "mjpc/tasks/tasks.h"
#include "mjpc/threadpool.h"

// #include "mjpc/task.h"
namespace mju = ::mujoco::util_mjpc;

std::unique_ptr<mjpc::ResidualFn> residual;

extern "C" {
void sensor(const mjModel* m, mjData* d, int stage);
}

// sensor callback
void sensor(const mjModel* model, mjData* data, int stage) {
  residual->Residual(model, data, data->sensordata);
}

class Handler {
 public:
  Handler() {
    franka_positions_ = std::vector<double>(3);
    franka_velocities_ = std::vector<double>(3);
    jack_positions_ = std::vector<double>(7);
    jack_velocities_ = std::vector<double>(6);
    franka_target_ = std::vector<double>(3);
    jack_target_ = std::vector<double>(7);
    jack_final_target_ = std::vector<double>(7);
  }
  ~Handler() {}
  void handle_mpc_state(const lcm::ReceiveBuffer* rbuf, const std::string& chan,
                        const dairlib::lcmt_c3_state* msg) {
    time_ = 1e-6 * msg->utime;

    // msg (from dairlib) is an lcmt_c3_state with ordering:
    // ee_xyz, jack_quat, jack_xyz, ee_lin_vel, jack_ang_vel, jack_lin_vel
    // MuJoCo does state orderings of position terms first, then rotation terms.
    franka_positions_ = {msg->state.begin(), msg->state.begin() + 3};
    franka_velocities_ = {msg->state.begin() + 10, msg->state.begin() + 10 + 3};
    // Flip ordering to match MuJoCo's linear-first convention.
    jack_positions_[0] = msg->state[7];
    jack_positions_[1] = msg->state[8];
    jack_positions_[2] = msg->state[9];
    jack_positions_[3] = msg->state[3];
    jack_positions_[4] = msg->state[4];
    jack_positions_[5] = msg->state[5];
    jack_positions_[6] = msg->state[6];
    // Flip ordering to match MuJoCo's linear-first convention.
    // NOTE:  This is different from PlateBalancing example, which I believe did
    // not adequately flip the velocities for the object.
    jack_velocities_[0] = msg->state[16];
    jack_velocities_[1] = msg->state[17];
    jack_velocities_[2] = msg->state[18];
    jack_velocities_[3] = msg->state[13];
    jack_velocities_[4] = msg->state[14];
    jack_velocities_[5] = msg->state[15];
  }
  void handle_mpc_target(const lcm::ReceiveBuffer* rbuf,
                         const std::string& chan,
                         const dairlib::lcmt_c3_state* msg) {
    franka_target_ = {msg->state.begin(), msg->state.begin() + 3};
    // Flip ordering to match MuJoCo's linear-first convention.
    jack_target_[0] = msg->state[7];
    jack_target_[1] = msg->state[8];
    jack_target_[2] = msg->state[9];
    jack_target_[3] = msg->state[3];
    jack_target_[4] = msg->state[4];
    jack_target_[5] = msg->state[5];
    jack_target_[6] = msg->state[6];
  }
  void handle_mpc_final_target(const lcm::ReceiveBuffer* rbuf,
                               const std::string& chan,
                               const dairlib::lcmt_c3_state* msg) {
    // Flip ordering to match MuJoCo's linear-first convention.
    jack_final_target_[0] = msg->state[7];
    jack_final_target_[1] = msg->state[8];
    jack_final_target_[2] = msg->state[9];
    jack_final_target_[3] = msg->state[3];
    jack_final_target_[4] = msg->state[4];
    jack_final_target_[5] = msg->state[5];
    jack_final_target_[6] = msg->state[6];
  }
  double time_;
  std::vector<double> franka_positions_;
  std::vector<double> franka_velocities_;
  std::vector<double> jack_positions_;
  std::vector<double> jack_velocities_;
  std::vector<double> franka_target_;
  std::vector<double> jack_target_;
  std::vector<double> jack_final_target_;
};

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  lcm::LCM lcm;
  if (!lcm.good()) return 1;

  // Set up the variables
  mjModel* model = new mjModel();
  std::vector<std::shared_ptr<mjpc::Task>> tasks = mjpc::GetTasks();
  // Note:  The task index needs to match the order of the tasks returned above.
  std::shared_ptr<mjpc::Task> task = tasks[17];
  std::unique_ptr<mjpc::ResidualFn> residual_fn;

  // Load the model
  std::string filename = task->XmlPath();
  constexpr int kErrorLength = 1024;
  char load_error[kErrorLength] = "";
  model = mj_loadXML(filename.c_str(), nullptr, load_error, kErrorLength);
  if (load_error[0]) {
    int error_length = mju::strlen_arr(load_error);
    if (load_error[error_length - 1] == '\n') {
      load_error[error_length - 1] = '\0';
    }
  }

  // Initialize the agent
  auto agent = std::make_shared<mjpc::Agent>(model, task);

  // Weird thing, where I have to define the sensor separate from the model??
  residual = agent->ActiveTask()->Residual();
  // order matters, need to set the global variable as the sensor callback
  mjcb_sensor = sensor;

  int actor_pos_start = 7;
  int object_pos_start = 0;
  int object_quat_start = 3;
  std::vector<double> action = {-0.1, 0.1, 0};
  // TODO:  seems to be in order object pos, object quat, ee pos but unsure.
  std::vector<double> qpos = {0.7, 0.00, 0.485, 1, 0, 0, 0, 0.55, 0.0, 0.45};
  std::vector<double> qvel = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  // Mocap bodies in order:  intermediate goal, final goal, end effector goal.
  std::vector<double> mocap_pos = {0.45, 0.1, 0.032,  // intermediate goal
                                   0.45, 0.2, 0.032,  // final goal
                                   0.45, 0, 0.132};   // end effector goal
  std::vector<double> mocap_quat = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  std::vector<double> user_data = {0};  // size is actually 0
  double time = 0;
  mjpc::State mpc_state = mjpc::State();

  mjpc::ThreadPool plan_pool(agent->planner_threads());
  mpc_state.Initialize(model);
  mpc_state.Allocate(model);

  /**
   * lcmtypes
   */
  dairlib::lcmt_robot_output robot_state;
  dairlib::lcmt_object_state jack_state;
  dairlib::lcmt_timestamped_saved_traj actor_traj;
  dairlib::lcmt_timestamped_saved_traj object_traj;
  dairlib::lcmt_saved_traj raw_actor_traj;
  dairlib::lcmt_saved_traj raw_object_traj;
  dairlib::lcmt_trajectory_block actor_pos_traj;
  dairlib::lcmt_trajectory_block actor_force_traj;
  dairlib::lcmt_trajectory_block object_pos_traj;
  dairlib::lcmt_trajectory_block object_quat_traj;
  int horizon = mjpc::GetNumberOrDefault(1.0e-2, model, "agent_horizon") /
                mjpc::GetNumberOrDefault(1.0e-2, model, "agent_timestep");
  horizon += 1;
  raw_actor_traj.trajectories = std::vector<dairlib::lcmt_trajectory_block>(2);
  raw_object_traj.trajectories = std::vector<dairlib::lcmt_trajectory_block>(2);
  actor_force_traj.num_points = horizon;
  actor_force_traj.num_datatypes = 3;
  actor_force_traj.datapoints =
      std::vector<std::vector<double>>(3, std::vector<double>(horizon, 0.0));
  actor_force_traj.time_vec = std::vector<double>(horizon);
  actor_force_traj.datatypes = std::vector<std::string>(3);
  actor_force_traj.trajectory_name = "end_effector_force_target";

  actor_pos_traj.num_points = horizon;
  actor_pos_traj.num_datatypes = 3;
  actor_pos_traj.datapoints =
      std::vector<std::vector<double>>(3, std::vector<double>(horizon));
  actor_pos_traj.time_vec = std::vector<double>(horizon);
  actor_pos_traj.datatypes = std::vector<std::string>(3);
  actor_pos_traj.trajectory_name = "end_effector_position_target";

  object_pos_traj.num_points = horizon;
  object_pos_traj.num_datatypes = 3;
  object_pos_traj.datapoints =
      std::vector<std::vector<double>>(3, std::vector<double>(horizon));
  object_pos_traj.time_vec = std::vector<double>(horizon);
  object_pos_traj.datatypes = std::vector<std::string>(3);
  object_pos_traj.trajectory_name = "object_position_target";

  object_quat_traj.num_points = horizon;
  object_quat_traj.num_datatypes = 4;
  object_quat_traj.datapoints =
      std::vector<std::vector<double>>(4, std::vector<double>(horizon));
  object_quat_traj.time_vec = std::vector<double>(horizon);
  object_quat_traj.datatypes = std::vector<std::string>(4);
  object_quat_traj.trajectory_name = "object_orientation_target";

  raw_actor_traj.trajectory_names = {actor_force_traj.trajectory_name,
                                     actor_pos_traj.trajectory_name};
  raw_actor_traj.num_trajectories = 2;
  raw_object_traj.trajectory_names = {object_pos_traj.trajectory_name,
                                      object_quat_traj.trajectory_name};
  raw_object_traj.num_trajectories = 2;
  Handler handlerObject;
  lcm.subscribe("C3_ACTUAL", &Handler::handle_mpc_state, &handlerObject);
  lcm.subscribe("C3_TARGET", &Handler::handle_mpc_target, &handlerObject);
  lcm.subscribe("C3_FINAL_TARGET", &Handler::handle_mpc_final_target,
                &handlerObject);
  /**
   * End lcmtypes
   */

  auto planner_id = mjpc::GetNumberOrDefault(0, model, "agent_planner");
  agent->GetModel()->opt.timestep =
      mjpc::GetNumberOrDefault(1.0e-2, model, "agent_timestep");

  std::cout << "planner id: " << planner_id << std::endl;
  std::cout << "horizon: " << horizon << std::endl;
  std::cout << "na: " << model->na << std::endl;
  std::cout << "model integrator: " << model->opt.integrator << std::endl;
  std::cout << "planning threads: " << agent->planner_threads() << std::endl;
  std::cout << "num parameters: " << agent->ActivePlanner().NumParameters()
            << std::endl;

  // control loop
  while (true) {
    if (lcm.getFileno() != 0) {
      // shouldn't need two handles, but doesn't work otherwise
      lcm.handle();
      lcm.handle();
    }

    time = handlerObject.time_;
    // Order of the positions are different between C3 and mjmpc:
    // C3 order:  {ee_xyz, object_quat, object_xyz}
    // MJPC order:  qpos is {object_xyz, object_quat, ee_xyz}
    mju_copy(qpos.data(), handlerObject.jack_positions_.data(), 7);
    mju_copy(qpos.data() + 7, handlerObject.franka_positions_.data(), 3);
    mju_copy(qvel.data(), handlerObject.jack_velocities_.data(), 6);
    mju_copy(qvel.data() + 6, handlerObject.franka_velocities_.data(), 3);

    // Copy over the new target state:  mocap ordering is intermediate, final,
    // then ee targets.
    mju_copy(mocap_pos.data(), handlerObject.jack_target_.data(), 3);
    mju_copy(mocap_pos.data() + 3, handlerObject.jack_final_target_.data(), 3);
    mju_copy(mocap_pos.data() + 6, handlerObject.franka_target_.data(), 3);

    mju_copy(mocap_quat.data(), handlerObject.jack_target_.data() + 3, 4);
    mju_copy(
      mocap_quat.data() + 4, handlerObject.jack_final_target_.data() + 3, 4);

    action[0] = actor_force_traj.datapoints[0][0];
    action[1] = actor_force_traj.datapoints[1][0];
    action[2] = actor_force_traj.datapoints[2][0];

    mpc_state.Set(model, qpos.data(), qvel.data(), action.data(),
                  mocap_pos.data(), mocap_quat.data(), user_data.data(), time);

    // agent plan
    agent->ActivePlanner().SetState(mpc_state);
    agent->ActivePlanner().OptimizePolicy(horizon, plan_pool);

    // copy trajectory to send via lcm
    auto trajectory = agent->ActivePlanner().BestTrajectory();

    // Scaling the action -- need to convert the ctrlrange to forcerange, both
    // defined in end_effector.xml.
    actor_force_traj.time_vec = trajectory->times;
    for (int k = 0; k < trajectory->horizon; ++k) {
      actor_force_traj.datapoints[0][k] =
          1.5 * trajectory->actions[0 + k * trajectory->dim_action];
      actor_force_traj.datapoints[1][k] =
          1.5 * trajectory->actions[1 + k * trajectory->dim_action];
      actor_force_traj.datapoints[2][k] =
          1.5 * trajectory->actions[2 + k * trajectory->dim_action];
    }
    actor_pos_traj.time_vec = trajectory->times;
    for (int k = 0; k < trajectory->horizon; ++k) {
      for (int n = 0; n < actor_pos_traj.datatypes.size(); ++n) {
        actor_pos_traj.datapoints[n][k] =
            trajectory
                ->states[(n + actor_pos_start) + k * trajectory->dim_state];
      }
    }
    object_pos_traj.time_vec = trajectory->times;
    for (int k = 0; k < trajectory->horizon; ++k) {
      for (int n = 0; n < object_pos_traj.datatypes.size(); ++n) {
        object_pos_traj.datapoints[n][k] =
            trajectory
                ->states[(n + object_pos_start) + k * trajectory->dim_state];
      }
    }
    object_quat_traj.time_vec = trajectory->times;
    for (int k = 0; k < trajectory->horizon; ++k) {
      for (int n = 0; n < object_quat_traj.datatypes.size(); ++n) {
        object_quat_traj.datapoints[n][k] =
            trajectory
                ->states[(n + object_quat_start) + k * trajectory->dim_state];
      }
    }

    raw_actor_traj.trajectories.at(0) = actor_force_traj;
    raw_actor_traj.trajectories.at(1) = actor_pos_traj;
    raw_object_traj.trajectories.at(0) = object_pos_traj;
    raw_object_traj.trajectories.at(1) = object_quat_traj;
    actor_traj.saved_traj = raw_actor_traj;
    actor_traj.utime = time * 1e6;
    object_traj.saved_traj = raw_object_traj;
    object_traj.utime = time * 1e6;
    lcm.publish("TRACKING_TRAJECTORY_ACTOR", &actor_traj);
    lcm.publish("TRACKING_TRAJECTORY_OBJECT", &object_traj);
  }

  return 0;
}