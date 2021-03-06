/*
 * Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
 *
 * This software may be modified and distributed under the terms of the
 * GPL-2+ license. See the accompanying LICENSE file for details.
 */

#ifndef ICUBARMMODEL_H
#define ICUBARMMODEL_H

#include <Eigen/Dense>

#include <iCub/iKin/iKinFwd.h>

#include <iCubFingersEncoders.h>

#include <SuperimposeMesh/SICAD.h>

#include <yarp/os/Bottle.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/ConstString.h>
#include <yarp/sig/Matrix.h>


class iCubArmModel
{
public:
    iCubArmModel(const bool use_thumb,
                 const bool use_forearm,
                 const std::string& laterality,
                 const std::string& context,
                 const std::string& port_prefix);

    virtual ~iCubArmModel() noexcept;

    std::tuple<bool, SICAD::ModelPathContainer> getMeshPaths();

    std::tuple<bool, std::string> getShaderPaths();

    std::tuple<bool, std::vector<Superimpose::ModelPoseContainer>> getModelPose(const Eigen::Ref<const Eigen::MatrixXd>& cur_states);

protected:
    bool file_found(const std::string& file);

    yarp::sig::Matrix getInvertedH(const double a, const double d, const double alpha, const double offset, const double q);

    std::tuple<bool, yarp::sig::Vector> readRootToFingers();

    bool setArmJoints(const yarp::sig::Vector& q);

private:
    iCubFingersEncoders fingers_encoders_;

    const std::string log_ID_ = "[iCubArmModel]";

    const std::string port_prefix_ = "iCubArmModel";

    const bool use_thumb_;

    const bool use_forearm_;

    const std::string laterality_;

    const std::string context_;

    SICAD::ModelPathContainer model_path_;

    std::string shader_path_;

    iCub::iKin::iCubArm icub_arm_;

    iCub::iKin::iCubFinger icub_kin_finger_[5];

    yarp::os::BufferedPort<yarp::os::Bottle> port_torso_enc_;

    yarp::os::BufferedPort<yarp::os::Bottle> port_arm_enc_;
};

#endif /* ICUBARMMODEL_H */
