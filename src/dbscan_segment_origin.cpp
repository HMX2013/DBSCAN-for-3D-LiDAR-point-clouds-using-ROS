#include <ros/ros.h>
#include <ros/console.h>
#include <std_msgs/Header.h>
#include <geometry_msgs/PoseStamped.h>
// #include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2/LinearMath/Quaternion.h>

#include <jsk_recognition_msgs/BoundingBox.h>
#include <jsk_recognition_msgs/BoundingBoxArray.h>

#include "autoware_msgs/CloudClusterArray.h"
#include <autoware_msgs/DetectedObjectArray.h>

#include <pcl_ros/point_cloud.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/filters/extract_indices.h>


#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <opencv2/core/version.hpp>

#if (CV_MAJOR_VERSION == 3)

#include "gencolors.cpp"

#else

#include <opencv2/contrib/contrib.hpp>
#include <autoware_msgs/DetectedObjectArray.h>

#endif

#include <dynamic_reconfigure/server.h>
#include <dbscan_segment_origin/dbscan_segment_origin_Config.h>
// #include "obstacle_detector.hpp"
#include "dbscan_kdtree.hpp"
#include "cluster.h"
using namespace cv;

typedef pcl::PointXYZ PointType;

std::vector<cv::Scalar> _colors;
static bool _pose_estimation=false;
static const double _initial_quat_w = 1.0;

std::string output_frame="velodyne";
std_msgs::Header _velodyne_header;

// Pointcloud Filtering Parameters
bool USE_PCA_BOX;
bool USE_TRACKING;
float VOXEL_GRID_SIZE;
Eigen::Vector4f ROI_MAX_POINT, ROI_MIN_POINT;
float CLUSTER_THRESH, ClusterTolerance;
int CLUSTER_MAX_SIZE, CLUSTER_MIN_SIZE, CorePointMinPt, MinClusterSize, MaxClusterSize;


class cloud_segmentation
{
 private:

  std::shared_ptr<DBSCAN_KDTREE<PointType>> dbscan_kdtree;
  // std::shared_ptr<LShapedFIT> L_shape_Fit; 

  ros::NodeHandle nh;
  // tf2_ros::Buffer tf2_buffer;
  // tf2_ros::TransformListener tf2_listener;
  tf::TransformListener *_transform_listener;
  tf::StampedTransform *_transform;


  dynamic_reconfigure::Server<dbscan_segment_origin::dbscan_segment_origin_Config> server;
  dynamic_reconfigure::Server<dbscan_segment_origin::dbscan_segment_origin_Config>::CallbackType f;

  pcl::PointCloud<pcl::PointXYZI>::Ptr segmentedCloudColor;

  ros::Subscriber sub_lidar_points;
  ros::Publisher pub_cloud_ground;
  ros::Publisher pub_cloud_clusters;
  ros::Publisher pub_jsk_bboxes;
  ros::Publisher pub_autoware_objects;
  ros::Publisher _pub_autoware_clusters_message;
  ros::Publisher _pub_autoware_detected_objects;
  ros::Publisher _pub_roi_area;
  ros::Publisher _pubSegmentedCloudColor;


  void lidarPointsCallback(const sensor_msgs::PointCloud2::ConstPtr& lidar_points);
  //jsk_recognition_msgs::BoundingBox transformJskBbox(const Box& box, const std_msgs::Header& header, const geometry_msgs::Pose& pose_transformed);
  //autoware_msgs::DetectedObject transformAutowareObject(const Box& box, const std_msgs::Header& header, const geometry_msgs::Pose& pose_transformed);
  pcl::PointCloud<PointType>::Ptr roi_filter_pcl(const sensor_msgs::PointCloud2::ConstPtr& laserRosCloudMsg, const float filter_res);
  pcl::PointCloud<PointType>::Ptr roi_rectangle_filter(const sensor_msgs::PointCloud2::ConstPtr& laserRosCloudMsg, const float filter_res, const Eigen::Vector4f& min_pt, const Eigen::Vector4f& max_pt);
  //Box pcaBoundingBox(pcl::PointCloud<PointType>::Ptr& cluster, const int id);
  //Box L_shape_BBox(const pcl::PointCloud<PointType>::Ptr& cluster, const int id);

  void dbscan_kdtree_origin(const pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud_ptr, autoware_msgs::CloudClusterArray &in_out_clusters, 
                                        const float CorePointMinPt, const float ClusterTolerance, const float MinClusterSize, const float MaxClusterSize);
  void publishSegmentedCloudsColor(const std_msgs::Header& header);
  void publish_ROI_area(const std_msgs::Header& header);
  void publish_autoware_cloudclusters(const ros::Publisher *in_publisher, const autoware_msgs::CloudClusterArray &in_clusters,
                          const std::string &in_target_frame, const std_msgs::Header &in_header);
  void publishAutowareDetectedObjects(const autoware_msgs::CloudClusterArray &in_clusters);

 public:
  cloud_segmentation();
  ~cloud_segmentation() {};

  void allocateMemory(){
    segmentedCloudColor.reset(new pcl::PointCloud<pcl::PointXYZI>());
  }

  void resetParameters(){
    segmentedCloudColor->clear();
  }

};

// Dynamic parameter server callback function
void dynamicParamCallback(dbscan_segment_origin::dbscan_segment_origin_Config& config, uint32_t level)
{
  // Pointcloud Filtering Parameters
  VOXEL_GRID_SIZE = config.voxel_grid_size;
  CorePointMinPt=config.CorePointMinPt;
  ClusterTolerance=config.ClusterTolerance;
  MinClusterSize=config.MinClusterSize;
  MaxClusterSize=config.MaxClusterSize;
  ROI_MAX_POINT = Eigen::Vector4f(config.roi_max_x, config.roi_max_y, config.roi_max_z, 1);
  ROI_MIN_POINT = Eigen::Vector4f(config.roi_min_x, config.roi_min_y, config.roi_min_z, 1);
}

// GroundPlaneFit::GroundPlaneFit():node_handle_("~"){
//  : tf2_listener(tf2_buffer)
cloud_segmentation::cloud_segmentation(){

  ros::NodeHandle private_nh("~");
  allocateMemory();

  std::string lidar_points_topic_ground;
  std::string lidar_points_topic;
  std::string cloud_ground_topic;
  std::string cloud_clusters_topic;
  std::string jsk_bboxes_topic;
  //std::string autoware_objects_topic;

  #if (CV_MAJOR_VERSION == 3)
    generateColors(_colors, 255);
  #else
    cv::generateColors(_colors, 255);
  #endif

  ROS_ASSERT(private_nh.getParam("lidar_points_topic", lidar_points_topic));
  // ROS_ASSERT(private_nh.getParam("cloud_ground_topic", cloud_ground_topic));
  //ROS_ASSERT(private_nh.getParam("cloud_clusters_topic", cloud_clusters_topic));
  // ROS_ASSERT(private_nh.getParam("jsk_bboxes_topic", jsk_bboxes_topic));
  //ROS_ASSERT(private_nh.getParam("autoware_objects_topic", autoware_objects_topic));


  sub_lidar_points = nh.subscribe(lidar_points_topic, 1, &cloud_segmentation::lidarPointsCallback, this);
  // pub_cloud_ground = nh.advertise<sensor_msgs::PointCloud2>(cloud_ground_topic, 1);
  // pub_cloud_clusters = nh.advertise<sensor_msgs::PointCloud2>(cloud_clusters_topic, 1);
  // pub_jsk_bboxes = nh.advertise<jsk_recognition_msgs::BoundingBoxArray>(jsk_bboxes_topic,1);
  // pub_autoware_objects = nh.advertise<autoware_msgs::DetectedObjectArray>(autoware_objects_topic, 1);
  _pubSegmentedCloudColor = nh.advertise<sensor_msgs::PointCloud2> ("/detection/segmented_cloud_color_marker", 1);
  _pub_autoware_clusters_message = nh.advertise<autoware_msgs::CloudClusterArray>("/detection/lidar_detector/cloud_clusters", 1);
  _pub_autoware_detected_objects = nh.advertise<autoware_msgs::DetectedObjectArray>("/detection/lidar_detector/objects", 1);
  _pub_roi_area = nh.advertise<jsk_recognition_msgs::BoundingBoxArray>("/detection/roi_area", 1);

  // Dynamic Parameter Server & Function
  f = boost::bind(&dynamicParamCallback, _1, _2);
  server.setCallback(f);

  // Create point processor
  // obstacle_detector = std::make_shared<ObstacleDetector<PointType>>();
  dbscan_kdtree = std::make_shared<DBSCAN_KDTREE<PointType>>();
 
  resetParameters();
}


void cloud_segmentation::publishSegmentedCloudsColor(const std_msgs::Header& header)
{
  sensor_msgs::PointCloud2 segmentedCloudColor_ros;
  
  // extract segmented cloud for visualization
  if (_pubSegmentedCloudColor.getNumSubscribers() != 0){
    pcl::toROSMsg(*segmentedCloudColor, segmentedCloudColor_ros);
    segmentedCloudColor_ros.header = header;
    _pubSegmentedCloudColor.publish(segmentedCloudColor_ros);
  }
}


pcl::PointCloud<PointType>::Ptr cloud_segmentation::roi_rectangle_filter(const sensor_msgs::PointCloud2::ConstPtr& laserRosCloudMsg, const float filter_res, const Eigen::Vector4f& min_pt, const Eigen::Vector4f& max_pt)
{
  // // Create the filtering object: downsample the dataset using a leaf size
  pcl::PointCloud<PointType>::Ptr input_cloud(new pcl::PointCloud<PointType>);
  pcl::fromROSMsg(*laserRosCloudMsg, *input_cloud);
  pcl::PointCloud<PointType>::Ptr cloud_roi(new pcl::PointCloud<PointType>);
  pcl::PointCloud<PointType>::Ptr input_cloud_filter(new pcl::PointCloud<PointType>);

  if (filter_res > 0)
  {
      // Create the filtering object: downsample the dataset using a leaf size
      pcl::VoxelGrid<PointType> vg;
      vg.setInputCloud(input_cloud);
      vg.setLeafSize(filter_res, filter_res, filter_res);
      vg.filter(*input_cloud_filter);

      // Cropping the ROI
      pcl::CropBox<PointType> roi_region(true);
      roi_region.setMin(min_pt);
      roi_region.setMax(max_pt);
      roi_region.setInputCloud(input_cloud_filter);
      roi_region.filter(*cloud_roi);
  }
  else
  {
      // Cropping the ROI
      pcl::CropBox<PointType> roi_region(true);
      roi_region.setMin(min_pt);
      roi_region.setMax(max_pt);
      roi_region.setInputCloud(input_cloud);
      roi_region.filter(*cloud_roi);
  }

  // Removing the car roof region
  std::vector<int> indices;
  pcl::CropBox<PointType> roof(true);

  roof.setMin(Eigen::Vector4f(-1.63, -0.6, -1.86, 1));
  roof.setMax(Eigen::Vector4f(0.97, 0.6, 0.19, 1));

  roof.setInputCloud(cloud_roi);
  roof.filter(indices);

  pcl::PointIndices::Ptr inliers(new pcl::PointIndices);
  for (auto& point : indices)
    inliers->indices.push_back(point);

  pcl::ExtractIndices<PointType> extract;
  extract.setInputCloud(cloud_roi);
  extract.setIndices(inliers);
  extract.setNegative(true);
  extract.filter(*cloud_roi);

  return input_cloud_filter;
}

pcl::PointCloud<PointType>::Ptr cloud_segmentation::roi_filter_pcl(const sensor_msgs::PointCloud2::ConstPtr& laserRosCloudMsg, const float filter_res)
{
  float horizonAngle, range;
  size_t  cloudSize;
  int j=0; 
  pcl::PointCloud<PointType>::Ptr roi_cloud_origin(new pcl::PointCloud<PointType>);
  PointType thisPoint;

  // ROS message transform to PCL type
  pcl::PointCloud<PointType>::Ptr origin_cloud_pcl(new pcl::PointCloud<PointType>);
  pcl::fromROSMsg(*laserRosCloudMsg, *origin_cloud_pcl);

  cloudSize = origin_cloud_pcl->points.size();

  //ROS_INFO("cloudSize=%d",cloudSize);

  // ROI pi/2 ~ -pi/2
  for (size_t i = 0; i < cloudSize; ++i)
  {

    thisPoint.x = origin_cloud_pcl->points[i].x;
    thisPoint.y = origin_cloud_pcl->points[i].y;
    thisPoint.z = origin_cloud_pcl->points[i].z;

    horizonAngle = atan2(thisPoint.x, thisPoint.y) * 180 / M_PI;
    range = sqrt(thisPoint.x * thisPoint.x + thisPoint.y * thisPoint.y + thisPoint.z * thisPoint.z);

    if (range < 1 || range > 3 || (horizonAngle > -180 && horizonAngle < -90))
      continue;

    roi_cloud_origin->push_back(thisPoint);

    j++;
  }
  ROS_INFO("TEST");

  // // Create the filtering object: downsample the dataset using a leaf size
  pcl::VoxelGrid<PointType> vg;
  pcl::PointCloud<PointType>::Ptr roi_cloud_filter(new pcl::PointCloud<PointType>);
  vg.setInputCloud(roi_cloud_origin);
  vg.setLeafSize(filter_res, filter_res, filter_res);
  vg.filter(*roi_cloud_filter);

  return roi_cloud_filter;
}


void cloud_segmentation::dbscan_kdtree_origin(const pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud_ptr, autoware_msgs::CloudClusterArray &in_out_clusters, 
                                        const float CorePointMinPt, const float ClusterTolerance, const float MinClusterSize, const float MaxClusterSize)
{
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr colored_clustered_cloud_ptr;
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
  if (in_cloud_ptr->points.size() > 0)
    tree->setInputCloud(in_cloud_ptr);

  std::vector<pcl::PointIndices> cluster_indices;

  dbscan_kdtree->setCorePointMinPts(CorePointMinPt);
  dbscan_kdtree->setClusterTolerance(ClusterTolerance);
  dbscan_kdtree->setMinClusterSize(MinClusterSize);
  dbscan_kdtree->setMaxClusterSize(MaxClusterSize);
  dbscan_kdtree->setSearchMethod(tree);
  dbscan_kdtree->setInputCloud(in_cloud_ptr);
  dbscan_kdtree->extract(cluster_indices);

  unsigned int k = 0;
  int intensity_mark = 1;
  
  std::vector<ClusterPtr> segment_clusters;
  pcl::PointXYZI cluster_color;

  for (auto& getIndices : cluster_indices)
  {
    for (auto& index : getIndices.indices){
      // cluster->points.push_back(cloud->points[index]);
      cluster_color.x=in_cloud_ptr->points[index].x;
      cluster_color.y=in_cloud_ptr->points[index].y;
      cluster_color.z=in_cloud_ptr->points[index].z;
      //cluster_color.intensity=intensity_mark;
      segmentedCloudColor->push_back(cluster_color);
      segmentedCloudColor->points.back().intensity = intensity_mark;
    }

    ClusterPtr cluster(new Cluster());
    cluster->SetCloud(in_cloud_ptr, getIndices.indices, _velodyne_header, k, (int) _colors[k].val[0],
                      (int) _colors[k].val[1],
                      (int) _colors[k].val[2], "", _pose_estimation);
    // cluster->SetCloud(in_cloud_ptr, it->indices, _velodyne_header, k, 1, 1, 1, "", _pose_estimation);
    segment_clusters.push_back(cluster);
    intensity_mark++;
    k++;
  }

  for (unsigned int i = 0; i < segment_clusters.size(); i++)
  {
    if (segment_clusters[i]->IsValid())
    {
      autoware_msgs::CloudCluster cloud_cluster;
      segment_clusters[i]->ToROSMessage(_velodyne_header, cloud_cluster);
      in_out_clusters.clusters.push_back(cloud_cluster);
    }
  }
}


void cloud_segmentation::publish_autoware_cloudclusters(const ros::Publisher *in_publisher, const autoware_msgs::CloudClusterArray &in_clusters,
                          const std::string &in_target_frame, const std_msgs::Header &in_header)
{
  if (in_target_frame != in_header.frame_id)
  {
    autoware_msgs::CloudClusterArray clusters_transformed;
    clusters_transformed.header = in_header;
    clusters_transformed.header.frame_id = in_target_frame;
    for (auto i = in_clusters.clusters.begin(); i != in_clusters.clusters.end(); i++)
    {
      autoware_msgs::CloudCluster cluster_transformed;
      cluster_transformed.header = in_header;
      try
      {
        _transform_listener->lookupTransform(in_target_frame, _velodyne_header.frame_id, ros::Time(),
                                             *_transform);
        pcl_ros::transformPointCloud(in_target_frame, *_transform, i->cloud, cluster_transformed.cloud);
        _transform_listener->transformPoint(in_target_frame, ros::Time(), i->min_point, in_header.frame_id,
                                            cluster_transformed.min_point);
        _transform_listener->transformPoint(in_target_frame, ros::Time(), i->max_point, in_header.frame_id,
                                            cluster_transformed.max_point);
        _transform_listener->transformPoint(in_target_frame, ros::Time(), i->avg_point, in_header.frame_id,
                                            cluster_transformed.avg_point);
        _transform_listener->transformPoint(in_target_frame, ros::Time(), i->centroid_point, in_header.frame_id,
                                            cluster_transformed.centroid_point);

        cluster_transformed.dimensions = i->dimensions;
        cluster_transformed.eigen_values = i->eigen_values;
        cluster_transformed.eigen_vectors = i->eigen_vectors;

        cluster_transformed.convex_hull = i->convex_hull;
        cluster_transformed.bounding_box.pose.position = i->bounding_box.pose.position;
        if(_pose_estimation)
        {
          cluster_transformed.bounding_box.pose.orientation = i->bounding_box.pose.orientation;
        }
        else
        {
          cluster_transformed.bounding_box.pose.orientation.w = _initial_quat_w;
        }
        clusters_transformed.clusters.push_back(cluster_transformed);
      }
      catch (tf::TransformException &ex)
      {
        ROS_ERROR("publishCloudClusters: %s", ex.what());
      }
    }
    in_publisher->publish(clusters_transformed);
    publishAutowareDetectedObjects(clusters_transformed);
  } else
  {
    in_publisher->publish(in_clusters);
    publishAutowareDetectedObjects(in_clusters);
  }
  in_publisher->publish(in_clusters);
  publishAutowareDetectedObjects(in_clusters);
}

void cloud_segmentation::publishAutowareDetectedObjects(const autoware_msgs::CloudClusterArray &in_clusters)
{
  autoware_msgs::DetectedObjectArray detected_objects;
  detected_objects.header = in_clusters.header;

  for (size_t i = 0; i < in_clusters.clusters.size(); i++)
  {
    autoware_msgs::DetectedObject detected_object;
    detected_object.header = in_clusters.header;
    detected_object.label = "unknown";
    detected_object.score = 1.;
    detected_object.space_frame = in_clusters.header.frame_id;
    detected_object.pose = in_clusters.clusters[i].bounding_box.pose;
    detected_object.dimensions = in_clusters.clusters[i].dimensions;
    detected_object.pointcloud = in_clusters.clusters[i].cloud;
    detected_object.convex_hull = in_clusters.clusters[i].convex_hull;
    detected_object.valid = true;

    detected_objects.objects.push_back(detected_object);
  }
  _pub_autoware_detected_objects.publish(detected_objects);
}


void cloud_segmentation::publish_ROI_area(const std_msgs::Header& header)
{
  // Construct Bounding Boxes from the clusters
  jsk_recognition_msgs::BoundingBoxArray jsk_bboxes;
  jsk_bboxes.header = header;

  jsk_recognition_msgs::BoundingBox DtectionArea_car;
  DtectionArea_car.pose.position.x=0;
  DtectionArea_car.pose.position.y=0; 
  DtectionArea_car.pose.position.z=0;
  // DtectionArea_car.dimensions.x=ROI_MAX_POINT[1]-ROI_MIN_POINT[1];
  // DtectionArea_car.dimensions.y=ROI_MAX_POINT[2]-ROI_MIN_POINT[2];
  // DtectionArea_car.dimensions.z=4;

  DtectionArea_car.dimensions.x=10;
  DtectionArea_car.dimensions.y=6;
  DtectionArea_car.dimensions.z=4;

  DtectionArea_car.header.frame_id="velodyne";

  jsk_bboxes.boxes.push_back(DtectionArea_car);

  jsk_recognition_msgs::BoundingBox car_remove;
  car_remove.pose.position.x=0;
  car_remove.pose.position.y=0; 
  car_remove.pose.position.z=0;
  car_remove.dimensions.x=0.97+1.63;
  car_remove.dimensions.y=1.2;
  car_remove.dimensions.z=0.19+1.86;
  car_remove.header.frame_id="velodyne";
  jsk_bboxes.boxes.push_back(car_remove);
  
  _pub_roi_area.publish(jsk_bboxes);
}

void cloud_segmentation::lidarPointsCallback(const sensor_msgs::PointCloud2::ConstPtr& lidar_points)
{
  _velodyne_header = lidar_points->header;
  autoware_msgs::CloudClusterArray cloud_clusters;
  cloud_clusters.header = _velodyne_header;

  //std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> clusters;
  pcl::PointCloud<PointType>::Ptr pcl_roi(new pcl::PointCloud<PointType>);

  // ROI
  //pcl_roi= roi_filter_pcl(lidar_points, VOXEL_GRID_SIZE);

  pcl_roi= roi_rectangle_filter(lidar_points, VOXEL_GRID_SIZE, ROI_MIN_POINT, ROI_MAX_POINT);

  const auto start_time = std::chrono::steady_clock::now();

  //clustering
  dbscan_kdtree_origin(pcl_roi, cloud_clusters, CorePointMinPt, ClusterTolerance, MinClusterSize, MaxClusterSize);

  // Time the whole process
  const auto end_time = std::chrono::steady_clock::now();
  const auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  std::cout << "segmentation took " << elapsed_time.count() << " milliseconds" << std::endl;

  //visualization, use indensity to show different color for each cluster.
  publish_ROI_area(_velodyne_header);

  publishSegmentedCloudsColor(_velodyne_header);

  
  //_pub_autoware_clusters_message.publish(cloud_clusters);

  publish_autoware_cloudclusters(&_pub_autoware_clusters_message, cloud_clusters, output_frame, _velodyne_header);

  //ROS_INFO("The obstacle_detector_node found %d obstacles in %.3f second", int(prev_boxes_.size()), float(elapsed_time.count()/1000.0));

  resetParameters();
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "descan_segment_origin_node");

  cloud_segmentation cloud_segmentation_node;

  ros::spin();

  return 0;
}