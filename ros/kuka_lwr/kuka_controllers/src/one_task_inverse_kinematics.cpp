#include "one_task_inverse_kinematics.h"
#include <pluginlib/class_list_macros.h>
#include <kdl_parser/kdl_parser.hpp>
#include <math.h>
#include <Eigen/LU>
#include "misc/pseudo_inversion.h"

namespace kuka_controllers 
{
	OneTaskInverseKinematics::OneTaskInverseKinematics() {}
	OneTaskInverseKinematics::~OneTaskInverseKinematics() {}

	bool OneTaskInverseKinematics::init(hardware_interface::EffortJointInterface *robot, ros::NodeHandle &n)
	{
		nh_ = n;

		// get URDF and name of root and tip from the parameter server
		std::string robot_description, root_name, tip_name;

		if (!ros::param::search(n.getNamespace(),"robot_description", robot_description))
		{
		    ROS_ERROR_STREAM("OneTaskInverseKinematics: No robot description (URDF) found on parameter server ("<<n.getNamespace()<<"/robot_description)");
		    return false;
		}
		if (!nh_.getParam("root_name", root_name))
		{
		    ROS_ERROR_STREAM("OneTaskInverseKinematics: No root name found on parameter server ("<<n.getNamespace()<<"/root_name)");
		    return false;
		}
		if (!nh_.getParam("tip_name", tip_name))
		{
		    ROS_ERROR_STREAM("OneTaskInverseKinematics: No tip name found on parameter server ("<<n.getNamespace()<<"/tip_name)");
		    return false;
		}
	 
		// Get the gravity vector (direction and magnitude)
		KDL::Vector gravity_ = KDL::Vector::Zero();
		gravity_(2) = 9.81;

		// Construct an URDF model from the xml string
		std::string xml_string;

		if (n.hasParam(robot_description))
			n.getParam(robot_description.c_str(), xml_string);
		else
		{
		    ROS_ERROR("Parameter %s not set, shutting down node...", robot_description.c_str());
		    n.shutdown();
		    return false;
		}

		if (xml_string.size() == 0)
		{
			ROS_ERROR("Unable to load robot model from parameter %s",robot_description.c_str());
		    n.shutdown();
		    return false;
		}

		ROS_DEBUG("%s content\n%s", robot_description.c_str(), xml_string.c_str());
		
		// Get urdf model out of robot_description
		urdf::Model model;
		if (!model.initString(xml_string))
		{
		    ROS_ERROR("Failed to parse urdf file");
		    n.shutdown();
		    return false;
		}
		ROS_INFO("Successfully parsed urdf file");
		
		KDL::Tree kdl_tree_;
		if (!kdl_parser::treeFromUrdfModel(model, kdl_tree_))
		{
		    ROS_ERROR("Failed to construct kdl tree");
		    n.shutdown();
		    return false;
		}


		// Populate the KDL chain
		if(!kdl_tree_.getChain(root_name, tip_name, kdl_chain_))
		{
		    ROS_ERROR_STREAM("Failed to get KDL chain from tree: ");
		    ROS_ERROR_STREAM("  "<<root_name<<" --> "<<tip_name);
		    ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfJoints()<<" joints");
		    ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfSegments()<<" segments");
		    ROS_ERROR_STREAM("  The segments are:");

		    KDL::SegmentMap segment_map = kdl_tree_.getSegments();
		    KDL::SegmentMap::iterator it;

		    for( it=segment_map.begin(); it != segment_map.end(); it++ )
		      ROS_ERROR_STREAM( "    "<<(*it).first);

		  	return false;
		}


		ROS_DEBUG("Number of segments: %d", kdl_chain_.getNrOfSegments());
		ROS_DEBUG("Number of joints in chain: %d", kdl_chain_.getNrOfJoints());



		// Get joint handles for all of the joints in the chain
		for(std::vector<KDL::Segment>::const_iterator it = kdl_chain_.segments.begin()+1; it != kdl_chain_.segments.end(); ++it)
		{
		    joint_handles_.push_back(robot->getHandle(it->getJoint().getName()));
		    ROS_DEBUG("%s", it->getJoint().getName().c_str() );
		}

		ROS_DEBUG(" Number of joints in handle = %lu", joint_handles_.size() );

		jnt_to_jac_solver_.reset(new KDL::ChainJntToJacSolver(kdl_chain_));
		id_solver_.reset(new KDL::ChainDynParam(kdl_chain_,gravity_));
		fk_pos_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));
		ik_vel_solver_.reset(new KDL::ChainIkSolverVel_pinv(kdl_chain_));
		ik_pos_solver_.reset(new KDL::ChainIkSolverPos_NR(kdl_chain_,*fk_pos_solver_,*ik_vel_solver_));

		joint_msr_states_.resize(kdl_chain_.getNrOfJoints());
		joint_des_states_.resize(kdl_chain_.getNrOfJoints());
		tau_cmd_.resize(kdl_chain_.getNrOfJoints());
		J_.resize(kdl_chain_.getNrOfJoints());
		PIDs_.resize(kdl_chain_.getNrOfJoints());

		sub_command_ = nh_.subscribe("command_configuration", 1, &OneTaskInverseKinematics::command_configuration, this);
		sub_gains_ = nh_.subscribe("set_gains", 1, &OneTaskInverseKinematics::set_gains, this);

		return true;
	}

	void OneTaskInverseKinematics::starting(const ros::Time& time)
	{
		// get joint positions
  		for(int i=0; i < joint_handles_.size(); i++) 
  		{
    		joint_msr_states_.q(i) = joint_handles_[i].getPosition();
    		joint_msr_states_.qdot(i) = joint_handles_[i].getVelocity();
    		joint_des_states_.q(i) = joint_msr_states_.q(i);
    	}

    	for (int i = 0; i < PIDs_.size(); i++)
    		PIDs_[i].initPid(100,1,0,0.0,0.0);

    	//I_ = Eigen::Matrix<double,7,7>::Identity(7,7);
    	//P_ = Eigen::Matrix<double,7,7>::Zero();

    	// computing forward kinematics
    	fk_pos_solver_->JntToCart(joint_msr_states_.q,x_);

    	//Desired posture 
    	x_des_ = x_;
	}

	void OneTaskInverseKinematics::update(const ros::Time& time, const ros::Duration& period)
	{

		// get joint positions
  		for(int i=0; i < joint_handles_.size(); i++) 
  		{
    		joint_msr_states_.q(i) = joint_handles_[i].getPosition();
    		joint_msr_states_.qdot(i) = joint_handles_[i].getVelocity();
    	}

    	// computing Jacobian
    	jnt_to_jac_solver_->JntToJac(joint_msr_states_.q,J_);

    	// computing J_pinv_
    	pseudo_inverse(J_,J_pinv_);

    	// computing forward kinematics
    	fk_pos_solver_->JntToCart(joint_msr_states_.q,x_);

    	// end-effector displacement 
    	x_err_.vel = x_des_.p - x_.p;

    	for (int i = 0; i < 3; i++)
    		x_err_.rot(i) = 0.0;	// for now, doesn't count orientation error

    	// computing q_dot
    	for (int i = 0; i < J_pinv_.rows(); i++)
    	{
    		joint_des_states_.qdot(i) = 0.0;
    		for (int k = 0; k < J_pinv_.cols(); k++)
    			joint_des_states_.qdot(i) += J_pinv_(i,k)*(x_err_(k));
    	}

    	// integrating q_dot -> getting q (Euler method)
    	for (int i = 0; i < joint_handles_.size(); i++)
    		joint_des_states_.q(i) += period.toSec()*joint_des_states_.qdot(i);

    	// set controls for joints
    	for (int i = 0; i < joint_handles_.size(); i++)
    	{
    		tau_cmd_(i) = PIDs_[i].computeCommand(joint_des_states_.q(i) - joint_msr_states_.q(i),period);
		   	joint_handles_[i].setCommand(tau_cmd_(i));
    	}
	}

	void OneTaskInverseKinematics::command_configuration(const geometry_msgs::PoseStampedConstPtr &msg)
	{
		// TODO: read orientation message. (now reads only position)
		KDL::Frame frame_des_(
			KDL::Vector(msg->pose.position.x,
						msg->pose.position.y,
						msg->pose.position.z));

		x_des_ = frame_des_;
	}

	void OneTaskInverseKinematics::set_gains(const std_msgs::Float64MultiArray::ConstPtr &msg)
	{
		if(msg->data.size() == 2*PIDs_.size())
		{
			for(int i = 0; i < PIDs_.size(); i++)
				PIDs_[i].setGains(msg->data[i],msg->data[i + PIDs_.size()],0.0,0.0,0.0);
		}
		else
			ROS_INFO("Number of PIDs gains must be = %lu", PIDs_.size());
	}
}

PLUGINLIB_EXPORT_CLASS(kuka_controllers::OneTaskInverseKinematics, controller_interface::ControllerBase)