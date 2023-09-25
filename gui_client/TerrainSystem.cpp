/*=====================================================================
TerrainSystem.cpp
-----------------
Copyright Glare Technologies Limited 2023 -
=====================================================================*/
#include "TerrainSystem.h"


#include "OpenGLEngine.h"
#include "graphics/PerlinNoise.h"
#include "PhysicsWorld.h"
#include "../shared/ImageDecoding.h"
#include <utils/TaskManager.h>
#include <utils/ContainerUtils.h>
#include <utils/RuntimeCheck.h>
#include "graphics/Voronoi.h"
#include "graphics/PNGDecoder.h"
#include "graphics/EXRDecoder.h"
#include "graphics/jpegdecoder.h"
#include "opengl/GLMeshBuilding.h"
#include "opengl/MeshPrimitiveBuilding.h"
#include "meshoptimizer/src/meshoptimizer.h"
#include "../dll/include/IndigoMesh.h"



TerrainSystem::TerrainSystem()
{
}


TerrainSystem::~TerrainSystem()
{
}


// Pack normal into GL_INT_2_10_10_10_REV format.
inline static uint32 packNormal(const Vec3f& normal)
{
	int x = (int)(normal.x * 511.f);
	int y = (int)(normal.y * 511.f);
	int z = (int)(normal.z * 511.f);
	// ANDing with 1023 isolates the bottom 10 bits.
	return (x & 1023) | ((y & 1023) << 10) | ((z & 1023) << 20);
}


/*
  Consider terrain chunk below, currently at depth 2 in the tree.


      depth 1
----------------------                                 ^
                                                       | morph_end_dist
              depth 2 -> depth 1 transition region     |
                                                       |
  -  -  -  -  -  - -  -                                |  ^
       ______                                          |  |
      |      |    depth 2                              |  |
      |      |                                         |  | morph_start_dist  
      |______|                                         |  |      
         ^                                             |  |    
         |                                             |  |     
         | dist from camera to nearest point in chunk  |  |    
         |                                             |  |  
         v                                             v  v
         *                                                
         Camera                                                

As the nearest point approaches the depth 1 / depth 2 boundary, we want to continuously morph to the lower-detail representation that it will have at depth 1

So we will provide the following information to the vertex shader: the (2d) AABB of the chunk, to compute dist from camera to nearest point in chunk,
as well as the distance from the camera to the transition region (morph_start_dist), for the current depth (depth 2 in this example) and the depth to the depth 1 / depth 2 boundary (morph_end_dist)







screen space angle 

alpha ~= chunk_w / d

where d = ||campos - chunk_centre||

quad_res = 512 / (2 ^ chunk_lod_lvl)

quad_w = 2 ^ chunk_lod_lvl

quad_w_screenspace ~= quad_w / d

= 2 ^ chunk_lod_lvl / d

say we have some target quad_w_screenspace: quad_w_screenspace_target

quad_w_screenspace_target = 2 ^ chunk_lod_lvl / d

2 ^ chunk_lod_lvl = quad_w_screenspace_target * d

chunk_lod_lvl = log_2(quad_w_screenspace_target * d)

also

d = (2 ^ chunk_lod_lvl) / quad_w_screenspace_target


Say d = 1000, quad_w_screenspace_target = 0.001

then chunk_lod_lvl = log_2(0.001 * 1000) = log_2(1) = 0

Say d = 4000, quad_w_screenspace_target = 0.001

then chunk_lod_lvl = log_2(0.001 * 4000) = log_2(4) = 2


----------------------------
chunk_w = world_w / 2^depth

quad_w = chunk_w / res = world_w / (2^depth * res)

quad_w_screenspace ~= quad_w / d = world_w / (2^depth * res * d)

2^depth * res * d * quad_w_screenspace = world_w

2^depth = world_w / (res * d * quad_w_screenspace)

depth = log2(world_w / (res * d * quad_w_screenspace))

----

max depth quad_w = world_w / (chunk_res * 2^max_depth)
= 131072 / (128 * 2^10) = 131072 / (128 * 1024) = 1


*/

static const bool GEOMORPHING_SUPPORT = false;

//static float world_w = 131072;//8192*4;
static float world_w = 32768;//8192*4;
// static float CHUNK_W = 512.f;
static int chunk_res = 127; // quad res per patch
const float quad_w_screenspace_target = 0.004f;
//const float quad_w_screenspace_target = 0.004f;
static const int max_depth = 14;

//static float world_w = 4096;
//// static float CHUNK_W = 512.f;
//static int chunk_res = 128; // quad res per patch
//const float quad_w_screenspace_target = 0.1f;
////const float quad_w_screenspace_target = 0.004f;
//static const int max_depth = 2;

const float scale_factor = 1.f / (8 * 1024);


static const float water_z = 10.f;

static Colour3f depth_colours[] = 
{
	Colour3f(1,0,0),
	Colour3f(0,1,0),
	Colour3f(0,0,1),
	Colour3f(1,1,0),
	Colour3f(0,1,1),
	Colour3f(1,0,1),
	Colour3f(0.2f,0.5f,1),
	Colour3f(1,0.5f,0.2f),
	Colour3f(0.5,1,0.5f),
};


void TerrainSystem::init(OpenGLEngine* opengl_engine_, PhysicsWorld* physics_world_, const Vec3d& campos, glare::TaskManager* task_manager_, ThreadSafeQueue<Reference<ThreadMessage> >* out_msg_queue_)
{
	//heightmap = PNGDecoder::decode("D:\\terrain\\height.png");
//	heightmap = EXRDecoder::decode("C:\\programming\\terraingen\\vs2022_build\\heightfield_with_deposited_sed.exr");
//	heightmap = ImageDecoding::decodeImage(".", "D:\\terrain\\Height Map_8192x8192.exr");
	//heightmap = ImageDecoding::decodeImage(".", "D:\\terrain\\Height Map_1024x1024.exr");

	//detail_heightmap = JPEGDecoder::decode(".", "C:\\Users\\nick\\Downloads\\cgaxis_dirt_with_large_rocks_38_46_4K\\dirt_with_large_rocks_38_46_height.jpg");
	//detail_heightmap = PNGDecoder::decode("D:\\terrain\\GroundPack2\\SAND-08\\tex\\SAND-08-BEACH_DEPTH_2k.png");
	//detail_heightmap = PNGDecoder::decode("D:\\terrain\\GroundPack2\\SAND-11\\tex\\SAND-11-DUNES_DEPTH_2k.png");
	small_dune_heightmap = PNGDecoder::decode("C:\\Users\\nick\\Downloads\\sand_ground_59_83_height.png");

	opengl_engine = opengl_engine_;
	physics_world = physics_world_;
	task_manager = task_manager_;
	out_msg_queue = out_msg_queue_;

	next_id = 0;

	root_node = new TerrainNode();
	root_node->parent = NULL;
	root_node->aabb = js::AABBox(Vec4f(-world_w/2, -world_w/2, -1, 1), Vec4f(world_w/2, world_w/2, 1, 1));
	root_node->depth = 0;
	root_node->id = next_id++;
	id_to_node_map[root_node->id] = root_node.ptr();
	

	Timer timer;


	// Make material
	terrain_mat.roughness = 1;
	terrain_mat.geomorphing = true;
	//terrain_mat.albedo_texture = opengl_engine->getTexture("D:\\terrain\\colour.png", /*allow_compression=*/false);
	//terrain_mat.albedo_texture = opengl_engine->getTexture("D:\\terrain\\Colormap_0.png", /*allow_compression=*/false);
	//terrain_mat.albedo_texture = opengl_engine->getTexture("N:\\indigo\\trunk\\testscenes\\ColorChecker_sRGB_from_Ref.png", /*allow_compression=*/true);
	terrain_mat.albedo_texture = opengl_engine->getTexture("C:\\programming\\terraingen\\vs2022_build\\colour.png", /*allow_compression=*/true);
	//terrain_mat.tex_matrix = Matrix2f(1.f/64, 0, 0, -1.f/64);
	terrain_mat.tex_matrix = Matrix2f(scale_factor, 0, 0, scale_factor);
	//terrain_mat.tex_translation = Vec2f(0.5f, 0.5f);
	terrain_mat.terrain = true;

	{
	//	ProgramKey key("water", ProgramKeyArgs());
	//
	//	OpenGLProgramRef water_prog = opengl_engine->buildProgram("water", key);
	//
	//	water_fbm_tex_location			= water_prog->getUniformLocation("fbm_tex");
	//	water_cirrus_tex_location		= water_prog->getUniformLocation("cirrus_tex");
	//	water_sundir_cs_location		= water_prog->getUniformLocation("sundir_cs");
	//
	//	water_prog->uses_colour_and_depth_buf_textures = true;
	//	
	//	//water_mat.uses_custom_shader = true;
	//	water_mat.auto_assign_shader = false;
	//
	//	water_mat.shader_prog = water_prog;

		water_mat.water = true;
	}

	//conPrint("Terrain init took " + timer.elapsedString() + " (" + toString(num_chunks) + " chunks)");


	//TEMP: create single large water object
	
#if 0
	const float large_water_quad_w = 20000;
	Reference<OpenGLMeshRenderData> quad_meshdata = MeshPrimitiveBuilding::makeQuadMesh(*opengl_engine->vert_buf_allocator, Vec4f(1,0,0,0), Vec4f(0,1,0,0), /*res=*/2);
	for(int y=0; y<16; ++y)
	for(int x=0; x<16; ++x)
	{
		const int offset_x = x - 8;
		const int offset_y = y - 8;
		if(!(offset_x == 0 && offset_y == 0))
		{
			// Tessellate ground mesh, to avoid texture shimmer due to large quads.
			GLObjectRef gl_ob = new GLObject();
			gl_ob->ob_to_world_matrix = Matrix4f::translationMatrix(0, 0, water_z) * Matrix4f::uniformScaleMatrix(large_water_quad_w) * Matrix4f::translationMatrix(-0.5f + offset_x, -0.5f + offset_y, 0);
			gl_ob->mesh_data = quad_meshdata;

			gl_ob->materials.resize(1);
			gl_ob->materials[0].albedo_linear_rgb = Colour3f(1,0,0);
			gl_ob->materials[0] = water_mat;
			opengl_engine->addObject(gl_ob);
		}
	}

	{
		// Tessellate ground mesh, to avoid texture shimmer due to large quads.
		GLObjectRef gl_ob = new GLObject();
		gl_ob->ob_to_world_matrix = Matrix4f::translationMatrix(0, 0, water_z) * Matrix4f::uniformScaleMatrix(large_water_quad_w) * Matrix4f::translationMatrix(-0.5f, -0.5f, 0);
		gl_ob->mesh_data = MeshPrimitiveBuilding::makeQuadMesh(*opengl_engine->vert_buf_allocator, Vec4f(1,0,0,0), Vec4f(0,1,0,0), /*res=*/64);

		gl_ob->materials.resize(1);
		//gl_ob->materials[0].albedo_linear_rgb = Colour3f(0,0,1);
		gl_ob->materials[0] = water_mat;
		opengl_engine->addObject(gl_ob);
	}



	// Create cylinder for water boundary
	{
		GLObjectRef gl_ob = new GLObject();
		const float wall_h = 1000.0f;
		gl_ob->ob_to_world_matrix = Matrix4f::translationMatrix(0, 0, -wall_h) * Matrix4f::scaleMatrix(25000, 25000, wall_h);
		gl_ob->mesh_data = MeshPrimitiveBuilding::makeCylinderMesh(*opengl_engine->vert_buf_allocator.ptr(), /*end_caps=*/false);

		gl_ob->materials.resize(1);
		gl_ob->materials[0].albedo_linear_rgb = Colour3f(1,0,0);
		//gl_ob->materials[0] = water_mat;

		opengl_engine->addObject(gl_ob);
	}
#endif
}


// Remove any opengl and physics objects inserted into the opengl and physics engines, in the subtree with given root node.
void TerrainSystem::removeAllNodeDataForSubtree(TerrainNode* node)
{
	if(node->gl_ob.nonNull())
		opengl_engine->removeObject(node->gl_ob);
	if(node->physics_ob.nonNull())
		physics_world->removeObject(node->physics_ob);

	for(size_t i=0; i<node->old_subtree_gl_obs.size(); ++i)
		opengl_engine->removeObject(node->old_subtree_gl_obs[i]);

	for(size_t i=0; i<node->old_subtree_phys_obs.size(); ++i)
		physics_world->removeObject(node->old_subtree_phys_obs[i]);

	for(int i=0; i<4; ++i)
		if(node->children[i].nonNull())
			removeAllNodeDataForSubtree(node->children[i].ptr());
}


void TerrainSystem::shutdown()
{
	if(root_node.nonNull())
		removeAllNodeDataForSubtree(root_node.ptr());
	root_node = NULL;

	id_to_node_map.clear();
}


static void appendSubtreeString(TerrainNode* node, std::string& s)
{
	for(int i=0; i<node->depth; ++i)
		s.push_back(' ');

	s += "node, id " + toString(node->id) + " building: " + boolToString(node->building) + ", subtree_built: " + boolToString(node->subtree_built) + "\n";

	if(node->children[0].nonNull())
	{
		for(int i=0; i<4; ++i)
			appendSubtreeString(node->children[i].ptr(), s);
	}
}


struct TerrainSysDiagnosticsInfo
{
	int num_interior_nodes;
	int num_leaf_nodes;
	int max_depth;
};

static void processSubtreeDiagnostics(TerrainNode* node, TerrainSysDiagnosticsInfo& info)
{
	info.max_depth = myMax(info.max_depth, node->depth);

	if(node->children[0].nonNull())
	{
		info.num_interior_nodes++;

		for(int i=0; i<4; ++i)
			processSubtreeDiagnostics(node->children[i].ptr(), info);
	}
	else
	{
		info.num_leaf_nodes++;
	}
}


std::string TerrainSystem::getDiagnostics() const
{
	/*std::string s;
	if(root_node.nonNull())
		appendSubtreeString(root_node.ptr(), s);
	return s;*/

	TerrainSysDiagnosticsInfo info;
	info.num_interior_nodes = 0;
	info.num_leaf_nodes = 0;
	info.max_depth = 0;

	if(root_node.nonNull())
		processSubtreeDiagnostics(root_node.ptr(), info);

	return 
		"num interior nodes: " + toString(info.num_interior_nodes) + "\n" +
		"num leaf nodes: " + toString(info.num_leaf_nodes) + "\n" +
		"max depth: " + toString(info.max_depth) + "\n";
}


struct VoronoiBasisNoise01
{
	inline static float eval(const Vec4f& p)
	{
		Vec4f closest_p;
		float dist;
		Voronoi::evaluate3d(p, 1.0f, closest_p, dist);
		return dist;
	}
};


inline static Vec2f toVec2f(const Vec4f& v)
{
	return Vec2f(v[0], v[1]);
}


float TerrainSystem::evalTerrainHeight(float p_x, float p_y, float quad_w, bool water) const
{
	if(water)
		return -4;
	else
	{
		
#if 1
		const float nx = p_x * scale_factor;
		const float ny = p_y * scale_factor;
		
		const float MIN_TERRAIN_Z = -50.f; // Have a max under-sea depth.  This allows having a flat sea-floor, which in turn allows a lower-res mesh to be used for seafloor chunks.

		const float eps = 1 / 1000.f; // Avoid edge pixels where wrapping occurs
		float terrain_h;
		if(nx < eps || nx > (1 - eps) || ny < eps || ny > (1 - eps))
			terrain_h = MIN_TERRAIN_Z;
		else
		{
			const float noise_xy_scale = 1 / 200.f;
			const Vec4f p = Vec4f(p_x * noise_xy_scale, p_y * noise_xy_scale, 0, 1);
			//const float fbm_val = PerlinNoise::ridgedMultifractal<float>(p, /*H=*/1, /*lacunarity=*/2, /*num octaves=*/10, /*offset=*/0.1f) * 0.2f;
			const float fbm_val = PerlinNoise::multifractal<float>(p, /*H=*/1, /*lacunarity=*/2, /*num octaves=*/10, /*offset=*/0.1f) * 5.5f;

			terrain_h = 0;//p_x; // myMax(MIN_TERRAIN_Z, heightmap->sampleSingleChannelTiledHighQual(nx, 1.f - ny, /*channel=*/0)) /*+ fbm_val*/;// + detail_h;

			const float dune_envelope = Maths::smoothStep(water_z + 0.4f, water_z + 1.5f, terrain_h);
			const float dune_xy_scale = 1 / 2.f;
			const float dune_h = small_dune_heightmap->sampleSingleChannelTiled(p_x * dune_xy_scale, p_y * dune_xy_scale, 0) * dune_envelope * 0.1f;

			terrain_h += dune_h;
		}
		return terrain_h;

#elif 0
	//	return p_x * 0.01f;
	//	const float xy_scale = 1 / 5.f;


		//const float num_octaves = 10;//-std::log2(quad_w * xy_scale);
	//	const Vec4f p = Vec4f(p_x * xy_scale, p_y * xy_scale, 0, 1);
		//const float fbm_val = PerlinNoise::multifractal<float>(p, /*H=*/1, /*lacunarity=*/2, /*num octaves=*/num_octaves, /*offset=*/0.1f);
		//const float fbm_val = PerlinNoise::noise(p) + PerlinNoise::noise(p * 8.0);
	//	const float fbm_val = PerlinNoise::FBM(p, 4);

		const float nx = p_x * scale_factor + 0.5f;
		const float ny = p_y * scale_factor + 0.5f;

		float terrain_h;
		if(nx < 0 || nx > 1 || ny < 0 || ny > 1)
			terrain_h = -300;
		else
			terrain_h = (heightmap->sampleSingleChannelTiledHighQual(nx, ny, 0) - 0.57f) * 800;// + fbm_val * 0.5;
		
		return terrain_h;

		//const float detail_xy_scale = 1 / 3.f;
		//return detail_heightmap->sampleSingleChannelTiled(p_x * detail_xy_scale, p_y * detail_xy_scale, 0) * 0.2f;
#else


	//	return PerlinNoise::noise(Vec4f(p_x / 30.f, p_y / 30.f, 0, 0)) * 10.f;
		//return p_x * 0.1f;//PerlinNoise::noise(Vec4f(p_x / 30.f, p_y / 30.f, 0, 0)) * 10.f;
		//return 0.f; // TEMP 
		float seaside_factor = Maths::smoothStep(-1000.f, -300.f, p_y);

		const float dist_from_origin = Vec2f(p_x, p_y).length();
		const float centre_flatten_factor = Maths::smoothStep(700.f, 1000.f, dist_from_origin); // Start the hills only x metres from origin
		
		// FBM feature size is (1 / xy_scale) * 2 ^ -num_octaves     = 1 / (xyscale * 2^num_octaves)
		// For example if xy_scale = 1/3000 and num_octaves = 10, we have feature size = 3000 / 1024 ~= 3.
		// We want the feature size to be = quad_w, e.g.
		// quad_w = (1 / xy_scale) * 2 ^ -num_octaves
		// so
		// quad_w * xy_scale = 2 ^ -num_octaves
		// log2(quad_w * xy_scale) = -num_octaves
		// num_octaves = - log2(quad_w * xy_scale)
		const float xy_scale = 1 / 3000.f;

		const float num_octaves = 10;//-std::log2(quad_w * xy_scale);

		const Vec4f p = Vec4f(p_x * xy_scale, p_y * xy_scale, 0, 1);
		//const float fbm_val = PerlinNoise::multifractal<float>(p, /*H=*/1, /*lacunarity=*/2, /*num octaves=*/num_octaves, /*offset=*/0.1f);
		const float fbm_val = PerlinNoise::FBM(p, /*num octaves=*/(int)num_octaves);

	//	float fbm_val = 0;
	//	static float octave_params[] = 
	//	{
	//		1.0f, 0.5f,
	//		2.0f, 0.25f,
	//		4.0f, 0.125f,
	//		128.0f, 0.04f,
	//		256.0f, 0.02f
	//	};
	//
	//	for(int i=0; i<staticArrayNumElems(octave_params)/2; ++i)
	//	{
	//		const float scale  = octave_params[i*2 + 0];
	//		const float weight = octave_params[i*2 + 1];
	//		fbm_val += PerlinNoise::noise(p * scale) * weight;
	//	}

		//fbm_val += (1 - std::fabs(PerlinNoise::noise(p * 100.f))) * 0.025f; // Ridged noise
		//fbm_val += VoronoiBasisNoise01::eval(p * 0.1f) * 0.1f;//0.025f; // Ridged noise
		//fbm_val += Voronoi::voronoiFBM(toVec2f(p * 10000.f), 2) * 0.01f;

		return -300 + seaside_factor * 300 + centre_flatten_factor * myMax(0.f, fbm_val - 0.2f) * 600;
#endif
	}
}


void TerrainSystem::makeTerrainChunkMesh(float chunk_x, float chunk_y, float chunk_w, bool build_physics_ob, bool water, TerrainChunkData& chunk_data_out) const
{
	/*
	 
	An example mesh with interior_vert_res=4, giving vert_res_with_borders=6
	There will be a 1 quad wide skirt border at the edge of the mesh.
	y
	^ 
	|
	-------------------------
	|   /|   /|   /|   /|   /|
	|  / |  / |  / |  / |  / | skirt
	| /  | /  | /  | /  | /  |
	|------------------------|
	|   /|   /|   /|   /|   /|
	|  / |  / |  / |  / |  / |
	| /  | /  | /  | /  | /  |
	|----|----|----|----|----|
	|   /|   /|   /|   /|   /|
	|  / |  / |  / |  / |  / |
	| /  | /  | /  | /  | /  |
	|----|----|----|----|----|
	|   /|   /|   /|   /|   /|
	|  / |  / |  / |  / |  / |
	| /  | /  | /  | /  | /  |
	|----|----|----|----|----|
	|   /|   /|   /|   /|   /|
	|  / |  / |  / |  / |  / |skirt
	| /  | /  | /  | /  | /  |
	|----|----|----|----|----|----> x
	skirt               skirt
	*/

	// Do a quick pass over the data, to see if the heightfield is completely flat here (e.g. is a flat chunk of sea-floor or ground plane).
	bool completely_flat = true;
	{
		const int CHECK_RES = 32;
		const float quad_w = chunk_w / (CHECK_RES - 1);
		const float z_0 = evalTerrainHeight(chunk_x, chunk_y, quad_w, water);
		for(int y=0; y<CHECK_RES; ++y)
		for(int x=0; x<CHECK_RES; ++x)
		{
			const float p_x = x * quad_w + chunk_x;
			const float p_y = y * quad_w + chunk_y;
			const float z    = evalTerrainHeight(p_x,      p_y,      quad_w, water);
			if(z != z_0)
			{
				completely_flat = false;
				goto done;
			}
		}
	}
done:

	const int interior_vert_res = completely_flat ? 8 : 128; // Number of vertices along the side of a chunk, excluding the 2 border vertices.  Use a power of 2 for Jolt.
	const int interior_quad_res = interior_vert_res - 1;
	const int vert_res_with_borders = interior_vert_res + 2;
	const int quad_res_with_borders = vert_res_with_borders - 1;

	//int lod_level_factor = 1 << lod_level;
	//int vert_res = myMax(4, MAX_RES / lod_level_factor); // Number of verts in x and y directions
	//int quad_res = vert_res - 1; // Number of quads in x and y directions
//	int quad_res_no_border = chunk_res; // myMax(1, MAX_RES / lod_level_factor); // Number of quads in x and y directions
//	int vert_res_no_border = quad_res_no_border + 1;
//	float quad_w = chunk_w / quad_res_no_border; // (CHUNK_W / MAX_RES) * lod_level_factor; // Width in metres of each quad
	float quad_w = chunk_w / interior_quad_res;
	//float chunk_x = (float)chunk_x_i * CHUNK_W;
	//float chunk_y = (float)chunk_y_i * CHUNK_W;

	//conPrint("quad_res: " + toString(quad_res));
	//conPrint("vert_res: " + toString(vert_res));
	//const int res = vert_res; // Num verts along each side
	
	//heightfield_out.resize(vert_res, vert_res);

	// inSampleCount / mBlockSize must be a power of 2 and minimally 2.
	// (padded_vert_res = inSampleCount)
	//const int blocksize = 2; // Jolt blocksize
	const int jolt_vert_res = interior_vert_res;//myMax(4, (int)Maths::roundToNextHighestPowerOf2(vert_res)); // Jolt has certain requirements for the heightfield data
//	//const int padded_vert_res = myMax(4, Maths::roundUpToMultipleOfPowerOf2(vert_res, 4)); // Jolt has certain requirements for the heightfield data
//	//Array2D<float> padded_heightfield(padded_vert_res, padded_vert_res);
	Array2D<float> jolt_heightfield;
	if(build_physics_ob)
	{
		jolt_heightfield.resizeNoCopy(jolt_vert_res, jolt_vert_res);
		//padded_heightfield.resize(jolt_vert_res, jolt_vert_res);
		//padded_heightfield.setAllElems(FLT_MAX); // TEMP inefficient
	}

	const size_t normal_size_B = 4;
	size_t vert_size_B = sizeof(float) * (3 + 2) + normal_size_B; // position, uv, normal
	if(GEOMORPHING_SUPPORT)
		vert_size_B += sizeof(float) + normal_size_B; // morph-z, morph-normal

	chunk_data_out.mesh_data = new OpenGLMeshRenderData();
	chunk_data_out.mesh_data->vert_data.resize(vert_size_B * vert_res_with_borders * vert_res_with_borders);
	

	

	//chunk_data_out.mesh_data->vert_index_buffer.resize(quad_res_with_borders * quad_res_with_borders * 6);
	chunk_data_out.mesh_data->vert_index_buffer_uint16.resize(quad_res_with_borders * quad_res_with_borders * 6);

	OpenGLMeshRenderData& meshdata = *chunk_data_out.mesh_data;

	//meshdata.setIndexType(GL_UNSIGNED_INT);
	meshdata.setIndexType(GL_UNSIGNED_SHORT);

	meshdata.has_uvs = true;
	meshdata.has_shading_normals = true;
	meshdata.batches.resize(1);
	meshdata.batches[0].material_index = 0;
	meshdata.batches[0].num_indices = (uint32)meshdata.vert_index_buffer_uint16.size();
	meshdata.batches[0].prim_start_offset = 0;

	meshdata.num_materials_referenced = 1;

	// NOTE: The order of these attributes should be the same as in OpenGLProgram constructor with the glBindAttribLocations.
	size_t in_vert_offset_B = 0;
	VertexAttrib pos_attrib;
	pos_attrib.enabled = true;
	pos_attrib.num_comps = 3;
	pos_attrib.type = GL_FLOAT;
	pos_attrib.normalised = false;
	pos_attrib.stride = (uint32)vert_size_B;
	pos_attrib.offset = (uint32)in_vert_offset_B;
	meshdata.vertex_spec.attributes.push_back(pos_attrib);
	in_vert_offset_B += sizeof(float) * 3;

	VertexAttrib normal_attrib;
	normal_attrib.enabled = true;
	normal_attrib.num_comps = 4; // 3;
	normal_attrib.type = GL_INT_2_10_10_10_REV; // GL_FLOAT;
	normal_attrib.normalised = true; // false;
	normal_attrib.stride = (uint32)vert_size_B;
	normal_attrib.offset = (uint32)in_vert_offset_B;
	meshdata.vertex_spec.attributes.push_back(normal_attrib);
	in_vert_offset_B += normal_size_B;

	const size_t uv_offset_B = in_vert_offset_B;
	VertexAttrib uv_attrib;
	uv_attrib.enabled = true;
	uv_attrib.num_comps = 2;
	uv_attrib.type = GL_FLOAT;
	uv_attrib.normalised = false;
	uv_attrib.stride = (uint32)vert_size_B;
	uv_attrib.offset = (uint32)uv_offset_B;
	meshdata.vertex_spec.attributes.push_back(uv_attrib);
	in_vert_offset_B += sizeof(float) * 2;

	size_t morph_offset_B, morph_normal_offset_B;
	if(GEOMORPHING_SUPPORT)
	{
		morph_offset_B = in_vert_offset_B;
		VertexAttrib morph_attrib;
		morph_attrib.enabled = true;
		morph_attrib.num_comps = 1;
		morph_attrib.type = GL_FLOAT;
		morph_attrib.normalised = false;
		morph_attrib.stride = (uint32)vert_size_B;
		morph_attrib.offset = (uint32)in_vert_offset_B;
		meshdata.vertex_spec.attributes.push_back(morph_attrib);
		in_vert_offset_B += sizeof(float);

		morph_normal_offset_B = in_vert_offset_B;
		VertexAttrib morph_normal_attrib;
		morph_normal_attrib.enabled = true;
		morph_normal_attrib.num_comps = 4; // 3;
		morph_normal_attrib.type = GL_INT_2_10_10_10_REV; // GL_FLOAT;
		morph_normal_attrib.normalised = true; // false;
		morph_normal_attrib.stride = (uint32)vert_size_B;
		morph_normal_attrib.offset = (uint32)in_vert_offset_B;
		meshdata.vertex_spec.attributes.push_back(morph_normal_attrib);
		in_vert_offset_B += normal_size_B;
	}

	assert(in_vert_offset_B == vert_size_B);

	js::AABBox aabb_os = js::AABBox::emptyAABBox();

	uint8* const vert_data = chunk_data_out.mesh_data->vert_data.data();

	Timer timer;

	Array2D<float> raw_heightfield(interior_vert_res, interior_vert_res);
	Array2D<Vec3f> raw_normals(interior_vert_res, interior_vert_res);
	for(int y=0; y<interior_vert_res; ++y)
	for(int x=0; x<interior_vert_res; ++x)
	{
		const float p_x = x * quad_w + chunk_x;
		const float p_y = y * quad_w + chunk_y;
		const float dx = 0.1f;
		const float dy = 0.1f;

		const float z    = evalTerrainHeight(p_x,      p_y,      quad_w, water); // z = h(p_x, p_y)
		const float z_dx = evalTerrainHeight(p_x + dx, p_y,      quad_w, water); // z_dx = h(p_x + dx, dy)
		const float z_dy = evalTerrainHeight(p_x,      p_y + dy, quad_w, water); // z_dy = h(p_x, p_y + dy)

		const Vec3f p_dx_minus_p(dx, 0, z_dx - z); // p(p_x + dx, dy) - p(p_x, p_y) = (p_x + dx, d_y, z_dx) - (p_x, p_y, z) = (d_x, 0, z_dx - z)
		const Vec3f p_dy_minus_p(0, dy, z_dy - z);

		const Vec3f normal = normalise(crossProduct(p_dx_minus_p, p_dy_minus_p));

		raw_heightfield.elem(x, y) = z;
		raw_normals.elem(x, y) = normal;
	}

	//conPrint("eval terrain height took     " + timer.elapsedStringMSWIthNSigFigs(4));
	timer.reset();

	for(int y=0; y<vert_res_with_borders; ++y)
	for(int x=0; x<vert_res_with_borders; ++x)
	{
		float p_x;
		int src_x;
		const float skirt_height = quad_w * 0.25f; // The skirt height needs to be large enough to cover any cracks, but smaller is better to avoid wasted fragment drawing.
		float z_offset = 0;
		if(x == 0) // If edge vert:
		{
			p_x = chunk_x;
			src_x = 0;
			z_offset = skirt_height;
		}
		else if(x == vert_res_with_borders-1) // If edge vert:
		{
			p_x = (interior_vert_res-1) * quad_w + chunk_x;
			src_x = interior_vert_res-1;
			z_offset = skirt_height;
		}
		else
		{
			p_x = (x-1) * quad_w + chunk_x;
			src_x = x-1;
		}

		float p_y;
		int src_y;
		if(y == 0) // If edge vert:
		{
			p_y = chunk_y;
			src_y = 0;
			z_offset = skirt_height;
		}
		else if(y == vert_res_with_borders-1) // If edge vert:
		{
			p_y = (interior_vert_res-1) * quad_w + chunk_y;
			src_y = interior_vert_res-1;
			z_offset = skirt_height;
		}
		else
		{
			p_y = (y-1) * quad_w + chunk_y;
			src_y = y-1;
		}

		//const float p_x = x * quad_w + chunk_x;
		//const float p_y = y * quad_w + chunk_y;
		//const float dx = 0.1f;
		//const float dy = 0.1f;

		const float z = raw_heightfield.elem(src_x, src_y) - z_offset; // evalTerrainHeight(p_x, p_y, water); // z = h(p_x, p_y)

		//const float z_dx = evalTerrainHeight(p_x + dx, p_y, water); // z_dx = h(p_x + dx, dy)
		//const float z_dy = evalTerrainHeight(p_x, p_y + dy, water); // z_dy = h(p_x, p_y + dy)
		//
		//const Vec3f p_dx_minus_p(dx, 0, z_dx - z); // p(p_x + dx, dy) - p(p_x, p_y) = (p_x + dx, d_y, z_dx) - (p_x, p_y, z) = (d_x, 0, z_dx - z)
		//const Vec3f p_dy_minus_p(0, dy, z_dy - z);
		//
		//const Vec3f normal = normalise(crossProduct(p_dx_minus_p, p_dy_minus_p));
		//conPrint(normal.toStringMaxNDecimalPlaces(4));
		const Vec3f normal = raw_normals.elem(src_x, src_y);

		if(build_physics_ob)
		{
			const int int_x = x - 1; // Don't include border/skirt vertices in Jolt heightfield.
			const int int_y = y - 1;
			if((int_x >= 0) && (int_x < jolt_vert_res) && (int_y >= 0) && (int_y < jolt_vert_res))
				jolt_heightfield.elem(int_x, jolt_vert_res - 1 - int_y) = z;
		}

		const Vec3f pos(p_x, p_y, z);
		std::memcpy(vert_data + vert_size_B * (y * vert_res_with_borders + x), &pos, sizeof(float)*3);

		aabb_os.enlargeToHoldPoint(pos.toVec4fPoint());

		const uint32 packed_normal = packNormal(normal);
		std::memcpy(vert_data + vert_size_B * (y * vert_res_with_borders + x) + sizeof(float) * 3, &packed_normal, sizeof(uint32));

		const Vec2f uv(p_x, p_y);
		std::memcpy(vert_data + vert_size_B * (y * vert_res_with_borders + x) + uv_offset_B, &uv, sizeof(float)*2);

		// Morph z-displacement:
		// Starred vertices, without the morph displacement, should have the position that the lower LOD level triangle would have, below.
		/*
		 y
		 ^ 
		 |    *         *        (*)
		 |----|----|----|----|----|
		 | \  | \  | \  | \  | \  |
		 |  \ |  \ |  \ |  \ |  \ |
		 |   \|   \|   \|   \|   \|
		*|----|----*----|----*----|
		 | \  | \  | \  | \  | \  |
		 |  \ |  \ |  \ |  \ |  \ |
		 |   \|   \|   \|   \|   \|
		 |---------|----|----|---> x             
		      *         *        (*)
	
		y
		 ^ 
		 |    *         *
		 |---------|---------|
		 | \       | \       |
		 |  \      |  \      |
		 |   \     |   \     |
		*|    \    |    \    |*
		 |     \   |     \   |
		 |      \  |      \  |
		 |       \ |       \ |
		 |---------|---------|---> x
		      *         *
		
			  
		*/

		if(GEOMORPHING_SUPPORT)
		{
			float morphed_z = z;
			Vec3f morphed_normal = normal;
			//if((y % 2) == 0)
			//{
			//	if(((x % 2) == 1) && (x + 1 < raw_heightfield.getWidth()))
			//	{
			//		assert(x >= 1 && x + 1 < raw_heightfield.getWidth());
			//		morphed_z      = 0.5f   * (raw_heightfield.elem(x-1, y) + raw_heightfield.elem(x+1, y));
			//		morphed_normal = normalise(raw_normals    .elem(x-1, y) + raw_normals    .elem(x+1, y));
			//	}
			//}
			//else
			//{
			//	if(((x % 2) == 0) && (y + 1 < raw_heightfield.getHeight()))
			//	{
			//		assert(y >= 1 && y + 1 < raw_heightfield.getHeight());
			//		morphed_z      = 0.5f   * (raw_heightfield.elem(x, y-1) + raw_heightfield.elem(x, y+1));
			//		morphed_normal = normalise(raw_normals    .elem(x, y-1) + raw_normals    .elem(x, y+1));
			//	}
			//}
			std::memcpy(vert_data + vert_size_B * (y * vert_res_with_borders + x) + morph_offset_B,        &morphed_z,           sizeof(float));

			const uint32 packed_morph_normal = packNormal(morphed_normal);
			std::memcpy(vert_data + vert_size_B * (y * vert_res_with_borders + x) + morph_normal_offset_B, &packed_morph_normal, sizeof(uint32));
		}
	}

	meshdata.aabb_os = aabb_os;

	
	//uint32* const indices = chunk_data_out.mesh_data->vert_index_buffer.data();
	uint16* const indices = chunk_data_out.mesh_data->vert_index_buffer_uint16.data();
	//std::vector<uint32> indices(chunk_data_out.mesh_data->vert_index_buffer_uint16.size());
	for(int y=0; y<quad_res_with_borders; ++y)
	for(int x=0; x<quad_res_with_borders; ++x)
	{
		// Trianglulate the quad in this way to match how Jolt triangulates the height field shape.
		// 
		// 
		// |----|
		// | \  |
		// |  \ |
		// |   \|
		// |----|--> x

		// bot left tri
		const int offset = (y*quad_res_with_borders + x) * 6;
		indices[offset + 0] = (uint16)(y       * vert_res_with_borders + x    ); // bot left
		indices[offset + 1] = (uint16)(y       * vert_res_with_borders + x + 1); // bot right
		indices[offset + 2] = (uint16)((y + 1) * vert_res_with_borders + x    ); // top left
		
		// top right tri
		indices[offset + 3] = (uint16)(y       * vert_res_with_borders + x + 1); // bot right
		indices[offset + 4] = (uint16)((y + 1) * vert_res_with_borders + x + 1); // top right
		indices[offset + 5] = (uint16)((y + 1) * vert_res_with_borders + x    ); // top left
	}

	//const size_t num_verts = vert_res_with_borders * vert_res_with_borders;

	Timer timer2;

//	std::vector<uint32> simplified_indices(indices.size());
//	float result_error = 0;
//	const size_t res_num_indices = meshopt_simplify(/*destination index buffer=*/simplified_indices.data(), indices.data(), indices.size(), 
//		(const float*)meshdata.vert_data.data(), num_verts, /*vertex positions stride=*/vert_size_B,
//		/*target_index_count=*/6, /*target error=*/0.001f, &result_error);
//	//printVar(result_error);
//	//conPrint("meshopt_simplify took " + timer2.elapsedStringNSigFigs(4));
//	timer2.reset();
//
//	simplified_indices.resize(res_num_indices);
//
//	indices = simplified_indices;

//	runtimeCheck(indices.size() % 3 == 0);
//	for(size_t i=0; i<indices.size(); ++i)
//		runtimeCheck(indices[i] < num_verts);


//	std::vector<uint32> reordered_indices(indices.size());
//	meshopt_optimizeVertexCache(reordered_indices.data(), indices.data(), indices.size(), num_verts);
//	//reordered_indices = indices;
//
//	js::Vector<uint8, 16> reordered_vert_data(meshdata.vert_data.size());
//	meshopt_optimizeVertexFetch(/*destination=*/reordered_vert_data.data(), /*indices=*/reordered_indices.data(), /*index count=*/reordered_indices.size(), 
//		/*vertices=*/meshdata.vert_data.data(), /*vertex count=*/vert_res_with_borders * vert_res_with_borders, /*vertex size=*/vert_size_B);

//	//conPrint("vertex optimisation took " + timer2.elapsedStringNSigFigs(4));
//
//	meshdata.vert_data = reordered_vert_data;
//
//	// Copy back to chunk_data_out.mesh_data->vert_index_buffer_uint16
//	for(size_t i=0; i<reordered_indices.size(); ++i)
//		chunk_data_out.mesh_data->vert_index_buffer_uint16[i] = (uint16)reordered_indices[i];

	//for(size_t i=0; i<indices.size(); ++i)
	//	chunk_data_out.mesh_data->vert_index_buffer_uint16[i] = (uint16)indices[i];

	// Update batch num indices
	//meshdata.batches[0].num_indices = (uint32)indices.size();

	//conPrint("Creating mesh took           " + timer.elapsedStringMSWIthNSigFigs(4));
	timer.reset();

	if(build_physics_ob)
		chunk_data_out.physics_shape = PhysicsWorld::createJoltHeightFieldShape(jolt_vert_res, jolt_heightfield, quad_w);

	//conPrint("Creating physics shape took  " + timer.elapsedStringMSWIthNSigFigs(4));
	//conPrint("---------------");
}


void TerrainSystem::removeLeafGeometry(TerrainNode* node)
{
	if(node->gl_ob.nonNull()) opengl_engine->removeObject(node->gl_ob);
	node->gl_ob = NULL;

	if(node->physics_ob.nonNull()) physics_world->removeObject(node->physics_ob);
	node->physics_ob = NULL;
}


void TerrainSystem::removeSubtree(TerrainNode* node, std::vector<GLObjectRef>& old_subtree_gl_obs_in_out, std::vector<PhysicsObjectRef>& old_subtree_phys_obs_in_out)
{
	ContainerUtils::append(old_subtree_gl_obs_in_out, node->old_subtree_gl_obs);
	ContainerUtils::append(old_subtree_phys_obs_in_out, node->old_subtree_phys_obs);

	if(node->children[0].isNull()) // If this is a leaf node:
	{
		// Remove mesh for leaf node, if any
		//removeLeafGeometry(node);
		if(node->gl_ob.nonNull())
			old_subtree_gl_obs_in_out.push_back(node->gl_ob);
		if(node->physics_ob.nonNull())
			old_subtree_phys_obs_in_out.push_back(node->physics_ob);
	}
	else // Else if this node is an interior node:
	{
		// Remove children
		for(int i=0; i<4; ++i)
		{
			removeSubtree(node->children[i].ptr(), old_subtree_gl_obs_in_out, old_subtree_phys_obs_in_out);
			id_to_node_map.erase(node->children[i]->id);

			if(node->children[i]->vis_aabb_gl_ob.nonNull())
				opengl_engine->removeObject(node->children[i]->vis_aabb_gl_ob);

			node->children[i] = NULL;
		}
	}
}


// The root node of the subtree, 'node', has already been created.
void TerrainSystem::createInteriorNodeSubtree(TerrainNode* node, const Vec3d& campos)
{
	// We should split this node into 4 children, and make it an interior node.
	const float cur_w = node->aabb.max_[0] - node->aabb.min_[0];
	const float child_w = cur_w * 0.5f;

	// bot left child
	node->children[0] = new TerrainNode();
	node->children[0]->parent = node;
	node->children[0]->depth = node->depth + 1;
	node->children[0]->aabb = js::AABBox(node->aabb.min_, node->aabb.max_ - Vec4f(child_w, child_w, 0, 0));

	// bot right child
	node->children[1] = new TerrainNode();
	node->children[1]->parent = node;
	node->children[1]->depth = node->depth + 1;
	node->children[1]->aabb = js::AABBox(node->aabb.min_ + Vec4f(child_w, 0, 0, 0), node->aabb.max_ - Vec4f(0, child_w, 0, 0));

	// top right child
	node->children[2] = new TerrainNode();
	node->children[2]->parent = node;
	node->children[2]->depth = node->depth + 1;
	node->children[2]->aabb = js::AABBox(node->aabb.min_ + Vec4f(child_w, child_w, 0, 0), node->aabb.max_);

	// top left child
	node->children[3] = new TerrainNode();
	node->children[3]->parent = node;
	node->children[3]->depth = node->depth + 1;
	node->children[3]->aabb = js::AABBox(node->aabb.min_ + Vec4f(0, child_w, 0, 0), node->aabb.max_ - Vec4f(child_w, 0, 0, 0));

	// Add an AABB visualisation for debugging
	if(false)
	{
		const Colour3f col = depth_colours[(node->depth + 1) % staticArrayNumElems(depth_colours)];
		for(int i=0; i<4; ++i)
		{
			float padding = 0.01f;
			Vec4f padding_v(padding,padding,padding,0);
			node->children[i]->vis_aabb_gl_ob = opengl_engine->makeAABBObject(node->children[i]->aabb.min_ - padding_v, node->children[i]->aabb.max_ + padding_v, Colour4f(col[0], col[1], col[2], 0.2f));
			opengl_engine->addObject(node->children[i]->vis_aabb_gl_ob);
		}
	}


	// Assign child nodes ids and add to id_to_node_map.
	for(int i=0; i<4; ++i)
	{
		node->children[i]->id = next_id++;
		id_to_node_map[node->children[i]->id] = node->children[i].ptr();
	}

	node->subtree_built = false;

	// Recurse to build child trees
	for(int i=0; i<4; ++i)
		createSubtree(node->children[i].ptr(), campos);
}


static const float USE_MIN_DIST_TO_AABB = 5.f;

// The root node of the subtree, 'node', has already been created.
void TerrainSystem::createSubtree(TerrainNode* node, const Vec3d& campos)
{
	//conPrint("Creating subtree, depth " + toString(node->depth) + ", at " + node->aabb.toStringMaxNDecimalPlaces(4));

	const float min_dist = myMax(USE_MIN_DIST_TO_AABB, node->aabb.distanceToPoint(campos.toVec4fPoint()));

	//const int desired_lod_level = myClamp((int)std::log2(quad_w_screenspace_target * min_dist), /*lowerbound=*/0, /*upperbound=*/8);
	// depth = log2(world_w / (res * d * quad_w_screenspace))
	const int desired_depth = myClamp((int)std::log2(world_w / (chunk_res * min_dist * quad_w_screenspace_target)), /*lowerbound=*/0, /*upperbound=*/max_depth);

	//assert(desired_lod_level <= node->lod_level);
	//assert(desired_depth >= node->depth);

	if(desired_depth > node->depth)
	{
		createInteriorNodeSubtree(node, campos);
	}
	else
	{
		assert(desired_depth <= node->depth);
		// This node should be a leaf node

		// Create geometry for it
		MakeTerrainChunkTask* task = new MakeTerrainChunkTask();
		task->node_id = node->id;
		task->chunk_x = node->aabb.min_[0];
		task->chunk_y = node->aabb.min_[1];
		task->chunk_w = node->aabb.max_[0] - node->aabb.min_[0];
		task->build_physics_ob = min_dist < 2000.f;
		//task->build_physics_ob = (max_depth - node->depth) < 3;
		task->terrain = this;
		task->out_msg_queue = out_msg_queue;
		task_manager->addTask(task);

		node->building = true;
		node->subtree_built = false;
	}
}


void TerrainSystem::updateSubtree(TerrainNode* cur, const Vec3d& campos)
{
	// We want each leaf node to have lod_level = desired_lod_level for that node

	// Get distance from camera to node

	const float min_dist = myMax(USE_MIN_DIST_TO_AABB, cur->aabb.distanceToPoint(campos.toVec4fPoint()));
	//printVar(min_dist);

	//const int desired_lod_level = myClamp((int)std::log2(quad_w_screenspace_target * min_dist), /*lowerbound=*/0, /*upperbound=*/8);
	const int desired_depth = myClamp((int)std::log2(world_w / (chunk_res * min_dist * quad_w_screenspace_target)), /*lowerbound=*/0, /*upperbound=*/max_depth);

	if(cur->children[0].isNull()) // If 'cur' is a leaf node (has no children, so is not interior node):
	{
		if(desired_depth > cur->depth) // If the desired lod level is greater than the leaf's lod level, we want to split the leaf into 4 child nodes
		{
			// Remove mesh for leaf node, if any
			//removeLeafGeometry(cur);
			// Don't remove leaf geometry yet, wait until subtree geometry is fully built to replace it.
			//cur->num_children_built = 0;
			if(cur->gl_ob.nonNull()) cur->old_subtree_gl_obs.push_back(cur->gl_ob);
			cur->gl_ob = NULL;
			if(cur->physics_ob.nonNull()) cur->old_subtree_phys_obs.push_back(cur->physics_ob);
			cur->physics_ob = NULL;
			
			createSubtree(cur, campos);
		}
	}
	else // Else if 'cur' is an interior node:
	{
		if(desired_depth <= cur->depth) // And it should be a leaf node, or not exist (it is currently too detailed)
		{
			// Change it into a leaf node:

			// Remove children of cur and their subtrees
			for(int i=0; i<4; ++i)
			{
				removeSubtree(cur->children[i].ptr(), cur->old_subtree_gl_obs, cur->old_subtree_phys_obs);
				id_to_node_map.erase(cur->children[i]->id);

				if(cur->children[i]->vis_aabb_gl_ob.nonNull())
					opengl_engine->removeObject(cur->children[i]->vis_aabb_gl_ob);

				cur->children[i] = NULL;
			}
		
			// Start creating geometry for this node:
			// Note that we may already be building geometry for this node, from a previous change from interior node to leaf node.
			// In this case don't make a new task, just wait for existing task.

			assert(cur->gl_ob.isNull());
			if(!cur->building)
			{
				// No chunk at this location, make one
				MakeTerrainChunkTask* task = new MakeTerrainChunkTask();
				task->node_id = cur->id;
				task->chunk_x = cur->aabb.min_[0];
				task->chunk_y = cur->aabb.min_[1];
				task->chunk_w = cur->aabb.max_[0] - cur->aabb.min_[0];
				task->build_physics_ob = min_dist < 2000.f;
				//task->build_physics_ob = (max_depth - cur->depth) < 3;
				task->terrain = this;
				task->out_msg_queue = out_msg_queue;
				task_manager->addTask(task);

				//conPrint("Making new node chunk");

				cur->subtree_built = false;
				cur->building = true;
			}
		}
		else // Else if 'cur' should still be an interior node:
		{
			assert(cur->children[0].nonNull());
			for(int i=0; i<4; ++i)
				updateSubtree(cur->children[i].ptr(), campos);
		}
	}
}


void TerrainSystem::updateCampos(const Vec3d& campos)
{
	updateSubtree(root_node.ptr(), campos);
}


// The subtree with root node 'node' is fully built, so we can remove any old meshes for it, and insert the new pending meshes.
void TerrainSystem::insertPendingMeshesForSubtree(TerrainNode* node)
{
	// Remove any old subtree GL obs and physics obs, now the mesh for this node is ready.
	for(size_t i=0; i<node->old_subtree_gl_obs.size(); ++i)
		opengl_engine->removeObject(node->old_subtree_gl_obs[i]);
	node->old_subtree_gl_obs.clear();

	for(size_t i=0; i<node->old_subtree_phys_obs.size(); ++i)
		physics_world->removeObject(node->old_subtree_phys_obs[i]);
	node->old_subtree_phys_obs.clear();


	if(node->children[0].isNull()) // If leaf node:
	{
		if(node->pending_gl_ob.nonNull())
		{
//FAILING			assert(node->gl_ob.isNull());
			node->gl_ob = node->pending_gl_ob;
			opengl_engine->addObject(node->gl_ob);
			node->pending_gl_ob = NULL;
		}

		if(node->pending_physics_ob.nonNull())
		{
			//			assert(node->physics_ob.isNull());
			node->physics_ob = node->pending_physics_ob;
			physics_world->addObject(node->physics_ob);
			node->pending_physics_ob = NULL;
		}
	}
	else
	{
		for(int i=0; i<4; ++i)
			insertPendingMeshesForSubtree(node->children[i].ptr());
	}
}


/*

When node a is subdivided into 4 (or more) nodes;
set a counter, num_children_built on node a to zero.
whenever node b, c, d, e is built, walk up tree to parent (a), and increment num_children_built.
When it reaches 4, this means that all children are built.  In that case remove the gl ob from node a, and add all gl obs in the subtrees of node a.

                                  a
 a          =>                    |_______________
                                  |    |    |     |
                                  b    c    d     e

                                  a
 a          =>                    |_______________
                                  |    |    |     |
                                  b    c    d     e
                                       |____________
                                       |     |     |
                                       f     g     h


A subtree with root node n is complete if all leaf nodes in the subtree are built.


When an interior node is changed into a leaf node (e.g. children are removed), remove children but add a list of their gl objects to their parent (old_subtree_gl_obs).

a
|_______________             =>                a
|    |    |     |
b    c    d     e

e.g. when b, c, d, e are removed, add list of their gl objects to node a.   When node a is built, remove gl objects in old_subtree_gl_obs from world, and add node 'a' gl object.

*/


bool TerrainSystem::areAllParentSubtreesBuilt(TerrainNode* node)
{
	TerrainNode* cur = node->parent;
	while(cur)
	{
		if(!cur->subtree_built)
			return false;
		cur = cur->parent;
	}

	return true;
}



void TerrainSystem::handleCompletedMakeChunkTask(const TerrainChunkGeneratedMsg& msg)
{
	// Lookup node based on id
	auto res = id_to_node_map.find(msg.node_id);
	if(res != id_to_node_map.end())
	{
		TerrainNode& node = *res->second;

		node.building = false;
		if(node.children[0].nonNull()) // If this is an interior node:
			return; // Discard the obsolete built mesh.  This will happen if a leaf node gets converted to an interior node while the mesh is building.

		// This node is a leaf node, and we have the mesh for it, therefore the subtree is complete.
		node.subtree_built = true;

		
		Reference<OpenGLMeshRenderData> mesh_data = msg.chunk_data.mesh_data;

		// Update node AABB, now that we have actual heighfield data.
		node.aabb = mesh_data->aabb_os;

		{
			if(!mesh_data->vert_index_buffer.empty())
				mesh_data->indices_vbo_handle = opengl_engine->vert_buf_allocator->allocateIndexData(mesh_data->vert_index_buffer.data(), mesh_data->vert_index_buffer.dataSizeBytes());
			else
				mesh_data->indices_vbo_handle = opengl_engine->vert_buf_allocator->allocateIndexData(mesh_data->vert_index_buffer_uint16.data(), mesh_data->vert_index_buffer_uint16.dataSizeBytes());

			mesh_data->vbo_handle = opengl_engine->vert_buf_allocator->allocate(mesh_data->vertex_spec, mesh_data->vert_data.data(), mesh_data->vert_data.dataSizeBytes());

#if DO_INDIVIDUAL_VAO_ALLOC
			mesh_data->individual_vao = new VAO(mesh_data->vbo_handle.vbo, mesh_data->indices_vbo_handle.index_vbo, mesh_data->vertex_spec);
#endif
			// Now data has been uploaded to GPU, clear CPU mem
			mesh_data->vert_data.clearAndFreeMem();
			mesh_data->vert_index_buffer.clearAndFreeMem();
			mesh_data->vert_index_buffer_uint16.clearAndFreeMem();
			mesh_data->vert_index_buffer_uint8.clearAndFreeMem();
		}

		GLObjectRef gl_ob = new GLObject();
		gl_ob->ob_to_world_matrix = Matrix4f::identity();
		gl_ob->mesh_data = mesh_data;

		// d = (2 ^ chunk_lod_lvl) / quad_w_screenspace_target
		//const float lod_transition_dist = (1 << max_lod_level) / quad_w_screenspace_target;
		/*chunk.gl_ob->morph_start_dist = lod_transition_dist;
		chunk.gl_ob->morph_end_dist = lod_transition_dist * 1.02f;*/
		//gl_ob->aabb_min_x = node.aabb.min_[0];
		//gl_ob->aabb_min_y = node.aabb.min_[1];
		//gl_ob->aabb_w = node.aabb.max_[0] - node.aabb.min_[0];

		// Compute distance at which this node will transition to a smaller depth value (node.depth - 1).
		//
		// From above:
		// depth = log2(world_w / (res * d * quad_w_screenspace))
		// 2^depth = world_w / (res * d * quad_w_screenspace);
		// res * d * quad_w_screenspace * 2^depth = world_w
		// d = world_w / (res * quad_w_screenspace * 2^depth)
		const float transition_depth = world_w / (chunk_res * quad_w_screenspace_target * (1 << node.depth));
		//printVar(transition_depth);

		gl_ob->morph_start_dist = transition_depth * 0.75f;
		gl_ob->morph_end_dist   = transition_depth;

		//printVar(gl_ob->morph_start_dist);
		//printVar(gl_ob->morph_end_dist);


		gl_ob->materials.resize(1);
		gl_ob->materials[0] = terrain_mat;
		//assert(node.depth >= 0 && node.depth < staticArrayNumElems(depth_colours));
		//gl_ob->materials[0].albedo_linear_rgb = depth_colours[node.depth % staticArrayNumElems(depth_colours)];



		PhysicsShape shape = msg.chunk_data.physics_shape;

		PhysicsObjectRef physics_ob = new PhysicsObject(/*collidable=*/true);
		physics_ob->shape = shape;
		physics_ob->pos = Vec4f(node.aabb.min_[0], node.aabb.min_[1], 0, 1); // chunk.pos;
		physics_ob->rot = Quatf::fromAxisAndAngle(Vec3f(1,0,0), Maths::pi_2<float>());
		physics_ob->scale = Vec3f(1.f);

		physics_ob->kinematic = false;
		physics_ob->dynamic = false;

		node.pending_gl_ob = gl_ob;
		node.pending_physics_ob = physics_ob;


		if(areAllParentSubtreesBuilt(&node))
		{
			insertPendingMeshesForSubtree(&node);
		}
		else
		{
			TerrainNode* cur = node.parent;
			while(cur)
			{
				bool cur_subtree_built = true;
				for(int i=0; i<4; ++i)
					if(!cur->children[i]->subtree_built)
					{
						cur_subtree_built = false;
						break;
					}

				if(!cur->subtree_built && cur_subtree_built)
				{
					// If cur subtree was not built before, and now it is:

					if(areAllParentSubtreesBuilt(cur))
					{
						insertPendingMeshesForSubtree(cur);
					}

					cur->subtree_built = true;
				}

				if(!cur_subtree_built)
					break;

				cur = cur->parent;
			}
		}
	}
}


void MakeTerrainChunkTask::run(size_t thread_index)
{
	try
	{
		// Make terrain
		terrain->makeTerrainChunkMesh(chunk_x, chunk_y, chunk_w, build_physics_ob, /*water=*/false, /*chunk data out=*/chunk_data);

		// Send message to out-message-queue (e.g. to MainWindow), saying that we have finished the work.
		TerrainChunkGeneratedMsg* msg = new TerrainChunkGeneratedMsg();
		msg->chunk_x = chunk_x;
		msg->chunk_y = chunk_y;
		msg->chunk_w = chunk_w;
		//msg->lod_level = lod_level;
		msg->chunk_data = chunk_data;
		msg->node_id = node_id;
		out_msg_queue->enqueue(msg);

		// Make water
		//TerrainSystem::makeTerrainChunkMesh(chunk_x_i, chunk_y_i, lod_level, /*water=*/true, chunk_data);
	}
	catch(glare::Exception& e)
	{
		conPrint(e.what());
	}
}
