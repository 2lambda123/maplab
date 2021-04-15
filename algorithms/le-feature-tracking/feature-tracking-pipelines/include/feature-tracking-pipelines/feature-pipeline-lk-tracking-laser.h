#ifndef FEATURE_TRACKING_PIPELINES_FEATURE_PIPELINE_LK_TRACKING_LASER_H_
#define FEATURE_TRACKING_PIPELINES_FEATURE_PIPELINE_LK_TRACKING_LASER_H_

#include <Eigen/Core>
#include <aslam/cameras/ncamera.h>
#include <aslam/matcher/match-helpers.h>
#include <functional>
#include <glog/logging.h>
#include <memory>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/video/tracking.hpp>
#include <sensors/lidar.h>
#include <string>
#include <utility>
#include <vector>

#include "feature-tracking-pipelines/feature-describer-base.h"
#include "feature-tracking-pipelines/feature-detector-base.h"
#include "feature-tracking-pipelines/feature-pipeline-base.h"
#include "feature-tracking-pipelines/flags.h"
#include "feature-tracking-pipelines/helpers.h"
#include "feature-tracking-pipelines/keyframe-features.h"
#include "feature-tracking-pipelines/pnp-ransac.h"

namespace feature_tracking_pipelines {
struct LkTrackingSettingsLaser {
  size_t max_num_features = FLAGS_NEW_feature_tracker_num_features;
  size_t num_pyramid_levels = FLAGS_NEW_feature_tracker_num_pyramid_levels;
  double min_eigen_threshold = FLAGS_NEW_feature_tracker_min_eigen_threshold;
  size_t operation_flag = FLAGS_NEW_feature_tracker_operation_flag;
  size_t criteria_max_count = FLAGS_NEW_feature_tracker_criteria_max_count;
  double criteria_epsilon = FLAGS_NEW_feature_tracker_criteria_epsilon;
  size_t window_height = FLAGS_NEW_feature_tracker_window_height;
  size_t window_width = FLAGS_NEW_feature_tracker_window_width;
  double min_tracking_distance =
      FLAGS_NEW_feature_tracker_min_tracking_distance;
};

LkTrackingSettingsLaser InitLkLaserTrackingSettingsFromGFlags() {
  LkTrackingSettingsLaser settings;
  CHECK_GT(FLAGS_NEW_feature_tracker_num_features, 0);
  settings.max_num_features = FLAGS_NEW_feature_tracker_num_features;
  settings.num_pyramid_levels = FLAGS_NEW_feature_tracker_num_pyramid_levels;
  settings.min_eigen_threshold = FLAGS_NEW_feature_tracker_min_eigen_threshold;
  settings.operation_flag = FLAGS_NEW_feature_tracker_operation_flag;
  settings.criteria_max_count = FLAGS_NEW_feature_tracker_criteria_max_count;
  settings.criteria_epsilon = FLAGS_NEW_feature_tracker_criteria_epsilon;
  settings.window_height = FLAGS_NEW_feature_tracker_window_height;
  settings.window_width = FLAGS_NEW_feature_tracker_window_width;
  settings.min_tracking_distance =
      FLAGS_NEW_feature_tracker_min_tracking_distance;
  return settings;
}

class FeaturePipelineLkTrackingLaser : public FeatureTrackingPipelineBase {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  FeaturePipelineLkTrackingLaser(
      const LkTrackingSettingsLaser& settings,
      const RansacSettings& ransac_settings,
      std::shared_ptr<FeatureDetectorBase> feature_detector,
      std::shared_ptr<FeatureDescriberBase> feature_describer)
      : FeatureTrackingPipelineBase(
            ransac_settings, feature_detector, feature_describer),
        settings_(settings) {
    // cv::namedWindow("Tracking 2", cv::WINDOW_NORMAL);
  }
  virtual ~FeaturePipelineLkTrackingLaser() = default;

  virtual void processImages2(
      const aslam::Quaternion& q_Icurr_Iprev,
      const std::vector<cv::Mat>& curr_camera_images,
      const std::vector<cv::Mat>& prev_camera_images,
      const std::vector<KeyframeFeatures>& previous_keyframe,
      std::vector<KeyframeFeatures>* current_keyframe_ptr,
      pcl::PointCloud<pcl::PointXYZI>::ConstPtr cloud,
      FeaturePipelineDebugData* optional_debug_data) override {
    // optional_debug_data is optional and can be a nullptr.
    CHECK_EQ(1, curr_camera_images.size());
    CHECK_EQ(1, previous_keyframe.size());
    CHECK_NOTNULL(current_keyframe_ptr)->clear();

    std::vector<KeyframeFeatures>& current_keyframe = *current_keyframe_ptr;
    current_keyframe.resize(1);

    std::vector<aslam::FrameToFrameMatches> inlier_matches_kp1_k,
        outlier_matches_kp1_k;
    inlier_matches_kp1_k.resize(1);
    outlier_matches_kp1_k.resize(1);
    cv::Mat tracker_img = curr_camera_images[0];
    cv::cvtColor(tracker_img, tracker_img, CV_GRAY2RGB);
    std::vector<cv::Point2f> keypoints_curr;
    bool show_illustrations = true;

    // TODO(schneith): Parallelize the loop over camera images.
    for (size_t frame_idx = 0u; frame_idx < 1u; ++frame_idx) {
      KeyframeFeatures previous_keyframe_features_removed =
          previous_keyframe[frame_idx];
      // Copy over the keypoints from the last tracking round.
      CHECK_EQ(previous_keyframe[frame_idx].frame_idx, frame_idx);
      current_keyframe[frame_idx].keypoint_detector_name =
          feature_detector_->getDetectorName();
      current_keyframe[frame_idx].keypoint_descriptor_name =
          feature_describer_->getDescriptorName();
      // Predict feature locations considering a rotation.
      std::vector<cv::Point2f> keypoints_prev;
      KeyframeFeaturesToCvPoints(
          previous_keyframe[frame_idx].keypoint_measurements, &keypoints_prev);

      Eigen::Matrix2Xd predicted_keypoints =
          previous_keyframe[frame_idx].keypoint_measurements;

      KeyframeFeaturesToCvPoints(predicted_keypoints, &keypoints_curr);

      // Track features from last to current frame using LK.
      std::vector<float> tracking_error;
      std::vector<unsigned char> tracking_successful;
      if (!keypoints_prev.empty()) {
        const cv::TermCriteria termination_criteria = cv::TermCriteria(
            cv::TermCriteria::COUNT | cv::TermCriteria::EPS,
            settings_.criteria_max_count, settings_.criteria_epsilon);
        const cv::Size window_size =
            cv::Size(settings_.window_width, settings_.window_height);

        cv::calcOpticalFlowPyrLK(
            prev_camera_images[frame_idx], curr_camera_images[frame_idx],
            keypoints_prev, keypoints_curr, tracking_successful, tracking_error,
            window_size, settings_.num_pyramid_levels, termination_criteria,
            settings_.operation_flag, settings_.min_eigen_threshold);

        CHECK_EQ(keypoints_curr.size(), keypoints_prev.size());
        CvKeypointsToKeyframeFeatures(
            keypoints_curr, &current_keyframe[frame_idx].keypoint_measurements);

        if (show_illustrations) {
          for (int i = 0; i < keypoints_curr.size(); ++i) {
            // Red circles and lines
            cv::circle(
                tracker_img, keypoints_curr[i], 3, cv::Scalar(0, 0, 255));
            cv::line(
                tracker_img, keypoints_prev[i], keypoints_curr[i],
                cv::Scalar(0, 0, 255));
          }
        }
      }

      int initial_keypoints = keypoints_curr.size();
      // Track IDs remain the same as in the previous frame.
      current_keyframe[frame_idx].keypoint_track_ids =
          previous_keyframe[frame_idx].keypoint_track_ids;

      // TODO(schneith): Re-evaluate the orientation and scale at the new
      // keypoint location. For now we propagate the value from the initial
      // keypoint detection; except for the orientation.
      current_keyframe[frame_idx].keypoint_sizes =
          previous_keyframe[frame_idx].keypoint_sizes;
      current_keyframe[frame_idx].keypoint_scales =
          previous_keyframe[frame_idx].keypoint_scales;
      current_keyframe[frame_idx].keypoint_scores =
          previous_keyframe[frame_idx].keypoint_scores;
      current_keyframe[frame_idx].keypoint_orientations_rad.setZero(
          2, current_keyframe[frame_idx].keypoint_measurements.cols());

      // Remove points for which the tracking failed.
      std::vector<size_t> failed_indices;
      GetElementIndicesInVector(
          tracking_successful, /*find_value=*/0u, &failed_indices);

      VLOG(2) << "Removing " << failed_indices.size() << " of "
              << current_keyframe[frame_idx].keypoint_measurements.cols()
              << " keypoints for which tracking failed.";

      RemoveKeypoints(failed_indices, &current_keyframe[frame_idx]);
      RemoveKeypoints(failed_indices, &previous_keyframe_features_removed);

      // Reject Points with unusable Lidar data
      std::vector<size_t> bad_lidar_indices;

      for (int i = 0;
           i < current_keyframe[frame_idx].keypoint_measurements.cols(); ++i) {
        Eigen::Vector3d position_vector;
        backProject3d(
            cloud, &(curr_camera_images[0]),
            current_keyframe[frame_idx].keypoint_measurements.col(i),
            &position_vector);
        if (position_vector.norm() < settings_.min_tracking_distance) {
          bad_lidar_indices.emplace_back(i);
        }
      }

      VLOG_IF(2, !bad_lidar_indices.empty())
          << "Removing " << bad_lidar_indices.size()
          << " points with bad Lidar data from a total of "
          << current_keyframe[frame_idx].keypoint_measurements.cols()
          << " matches.";

      RemoveKeypoints(bad_lidar_indices, &current_keyframe[frame_idx]);
      RemoveKeypoints(bad_lidar_indices, &previous_keyframe_features_removed);

      // Reject outliers using PNP-ransac.
      Eigen::Matrix3d ransac_rotation_matrix;
      Eigen::Vector3d ransac_translation;
      std::vector<size_t> ransac_inliers;
      std::vector<size_t> ransac_outliers;
      PerformTemporalFrameToFrameRansac(
          cloud, current_keyframe[frame_idx],
          previous_keyframe_features_removed, &ransac_rotation_matrix,
          &ransac_translation, &ransac_inliers, &ransac_outliers);

      VLOG(2) << "Ransac Outliers: " << ransac_outliers.size();
      VLOG(2) << "Ransac Inliers: " << ransac_inliers.size();

      Eigen::Matrix3d rotation_matrix_gyro = q_Icurr_Iprev.getRotationMatrix();

      Eigen::Matrix3d relative_rotation =
          ransac_rotation_matrix.transpose() * rotation_matrix_gyro;
      double rotation_error = acos((relative_rotation.trace() - 1) / 2);
      VLOG(2) << "Rotation Error: " << rotation_error * 180 / M_PI << "°";
      if (show_illustrations) {
        for (int i = 0; i < ransac_outliers.size(); ++i) {
          cv::Point2f point;
          point.x = current_keyframe[frame_idx].keypoint_measurements(
              0, ransac_outliers[i]);
          point.y = current_keyframe[frame_idx].keypoint_measurements(
              1, ransac_outliers[i]);
          cv::circle(tracker_img, point, 4, cv::Scalar(0, 0, 255), CV_FILLED);
        }
      }

      RemoveKeypoints(ransac_outliers, &current_keyframe[frame_idx]);
      RemoveKeypoints(ransac_outliers, &previous_keyframe_features_removed);

      // Remove the non-describable features.
      // TODO(schneith): figure out the effect of this.
      std::vector<size_t> non_describable_indices;
      if (feature_describer_->hasNonDescribableFeatures(
              current_keyframe[frame_idx].keypoint_measurements,
              current_keyframe[frame_idx].keypoint_scales,
              &non_describable_indices)) {
        RemoveKeypoints(non_describable_indices, &current_keyframe[frame_idx]);
        RemoveKeypoints(
            non_describable_indices, &previous_keyframe_features_removed);
      }

      // Remove features that have converged, preferring to keep longer tracks
      // (which is equal to a lower track id).
      constexpr double kMinDistance = 3.0;
      cv::Mat occupancy_image(
          curr_camera_images[frame_idx].rows,
          curr_camera_images[frame_idx].cols, CV_8UC1, cv::Scalar(255));

      std::vector<size_t> keypoint_indices_sorted_by_track_id;
      GetKeypointIndicesSortedByTrackId(
          current_keyframe[frame_idx], &keypoint_indices_sorted_by_track_id);

      std::vector<size_t> converged_keypoint_indices;
      for (int kp_idx : keypoint_indices_sorted_by_track_id) {
        CHECK_LT(
            kp_idx, current_keyframe[frame_idx].keypoint_measurements.cols());
        CHECK_GE(current_keyframe[frame_idx].keypoint_track_ids(0, kp_idx), 0);

        cv::Point keypoint(
            current_keyframe[frame_idx].keypoint_measurements(0, kp_idx),
            current_keyframe[frame_idx].keypoint_measurements(1, kp_idx));

        if (occupancy_image.at<uchar>(keypoint) == 255) {
          cv::circle(
              occupancy_image, keypoint, kMinDistance, cv::Scalar(0),
              CV_FILLED);
        } else {
          converged_keypoint_indices.emplace_back(kp_idx);
        }
      }

      VLOG_IF(2, !converged_keypoint_indices.empty())
          << "Removing " << converged_keypoint_indices.size()
          << " converged features "
          << " from a total of "
          << current_keyframe[frame_idx].keypoint_measurements.cols()
          << " features.";

      RemoveKeypoints(converged_keypoint_indices, &current_keyframe[frame_idx]);
      RemoveKeypoints(
          converged_keypoint_indices, &previous_keyframe_features_removed);

      KeyframeFeaturesToCvPoints(
          current_keyframe[frame_idx].keypoint_measurements, &keypoints_curr);
      KeyframeFeaturesToCvPoints(
          previous_keyframe_features_removed.keypoint_measurements,
          &keypoints_prev);

      // Green circles
      if (show_illustrations) {
        for (int i = 0; i < keypoints_curr.size(); ++i) {
          cv::circle(tracker_img, keypoints_curr[i], 3, cv::Scalar(0, 255, 0));
          cv::line(
              tracker_img, keypoints_prev[i], keypoints_curr[i],
              cv::Scalar(0, 255, 0));
        }
      }
      int succ_keypoints = keypoints_curr.size();

      // Create a detection mask to prevent new detections close to features
      // that are already being tracked.
      // TODO(schneith): Expose as settings.
      constexpr size_t kRejectFeatureRadiusPx = 10u;

      cv::Mat detection_mask(
          curr_camera_images[frame_idx].rows,
          curr_camera_images[frame_idx].cols, CV_8UC1, cv::Scalar(255));

      keypoint_indices_sorted_by_track_id.clear();
      GetKeypointIndicesSortedByTrackId(
          current_keyframe[frame_idx], &keypoint_indices_sorted_by_track_id);

      for (int kp_idx : keypoint_indices_sorted_by_track_id) {
        CHECK_LT(
            kp_idx, current_keyframe[frame_idx].keypoint_measurements.cols());
        CHECK_GE(current_keyframe[frame_idx].keypoint_track_ids(0, kp_idx), 0);

        cv::Point keypoint(
            current_keyframe[frame_idx].keypoint_measurements(0, kp_idx),
            current_keyframe[frame_idx].keypoint_measurements(1, kp_idx));

        if (detection_mask.at<uchar>(keypoint) == 255) {
          cv::circle(
              detection_mask, keypoint, kRejectFeatureRadiusPx, cv::Scalar(0),
              CV_FILLED);
        }
      }
      // Adjust the mask for invalid lidar data
      for (int v = 0; v < curr_camera_images[frame_idx].rows - 4; v += 4) {
        for (int u = 0; u < curr_camera_images[frame_idx].cols - 4; u += 4) {
          Eigen::Vector3d position_vector;
          Eigen::Vector2d point;
          point << u + 2, v + 2;
          backProject3d(cloud, point, &position_vector);
          if (position_vector.norm() < settings_.min_tracking_distance) {
            cv::Point cvpoint(point[0], point[1]);
            cv::circle(detection_mask, cvpoint, 5, cv::Scalar(0), CV_FILLED);
            // cv::circle(
            //     tracker_img, cvpoint, 5, cv::Scalar(0, 0, 255), CV_FILLED);
          }
        }
      }

      // Fill up the keypoint slots using new detections.
      // We delay redetections for performance reasons until the successfully
      // tracked feature count drops under kRedetectThresholdPercent percent of
      // the max feature count.
      constexpr double kRedetectThresholdPercent = 0.7;
      VLOG(5) << "current detections: "
              << current_keyframe[frame_idx].keypoint_measurements.cols()
              << " vs. "
              << (kRedetectThresholdPercent * settings_.max_num_features);

      const bool should_redetect =
          (current_keyframe[frame_idx].keypoint_measurements.cols() <
           (kRedetectThresholdPercent * settings_.max_num_features));
      int new_keypoints = 0;
      if (should_redetect)
        VLOG(5) << "should redetect is true";
      else
        VLOG(5) << "should redetect is false";
      if (should_redetect) {
        const int num_to_detect =
            settings_.max_num_features -
            current_keyframe[frame_idx].keypoint_measurements.cols();
        CHECK_GE(num_to_detect, 0);

        KeyframeFeatures new_detections;
        feature_detector_->detectFeatures(
            curr_camera_images[frame_idx], num_to_detect, detection_mask,
            &new_detections);
        const int num_detected = new_detections.keypoint_measurements.cols();

        // Assign track ids to new detections.
        std::pair<size_t, size_t> ids_start_end =
            track_id_provider_.getIds(num_detected);
        new_detections.keypoint_track_ids.resize(Eigen::NoChange, num_detected);
        new_detections.keypoint_track_ids.setLinSpaced(
            ids_start_end.first, ids_start_end.second - 1);
        CHECK_EQ(
            new_detections.keypoint_track_ids.cols(),
            new_detections.keypoint_measurements.cols());

        AppendKeypoints(new_detections, &current_keyframe[frame_idx]);
        new_keypoints = new_detections.keypoint_measurements.cols();
        KeyframeFeaturesToCvPoints(
            new_detections.keypoint_measurements, &keypoints_curr);
        if (show_illustrations) {
          // Blue circles
          for (int i = 0; i < keypoints_curr.size(); ++i) {
            cv::circle(
                tracker_img, keypoints_curr[i], 3, cv::Scalar(255, 0, 0));
          }
        }
      }
      if (show_illustrations) {
        std::stringstream failed_stream;
        std::stringstream succ_stream;
        std::stringstream new_stream;
        succ_stream << "Successfull: " << succ_keypoints;
        failed_stream << "Failed: " << initial_keypoints - succ_keypoints;
        new_stream << "New: " << new_keypoints;
        cv::putText(
            tracker_img, succ_stream.str(), cv::Point2f(1900, 15),
            cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0, 255, 0));
        cv::putText(
            tracker_img, failed_stream.str(), cv::Point2f(1900, 35),
            cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0, 0, 255));
        cv::putText(
            tracker_img, new_stream.str(), cv::Point2f(1900, 55),
            cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(255, 0, 0));
      }

      // Extract the descriptors for all newly detected and also for the new
      // location of the tracked keypoints.
      VLOG(5) << "BEFORE DESCRIBTION: "
              << current_keyframe[frame_idx].keypoint_measurements.cols();
      feature_describer_->describeFeatures(
          curr_camera_images[frame_idx], &current_keyframe[frame_idx]);
      VLOG(5) << "AFTER DESCRIBTION: "
              << current_keyframe[frame_idx].keypoint_measurements.cols();

      // Assume a constant measurement uncertainty.
      // This needs to done after the feature extraction
      // since the number of keypoints can change in there.
      std::size_t num_keypoints =
          current_keyframe[frame_idx].keypoint_measurements.cols();
      Eigen::VectorXd uncertainties(num_keypoints);
      uncertainties.setConstant(0.8);
      current_keyframe[frame_idx].keypoint_measurement_uncertainties =
          uncertainties;
      current_keyframe[frame_idx].keypoint_vectors.resize(3, num_keypoints);

      // Reproject feature to add 3D information to the frame
      for (int i = 0; i < num_keypoints; ++i) {
        Eigen::Vector3d position_vector;
        Eigen::Vector2d point;
        point << current_keyframe[frame_idx].keypoint_measurements(0, i),
            current_keyframe[frame_idx].keypoint_measurements(1, i);
        backProject3d(cloud, point, &position_vector);
        current_keyframe[frame_idx].keypoint_vectors.col(i) = position_vector;
      }
      // cv::imshow("Mask", detection_mask);
      cv::imwrite(
          "/home/marius/Documents/Masterarbeit/picture_tests/bild.jpg",
          tracker_img);
    }

    if (optional_debug_data != nullptr) {
      optional_debug_data->inlier_matches_kp1_k.swap(inlier_matches_kp1_k);
      optional_debug_data->outlier_matches_kp1_k.swap(outlier_matches_kp1_k);
    }

    cv::imshow("Tracking 2", tracker_img);
    cv::waitKey(1);
  }

 private:
  const LkTrackingSettingsLaser settings_;

  TrackIdProvider track_id_provider_;
};

}  // namespace feature_tracking_pipelines

#endif  // FEATURE_TRACKING_PIPELINES_FEATURE_PIPELINE_LK_TRACKING_H_
