#include <rct_optimizations/dh_chain_kinematic_calibration.h>
#include <rct_ros_tools/parameter_loaders.h>
#include <rct_ros_tools/exceptions.h>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>
#include <random>
#include <yaml-cpp/yaml.h>

using namespace rct_optimizations;
using namespace rct_ros_tools;

KinematicMeasurement::Set loadMeasurements(const std::string& filename)
{
  KinematicMeasurement::Set measurements;

  try
  {
    YAML::Node n = YAML::LoadFile(filename);
    measurements.reserve(n.size());

    for (auto it = n.begin(); it != n.end(); ++it)
    {
      KinematicMeasurement measurement;

      // Target chain joints
      {
        YAML::Node joints = it->second["target_joints"];
        measurement.target_chain_joints.resize(joints.size());
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
          measurement.target_chain_joints[i] = joints[i].as<double>();
        }
      }

      // Camera chain joints
      {
        YAML::Node joints = it->second["camera_joints"];
        measurement.camera_chain_joints.resize(joints.size());
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
          measurement.camera_chain_joints[i] = joints[i].as<double>();
        }
      }

      // Camera to target pose
      {
        YAML::Node pose = it->second["pose"];
        double x = pose["x"].as<double>();
        double y = pose["y"].as<double>();
        double z = pose["z"].as<double>();
        double qw = pose["qw"].as<double>();
        double qx = pose["qx"].as<double>();
        double qy = pose["qy"].as<double>();
        double qz = pose["qz"].as<double>();

        measurement.camera_to_target = Eigen::Isometry3d::Identity();
        measurement.camera_to_target.translate(Eigen::Vector3d(x, y, z));
        measurement.camera_to_target.rotate(Eigen::Quaterniond(qw, qx, qy, qz));

        // Add the measurement to the set
        measurements.push_back(measurement);
      }
    }
  }
  catch (YAML::Exception& ex)
  {
    throw rct_ros_tools::BadFileException(std::string("YAML failure: ") + ex.what());
  }

  return measurements;
}

DHChain createTwoAxisPositioner()
{
  std::vector<DHTransform> transforms;
  transforms.reserve(2);

  Eigen::Vector4d p1, p2;
  p1 << 0.0, 0.0, 0.0, -M_PI / 2.0;
  p2 << -0.475, -M_PI / 2.0, 0.0, 0.0;

  // Add the first DH transform
  {
    DHTransform t(p1, DHJointType::REVOLUTE, "j1");
    t.max = M_PI;
    t.min = -M_PI;
    transforms.push_back(t);
  }
  // Add the second DH transform
  {
    DHTransform dh_transform(p2, DHJointType::REVOLUTE, "j2");
    dh_transform.max = 2.0 * M_PI;
    dh_transform.min = -2.0 * M_PI;
    transforms.push_back(dh_transform);
  }

  // Set an arbitrary base offset
  Eigen::Isometry3d base_offset(Eigen::Isometry3d::Identity());
  base_offset.translate(Eigen::Vector3d(2.2, 0.0, 1.6));
  base_offset.rotate(Eigen::AngleAxisd(M_PI / 2.0, Eigen::Vector3d::UnitX()));

  return DHChain(transforms, base_offset);
}

void printResults(const KinematicCalibrationResult& result)
{
  Eigen::IOFormat fmt(4, 0, "|", "\n", "|", "|");

  std::stringstream ss;
  ss << "\nCalibration " << (result.converged ? "did" : "did not") << " converge\n";
  ss << "Initial cost per observation: " << std::sqrt(result.initial_cost_per_obs) << "\n";
  ss << "Final cost per observation: " << std::sqrt(result.final_cost_per_obs) << "\n";

  ss << "\nCamera mount to camera\n" << result.camera_mount_to_camera.matrix().format(fmt) << "\n";
  ss << "Euler ZYX: " << result.camera_mount_to_camera.rotation().eulerAngles(2, 1, 0).transpose().format(fmt) << "\n";

  ss << "\nTarget mount to target\n" << result.target_mount_to_target.matrix().format(fmt) << "\n";
  ss << "Euler ZYX: " << result.target_mount_to_target.rotation().eulerAngles(2, 1, 0).transpose().format(fmt) << "\n";

  ss << "\nDH parameter offsets\n" << result.target_chain_dh_offsets.matrix().format(fmt) << "\n";
  ss << result.covariance.printCorrelationCoeffAboveThreshold(0.5);

  std::cout << ss.str() << std::endl;
}

struct Stats
{
  double pos_mean;
  double pos_stdev;
  double rot_mean;
  double rot_stdev;

  void print() const
  {
    std::cout << "Position Difference Mean: " << pos_mean << std::endl;
    std::cout << "Position Difference Std. Dev.: " << pos_stdev << std::endl;
    std::cout << "Orientation Difference Mean: " << rot_mean << std::endl;
    std::cout << "Orientation difference Std. Dev.: " << rot_stdev << "\n" << std::endl;
  }

  void percentDiff(const Stats& other)
  {
    std::cout << "Position: " << 100.0 * (pos_mean - other.pos_mean) / pos_mean << "%" << std::endl;
    std::cout << "Position Std. Dev.: " << 100.0 * (pos_stdev - other.pos_stdev) / pos_stdev << "%" << std::endl;
    std::cout << "Orientation: " << 100.0 * (rot_mean - other.rot_mean) / rot_mean << "%" << std::endl;
    std::cout << "Orientation Std. Dev.: " << 100.0 * (rot_stdev - other.rot_stdev) / rot_stdev << "%" << "\n" << std::endl;
  }
};

Stats compareToMeasurements(const DHChain& initial_camera_chain, const DHChain& initial_target_chain,
                            const KinematicCalibrationResult& result, const KinematicMeasurement::Set& measurements)
{
  // Test the result by moving the robot around to a lot of positions and seeing of the results match
  DHChain camera_chain(initial_camera_chain, result.camera_chain_dh_offsets);
  DHChain target_chain(initial_target_chain, result.target_chain_dh_offsets);

  namespace ba = boost::accumulators;
  ba::accumulator_set<double, ba::stats<ba::tag::mean, ba::tag::variance>> pos_acc;
  ba::accumulator_set<double, ba::stats<ba::tag::mean, ba::tag::variance>> ori_acc;

  for (const KinematicMeasurement& m : measurements)
  {
    // Build the transforms from the camera chain base out to the camera
    Eigen::Isometry3d camera_chain_fk = camera_chain.getFK<double>(m.camera_chain_joints);
    Eigen::Isometry3d camera_base_to_camera = camera_chain_fk * result.camera_mount_to_camera;

    // Build the transforms from the camera chain base out to the target
    Eigen::Isometry3d target_chain_fk = target_chain.getFK<double>(m.target_chain_joints);
    Eigen::Isometry3d camera_base_to_target =
        result.camera_base_to_target_base * target_chain_fk * result.target_mount_to_target;

    // Now that we have two transforms in the same frame, get the target point in the camera frame
    Eigen::Isometry3d camera_to_target = camera_base_to_camera.inverse() * camera_base_to_target;

    // Compare
    Eigen::Isometry3d diff = camera_to_target.inverse() * m.camera_to_target;
    pos_acc(diff.translation().norm());
    ori_acc(Eigen::Quaterniond(camera_to_target.linear()).angularDistance(Eigen::Quaterniond(m.camera_to_target.linear())));
  }

  Stats stats;
  stats.pos_mean = ba::mean(pos_acc);
  stats.pos_stdev = std::sqrt(ba::variance(pos_acc));
  stats.rot_mean = ba::mean(ori_acc);
  stats.rot_stdev = std::sqrt(ba::variance(ori_acc));

  return stats;
}

template <typename T>
bool get(const ros::NodeHandle& nh, const std::string& key, T& val)
{
  if (!nh.getParam(key, val))
  {
    ROS_ERROR_STREAM("Failed to get '" << key << "' parameter");
    return false;
  }
  return true;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "kinematic_calibration_example");
  ros::NodeHandle pnh("~");

  std::string measurements_file;
  if (!get(pnh, "measurements_file", measurements_file))
    return -1;

  // Load the observations
  KinematicMeasurement::Set measurements = loadMeasurements(measurements_file);

  // Create the problem
  KinematicCalibrationProblemPose6D problem(DHChain(std::vector<DHTransform>{}), createTwoAxisPositioner());

  // Add the observations
  problem.observations = measurements;

  // Set the initial transform guesses
  problem.camera_mount_to_camera_guess = loadPose(pnh, "camera_mount_to_camera_guess");
  problem.target_mount_to_target_guess = loadPose(pnh, "target_mount_to_target_guess");
  problem.camera_base_to_target_base_guess = Eigen::Isometry3d::Identity();

  // Set the DH chain offset standard deviation expectations
  problem.camera_chain_offset_stdev = 0.001;
  problem.target_chain_offset_stdev = 0.005;

  // Mask a few DH parameters in the target chain (index 1)
  {
    Eigen::Matrix<bool, Eigen::Dynamic, 4> mask =
        Eigen::Matrix<bool, Eigen::Dynamic, 4>::Constant(problem.target_chain.dof(), 4, false);

    // Mask the last row because they duplicate the target mount to target transform
    mask.bottomRows(1) << true, true, true, true;

    // Add the mask to the problem
    problem.mask.at(1) = createDHMask(mask);
  }

  /* Mask the camera base to target base transform (duplicated by target mount to target transform when
   * the target chain has no joints */
  problem.mask.at(6) = { 0, 1, 2 };
  problem.mask.at(7) = { 0, 1, 2 };

  // Set up the Ceres optimization parameters
  ceres::Solver::Options options;
  options.max_num_iterations = 500;
  options.num_threads = 4;
  options.minimizer_progress_to_stdout = true;
  options.use_nonmonotonic_steps = true;

  // Run the calibration
  std::cout << "Starting kinematic calibration optimization..." << std::endl;
  Stats cal_stats_optimal_dh;
  {
    KinematicCalibrationResult result = optimize(problem, 100.0, options);
    printResults(result);

    //  test(problem.camera_chain, problem.target_chain, result, measurement_sets.second);
    cal_stats_optimal_dh = compareToMeasurements(problem.camera_chain, problem.target_chain, result, measurements);

    std::cout << "DH calibration validation:" << std::endl;
    cal_stats_optimal_dh.print();
  }

  // Re-run the calibration without optimizing the chain parameters
  Stats cal_stats_static_dh;
  {
    // Mask the entire target chain
    {
      Eigen::Matrix<bool, Eigen::Dynamic, 4> mask =
          Eigen::Matrix<bool, Eigen::Dynamic, 4>::Constant(problem.target_chain.dof(), 4, true);

      problem.mask.at(1) = createDHMask(mask);
    }

    KinematicCalibrationResult result = optimize(problem, 100.0, options);
    printResults(result);

    // Compare the results of this optimization with the measurements using the measured joints and nominal kinematic chain
    cal_stats_static_dh = compareToMeasurements(problem.camera_chain, problem.target_chain, result, measurements);
    std::cout << "Calibration validation - static DH parameters:" << std::endl;
    cal_stats_static_dh.print();
  }

  // Print the percentage difference between the calibrations
  std::cout << "Percent improvement: calibration vs. nominal kinematic model" << std::endl;
  cal_stats_static_dh.percentDiff(cal_stats_optimal_dh);

  return 0;
}
