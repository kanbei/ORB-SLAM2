/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/


#include <iostream>
#include <algorithm>
#include <fstream>
#include <chrono>

#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <visualization_msgs/Marker.h>

#include <opencv2/core/core.hpp>

#include "../../../include/System.h"
#include "ros_viewer.h"

using namespace std;

class ImageGrabber
{
public:
    ImageGrabber(ORB_SLAM2::System* pSLAM):mpSLAM(pSLAM){}

    void GrabRGBD(const sensor_msgs::ImageConstPtr& msgRGB,const sensor_msgs::ImageConstPtr& msgD);

    void pub_camera(const cv::Mat mTcw);

    ORB_SLAM2::System* mpSLAM;

    int loop_id;//recored the number of loopClosing
};

/// Creat ros-related viewer
My_Viewer::ros_viewer* ros_view;
std::thread* mptViewer;
tf::TransformBroadcaster* tfb_;

// the world coordinates in ORB-SLAM was set to be the first frame coordinates
cv::Mat coordinateTransform(cv::Mat mTcw)
{
  // rotate to world coordinates
  float rot[3][3] = {{0,-1,0},{0,0,-1},{1,0,0}};
  float trans[3]  = {0.,0.,0.5};
  cv::Mat mR1w = cv::Mat(3,3,CV_32F,rot);
  cv::Mat mtw1 = cv::Mat(3,1,CV_32F,trans);

  cv::Mat mRc1 = mTcw.rowRange(0,3).colRange(0,3);
  cv::Mat mtc1 = mTcw.rowRange(0,3).col(3);
  cv::Mat mt1c = -mRc1.t()*mtc1;
  cv::Mat mRcw = mRc1*mR1w;
  cv::Mat mtcw = -mRc1*mt1c - mRcw*mtw1;

  cv::Mat mTcwr = cv::Mat::eye(4,4,CV_32F);
  mRcw.copyTo(mTcwr.rowRange(0,3).colRange(0,3));
  mtcw.copyTo(mTcwr.rowRange(0,3).col(3));

  return mTcwr.clone();
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "RGBD");
    ros::start();

    if(argc != 3)
    {
        cerr << endl << "Usage: rosrun ORB_SLAM2 RGBD path_to_vocabulary path_to_settings" << endl;        
        ros::shutdown();
        return 1;
    }    

    // Create SLAM system. It initializes all system threads and gets ready to process frames.
    ORB_SLAM2::System SLAM(argv[1],argv[2],ORB_SLAM2::System::RGBD,true);

    ImageGrabber igb(&SLAM);
    igb.loop_id = 0;

    ros::NodeHandle nh;

    /// setup ros viewer, a new thread
    ros_view = new My_Viewer::ros_viewer(argv[2]);
    mptViewer = new thread(&My_Viewer::ros_viewer::Run,ros_view);

    tfb_ = new tf::TransformBroadcaster();

    // TODO: use ros launch file to config. Now, adjust topics to be subscribed mannually
//    message_filters::Subscriber<sensor_msgs::Image> rgb_sub(nh, "/camera/rgb/image_color", 1);//image_raw, image_color
//    message_filters::Subscriber<sensor_msgs::Image> depth_sub(nh, "camera/depth_registered/image_raw", 1);//_registered
    message_filters::Subscriber<sensor_msgs::Image> rgb_sub(nh, "/camera/rgb/image_raw", 1);//image_raw, image_color
    message_filters::Subscriber<sensor_msgs::Image> depth_sub(nh, "camera/depth/image_raw", 1);//_registered
    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image> sync_pol;
    message_filters::Synchronizer<sync_pol> sync(sync_pol(10), rgb_sub,depth_sub);
    sync.registerCallback(boost::bind(&ImageGrabber::GrabRGBD,&igb,_1,_2));

    ros::spin();

    // Stop all threads
    SLAM.Shutdown();

    // Save camera trajectory
    SLAM.SaveKeyFrameTrajectoryTUM("KeyFrameTrajectory.txt");

    ros::shutdown();

    return 0;
}

void ImageGrabber::GrabRGBD(const sensor_msgs::ImageConstPtr& msgRGB,const sensor_msgs::ImageConstPtr& msgD)
{
    // Copy the ros image message to cv::Mat.
//    cv_bridge::CvImageConstPtr cv_ptrRGB;
    cv_bridge::CvImagePtr cv_ptrRGB;
    try
    {
//        cv_ptrRGB = cv_bridge::toCvShare(msgRGB);
        cv_ptrRGB = cv_bridge::toCvCopy(msgRGB);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    cv_bridge::CvImageConstPtr cv_ptrD;
    try
    {
        cv_ptrD = cv_bridge::toCvShare(msgD);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

//    cout << "image callback.." << cv_ptrRGB->image.channels() << endl;
//    cv::imshow("img", cv_ptrRGB->image);
//    cv::waitKey(100);
    cv::Mat mTcw(4,4,CV_32F);
    mTcw = mpSLAM->TrackRGBD(cv_ptrRGB->image,cv_ptrD->image,cv_ptrRGB->header.stamp.toSec());
    pub_camera(mTcw);

    /// update 3D grid map, implemented in ros_viewer. The first kf is not considered
    if (mpSLAM->mbNewKeyframe){
        cout<<"new KF:"<<mpSLAM->mbNewKeyframeID<<endl;
      ros_view->addKfToQueue(cv_ptrRGB->image,cv_ptrD->image,
                             cv_ptrRGB->header.stamp.toSec(), coordinateTransform(mTcw),mpSLAM->mbNewKeyframeID);
    }

    //daysun
    ///if local optimization
    if(mpSLAM->isLocalOptimization()){
        bool isRun =true;
        if(mpSLAM->isLoopDetected()){
            isRun = false;
        }
        if(isRun){
            cout<<"local:";
            //clone id
            std::vector<ORB_SLAM2::KeyFrame *>updateId( mpSLAM->getUpdatedLocalId());
            //get pose according to id
             std::map<double, cv::Mat> kfposes;
            for( std::vector<ORB_SLAM2::KeyFrame *>::const_iterator it=updateId.begin(),itEnd=updateId.end();it != itEnd;it++){
                ORB_SLAM2::KeyFrame * pKF = *it;
                ros_view->updateLocalId.push_back(pKF->mnId);
                cout<<pKF->mnId<<"\t";
                cv::Mat pose = pKF->GetPose();
                kfposes[pKF->mTimeStamp] = coordinateTransform(pose).clone();
            }
            cout<<endl;
            ros_view->addLocalupdate(kfposes);
        }/*else{
            cout<<"none\n";
        }*/
    }

    /// if loop is closed
    if (mpSLAM->isLoopCorrected()){        
        loop_id +=1;
        ros_view->setLoopId(loop_id);
      std::map<double, cv::Mat> kfposes = mpSLAM->getUpdatedKFposes();
      for(std::map<double, cv::Mat>::iterator mit=kfposes.begin(), mend=kfposes.end(); mit!=mend; mit++)
      {
          cv::Mat pose = mit->second;
          mit->second = coordinateTransform(pose).clone();
      }
      ros_view->addUpdatedKF(kfposes);
    }
}

void ImageGrabber::pub_camera(const cv::Mat mTcw1)
{
  cv::Mat mTcw = coordinateTransform(mTcw1);
  tf::Matrix3x3 rot(mTcw.at<float>(0,0), mTcw.at<float>(0,1), mTcw.at<float>(0,2),
                    mTcw.at<float>(1,0), mTcw.at<float>(1,1), mTcw.at<float>(1,2),
                    mTcw.at<float>(2,0), mTcw.at<float>(2,1), mTcw.at<float>(2,2));
  tf::Vector3 position(mTcw.at<float>(0,3), mTcw.at<float>(1,3), mTcw.at<float>(2,3));
  tf::Transform camtf = tf::Transform(rot,
                             position);
  tf::StampedTransform tf_stamped(camtf.inverse(),
                                      ros::Time::now(),
                                      "world", "camera_link");
  tfb_->sendTransform(tf_stamped);

}


