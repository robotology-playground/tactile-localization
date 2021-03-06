/*
 * Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
 *
 * This software may be modified and distributed under the terms of the
 * GPL-2+ license. See the accompanying LICENSE file for details.
 */

#include <ObjectOcclusion.h>

#include "opencv2/imgproc.hpp"

using namespace Eigen;


ObjectOcclusion::ObjectOcclusion(std::unique_ptr<MeshModel> mesh_model, const std::string cut_method, const double occlusion_scale) :
    mesh_model_(std::move(mesh_model)),
    cut_method_(cut_method),
    occlusion_scale_(occlusion_scale)
{ }


void ObjectOcclusion::reset()
{
    occlusion_area_set_ = false;
}


void ObjectOcclusion::findOcclusionArea()
{
    // Get occlusion pose
    bool valid_pose;
    MatrixXd pose;
    std::tie(valid_pose, pose) = getOcclusionPose();

    if (!valid_pose)
        return;

    // Get camera pose
    bool valid_camera_pose;
    VectorXd camera_origin;
    VectorXd camera_orientation;
    std::tie(valid_camera_pose, camera_origin, camera_orientation) = getCameraPose();

    if (!valid_camera_pose)
        return;

    // Get the sicad model pose
    bool valid_sicad_pose;
    std::vector<Superimpose::ModelPoseContainer> sicad_poses;
    std::tie(valid_sicad_pose, sicad_poses) = mesh_model_->getModelPose(pose);

    if (!valid_sicad_pose)
        return;

    // Render the occlusion mask
    cv::Mat occlusion_mask;
    object_sicad_->superimpose(sicad_poses[0], camera_origin.data(), camera_orientation.data(), occlusion_mask);

    // Convert to gray scale
    cv::cvtColor(occlusion_mask, occlusion_mask, CV_BGR2GRAY);

    if (cut_method_ == "convex_hull")
    {
        // Find contours
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(occlusion_mask, contours, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);

        if (contours.size() != 0)
        {
            // Find the contour with max area
            std::size_t max_index = 0;
            std::size_t max_area = cv::contourArea(contours[0]);

            for (std::size_t i = 1; i < contours.size(); i++)
            {
                double area = cv::contourArea(contours[i]);

                if (area > max_area)
                {
                    max_area = area;
                    max_index = i;
                }
            }

            // Take the contour having the maximum number of points and
            // find the convex hull
            std::vector<cv::Point> occlusion_area_not_scaled;
            cv::convexHull(contours[max_index], occlusion_area_not_scaled);

            // Enlarge the convex hull a bit
            occlusion_area_ = enlargeConvexHull(occlusion_area_not_scaled, occlusion_scale_);

            occlusion_area_set_ = true;
        }
    }
}


void ObjectOcclusion::drawOcclusionArea(cv::Mat& image)
{
    if (occlusion_area_set_)
    {
        std::vector<std::vector<cv::Point>> contours;
        contours.push_back(occlusion_area_);
        drawContours(image, contours, 0, cv::Scalar(255, 0, 0));
    }
}


std::tuple<bool, bool, cv::Mat> ObjectOcclusion::removeOcclusion(const cv::Mat& mask_in)
{
    if (!occlusion_area_set_)
        return std::make_tuple(false, false, mask_in);

    // Create a white mask on black background in order to identify if there is occlusion
    cv::Mat mask(mask_in.rows, mask_in.cols, CV_8UC1, cv::Scalar(0));
    std::vector<std::vector<cv::Point>> contours;
    contours.push_back(occlusion_area_);
    drawContours(mask, contours, 0, cv::Scalar(255), CV_FILLED);

    // Verifiy if there is occlusion
    cv::Mat intersection;
    cv::bitwise_and(mask_in, mask, intersection);
    if (cv::countNonZero(intersection) < 100)
        return std::make_tuple(true, false, mask_in);

    // Filter out occlusion from the input mask
    cv::Mat inverted_mask;
    cv::Mat filtered_mask;
    cv::bitwise_not(mask, inverted_mask);
    cv::bitwise_and(mask_in, inverted_mask, filtered_mask);

    return std::make_tuple(true, true, filtered_mask);
}


std::vector<cv::Point> ObjectOcclusion::enlargeConvexHull(const std::vector<cv::Point>& contour, const double& scale_factor)
{
    // Evaluate the centroid
    double c_x = 0.0;
    double c_y = 0.0;

    for (const cv::Point& point : contour)
    {
        c_x += point.x;
        c_y += point.y;
    }

    c_x /= contour.size();
    c_y /= contour.size();

    // Scale each point in the contour
    std::vector<cv::Point> scaled_points;
    for (const cv::Point& point : contour)
    {
        // Evaluate scaled point
        cv::Point scaled_point (c_x + (point.x - c_x) * scale_factor, c_y + (point.y - c_y) * scale_factor);

        scaled_points.push_back(scaled_point);
    }

    return scaled_points;
}
