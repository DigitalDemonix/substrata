/*=====================================================================
PhysicsObject.h
---------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#pragma once


#include <maths/Vec4f.h>
#include <maths/Quat.h>
#include <maths/vec3.h>
#include <maths/Matrix4f.h>
#include <utils/Vector.h>
#include <utils/ThreadSafeRefCounted.h>
#include <utils/Reference.h>
#include <physics/jscol_aabbox.h>
#if USE_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#endif

class RayTraceResult;


/*=====================================================================
PhysicsShape
------------
Acceleration structure for a mesh.
RayMesh in old code, Jolt shape with Jolt code.
=====================================================================*/
class PhysicsShape
{
public:
	js::AABBox getAABBOS() const;

	JPH::Ref<JPH::Shape> jolt_shape;
};


/*=====================================================================
PhysicsObject
-------------

=====================================================================*/
class PhysicsObject : public ThreadSafeRefCounted
{
public:
	GLARE_ALIGNED_16_NEW_DELETE

	friend class PhysicsWorld;

	PhysicsObject(bool collidable);
	PhysicsObject(bool collidable, const PhysicsShape& shape, void* userdata, int userdata_type);
	~PhysicsObject();


	void traceRay(const Ray& ray, RayTraceResult& results_out) const;

	const js::AABBox getAABBoxWS() const;

	const Matrix4f getObToWorldMatrix() const;
	const Matrix4f getWorldToObMatrix() const;

	size_t getTotalMemUsage() const;

public:
	PhysicsShape shape;

	bool collidable; // Is this object solid, for the purposes of player physics?

	void* userdata;
	int userdata_type;

	Vec4f pos;
	Quatf rot; // Set in PhysicsWorld::think() from Jolt data
	Vec3f scale;

#if USE_JOLT
	JPH::BodyID jolt_body_id;
	bool is_sphere;
	bool is_cube;
	bool is_player;
#endif
	bool dynamic;

private:
	
};


typedef Reference<PhysicsObject> PhysicsObjectRef;


struct PhysicsObjectHash
{
	size_t operator() (const PhysicsObjectRef& ob) const
	{
		return (size_t)ob.getPointer() >> 3; // Assuming 8-byte aligned, get rid of lower zero bits.
	}
};
