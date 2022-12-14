#include <ros/ros.h>
#include <sensor_msgs/CameraInfo.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <sensor_msgs/Image.h>
#include "autoware_msgs/ProjectionMatrix.h"

#include "tools_colored_print.hpp"
#include <csignal>  
#include <pcl/common/eigen.h>
#include <cmath>
#include <Eigen/Eigen>
//dynamic reconfigure includes
#include <dynamic_reconfigure/server.h>
#include <calibration_publisher/CalibConfig.h>

static Eigen::Matrix4d M_cam2lidar;
static cv::Mat CameraMat;
static cv::Mat DistCoeff;
static cv::Size ImageSize;
static std::string DistModel;

static ros::Publisher camera_info_pub;
static ros::Publisher projection_matrix_pub;

static bool instrinsics_parsed_;

static bool isRegister_tf;
static bool isPublish_extrinsic;
static bool isPublish_cameraInfo;

static std::string camera_id_str_;
static std::string camera_frame_;
static std::string target_frame_;

static sensor_msgs::CameraInfo camera_info_msg_;
static autoware_msgs::ProjectionMatrix extrinsic_matrix_msg_;

void SigHandle(int sig)
{
	std::cout.precision(10);//设置输出精度
  scope_color(ANSI_COLOR_CYAN_BOLD);
	std::cout<<"========================================="<<std::endl;
  std::cout<<"Modified extrinsic matrix:"<<std::endl;
	std::cout<< M_cam2lidar <<std::endl;
	std::cout<<"Euler angles(RPY): "<< M_cam2lidar.block<3,3>(0,0).eulerAngles(0,1,2).transpose() <<std::endl;
	std::cout<<"Translation(XYZ):    "<< M_cam2lidar(0,3)<<"   "<<M_cam2lidar(1,3)<<"   "<<M_cam2lidar(2,3) <<std::endl;
	std::cout<<"========================================="<<std::endl;
	cout<<ANSI_COLOR_RESET;
}
template<typename T>
T rad2deg(T radians)
{
  return radians * 180.0 / M_PI;
}

template<typename T>
T deg2rad(T degrees)
{
  return degrees * M_PI / 180.0;
}
void tfRegistration(const Eigen::Matrix4d &camExtMat, const ros::Time &timeStamp)
{
	tf::Matrix3x3 rotation_mat;
	double roll = 0, pitch = 0, yaw = 0;
	tf::Quaternion quaternion;
	tf::Transform transform;
	static tf::TransformBroadcaster broadcaster;

	rotation_mat.setValue(camExtMat(0, 0), camExtMat(0, 1), camExtMat(0, 2),
	                      camExtMat(1, 0), camExtMat(1, 1), camExtMat(1, 2),
	                      camExtMat(2, 0), camExtMat(2, 1), camExtMat(2, 2));

	rotation_mat.getRPY(roll, pitch, yaw, 1);

	quaternion.setRPY(roll, pitch, yaw);

	transform.setOrigin(tf::Vector3(camExtMat(0, 3), camExtMat(1, 3), camExtMat(2, 3)));

	transform.setRotation(quaternion);

	broadcaster.sendTransform(tf::StampedTransform(transform, timeStamp, target_frame_, camera_frame_));
}

void projectionMatrix_sender(const Eigen::Matrix4d &projMat, const ros::Time &timeStamp)
{
	for (int row = 0; row < 4; row++)
	{
		for (int col = 0; col < 4; col++)
		{
			extrinsic_matrix_msg_.projection_matrix[row * 4 + col] = projMat(row, col);
		}
	}
	extrinsic_matrix_msg_.header.stamp = timeStamp;
	extrinsic_matrix_msg_.header.frame_id = camera_frame_;
	projection_matrix_pub.publish(extrinsic_matrix_msg_);
}

void cameraInfo_sender(const cv::Mat &camMat, const cv::Mat &distCoeff, const cv::Size &imgSize,
                       const std::string &distModel, const ros::Time &timeStamp)
{
	if (!instrinsics_parsed_)
	{
		for (int row = 0; row < 3; row++)
		{
			for (int col = 0; col < 3; col++)
			{
				camera_info_msg_.K[row * 3 + col] = camMat.at<double>(row, col);
			}
		}

		for (int row = 0; row < 3; row++)
		{
			for (int col = 0; col < 4; col++)
			{
				if (col == 3)
				{
					camera_info_msg_.P[row * 4 + col] = 0.0f;
				} else
				{
					camera_info_msg_.P[row * 4 + col] = camMat.at<double>(row, col);
				}
			}
		}

		for (int row = 0; row < distCoeff.rows; row++)
		{
			for (int col = 0; col < distCoeff.cols; col++)
			{
				camera_info_msg_.D.push_back(distCoeff.at<double>(row, col));
			}
		}
		camera_info_msg_.distortion_model = distModel;
		camera_info_msg_.height = imgSize.height;
		camera_info_msg_.width = imgSize.width;

		instrinsics_parsed_ = true;
	}
	camera_info_msg_.header.stamp = timeStamp;
	camera_info_msg_.header.frame_id = camera_frame_;


	camera_info_pub.publish(camera_info_msg_);
}

static void image_raw_cb(const sensor_msgs::Image &image_msg)
{
	// ros::Time timeStampOfImage = image_msg.header.stamp;

	ros::Time timeStampOfImage;
	timeStampOfImage.sec = image_msg.header.stamp.sec;
	timeStampOfImage.nsec = image_msg.header.stamp.nsec;

	/* create TF between velodyne and camera with time stamp of /image_raw */
	if (isRegister_tf)
	{
		tfRegistration(M_cam2lidar, timeStampOfImage);
	}

	if (isPublish_cameraInfo)
	{
		cameraInfo_sender(CameraMat, DistCoeff, ImageSize, DistModel, timeStampOfImage);
	}
	if (isPublish_extrinsic)
	{
		projectionMatrix_sender(M_cam2lidar, timeStampOfImage);
	}
}

void reconfigure_callback(calibration_publisher::CalibConfig& config) {
	double x = config.x;
	double y = config.y;
	double z = config.z;
	double roll = deg2rad(config.roll);
	double pitch = deg2rad(config.pitch);
	double yaw = deg2rad(config.yaw);
	ROS_INFO("x:%f  y:%f z:%f roll:%f pitch:%f yaw:%f", x, y, z,config.roll, config.pitch, config.yaw);
	Eigen::Affine3d A_cam2lidar;
	pcl::getTransformation(x, y, z, roll, pitch, yaw,A_cam2lidar);//由x,y,z平移和roll,pitch,yaw欧拉角旋转(3-2-1姿态序列)生成对应的仿射变换
	M_cam2lidar = A_cam2lidar.matrix();
}

int main(int argc, char *argv[])
{
	ros::init(argc, argv, "calibration_publisher");
	ros::NodeHandle n;
	ros::NodeHandle private_nh("~");

	char __APP_NAME__[] = "calibration_publisher";

	if (!private_nh.getParam("register_lidar2camera_tf", isRegister_tf))
	{
		isRegister_tf = true;
	}

	if (!private_nh.getParam("publish_extrinsic_mat", isPublish_extrinsic))
	{
		isPublish_extrinsic = true; /* publish extrinsic_mat in default */
	}

	if (!private_nh.getParam("publish_camera_info", isPublish_cameraInfo))
	{
		isPublish_cameraInfo = true; /* doesn't publish camera_info in default */
	}


	private_nh.param<std::string>("camera_frame", camera_frame_, "camera");
	ROS_INFO("[%s] camera_frame: '%s'", __APP_NAME__, camera_frame_.c_str());

	private_nh.param<std::string>("target_frame", target_frame_, "lidar");
	ROS_INFO("[%s] target_frame: '%s'", __APP_NAME__, target_frame_.c_str());

	std::string calibration_file;
	private_nh.param<std::string>("calibration_file", calibration_file, "/home/test/20220720_143417_autoware_lidar_camera_calibration.yaml");
	ROS_INFO("[%s] calibration_file: '%s'", __APP_NAME__, calibration_file.c_str());
	if (calibration_file.empty())
	{
		ROS_ERROR("[%s] Missing calibration file path '%s'.", __APP_NAME__, calibration_file.c_str());
		ros::shutdown();
		return -1;
	}

	cv::FileStorage fs(calibration_file, cv::FileStorage::READ);
	if (!fs.isOpened())
	{
		ROS_ERROR("[%s] Cannot open file calibration file '%s'", __APP_NAME__, calibration_file.c_str());
		ros::shutdown();
		return -1;
	}
	cv::Mat CameraExtrinsicMat;
	fs["CameraExtrinsicMat"] >> CameraExtrinsicMat;
	fs["CameraMat"] >> CameraMat;
	fs["DistCoeff"] >> DistCoeff;
	fs["ImageSize"] >> ImageSize;
	fs["DistModel"] >> DistModel;
	fs.release();//释放资源
	M_cam2lidar<<CameraExtrinsicMat.at<double>(0, 0), CameraExtrinsicMat.at<double>(0, 1), CameraExtrinsicMat.at<double>(0, 2),CameraExtrinsicMat.at<double>(0, 3),
	CameraExtrinsicMat.at<double>(1, 0), CameraExtrinsicMat.at<double>(1, 1), CameraExtrinsicMat.at<double>(1, 2), CameraExtrinsicMat.at<double>(1, 3),
	CameraExtrinsicMat.at<double>(2, 0), CameraExtrinsicMat.at<double>(2, 1), CameraExtrinsicMat.at<double>(2, 2), CameraExtrinsicMat.at<double>(2, 3),
	0.0, 0.0, 0.0, 1.0;
	double x_init,y_init,z_init,roll_init,pitch_init,yaw_init;
 	pcl::getTranslationAndEulerAngles(Eigen::Affine3d(M_cam2lidar),x_init,y_init,z_init,roll_init,pitch_init,yaw_init);//获取仿射变换等效的x,y,z平移和欧拉角旋转(3-2-1姿态序列)
	//弧度转化为角度
	roll_init=rad2deg(roll_init);
	pitch_init=rad2deg(pitch_init);
	yaw_init=rad2deg(yaw_init);

	//动态参数服务器
	dynamic_reconfigure::Server<calibration_publisher::CalibConfig> server;
	dynamic_reconfigure::Server<calibration_publisher::CalibConfig>::CallbackType server_callback = boost::bind(&reconfigure_callback, _1);
	//回调函数与服务端绑定
	server.setCallback(server_callback);

	std::string image_topic_name;
	std::string camera_info_name;
	std::string projection_matrix_topic;

	private_nh.param<std::string>("image_topic_src", image_topic_name, "/image_raw");
	ROS_INFO("[%s] image_topic_name: %s", __APP_NAME__, image_topic_name.c_str());

	private_nh.param<std::string>("camera_info_topic", camera_info_name, "/camera_info");
	ROS_INFO("[%s] camera_info_name: %s", __APP_NAME__, camera_info_name.c_str());

	private_nh.param<std::string>("projection_matrix_topic", projection_matrix_topic, "/projection_matrix");
	ROS_INFO("[%s] projection_matrix_topic: %s", __APP_NAME__, projection_matrix_topic.c_str());

	instrinsics_parsed_ = false;

	std::string name_space_str = ros::this_node::getNamespace();
	if (name_space_str != "/")
	{
		image_topic_name = name_space_str + image_topic_name;
		camera_info_name = name_space_str + camera_info_name;
		projection_matrix_topic = name_space_str + projection_matrix_topic;
		if (name_space_str.substr(0, 2) == "//")
		{
			/* if name space obtained by ros::this::node::getNamespace()
			   starts with "//", delete one of them */
			name_space_str.erase(name_space_str.begin());
		}
		camera_id_str_ = name_space_str;
	}

	ros::Subscriber image_sub;

	image_sub = n.subscribe(image_topic_name, 10, image_raw_cb);

	camera_info_pub = n.advertise<sensor_msgs::CameraInfo>(camera_info_name, 10, true);

	projection_matrix_pub = n.advertise<autoware_msgs::ProjectionMatrix>(projection_matrix_topic, 10, true);

	signal(SIGINT, SigHandle);//处理键盘Ctrl+C信号
	ros::spin();

	return 0;
}
