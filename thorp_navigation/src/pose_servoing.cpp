/*
 * Found here: https://github.com/lagadic/demo_pioneer/blob/master/src/visual_servo_pioneer.cpp
 * This is a freak that does "visual servoing" using just a pose. I suppose it somehow projects the pose into the camera
 */

#include <ros/ros.h>
#include <ros/console.h>
#include <std_msgs/Int8.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/PoseStamped.h>
#include <visp_bridge/3dpose.h>
#include <visp_bridge/camera.h>
#include <sensor_msgs/CameraInfo.h>

#include <visp/vpAdaptiveGain.h>
#include <visp/vpCameraParameters.h>
#include <visp/vpDot.h>
#include <visp/vpDot2.h>
#include <visp/vpFeatureBuilder.h>
#include <visp/vpFeatureDepth.h>
#include <visp/vpFeaturePoint.h>
#include <visp/vpHomogeneousMatrix.h>
#include <visp/vpPioneer.h>
#include <visp/vpServo.h>
#include <visp/vpVelocityTwistMatrix.h>

class VS
{
private:
  ros::NodeHandle nh_;
  ros::NodeHandle pnh_{"~"};
  ros::Publisher twist_pub_; // cmd_vel
  ros::Subscriber pose_sub_;  // pose_stamped
  ros::Subscriber status_sub_;  // pose_stamped
  ros::Subscriber cam_info_sub_; // camera parameters

  vpServo task;
  // Current and desired visual feature associated to the x coordinate of the point
  vpFeaturePoint s_x, s_xd;
  vpFeatureDepth s_Z, s_Zd;

  vpCameraParameters cam;
  bool Stream_info_camera; //Is equal to one if we received the information about the camera
  double depth;
  double Z, Zd;
  double lambda;
  double target_distance;

  bool valid_pose;
  bool valid_pose_prev;

  double t_start_loop;
  double t_init;

  vpColVector v;
  vpColVector vi;
  double mu;
  vpAdaptiveGain lambda_adapt;

public:
  void init_vs();
  void poseCallback(const geometry_msgs::PoseStampedConstPtr &msg);
  void statusCallback(const std_msgs::Int8ConstPtr &msg);
  void cameraInfoCb(const sensor_msgs::CameraInfo& msg);
  VS(int argc, char **argv);
  virtual ~VS()
  {
    task.kill();
  };
};

VS::VS(int argc, char **argv)
{
  //init_vs();

  status_sub_ = pnh_.subscribe("status", 1, &VS::statusCallback, this);
  pose_sub_ = nh_.subscribe("target_object_pose", 1, &VS::poseCallback, this);
  twist_pub_ = nh_.advertise<geometry_msgs::Twist>("cmd_vel", 1);
  // Subscribe to the topic Camera info in order to receive the camera parameter. The callback function will be called only one time.
  cam_info_sub_ = nh_.subscribe("camera_info", 1, &VS::cameraInfoCb,this);

  target_distance = 1.0;

  depth = 0.4;
  lambda = 1.;
  valid_pose = false;
  valid_pose_prev = false;

  Stream_info_camera = 0;

  Z = Zd = depth;

  v.resize(2);
  vi.resize(2);
  v = 0;
  vi = 0;
  mu = 4;

  t_start_loop = 0.0;
  t_init = 0.0;
}

void VS::init_vs()
{
  //cam.initPersProjWithoutDistortion(800, 795, 320, 216);


  lambda_adapt.initStandard(3, 0.2, 40);

  task.setServo(vpServo::EYEINHAND_L_cVe_eJe);
  task.setInteractionMatrixType(vpServo::DESIRED, vpServo::PSEUDO_INVERSE);
  task.setLambda(lambda_adapt);

  vpPioneer robot; // Pas top ! devrait etre vpRobotPioneer
  vpVelocityTwistMatrix cVe = robot.get_cVe();
  vpMatrix eJe = robot.get_eJe();
  task.set_cVe(cVe);
  task.set_eJe(eJe);

  vpImagePoint ip(0, 0);

  // Create the current x visual feature
  vpFeatureBuilder::create(s_x, cam, ip);

  // Create the desired x* visual feature
  s_xd.buildFrom(0, 0, Zd);

  // Add the feature
  task.addFeature(s_x, s_xd, vpFeaturePoint::selectX());

  s_Z.buildFrom(s_x.get_x(), s_x.get_y(), Z, 0); // log(Z/Z*) = 0 that's why the last parameter is 0
  s_Zd.buildFrom(s_x.get_x(), s_x.get_y(), Zd, 0); // log(Z/Z*) = 0 that's why the last parameter is 0

  // Add the feature
  task.addFeature(s_Z, s_Zd);
}

void VS::statusCallback(const std_msgs::Int8ConstPtr &msg)
{
  if (msg->data)
    valid_pose = true;
  else
    valid_pose = false;
}

void VS::poseCallback(const geometry_msgs::PoseStampedConstPtr &msg)
{
  if (!Stream_info_camera ) // We check if the streaming of images is started or not
  {
    ROS_WARN_THROTTLE(1.0, "Waiting for the camera parameters");
    return;
  }

  geometry_msgs::Twist out_cmd_vel;
  try
  {
    t_start_loop = vpTime::measureTimeMs();

    vpHomogeneousMatrix cMo = visp_bridge::toVispHomogeneousMatrix(msg->pose);

    vpPoint origin;
    origin.setWorldCoordinates(0, 0, 0);
    origin.project(cMo);
    Z = origin.get_Z();

    if (Z <= 0)
      ROS_DEBUG("Z <= 0");

    ROS_DEBUG("Received a new pose; distance %.2fm (target distance %.2fm)", Z, target_distance);

    if (!valid_pose || Z <= target_distance)
    {
      ROS_DEBUG("not valid pose  %d", Z <= target_distance);

      out_cmd_vel.linear.x = 0;
      out_cmd_vel.linear.y = 0;
      out_cmd_vel.linear.z = 0;
      out_cmd_vel.angular.x = 0;
      out_cmd_vel.angular.y = 0;
      out_cmd_vel.angular.z = 0;
      twist_pub_.publish(out_cmd_vel);

      valid_pose = false;
      valid_pose_prev = valid_pose;

      return;
    }

    // Update the current x feature
    s_x.set_xyZ(origin.p[0], origin.p[1], Z);

    // Update log(Z/Z*) feature. Since the depth Z change, we need to update the intection matrix
    s_Z.buildFrom(s_x.get_x(), s_x.get_y(), Z, log(Z/Zd));

    vpPioneer robot; // Pas top ! devrait etre vpRobotPioneer
    vpVelocityTwistMatrix cVe = robot.get_cVe();
    vpMatrix eJe = robot.get_eJe();
    task.set_cVe(cVe);
    task.set_eJe(eJe);

    // Compute the control law. Velocities are computed in the mobile robot reference frame
    v = task.computeControlLaw();

    if (0)
    { //valid_pose_prev == false) {
      // Start a new visual servo
      ROS_INFO("Reinit visual servo");

      t_init = t_start_loop;
      vi = v;
    }

    v = v - vi*exp(-mu*(t_start_loop - t_init)/1000.0);
    double max_linear_vel = 0.5;
    double max_angular_vel = vpMath::rad(50);

    if (std::abs(v[0]) > max_linear_vel || std::abs(v[1]) > max_angular_vel)
    {
      ROS_INFO("Vel exceed max allowed");
      for (unsigned int i = 0; i < v.size(); i++)
        ROS_INFO("v[%d]=%f", i, v[i]);
//      v = 0;
    }

    out_cmd_vel.linear.x = v[0];
    out_cmd_vel.linear.y = 0;
    out_cmd_vel.linear.z = 0;
    out_cmd_vel.angular.x = 0;
    out_cmd_vel.angular.y = 0;
    out_cmd_vel.angular.z = v[1];

    twist_pub_.publish(out_cmd_vel);
    valid_pose_prev = valid_pose;

    valid_pose = false;
  }
  catch (...)
  {
    ROS_INFO("Catch an exception: set vel to 0");
    out_cmd_vel.linear.x = 0;
    out_cmd_vel.linear.y = 0;
    out_cmd_vel.linear.z = 0;
    out_cmd_vel.angular.x = 0;
    out_cmd_vel.angular.y = 0;
    out_cmd_vel.angular.z = 0;
    twist_pub_.publish(out_cmd_vel);
  }
}


void VS::cameraInfoCb(const sensor_msgs::CameraInfo& msg)
{
  ROS_DEBUG("Received Camera INFO");
  // Convert the parameter in the visp format
  cam = visp_bridge::toVispCameraParameters(msg);
  cam.printParameters();

  // Stop the subscriber (we don't need it anymore)
  cam_info_sub_.shutdown();

  Stream_info_camera = 1;
  init_vs();
}



int main(int argc, char **argv)
{
  ros::init(argc, argv, "pose_servoing");

  VS vs(argc, argv);

  ros::spin();
}
