/*
 * Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
 *
 * This software may be modified and distributed under the terms of the
 * GPL-2+ license. See the accompanying LICENSE file for details.
 */

#include <ArmController.h>
#include <GazeController.h>
#include <LemniscateGenerator.h>
#include <SinusoidalTrajectory.h>
#include <ObjectHelper.h>
#include <thrift/ObjectTrackingManipulationIDL.h>

#include <iCub/action/actionPrimitives.h>

#include <yarp/dev/CartesianControl.h>
#include <yarp/eigen/Eigen.h>
#include <yarp/math/Math.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/ResourceFinder.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/LogStream.h>
#include <yarp/sig/Vector.h>

#include <Eigen/Dense>

#include <cstdlib>
#include <deque>
#include <string>

using namespace Eigen;
using namespace yarp::dev;
using namespace yarp::eigen;
using namespace yarp::math;
using namespace yarp::os;
using namespace yarp::sig;
using namespace iCub::action;

enum class Status { Idle,
                    MoveUp, MoveDown, MoveLeft, MoveRight, MoveIn, MoveOut, WaitHandMove,
                    LatchCoarseApproachPoint,
                    MoveCoarseApproach, MovePreciseApproach,
                    TestCoarseApproach, TestPreciseApproach,
                    OpenHand, CloseHand, PostCloseHand, WaitOpenHand, WaitCloseHand,
                    DoPour,
                    MoveRest, WaitArmRest,
                    MoveHome, WaitArmHome,
                    StartMotion, MoveTrajectory,
                    Stop };


Vector loadVectorDouble
(
    ResourceFinder &rf,
    const std::string key,
    const std::size_t size
)
{
    bool ok = true;

    if (rf.find(key).isNull())
        ok = false;

    Bottle* b = rf.find(key).asList();
    if (b == nullptr)
        ok = false;

    if (b->size() != size)
        ok = false;

    if (!ok)
    {
        yError() << "[Main]" << "Unable to load vector" << key;
        std::exit(EXIT_FAILURE);
    }

    Vector vector(size);
    for (std::size_t i = 0; i < b->size(); i++)
    {
        Value item_v = b->get(i);
        if (item_v.isNull())
            return Vector(0);

        if (!item_v.isDouble())
            return Vector(0);

        vector(i) = item_v.asDouble();
    }

    return vector;
}


class ObjectTrackingManipulation: public yarp::os::RFModule,
                                  public ObjectTrackingManipulationIDL
{
protected:
    /*
     * Period.
     */
    double period_;

    /*
     * Object helper.
     */
    std::unique_ptr<ObjectHelper> helper_;

    /*
     * Trajectory generator (position).
     */
    std::unique_ptr<LemniscateGenerator> generator_;

    /*
     * Trajectory generator (orientation).
     */
    std::unique_ptr<SinusoidalTrajectory> generator_orientation_;

    /*
     * Controllers.
     */

    // action primitives for fingers opening and closing
    std::unique_ptr<ActionPrimitives> hand_action_;

    // gaze controller
    GazeController gaze_;

    // arm controllers
    ArmController right_arm_;
    ArmController left_arm_;
    ICartesianControl* actions_arm_;

    // cartesian controller trajectory times
    // used for home/rest actions
    double default_traj_time_;
    // used for move in a certain direction actions
    double default_shift_time_;
    // used for trajectory following action
    double default_traj_follow_time;

    // max_torso_pitch
    bool limit_torso_pitch_;
    double max_torso_pitch_;

    // max_torso_yaw
    bool limit_torso_yaw_;
    double max_torso_yaw_;

    // hand shift for movements
    double hand_shift_;

    /*
     * Positional part of hand trajectory
     */

    // scalings for execution of trajectory
    double traj_time_scale_;
    double traj_scale_;

    // offset for center of the trajectory
    double traj_center_z_offset_;

    // orientation for trajectory
    bool invert_traj_orientation_;

    // pitch and roll angle used during
    // approaching phase
    double hand_default_pitch_;
    double hand_default_roll_left_;
    double hand_default_roll_right_;

    // parameters for pouring action
    yarp::sig::Vector pour_translation_;
    yarp::sig::Vector pour_rotation_;
    double pour_traj_time_;

    /*
     */

    /*
     * Orientation part of hand trajectory
     */

    yarp::sig::Vector angle_traj_time_scale_;
    yarp::sig::Vector angle_traj_scale_;

    /*
     */

    /*
     * Orientation part of hand trajectory
     */

    yarp::sig::Vector post_close_hand_delta_;

    /*
     */

    // left and right arm rest positions
    yarp::sig::Vector left_arm_home_pos_;
    yarp::sig::Vector left_arm_home_att_;
    yarp::sig::Vector right_arm_home_pos_;
    yarp::sig::Vector right_arm_home_att_;

    yarp::sig::Vector left_arm_rest_pos_;
    yarp::sig::Vector left_arm_rest_att_;
    yarp::sig::Vector right_arm_rest_pos_;
    yarp::sig::Vector right_arm_rest_att_;

    /*
     * Rpc server.
     */

    // rpc server
    yarp::os::RpcServer rpc_port_;

    // mutexes required to share data between
    // the RFModule thread and the rpc server thread
    yarp::os::Mutex mutex_;

    /*
     * Status, booleans, defaults and names.
     */

    // status of the module
    Status status_;
    Status previous_status_;

    // cartesian context
    bool home_move_context_restored_;

    // name of the robot
    std::string robot_;
    std::string actions_laterality_;

    // name of arm to be used
    bool enable_torso_;
    std::string torso_laterality_;
    std::string hand_under_use_;
    std::string hand_rest_;
    std::string hand_home_;

    // required to implement counters
    // and timeouts
    bool is_timer_started_;
    double last_time_;
    double hand_move_timeout_;
    double hand_home_timeout_;
    double hand_rest_timeout_;
    double hand_open_timeout_;
    double hand_close_timeout_;

    /*
     * Thrift
     */

    std::string set_hand(const std::string& laterality)
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        mutex_.lock();

        std::string reply;

        if ((laterality != "right") && (laterality != "left"))
            reply = "[FAILED] You should specify a valid hand name.";
        else
        {
            hand_under_use_ = laterality;
            reply = "[OK] Set hand " + hand_under_use_ +  ".";
        }

        mutex_.unlock();

        return reply;
    }

    std::string unset_hand()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        mutex_.lock();

        std::string reply;

        hand_under_use_.clear();

        reply = "[OK] Current hand is now:" + hand_under_use_ + ".";

        mutex_.unlock();

        return reply;
    }

    std::string gaze_track_on()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        mutex_.lock();

        std::string reply;

        if (!gaze_.getGazeInterface().setTrackingMode(true))
            reply = "[FAILED] Unable to set gaze tracking.";
        else
            reply = "[OK] Set gaze tracking on.";

        mutex_.unlock();

        return reply;
    }

    std::string gaze_track_off()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        mutex_.lock();

        std::string reply;

        if (!gaze_.getGazeInterface().setTrackingMode(false))
            reply = "[FAILED] Unable to stop gaze tracking.";
        else
            reply = "[OK] Stopped gaze tracking.";

        mutex_.unlock();

        return reply;
    }

    std::string latch_approach()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::LatchCoarseApproachPoint;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string coarse_approach()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveCoarseApproach;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string test_coarse_approach()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::TestCoarseApproach;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string precise_approach()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MovePreciseApproach;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string test_precise_approach()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::TestPreciseApproach;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string open_hand()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::OpenHand;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string close_hand()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::CloseHand;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string do_pour()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::DoPour;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_up()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveUp;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_down()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveDown;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_left()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveLeft;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_right()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveRight;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_in()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveIn;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_out()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveOut;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_rest(const std::string& laterality)
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((laterality != "right") && (laterality != "left"))
            return "[FAILED] You should specify a valid hand name.";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveRest;
        hand_rest_ = laterality;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string move_home(const std::string& laterality)
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((laterality != "right") && (laterality != "left"))
            return "[FAILED] You should specify a valid hand name.";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::MoveHome;
        hand_home_ = laterality;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string start_motion()
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((hand_under_use_ != "left") && (hand_under_use_ != "right"))
            return "[FAILED] You need to set an hand using set_hand <laterality>";

        mutex_.lock();

        std::string reply;

        previous_status_ = status_;
        status_ = Status::StartMotion;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string get_hand_pose(const std::string& laterality)
    {
        if (status_ != Status::Idle)
            return "[FAILED] Wait for completion of the current phase.";

        if ((laterality != "left") && (laterality != "right"))
            return "[FAILED] You need to specify a valid laterality.";

        mutex_.lock();

        std::string reply;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return "[FAILED] Cannot get the interface to the requested hand.";

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return "[FAILED] Cannot get the current pose of the requested hand.";

        reply = "[OK]\n\tPosition: " + pos.toString() + "\n\tOrientation: " + att.toString() + "\n\t";

        mutex_.unlock();

        return reply;
    }

    std::string stop()
    {
        mutex_.lock();

        std::string reply;

        // change status
        previous_status_ = status_;
        status_ = Status::Stop;

        reply = "[OK] Command issued.";

        mutex_.unlock();

        return reply;
    }

    std::string quit()
    {
        // stop the module
        stopModule();

        return "[OK] Closing...";
    }

    /*
     * Implementation
     */

    /*
     * Get an arm controller.
     */
    ArmController* getArmController(const std::string laterality)
    {
        if (laterality == "right")
            return &right_arm_;
        else if (laterality == "left")
            return &left_arm_;
        else
            return nullptr;
    }

    /*
     * Check if arm motion is done.
     */
    bool checkArmMotionDone(const std::string laterality, bool& is_done)
    {
        ArmController *arm = getArmController(laterality);

        if (arm != nullptr)
            return arm->cartesian()->checkMotionDone(&is_done);
        else
            return false;
    }

    /*
     * Restore context.
     */
    bool restoreArmContext(const std::string laterality)
    {
        ArmController *arm = getArmController(laterality);

        if (arm != nullptr)
        {
            arm->restoreContext();
            return true;
        }
        else
            return false;
    }

    /*
     * Issue a pouring movement.
     */
    bool doPour()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // change orientation of the hand as required
        yarp::sig::Vector axis_angle_x(4);
        yarp::sig::Matrix rot_x;
        axis_angle_x[0] = 1.0;
        axis_angle_x[1] = 0.0;
        axis_angle_x[2] = 0.0;
        axis_angle_x[3] = pour_rotation_(0) * (M_PI / 180.0);
        rot_x = yarp::math::axis2dcm(axis_angle_x);

        yarp::sig::Vector axis_angle_y(4);
        yarp::sig::Matrix rot_y;
        axis_angle_y[0] = 0.0;
        axis_angle_y[1] = 1.0;
        axis_angle_y[2] = 0.0;
        axis_angle_y[3] = pour_rotation_(1) * (M_PI / 180.0);
        rot_y = yarp::math::axis2dcm(axis_angle_y);

        yarp::sig::Vector axis_angle_z(4);
        yarp::sig::Matrix rot_z;
        axis_angle_z[0] = 0.0;
        axis_angle_z[1] = 0.0;
        axis_angle_z[2] = 1.0;
        axis_angle_z[3] = pour_rotation_(2) * (M_PI / 180.0);
        rot_z = yarp::math::axis2dcm(axis_angle_z);

        yarp::sig::Matrix rot_hand = yarp::math::axis2dcm(att);

        yarp::sig::Matrix rot_updated = rot_z * rot_y * rot_x * rot_hand;
        yarp::sig::Vector att_updated = yarp::math::dcm2axis(rot_updated);

        // set trajectory time
        if (pour_traj_time_ < 1.0)
        {
            yError() << "You are requesting a trajectory time" << pour_traj_time_ << "less than 1 seconds!";
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(pour_traj_time_)))
            return false;

        // issue command
        return arm->cartesian()->goToPoseSync(pos + pour_translation_, att_updated);
    }

    /*
     * Issue a coarse approach to the object.
     */
    bool moveCoarseApproach()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // pick the required hand orientation
        Vector orientation = helper_->getRequiredHandOrientation();

        // set the required hand orientation
        arm->setHandAttitude(orientation(0), orientation(1), orientation(2));

        // pick the coarse approach position
        bool valid_position;
        Vector position;
        std::tie(valid_position, position) = helper_->getCoarseApproachPoint();
        if (!valid_position)
            return false;

        // set trajectory time
        if (default_traj_time_ < 1.0)
        {
            yError() << "You are requesting a trajectory time" << default_traj_time_ << "less than 1 seconds!";
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(default_traj_time_)))
            return false;

        // issue command
        return arm->goToPos(position);
    }

    /*
     * Issue a coarse approach to the object.
     */
    bool testCoarseApproach()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the required hand orientation
        Vector orientation = helper_->getRequiredHandOrientation();

        // pick the coarse approach position
        bool valid_position;
        Vector position;
        std::tie(valid_position, position) = helper_->getCoarseApproachPoint();
        if (!valid_position)
        {
            yInfo() << "Coarse approach pose not available:" << position.toString();

            return false;
        }

        yInfo() << "Coarse approach position:" << position.toString();
        yInfo() << "Coarse approach orientation:" << orientation.toString();

        return true;
    }


    /*
     * Issue a coarse approach to the object.
     */
    bool evaluateCoarseApproachPoint()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // request evaluation of approach point
        if (!helper_->evaluateApproachPosition())
            return false;

        // test approach point for safety
        if (!testCoarseApproach())
            return false;

        return true;
    }

    /*
     * Issue a precise approach to the object.
     */
    bool movePreciseApproach()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // pick the required hand orientation
        // Vector orientation = helper_->getRequiredHandOrientation();

        // set the required hand orientation
        // arm->setHandAttitude(orientation(0), orientation(1), orientation(2));

        // pick the direction to be used during precise approaching
        Vector delta = helper_->getPreciseApproachDirection();

        // set trajectory time
        if (default_traj_time_ < 1.0)
        {
            yError() << "You are requesting a trajectory time" << default_traj_time_ << "less than 1 seconds!";
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(default_traj_time_)))
            return false;

        // issue command
        return arm->cartesian()->goToPoseSync(pos + delta, att);
    }

    /*
     * Issue a precise approach to the object.
     */
    bool testPreciseApproach()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // pick the coarse approach position
        Vector delta = helper_->getPreciseApproachDirection();

        yInfo() << "Precise approach position:" << (pos + delta).toString();

        return true;
    }

    /*
     * Open the hand.
     */
    bool openHand()
    {
        return hand_action_->pushAction("open_hand");
    }

    /*
     * Close the hand.
     */
    bool closeHand()
    {
        return hand_action_->pushAction("close_hand");
    }

    /*
     * Action to be done after hand closure
     */
    bool postCloseHand()
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // shift position as required
        pos(0) += post_close_hand_delta_(0);
        pos(1) += post_close_hand_delta_(1);
        pos(2) += post_close_hand_delta_(2);

        // change orientation of the hand as required
        yarp::sig::Vector axis_angle_x(4);
        yarp::sig::Matrix rot_x;
        axis_angle_x[0] = 1.0;
        axis_angle_x[1] = 0.0;
        axis_angle_x[2] = 0.0;
        axis_angle_x[3] = post_close_hand_delta_(3) * (M_PI / 180.0);
        rot_x = yarp::math::axis2dcm(axis_angle_x);

        yarp::sig::Vector axis_angle_y(4);
        yarp::sig::Matrix rot_y;
        axis_angle_y[0] = 0.0;
        axis_angle_y[1] = 1.0;
        axis_angle_y[2] = 0.0;
        axis_angle_y[3] = post_close_hand_delta_(4) * (M_PI / 180.0);
        rot_y = yarp::math::axis2dcm(axis_angle_y);

        yarp::sig::Vector axis_angle_z(4);
        yarp::sig::Matrix rot_z;
        axis_angle_z[0] = 0.0;
        axis_angle_z[1] = 0.0;
        axis_angle_z[2] = 1.0;
        axis_angle_z[3] = post_close_hand_delta_(5) * (M_PI / 180.0);
        rot_z = yarp::math::axis2dcm(axis_angle_z);

        yarp::sig::Matrix rot_hand = yarp::math::axis2dcm(att);

        yarp::sig::Matrix rot_updated = rot_z * rot_y * rot_x * rot_hand;
        yarp::sig::Vector att_updated = yarp::math::dcm2axis(rot_updated);

        // set trajectory time
        if (default_shift_time_ < 1.0)
        {
            yError() << "You are requesting a shift time" << default_shift_time_ << "less than 1 seconds!";
            return false;
        }
        if(!(arm->cartesian()->setTrajTime(default_shift_time_)))
            return false;

        // issue command
        return arm->cartesian()->goToPoseSync(pos, att_updated);
    }

    /*
     * Move the hand in the requested direction.
     */
    bool moveHand(const std::string laterality, const std::string direction)
    {
        // check if the hand name is valid
        if ((hand_under_use_.empty()) || ((hand_under_use_ != "right") && (hand_under_use_ != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // shift position as required
        if (direction == "up")
            pos[2] += hand_shift_;
        else if (direction == "down")
            pos[2] -= hand_shift_;
        else if (direction == "left")
            pos[1] -= hand_shift_;
        else if (direction == "right")
            pos[1] += hand_shift_;
        else if (direction == "in")
            pos[0] -= hand_shift_;
        else if (direction == "out")
            pos[0] += hand_shift_;

        // set trajectory time
        if (default_shift_time_ < 1.0)
        {
            yError() << "You are requesting a shift time" << default_shift_time_ << "less than 1 seconds!";
            return false;
        }
        if(!(arm->cartesian()->setTrajTime(default_shift_time_)))
            return false;

        // issue command
        return arm->cartesian()->goToPoseSync(pos, att);
    }

    /*
     * Move the arm in the default rest position.
     */
    bool moveArmRest(const std::string laterality)
    {
        // check if the hand name is valid
        if ((laterality.empty()) || ((laterality != "right") && (laterality != "left")))
            return false;

        hand_rest_ = laterality;

        // pick the correct arm
        ArmController* arm = getArmController(hand_rest_);
        if (arm == nullptr)
            return false;

        // set trajectory time
        if (default_traj_time_ < 1.0)
        {
            yError() << "You are requesting a trajectory time" << default_traj_time_ << "less than 1 seconds!";
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(default_traj_time_)))
            return false;

        // issue command
        bool ok;
        if (hand_rest_ == "left")
            ok = left_arm_.cartesian()->goToPoseSync(left_arm_rest_pos_, left_arm_rest_att_);
        else if (hand_rest_ == "right")
            ok = right_arm_.cartesian()->goToPoseSync(right_arm_rest_pos_, right_arm_rest_att_);
        return ok;
    }

    bool moveArmHome(const std::string laterality)
    {
        // check if the hand name is valid
        if ((laterality.empty()) || ((laterality != "right") && (laterality != "left")))
            return false;

        hand_home_ = laterality;

        // pick the correct arm
        ArmController* arm = getArmController(hand_home_);
        if (arm == nullptr)
            return false;

        if (enable_torso_)
        {
            bool ok_torso_0;
            arm->storeContext();
            ok_torso_0 = arm->cartesian()->setLimits(0,0.0,0.0);
            ok_torso_0 &= arm->cartesian()->setLimits(1,0.0,0.0);
            ok_torso_0 &= arm->cartesian()->setLimits(2,0.0,0.0);
            if (!ok_torso_0)
            {
                yError() << "Unable to force home torso sulution to 0. Resetting context.";
                arm->restoreContext();
                return false;
            }
        }

        // set trajectory time
        if (default_traj_time_ < 1.0)
        {
            yError() << "You are requesting a trajectory time" << default_traj_time_ << "less than 1 seconds!";
            arm->restoreContext();
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(default_traj_time_)))
        {
            arm->restoreContext();
            return false;
        }

        // issue command
        bool ok_cmd;
        if (hand_home_ == "left")
            ok_cmd = left_arm_.cartesian()->goToPoseSync(left_arm_home_pos_, left_arm_home_att_);
        else if (hand_home_ == "right")
            ok_cmd = right_arm_.cartesian()->goToPoseSync(right_arm_home_pos_, right_arm_home_att_);
        if (!ok_cmd)
        {
            arm->restoreContext();

            return false;
        }

        if (enable_torso_)
        {
            home_move_context_restored_ = false;
        }

        return true;
    }

    /*
     * Initialize trajectory generator
     */
    bool startMoveHandTrajctory()
    {
        // reset the trajectory generator
        generator_->reset();
        generator_orientation_->reset();

        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        yarp::sig::Vector pos;
        yarp::sig::Vector att;
        if (!arm->cartesian()->getPose(pos, att))
            return false;

        // set the center within the trajectory generator
        Eigen::Vector3d center;
        center = toEigen(pos);
        center(1) -= 0.03;
        center(2) += traj_center_z_offset_;
        generator_->setCenter(center);

        // set the center within the orientation generator
        Eigen::Vector3d axis;
        axis(0) = att(0);
        axis(1) = att(1);
        axis(2) = att(2);
        Eigen::AngleAxisd angle_axis(att(3), axis);
        generator_orientation_->setCenter(angle_axis.toRotationMatrix().eulerAngles(2, 1, 0));

        // set trajectory time
        if (default_traj_follow_time < 0.8)
        {
            yError() << "You are requesting a trajectory time" << default_traj_follow_time << "less than 0.8 seconds!";
            return false;
        }
        if (!(arm->cartesian()->setTrajTime(default_traj_follow_time)))
            return false;

        return true;
    }

    /*
     * Set next set point along the trajectory
     */
    bool moveHandTrajectory(const double& time)
    {
        // pick the correct arm
        ArmController* arm = getArmController(hand_under_use_);
        if (arm == nullptr)
            return false;

        // get the current position of the palm of the hand
        // in case it is required to fix one of the two
        // yarp::sig::Vector pos;
        // yarp::sig::Vector att;
        // if (!arm->cartesian()->getPose(pos, att))
        //     return false;

        // get the current position along the trajectory
        Eigen::Vector3d pose = generator_->getCurrentPose(time);
        yarp::sig::Vector traj_pos(3, pose.data());

        // get the current orientation along the trajectory
        Eigen::Vector3d orientation = generator_orientation_->getCurrentPose(time);
        Eigen::AngleAxisd angle_axis(Eigen::AngleAxisd(orientation(0), Eigen::Vector3d::UnitZ()) *
                                     Eigen::AngleAxisd(orientation(1), Eigen::Vector3d::UnitY()) *
                                     Eigen::AngleAxisd(orientation(2), Eigen::Vector3d::UnitX()));
        yarp::sig::Vector axis_angle(4);
        yarp::sig::Vector axis(3, angle_axis.axis().data());
        axis_angle.setSubvector(0, axis);
        axis_angle(3) = angle_axis.angle();

        return arm->cartesian()->goToPose(traj_pos, axis_angle);
    }

    /*
     * Stop control of the specified arm.
     */
    bool stopArm(const std::string& laterality)
    {
        // check if the hand name is valid
        if ((laterality.empty()) || ((laterality != "right") && (laterality != "left")))
            return false;

        // pick the correct arm
        ArmController* arm = getArmController(laterality);
        if (arm == nullptr)
            return false;

        return arm->cartesian()->stopControl();
    }

public:
    ObjectTrackingManipulation() :
        gaze_("object-tracking-manipulation")
    { }

    bool configure(yarp::os::ResourceFinder &rf)
    {
        // open the rpc server
        if (!rpc_port_.open("/object-tracking-manipulation/cmd:i"))
        {
            yError() << "ObjectManipulationDemo: unable to open the RPC command port.";

            return false;
        }

        if (!(this->yarp().attachAsServer(rpc_port_)))
        {
            yError() << "ObjectManipulationDemo: unable to attach the RPC command port to the RPC server.";

            return false;
        }

        /*
         * Parameters from configuration
         */

        robot_ = rf.check("robot", Value("icub")).asString();
        yInfo() << "- robot" << robot_;

        actions_laterality_ = rf.check("actions_laterality", Value("right")).asString();
        yInfo() << "- actions_laterality" << actions_laterality_;

        period_ = rf.check("period", Value(0.02)).asDouble();
        yInfo() << "- period" << period_;

        enable_torso_ = rf.check("enable_torso", Value(false)).asBool();
        torso_laterality_ = rf.check("torso_laterality", Value("right")).asString();
        yInfo() << "- enable_torso" << enable_torso_;
        yInfo() << "- torso_laterality" << torso_laterality_;

        limit_torso_pitch_ = rf.check("limit_torso_pitch", Value(true)).asBool();
        max_torso_pitch_ = rf.check("max_torso_pitch", Value(10.0)).asDouble();
        yInfo() << "- limit_torso_pitch" << limit_torso_pitch_;
        yInfo() << "- max_torso_pitch" << max_torso_pitch_;

        limit_torso_yaw_ = rf.check("limit_torso_yaw", Value(true)).asBool();
        max_torso_yaw_ = rf.check("max_torso_yaw", Value(10.0)).asDouble();
        yInfo() << "- limit_torso_yaw" << limit_torso_yaw_;
        yInfo() << "- max_torso_yaw" << max_torso_yaw_;

        default_traj_time_ = rf.check("default_traj_time", Value(5.0)).asDouble();
        yInfo() << "- default_traj_time" << default_traj_time_;

        default_shift_time_ = rf.check("default_shift_time", Value(5.0)).asDouble();
        yInfo() << "- default_shift_time" << default_shift_time_;

        default_traj_follow_time = rf.check("default_following_time", Value(2.0)).asDouble();
        yInfo() << "- default_following_time" << default_traj_follow_time;

        hand_shift_ = rf.check("hand_shift", Value(0.01)).asDouble();
        yInfo() << "- hand_shift" << hand_shift_;

        traj_scale_ = rf.check("traj_scale", Value(0.01)).asDouble();
        traj_time_scale_  = rf.check("traj_time_scale", Value(1.0)).asDouble();
        traj_center_z_offset_  = rf.check("traj_center_z_offset", Value(1.0)).asDouble();
        invert_traj_orientation_ = rf.check("invert_traj_orientation", Value(false)).asBool();
        yInfo() << "- traj_scale" << traj_scale_;
        yInfo() << "- traj_time_scale" << traj_time_scale_;
        yInfo() << "- traj_center_z_offset" << traj_center_z_offset_;
        yInfo() << "- invert_traj_orientation" << invert_traj_orientation_;

        angle_traj_scale_ = loadVectorDouble(rf, "angle_traj_scale", 3);
        angle_traj_time_scale_ = loadVectorDouble(rf, "angle_traj_time_scale", 3);
        yInfo() << "- angle_traj_scale" << angle_traj_scale_.toString();
        yInfo() << "- angle_traj_time_scale" << angle_traj_time_scale_.toString();

        post_close_hand_delta_ = loadVectorDouble(rf, "post_close_hand_delta", 6);
        yInfo() << "- post_close_hand_delta" << post_close_hand_delta_.toString();

        hand_move_timeout_  = rf.check("hand_move_timeout", Value(10.0)).asDouble();
        hand_home_timeout_  = rf.check("hand_home_timeout", Value(10.0)).asDouble();
        hand_rest_timeout_  = rf.check("hand_rest_timeout", Value(10.0)).asDouble();
        hand_open_timeout_  = rf.check("hand_open_timeout", Value(10.0)).asDouble();
        hand_close_timeout_ = rf.check("hand_close_timeout", Value(10.0)).asDouble();
        yInfo() << "- hand_move_timeout"  << hand_move_timeout_;
        yInfo() << "- hand_home_timeout"  << hand_home_timeout_;
        yInfo() << "- hand_rest_timeout"  << hand_rest_timeout_;
        yInfo() << "- hand_open_timeout"  << hand_open_timeout_;
        yInfo() << "- hand_close_timeout" << hand_close_timeout_;

        hand_default_pitch_      = rf.check("hand_default_pitch", Value(0.0)).asDouble();
        hand_default_roll_left_  = rf.check("hand_default_roll_left", Value(0.0)).asDouble();
        hand_default_roll_right_ = rf.check("hand_default_roll_right", Value(0.0)).asDouble();
        yInfo() << "- hand_default_pitch" << hand_default_pitch_;
        yInfo() << "- hand_default_roll_left" << hand_default_roll_left_;
        yInfo() << "- hand_default_roll_right" << hand_default_roll_right_;

        Vector left_arm_home_pose = loadVectorDouble(rf, "left_arm_home_pose", 7);
        left_arm_home_pos_ = left_arm_home_pose.subVector(0, 2);
        left_arm_home_att_ = left_arm_home_pose.subVector(3, 6);
        yInfo() << "- left_arm_home_pos" << left_arm_home_pos_.toString();
        yInfo() << "- left_arm_home_att" << left_arm_home_att_.toString();

        Vector left_arm_rest_pose = loadVectorDouble(rf, "left_arm_rest_pose", 7);
        left_arm_rest_pos_ = left_arm_rest_pose.subVector(0, 2);
        left_arm_rest_att_ = left_arm_rest_pose.subVector(3, 6);
        yInfo() << "- left_arm_rest_pos" << left_arm_rest_pos_.toString();
        yInfo() << "- left_arm_rest_att" << left_arm_rest_att_.toString();

        Vector right_arm_home_pose = loadVectorDouble(rf, "right_arm_home_pose", 7);
        right_arm_home_pos_ = right_arm_home_pose.subVector(0, 2);
        right_arm_home_att_ = right_arm_home_pose.subVector(3, 6);
        yInfo() << "- right_arm_home_pos" << right_arm_home_pos_.toString();
        yInfo() << "- right_arm_home_att" << right_arm_home_att_.toString();

        Vector right_arm_rest_pose = loadVectorDouble(rf, "right_arm_rest_pose", 7);
        right_arm_rest_pos_ = right_arm_rest_pose.subVector(0, 2);
        right_arm_rest_att_ = right_arm_rest_pose.subVector(3, 6);
        yInfo() << "- right_arm_rest_pos" << right_arm_rest_pos_.toString();
        yInfo() << "- right_arm_rest_att" << right_arm_rest_att_.toString();

        pour_translation_ = loadVectorDouble(rf, "pour_translation", 3);
        pour_rotation_ = loadVectorDouble(rf, "pour_rotation", 3);
        pour_traj_time_ = rf.check("pour_traj_time", Value(2.0)).asDouble();
        yInfo() << "- pour_translation" << pour_translation_.toString();
        yInfo() << "- pour_rotation" << pour_rotation_.toString();
        yInfo() << "- pour_traj_time" << pour_traj_time_;

        /*
         * Object helper
         */
        helper_ = std::unique_ptr<ObjectHelper>(new ObjectHelper("object-tracking-manipulation", "object-tracking-manipulation", actions_laterality_));

        /*
         * Trajectory generator (position)
         */
        generator_ = std::unique_ptr<LemniscateGenerator>(new LemniscateGenerator(traj_scale_, traj_time_scale_, invert_traj_orientation_));

        /*
         * Trajectory generator (orientation)
         */
        generator_orientation_ = std::unique_ptr<SinusoidalTrajectory>(new SinusoidalTrajectory(toEigen(angle_traj_scale_), toEigen(angle_traj_time_scale_)));

        /*
         * Arm Controllers
         */

        // configure arm controllers
        bool ok_right_ctl = right_arm_.configure("object-tracking-manipulation", robot_, "right");
        if (!ok_right_ctl)
        {
            yError() << "ObjectManipulationDemo: unable to configure the right arm controller";
            return false;
        }

        bool ok_left_ctl = left_arm_.configure("object-tracking-manipulation", robot_, "left");
        if (!ok_left_ctl)
        {
            yError() << "ObjectManipulationDemo: unable to configure the left arm controller";
            return false;
        }

        // enable torso on one arm only
        if (enable_torso_)
        {
            ArmController& arm_ctl = (torso_laterality_ == "left" ? left_arm_ : right_arm_);
            arm_ctl.enableTorso();
            if (limit_torso_pitch_)
                arm_ctl.limitTorsoPitch(max_torso_pitch_);
            if (limit_torso_yaw_)
                arm_ctl.limitTorsoYaw(max_torso_yaw_);
        }

        /*
         * Fingers controllers
         */

        // get path of the springy fingers calibration file
        const std::string springy_file_path = rf.findFile("springy_calibration_" + actions_laterality_ + ".ini");

        // get path of hand sequences file
        const std::string hand_sequences_path = rf.findFile("hand_sequences.ini");

        // initialize hand actions
        Property hand_action_properties;
        hand_action_properties.put("local", "object-tracking-manipulation/action-primitives");
        hand_action_properties.put("part", actions_laterality_ + "_arm");
        hand_action_properties.put("robot", robot_);
        hand_action_properties.put("torso_yaw", "off");
        hand_action_properties.put("torso_pitch", "off");
        hand_action_properties.put("torso_roll", "off");
        hand_action_properties.put("grasp_model_type", "springy");
        hand_action_properties.put("grasp_model_file", springy_file_path.c_str());
        hand_action_properties.put("hand_sequences_file", hand_sequences_path.c_str());
        hand_action_ = std::unique_ptr<ActionPrimitives>(new ActionPrimitives(hand_action_properties));

        // store a reference to the instance of the cartesian interface opened by ActionPrimivites
        // since ActionPrimitives is used only to move the fingers,
        // however, there should not be any need to deal with the context of the underlying cartesian interface
        hand_action_->getCartesianIF(actions_arm_);

        // print availables hand sequences
        std::deque<std::string> q = hand_action_->getHandSeqList();
        yInfo() << "List of available hand sequence keys:";
        for (std::size_t i = 0; i < q.size(); i++)
        {
            Bottle sequence;
            hand_action_->getHandSequence(q[i],sequence);

            yInfo() << "hand_seq_name:"        << q[i];
            yInfo() << "hand_seq_description:" << sequence.toString();
        }

        /*
         * Defaults
         */

        is_timer_started_ = false;

        // set default status
        status_ = Status::Idle;
        previous_status_ = Status::Idle;

        // clear arm names
        hand_under_use_.clear();
        hand_rest_.clear();
        hand_home_.clear();

        home_move_context_restored_ = true;

        return true;
    }

    bool close()
    {
        // stop control of arms
        stopArm("left");
        stopArm("right");

        // close arm controllers
        right_arm_.close();
        left_arm_.close();

        // close ports
        rpc_port_.close();

        return true;
    }

    double getPeriod()
    {
        return period_;
    }

    bool updateModule()
    {
        if(isStopping())
            return false;

        mutex_.lock();

        // get the current and previous status
        const Status curr_status = status_;
        const Status prev_status = previous_status_;

        // get the current hand names
        const std::string hand_under_use_l = hand_under_use_;
        const std::string hand_home_l = hand_home_;
        const std::string hand_rest_l = hand_rest_;

        mutex_.unlock();

        switch(curr_status)
        {
        case Status::Idle:
        {
            // nothing to do here
            break;
        }

        case Status::OpenHand:
        {
            if (!openHand())
            {
                yError() << "[OPEN HAND] error while trying to open the hand.";
                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitOpenHand;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::CloseHand:
        {
            if (!closeHand())
            {
                yError() << "[CLOSE HAND] error while trying to close the hand.";
                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitCloseHand;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::WaitOpenHand:
        {
            // check status
            bool is_done = false;
	    // non-blocking checkActionsDone is broken. Using a timeout.
            bool ok = hand_action_->checkActionsDone(is_done, false);

            // handle failure and timeout
            if (!ok)//|| ((yarp::os::Time::now() - last_time_ > hand_open_timeout_)))
            {
                yError() << "[WAIT OPEN HAND] check motion done failed.";

                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go back to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            if ((yarp::os::Time::now() - last_time_) > hand_open_timeout_)
            {
                // move completed
                yInfo() << "[WAIT OPEN HAND] done.";

                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();
            }

            break;
        }

        case Status::WaitCloseHand:
        {
            // check status
            bool is_done = false;
	    // non-blocking checkActionsDone is broken. Using a timeout.
            bool ok = hand_action_->checkActionsDone(is_done, false);

            // handle failure and timeout
            if (!ok)// || ((yarp::os::Time::now() - last_time_ > hand_close_timeout_)))
            {
                yError() << "[WAIT CLOSE HAND] check motion done failed or timeout reached.";

                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go back to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            if ((yarp::os::Time::now() - last_time_) > hand_close_timeout_)
            {
                // move completed
                yInfo() << "[WAIT CLOSE HAND] done.";

                // stop control
                hand_action_->stopControl();

                mutex_.lock();

                // go to PostCloseHand
                status_ = Status::PostCloseHand;

                mutex_.unlock();
            }

            break;
        }

        case Status::PostCloseHand:
        {
            if (!postCloseHand())
            {
                yError() << "[POST CLOSE HAND] error while trying to move hand.";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitHandMove;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::LatchCoarseApproachPoint:
        {
            if (!evaluateCoarseApproachPoint())
            {
                yError() << "[LATCH COARSE APPROACH POINT] error while trying evaluate the approach point.";

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

	    mutex_.lock();

	    // go to Idle
	    status_ = Status::Idle;

	    mutex_.unlock();

            break;
        }

        case Status::MoveCoarseApproach:
        {
            if (!moveCoarseApproach())
            {
                yError() << "[MOVE COARSE APPROACH] error while trying to move hand.";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitHandMove;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::TestCoarseApproach:
        {
            if (!testCoarseApproach())
            {
                yError() << "[TEST COARSE APPROACH] error while trying to simulate command.";
            }

            mutex_.lock();

            // go to Idle
            status_ = Status::Idle;

            mutex_.unlock();

            break;
        }

        case Status::MovePreciseApproach:
        {
            if (!movePreciseApproach())
            {
                yError() << "[MOVE PRECISE APPROACH] error while trying to move hand.";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitHandMove;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::TestPreciseApproach:
        {
            if (!testPreciseApproach())
            {
                yError() << "[TEST PRECISE APPROACH] error while trying to simulate command.";
            }

            mutex_.lock();

            // go to Idle
            status_ = Status::Idle;

            mutex_.unlock();

            break;
        }

        case Status::StartMotion:
        {
            if (!startMoveHandTrajctory())
            {
                yError() << "[START MOTION] error while trying to initialize the trajectory.";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to MoveTrajectory
            status_ = Status::MoveTrajectory;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::MoveTrajectory:
        {
            // evaluate current time
            double current_time = yarp::os::Time::now() - last_time_;

            if (!moveHandTrajectory(current_time))
            {
                yError() << "[MOVE TRAJECTORY] error while trying to move hand";

                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            // execute until user requests stop
            break;
        }

        case Status::MoveUp:
        case Status::MoveDown:
        case Status::MoveLeft:
        case Status::MoveRight:
        case Status::MoveIn:
        case Status::MoveOut:
        {
            std::string direction;
            if (curr_status == Status::MoveUp)
                direction = "up";
            else if (curr_status == Status::MoveDown)
                direction = "down";
            else if (curr_status == Status::MoveLeft)
                direction = "left";
            else if (curr_status == Status::MoveRight)
                direction = "right";
            else if (curr_status == Status::MoveIn)
                direction = "in";
            else if (curr_status == Status::MoveOut)
                direction = "out";

            if (!moveHand(hand_under_use_l, direction))
            {
                yError() << "[MOVE HAND] error while trying to move hand" << direction +  ".";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitHandMove;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::WaitHandMove:
        {
            // check status
            bool is_done = false;
            bool ok = checkArmMotionDone(hand_under_use_l, is_done);

            // handle failure and timeout
            if (!ok || ((yarp::os::Time::now() - last_time_ > hand_move_timeout_)))
            {
                yError() << "[WAIT HAND MOVE] check motion done failed or timeout reached.";

                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go back to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            if (is_done)
            {
                // move completed
                yInfo() << "[WAIT HAND MOVE] done.";

                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();
            }

            break;
        }

        case Status::MoveRest:
        {
            if (!moveArmRest(hand_rest_l))
            {
                yError() << "[MOVE REST] error while trying to move arm to rest position.";

                // stop control
                stopArm(hand_rest_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                // reset arm name
                hand_rest_.clear();

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitArmRest
            status_ = Status::WaitArmRest;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::WaitArmRest:
        {
            // check status
            bool is_done = false;
            bool ok = checkArmMotionDone(hand_rest_l, is_done);

            // handle failure and timeout
            if (!ok || ((yarp::os::Time::now() - last_time_ > hand_rest_timeout_)))
            {
                yError() << "[WAIT MOVE REST] check motion done failed or timeout reached.";
                // stop control
                stopArm(hand_rest_l);

                mutex_.lock();

                // go back to Idle
                status_ = Status::Idle;

                // reset arm name
                hand_rest_.clear();

                mutex_.unlock();

                break;
            }

            if (is_done)
            {
                // approach completed
                yInfo() << "[WAIT ARM REST] done";

                // stop control
                stopArm(hand_rest_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                // clear name
                hand_rest_.clear();

                mutex_.unlock();
            }

            break;
        }

        case Status::MoveHome:
        {
            if (!moveArmHome(hand_home_l))
            {
                yError() << "[MOVE REST] error while trying to move arm to home position.";

                // stop control
                stopArm(hand_home_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                // reset arm name
                hand_home_.clear();

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitArmHome
            status_ = Status::WaitArmHome;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::WaitArmHome:
        {
            // check status
            bool is_done = false;
            bool ok = checkArmMotionDone(hand_home_l, is_done);

            // handle failure and timeout
            if (!ok || ((yarp::os::Time::now() - last_time_ > hand_home_timeout_)))
            {
                yError() << "[WAIT MOVE HOME] check motion done failed or timeout reached.";
                // stop control
                stopArm(hand_home_l);

                if (enable_torso_)
                {
                    restoreArmContext(hand_home_l);
                    home_move_context_restored_ = true;
                }

                mutex_.lock();

                // go back to Idle
                status_ = Status::Idle;

                // reset arm name
                hand_home_.clear();

                mutex_.unlock();

                break;
            }

            if (is_done)
            {
                // approach completed
                yInfo() << "[WAIT ARM HOME] done";

                // stop control
                stopArm(hand_home_l);

                if (enable_torso_)
                {
                    restoreArmContext(hand_home_l);
                    home_move_context_restored_ = true;
                }

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                // clear name
                hand_home_.clear();

                mutex_.unlock();
            }

            break;
        }

        case Status::DoPour:
        {
            if (!doPour())
            {
                yError() << "[DO POUR] error while trying to move hand.";
                // stop control
                stopArm(hand_under_use_l);

                mutex_.lock();

                // go to Idle
                status_ = Status::Idle;

                mutex_.unlock();

                break;
            }

            mutex_.lock();

            // go to WaitHandMove
            status_ = Status::WaitHandMove;

            mutex_.unlock();

            // reset timer
            last_time_ = yarp::os::Time::now();

            break;
        }

        case Status::Stop:
        {
            mutex_.lock();

            // stop control
            stopArm("right");
            stopArm("left");
            hand_action_->stopControl();

            // stop gaze tracking
            gaze_.getGazeInterface().setTrackingMode(false);

            if (enable_torso_)
            {
                if ((previous_status_ == Status::MoveHome) || (previous_status_ == Status::WaitArmHome))
                {
                    yInfo() << "Note: stopping while previous was MoveHome or WaitArmHome.";
                    yInfo() << "Note: hand_home_ = "  + hand_home_;

                    if ((!home_move_context_restored_) && ((hand_home_ == "left") || (hand_home_ == "right")))
                    {
                        yInfo() << "Note: restore context previous to going in MoveHome.";

                        restoreArmContext(hand_home_);

                        home_move_context_restored_ = true;
                    }

                }
            }
            // go back to Idle
            status_ = Status::Idle;

            mutex_.unlock();

            break;
        }
        }

        return true;
    }
};

int main(int argc, char** argv)
{
    yarp::os::Network yarp;
    if (!yarp.checkNetwork())
    {
        yError() << "YARP doesn't seem to be available";

        return EXIT_FAILURE;
    }

    ObjectTrackingManipulation module;
    yarp::os::ResourceFinder rf;
    rf.setVerbose(true);
    rf.setDefaultContext("object-tracking-manipulation");
    rf.setDefaultConfigFile("manipulation_config.ini");
    rf.configure(argc,argv);

    return module.runModule(rf);
}
