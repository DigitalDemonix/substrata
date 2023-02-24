/*=====================================================================
HoverCarPhysics.cpp
-------------------
Copyright Glare Technologies Limited 2023 -
=====================================================================*/
#include "HoverCarPhysics.h"


#include "PhysicsWorld.h"
#include "PhysicsObject.h"
#include "JoltUtils.h"
#include <StringUtils.h>
#include <ConPrint.h>
#include <PlatformUtils.h>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>


HoverCarPhysics::HoverCarPhysics(JPH::BodyID car_body_id_, HoverCarPhysicsSettings settings_)
{
	car_body_id = car_body_id_;
	settings = settings_;
	unflip_up_force_time_remaining = -1;
	cur_seat_index = 0;
}


HoverCarPhysics::~HoverCarPhysics()
{
}


// https://en.wikipedia.org/wiki/Lift_coefficient
static float liftCoeffForCosTheta(float cos_theta)
{
	// Set to 1 if theta < 25
	const float theta = std::acos(cos_theta);
	const float alpha = Maths::pi_2<float>() - theta;

	if(alpha < degreeToRad(25.f))
		return 2;
	else
		return 0;
}


HoverCarPhysicsUpdateEvents HoverCarPhysics::update(PhysicsWorld& physics_world, const PlayerPhysicsInput& physics_input, float dtime)
{
	HoverCarPhysicsUpdateEvents events;

	if(cur_seat_index == 0)
	{
		float forward = 0.0f, right = 0.0f, up = 0.f, brake = 0.0f, hand_brake = 0.0f;
		// Determine acceleration and brake
		if (physics_input.W_down || physics_input.up_down)
			forward = 1.0f;
		else if(physics_input.S_down || physics_input.down_down)
			forward = -1.0f;


		// Hand brake will cancel gas pedal
		if(physics_input.space_down)
		{
			hand_brake = 1.0f;
			up = 1.f;
		}

		if(physics_input.C_down)
			up = -1.f;

		// Steering
		if(physics_input.A_down)
			right = -1.0f;
		else if(physics_input.D_down)
			right = 1.0f;
	

		JPH::BodyInterface& body_interface = physics_world.physics_system->GetBodyInterface();

		// On user input, assure that the car is active
		if (right != 0.0f || forward != 0.0f || brake != 0.0f || hand_brake != 0.0f)
			body_interface.ActivateBody(car_body_id);


		//const JPH::Mat44 transform = body_interface.GetCenterOfMassTransform(car_body_id);
		const JPH::Mat44 transform = body_interface.GetWorldTransform(car_body_id);

		JPH::Float4 cols[4];
		transform.StoreFloat4x4(cols);

		const Matrix4f to_world(&cols[0].x);

		// Vectors in y-forward space
		const Vec4f forwards_y_for(0,1,0,0);
		const Vec4f right_y_for(1,0,0,0);
		const Vec4f up_y_for(0,0,1,0);
	
		// model/object space to y-forward space = R
		// y-forward space to model/object space = R^-1

		// The particular R will depend on the space the modeller chose.

		const JPH::Quat R_quat = toJoltQuat(settings.script_settings.model_to_y_forwards_rot_2 * settings.script_settings.model_to_y_forwards_rot_1);
	
		const Matrix4f R_inv = ((settings.script_settings.model_to_y_forwards_rot_2 * settings.script_settings.model_to_y_forwards_rot_1).conjugate()).toMatrix();


		const Vec4f forwards_os = R_inv * forwards_y_for;
		const Vec4f right_os = R_inv * right_y_for;
		const Vec4f up_os = crossProduct(right_os, forwards_os);

		const Vec4f right_vec_ws = to_world * right_os;
		const Vec4f forward_vec_ws = to_world * forwards_os;
		const Vec4f up_vec_ws = to_world * up_os;

		if(up_vec_ws[2] > 0) // Don't apply 'upwards' force if car is flipped at all, so unflipping can work.
		{
			// Increase upwards force to componsate for car tilt
			const float cos_theta = up_vec_ws[2];
			const float up_force_factor = 1 / myMax(0.7f, cos_theta);

			const Vec4f up_force = up_vec_ws * (1.f + up * 0.6f) * up_force_factor * settings.hovercar_mass * 9.81;
			body_interface.AddForce(car_body_id, toJoltVec3(up_force));
		}

		// If car is nearly upside down, apply a world-space up force, so it can be righted when off the ground.
		// Apply it for some time until the car is sufficiently righted, otherwise the car can oscillate in and out of the 'nearly upside down' orientation.
		if(unflip_up_force_time_remaining > 0) // If currently unflipping car:
		{
			if(up_vec_ws[2] > 0.2) // If car has been sufficiently unflipped:
				unflip_up_force_time_remaining = -1; // Stop unflipping
			else
			{
				// conPrint("car nearly upside down, applying upwards force.");
				const Vec4f up_force = Vec4f(0,0,1,0) * 1.0f * settings.hovercar_mass * 9.81;
				body_interface.AddForce(car_body_id, toJoltVec3(up_force));

				unflip_up_force_time_remaining -= dtime;
			}
		}
		else
		{
			// If car is nearly upside down, start unflipping.
			if(up_vec_ws[2] < -0.9) 
				unflip_up_force_time_remaining = 1;
		}


		const Vec4f forwards_control_force = forward_vec_ws * settings.hovercar_mass * 10 * forward;
		body_interface.AddForce(car_body_id, toJoltVec3(forwards_control_force));

		// Add an extra upwards force to compensate for the upwards component of the forwards_control_force
		const float extra_up_force_mag = -forwards_control_force[2];
		const Vec4f extra_up_force = up_vec_ws * extra_up_force_mag;
		body_interface.AddForce(car_body_id, toJoltVec3(extra_up_force));

	
		const Vec4f forwards_control_torque = right_vec_ws * settings.hovercar_mass * -0.5 * forward;
		body_interface.AddTorque(car_body_id, toJoltVec3(forwards_control_torque));


		const Vec4f yaw_control_torque = up_vec_ws * settings.hovercar_mass * -3 * right;
		body_interface.AddTorque(car_body_id, toJoltVec3(yaw_control_torque));

		const Vec4f roll_control_torque = forward_vec_ws * settings.hovercar_mass * 2.f * right;
		body_interface.AddTorque(car_body_id, toJoltVec3(roll_control_torque));


		// TEMP: Induce roll for testing stabilisation:
		//if(physics_input.C_down)
		//{
		//	const Vec4f roll_torque = to_world * (Vec4f(0,1,0,0) * hovercar_mass * -3);
		//	body_interface.AddTorque(jolt_body->GetID(), toJoltVec3(roll_torque));
		//}

		// Get current rotation, compute the desired rotation, which is a rotation with the current yaw but no pitch or roll, 
		// compute a rotation to get from current to desired
		const JPH::Quat current_rot = body_interface.GetRotation(car_body_id);
	
		const float current_yaw_angle = std::atan2(right_vec_ws[1], right_vec_ws[0]); // = rotation of right vector around the z vector

		const JPH::Quat desired_rot = JPH::Quat::sRotation(JPH::Vec3(0,0,1), current_yaw_angle) * R_quat;

		const JPH::Quat cur_to_desired_rot = desired_rot * current_rot.Conjugated();
		JPH::Vec3 axis;
		float angle;
		cur_to_desired_rot.GetAxisAngle(axis, angle);

		// conPrint("Axis: " + toVec4fVec(axis).toStringMaxNDecimalPlaces(2) + " angle: " + toString(angle));

		// Choose a desired angular velocity which is proportional in magnitude to how far we need to rotate.
		// Note that we can't just apply a constant torque in the desired rotation direction, or we get angular oscillations around the desired rotation.
		const JPH::Vec3 desired_angular_vel = (axis * angle) * 3;

		// Apply a torque to try and match the desired angular velocity.
		const JPH::Vec3 angular_vel = body_interface.GetAngularVelocity(car_body_id);
		const JPH::Vec3 correction_torque = (desired_angular_vel - angular_vel) * settings.hovercar_mass * 1.5f;
		body_interface.AddTorque(car_body_id, correction_torque);
	 
		//--------- Apply aerodynamic drag and lift forces ------

		const Vec4f linear_vel = toVec4fVec(body_interface.GetLinearVelocity(car_body_id));
		const float v_mag = linear_vel.length();
		if(v_mag > 1.0e-3f)
		{
			const Vec4f normed_linear_vel = linear_vel / v_mag;
		
			// Apply drag force
			const float rho = 1.293f; // air density, kg m^-3

			// Compute drag force on front/back of vehicle
			const float forwards_area = 2.f; // TEMP HACK APPROX
			const float projected_forwards_area = absDot(normed_linear_vel, forward_vec_ws) * forwards_area;
			const float forwards_C_d = 0.2f; // drag coefficient  (Tesla model S drag coeffcient is about 0.2 apparently)
			const float forwards_F_d = 0.5f * rho * v_mag*v_mag * forwards_C_d * projected_forwards_area;

			// Compute drag force on side of vehicle
			const float side_area = 4.f; // TEMP HACK APPROX
			const float projected_side_area = absDot(normed_linear_vel, right_vec_ws) * side_area;
			const float side_C_d = 0.5f; // drag coefficient - roughly that of a sphere
			const float side_F_d = 0.5f * rho * v_mag*v_mag * side_C_d * projected_side_area;

			// Compute drag force on top of vehicle
			const float top_area = 8.f; // TEMP HACK APPROX
			const float projected_top_bottom_area = absDot(normed_linear_vel, up_vec_ws) * top_area;
			const float top_C_d = 0.75f; // drag coefficient
			const float top_F_d = 0.5f * rho * v_mag*v_mag * top_C_d * projected_top_bottom_area;

			const float total_F_d = forwards_F_d + side_F_d + top_F_d;
			const Vec4f F_d = normed_linear_vel * -total_F_d;
			body_interface.AddForce(car_body_id, toJoltVec3(F_d));


			// Compute lift force on bottom/top of vehicle.
			// Lift force is by definition orthogonal to the incoming flow direction, which means orthogonal to the linear velocity in our case.

			const Vec4f up_lift_force_dir = removeComponentInDir(up_vec_ws, normed_linear_vel);
			if(up_lift_force_dir.length() > 1.0e-3f)
			{
				// Compute lift from top surface - this is a downwards force
				const float top_dot = dot(normed_linear_vel, up_vec_ws);
				const float top_C_L = liftCoeffForCosTheta(top_dot);
				const float projected_top_area = top_dot * top_area;
				if(top_C_L > 0)
				{
					if(projected_top_area > 0)
					{
						const float top_L = 0.5f * rho * v_mag*v_mag * projected_top_area * top_C_L;
						body_interface.AddForce(car_body_id, toJoltVec3(-normalise(up_lift_force_dir) * top_L));
					}
				}

				// Compute lift from bottom surface - this is an upwards force
				const float projected_bottom_area = -projected_top_area;
				const float bottom_C_L = liftCoeffForCosTheta(-top_dot);
				if(bottom_C_L > 0)
				{
					if(projected_bottom_area > 0)
					{
						const float bottom_L = 0.5f * rho * v_mag*v_mag * projected_bottom_area * bottom_C_L;
						body_interface.AddForce(car_body_id, toJoltVec3(normalise(up_lift_force_dir) * bottom_L));
					}
				}
			}

			const Vec4f right_lift_force_dir = removeComponentInDir(right_vec_ws, normed_linear_vel);
			if(right_lift_force_dir.length() > 1.0e-3f)
			{
				// Compute lift from right surface - this is a leftwards force
				const float right_dot = dot(normed_linear_vel, right_vec_ws);
				const float right_C_L = liftCoeffForCosTheta(right_dot);
				const float projected_right_area = dot(normed_linear_vel, right_vec_ws) * side_area;
				if(projected_right_area > 0)
				{
					const float right_L = 0.5f * rho * v_mag*v_mag * projected_right_area * right_C_L;
					body_interface.AddForce(car_body_id, toJoltVec3(-normalise(right_lift_force_dir) * right_L));
				}

				// Compute lift from left surface - this is a rightwards force
				const float projected_left_area = -projected_right_area;
				const float left_C_L = liftCoeffForCosTheta(-right_dot);
				if(projected_left_area > 0)
				{
					const float left_L = 0.5f * rho * v_mag*v_mag * projected_left_area * left_C_L;
					body_interface.AddForce(car_body_id, toJoltVec3(normalise(right_lift_force_dir) * left_L));
				}
			}
		}
	} // end if seat == 0

	// const float speed_km_h = v_mag * (3600.0f / 1000.f);
	// conPrint("speed (km/h): " + doubleToStringNDecimalPlaces(speed_km_h, 1));

	return events;
}


Vec4f HoverCarPhysics::getFirstPersonCamPos(PhysicsWorld& physics_world) const
{
	const Matrix4f seat_to_world = getSeatToWorldTransform(physics_world);
	return seat_to_world * Vec4f(0,0,0.7f,1); // Raise camera position to appox head position
}


Matrix4f HoverCarPhysics::getBodyTransform(PhysicsWorld& physics_world) const
{
	JPH::BodyInterface& body_interface = physics_world.physics_system->GetBodyInterface();
	const JPH::Mat44 transform = body_interface.GetWorldTransform(car_body_id);

	JPH::Float4 cols[4];
	transform.StoreFloat4x4(cols);

	return Matrix4f(&cols[0].x);
}


// Sitting position is (0,0,0) in seat space, forwards is (0,1,0), right is (1,0,0)

// Seat to world = object to world * seat to object
// model/object space to y-forward space = R
// y-forward space to model/object space = R^-1
// 
// seat_to_object = seat_translation_model_space * R^1
//
// So  
// Seat_to_world = object_to_world * seat_translation_model_space * R^1
Matrix4f HoverCarPhysics::getSeatToWorldTransform(PhysicsWorld& physics_world) const
{ 
	const Matrix4f R_inv = ((settings.script_settings.model_to_y_forwards_rot_2 * settings.script_settings.model_to_y_forwards_rot_1).conjugate()).toMatrix();

	// Seat to world = object to world * seat to object
	return getBodyTransform(physics_world) * Matrix4f::translationMatrix(settings.script_settings.seat_settings[this->cur_seat_index].seat_position) * R_inv;
}


Vec4f HoverCarPhysics::getLinearVel(PhysicsWorld& physics_world) const
{
	JPH::BodyInterface& body_interface = physics_world.physics_system->GetBodyInterface();
	return toVec4fVec(body_interface.GetLinearVelocity(car_body_id));
}
