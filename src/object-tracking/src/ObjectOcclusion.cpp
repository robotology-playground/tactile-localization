#include <ObjectOcclusion.h>

#include "opencv2/imgproc.hpp"

using namespace Eigen;


ObjectOcclusion::ObjectOcclusion(std::unique_ptr<MeshModel> mesh_model, const std::string cut_method) :
    mesh_model_(std::move(mesh_model)),
    cut_method_(cut_method)
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

        // Consider only the first contour found
        if (contours.size() != 0)
        {
            // Find the convex hull
            cv::convexHull(contours[0], occlusion_area_);

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


std::tuple<bool, cv::Mat> ObjectOcclusion::removeOcclusion(const cv::Mat& mask_in)
{
    if (!occlusion_area_set_)
        return std::make_tuple(false, mask_in);

    // Create a black mask on white background
    cv::Mat mask_out(mask_in.rows, mask_in.cols, CV_8UC1, cv::Scalar(255));
    std::vector<std::vector<cv::Point>> contours;
    contours.push_back(occlusion_area_);
    drawContours(mask_out, contours, 0, cv::Scalar(0), CV_FILLED);

    // Filter out occlusion from input mask
    cv::bitwise_and(mask_out, mask_in, mask_out);

    return std::make_tuple(true, mask_out);
}
