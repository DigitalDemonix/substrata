
in vec3 normal_cs;
in vec3 normal_ws;
in vec3 pos_cs;
#if GENERATE_PLANAR_UVS
in vec3 pos_os;
#endif
in vec3 pos_ws;
in vec2 texture_coords;
in vec3 cam_to_pos_ws;
flat in ivec4 light_indices_0;
flat in ivec4 light_indices_1;

uniform sampler2D specular_env_tex;
uniform sampler2D fbm_tex;


//----------------------------------------------------------------------------------------------------------------------------
#if USE_MULTIDRAW_ELEMENTS_INDIRECT

flat in int material_index;


layout(std430) buffer PhongUniforms
{
	MaterialData material_data[];
};

#define MAT_UNIFORM					material_data[material_index]

#define DIFFUSE_TEX					MAT_UNIFORM.diffuse_tex
#define EMISSION_TEX				MAT_UNIFORM.emission_tex

//----------------------------------------------------------------------------------------------------------------------------
#else // else if !USE_MULTIDRAW_ELEMENTS_INDIRECT:


layout (std140) uniform PhongUniforms
{
	MaterialData matdata;

} mat_data;

#define MAT_UNIFORM mat_data.matdata


#if !USE_BINDLESS_TEXTURES
uniform sampler2D diffuse_tex;
uniform sampler2D emission_tex;
#endif


#if USE_BINDLESS_TEXTURES
#define DIFFUSE_TEX  MAT_UNIFORM.diffuse_tex
#define EMISSION_TEX MAT_UNIFORM.emission_tex
#else
#define DIFFUSE_TEX  diffuse_tex
#define EMISSION_TEX emission_tex
#endif

#endif // end if !USE_MULTIDRAW_ELEMENTS_INDIRECT
//----------------------------------------------------------------------------------------------------------------------------

#if USE_SSBOS
layout (std430) buffer LightDataStorage
{
	LightData light_data[];
};
#else
layout (std140) uniform LightDataStorage
{
	LightData light_data[256];
};
#endif


#if ORDER_INDEPENDENT_TRANSPARENCY
// Various outputs for order-independent transparency.
layout(location = 0) out vec4 transmittance_out;
layout(location = 1) out vec4 accum_out;
layout(location = 2) out float av_transmittance_out;
#else
layout(location = 0) out vec4 colour_out;
#endif


float square(float x) { return x*x; }
float pow4(float x) { return (x*x)*(x*x); }
float pow5(float x) { return x*x*x*x*x; }
float pow6(float x) { return x*x*x*x*x*x; }

float fresnelApprox(float cos_theta_i, float n2)
{
	//float r_0 = square((1.0 - n2) / (1.0 + n2));
	//return r_0 + (1.0 - r_0)*pow5(1.0 - cos_theta_i); // https://en.wikipedia.org/wiki/Schlick%27s_approximation

	float sintheta_i = sqrt(1.0 - cos_theta_i*cos_theta_i); // Get sin(theta_i)
	float sintheta_t = sintheta_i / n2; // Use Snell's law to get sin(theta_t)

	float costheta_t = sqrt(1.0 - sintheta_t*sintheta_t); // Get cos(theta_t)

	float a2 = square(cos_theta_i - n2*costheta_t);
	float b2 = square(cos_theta_i + n2*costheta_t);

	float c2 = square(n2*cos_theta_i - costheta_t);
	float d2 = square(costheta_t + n2*cos_theta_i);

	return 0.5 * (a2*d2 + b2*c2) / (b2*d2);
}

float trowbridgeReitzPDF(float cos_theta, float alpha2)
{
	return cos_theta * alpha2 / (3.1415926535897932384626433832795 * square(square(cos_theta) * (alpha2 - 1.0) + 1.0));
}

float alpha2ForRoughness(float r)
{
	return pow4(r);
}

float rayPlaneIntersect(vec3 raystart, vec3 ray_unitdir, float plane_h)
{
	float start_to_plane_dist = raystart.z - plane_h;

	return start_to_plane_dist / -ray_unitdir.z;
}

vec3 toNonLinear(vec3 x)
{
	// Approximation to pow(x, 0.4545).  Max error of ~0.004 over [0, 1].
	return 0.124445006f*x*x + -0.35056138f*x + 1.2311935*sqrt(x);
}

float fbm(vec2 p)
{
	return (texture(fbm_tex, p).x - 0.5) * 2.f;
}

vec2 rot(vec2 p)
{
	float theta = 1.618034 * 3.141592653589 * 2.0;
	return vec2(cos(theta) * p.x - sin(theta) * p.y, sin(theta) * p.x + cos(theta) * p.y);
}

float fbmMix(vec2 p)
{
	return 
		fbm(p) +
		fbm(rot(p * 2.0)) * 0.5;
}


void main()
{
	vec3 use_normal_cs;
	vec3 use_normal_ws;
	vec2 use_texture_coords = texture_coords;
	if((MAT_UNIFORM.flags & HAVE_SHADING_NORMALS_FLAG) != 0)
	{
		use_normal_cs = normal_cs;
		use_normal_ws = normal_ws;
	}
	else
	{
		vec3 dp_dx = dFdx(pos_cs);
		vec3 dp_dy = dFdy(pos_cs); 
		vec3 N_g = normalize(cross(dp_dx, dp_dy)); 
		use_normal_cs = N_g;

#if GENERATE_PLANAR_UVS
		// For voxels: Compute texture coords based on object-space geometric normal.
		dp_dx = dFdx(pos_os);
		dp_dy = dFdy(pos_os);
		vec3 N_g_os = cross(dp_dx, dp_dy);

		if(abs(N_g_os.x) > abs(N_g_os.y) && abs(N_g_os.x) > abs(N_g_os.z))
		{
			use_texture_coords.x = pos_os.y;
			use_texture_coords.y = pos_os.z;
			if(N_g_os.x < 0.0)
				use_texture_coords.x = -use_texture_coords.x;
		}
		else if(abs(N_g_os.y) > abs(N_g_os.x) && abs(N_g_os.y) > abs(N_g_os.z))
		{
			use_texture_coords.x = pos_os.x;
			use_texture_coords.y = pos_os.z;
			if(N_g_os.y > 0.0)
				use_texture_coords.x = -use_texture_coords.x;
		}
		else
		{
			use_texture_coords.x = pos_os.x;
			use_texture_coords.y = pos_os.y;
			if(N_g_os.z < 0.0)
				use_texture_coords.x = -use_texture_coords.x;
		}
#endif
		// Compute world-space geometric normal.
		dp_dx = dFdx(pos_ws);
		dp_dy = dFdy(pos_ws);
		N_g = cross(dp_dx, dp_dy);
		use_normal_ws = N_g;
	}

	vec2 main_tex_coords = MAT_UNIFORM.texture_upper_left_matrix_col0 * use_texture_coords.x + MAT_UNIFORM.texture_upper_left_matrix_col1 * use_texture_coords.y + MAT_UNIFORM.texture_matrix_translation;

	vec4 emission_col = MAT_UNIFORM.emission_colour;
	if((MAT_UNIFORM.flags & HAVE_EMISSION_TEX_FLAG) != 0)
	{
		emission_col *= texture(EMISSION_TEX, main_tex_coords);
	}

	vec4 col;
#if !ORDER_INDEPENDENT_TRANSPARENCY
	float alpha;
#endif
	if((MAT_UNIFORM.flags & IS_HOLOGRAM_FLAG) != 0)
	{
		col = emission_col;
#if ORDER_INDEPENDENT_TRANSPARENCY
		transmittance_out = vec4(1, 1, 1, 1);
		av_transmittance_out = 1.0;
#else
		alpha = 0.0; // For completely additive blending (hologram shader), we don't multiply by alpha in the fragment shader, and set a fragment colour with alpha = 0, so dest factor = 1 - 0 = 1.
#endif
	}
	else
	{
		vec3 unit_normal_cs = normalize(use_normal_cs);

		vec3 frag_to_cam = normalize(-pos_cs);

		const float ior = 2.0f;

		vec3 unit_normal_ws = normalize(use_normal_ws);
		if(dot(unit_normal_ws, cam_to_pos_ws) > 0.0)
			unit_normal_ws = -unit_normal_ws;

		vec3 unit_cam_to_pos_ws = normalize(cam_to_pos_ws);

		float roughness = 0.2;
		float fresnel_scale = 1.0;

		//----------------------- Direct lighting from interior lights ----------------------------
		// Load indices into a local array, so we can iterate over the array in a for loop.  TODO: find a better way of doing this.
		int indices[8];
		indices[0] = light_indices_0.x;
		indices[1] = light_indices_0.y;
		indices[2] = light_indices_0.z;
		indices[3] = light_indices_0.w;
		indices[4] = light_indices_1.x;
		indices[5] = light_indices_1.y;
		indices[6] = light_indices_1.z;
		indices[7] = light_indices_1.w;

		vec4 local_light_radiance = vec4(0.f);
		for(int i=0; i<8; ++i)
		{
			int light_index = indices[i];
			if(light_index >= 0)
			{
				vec3 light_emitted_radiance = light_data[light_index].col.xyz;

				vec3 pos_to_light = light_data[light_index].pos.xyz - pos_ws;
				float pos_to_light_len2 = dot(pos_to_light, pos_to_light);
				vec3 unit_pos_to_light = pos_to_light * inversesqrt(pos_to_light_len2);

				float dir_factor;
				if(light_data[light_index].light_type == 0) // Point light:
				{
					dir_factor = 1.0;
				}
				else
				{
					// light_type == 1: spotlight
					float from_light_cos_angle = -dot(light_data[light_index].dir.xyz, unit_pos_to_light);
					dir_factor =
						smoothstep(0.4f, 0.9f, from_light_cos_angle) * 0.03 + // A little light outside of the main cone
						smoothstep(light_data[light_index].cone_cos_angle_start, light_data[light_index].cone_cos_angle_end, from_light_cos_angle);
				}

				float cos_theta_term = max(0.f, dot(unit_normal_ws, unit_pos_to_light));

				// Compute specular bsdf
				vec3 h_ws = normalize(unit_pos_to_light - unit_cam_to_pos_ws);
				float h_cos_theta = abs(dot(h_ws, unit_normal_ws));
				vec4 specular_fresnel = vec4(fresnelApprox(h_cos_theta, ior));

				vec4 specular = trowbridgeReitzPDF(h_cos_theta, alpha2ForRoughness(roughness)) * fresnel_scale * specular_fresnel;

				vec3 bsdf = specular.xyz;
				vec3 reflected_radiance = bsdf * cos_theta_term * light_emitted_radiance * dir_factor / pos_to_light_len2;

				local_light_radiance.xyz += reflected_radiance;
			}
		}


		vec3 sunrefl_h = normalize(frag_to_cam + sundir_cs.xyz);
		float sunrefl_h_cos_theta = abs(dot(sunrefl_h, unit_normal_cs));
		
		float sun_specular = trowbridgeReitzPDF(sunrefl_h_cos_theta, alpha2ForRoughness(roughness)) * 
			fresnel_scale * fresnelApprox(sunrefl_h_cos_theta, ior);

		// Reflect cam-to-fragment vector in ws normal
		vec3 reflected_dir_ws = unit_cam_to_pos_ws - unit_normal_ws * (2.0 * dot(unit_normal_ws, unit_cam_to_pos_ws));

		//========================= Look up env map for reflected dir ============================
		int map_lower = int(roughness * 6.9999);
		int map_higher = map_lower + 1;
		float map_t = roughness * 6.9999 - float(map_lower);

		float refl_theta = acos(reflected_dir_ws.z);
		float refl_phi = atan(reflected_dir_ws.y, reflected_dir_ws.x) - 1.f; // -1.f is to rotate reflection so it aligns with env rotation.
		vec2 refl_map_coords = vec2(refl_phi * (1.0 / 6.283185307179586), clamp(refl_theta * (1.0 / 3.141592653589793), 1.0 / 64.0, 1.0 - 1.0 / 64.0)); // Clamp to avoid texture coord wrapping artifacts.

		vec4 spec_refl_light_lower  = texture(specular_env_tex, vec2(refl_map_coords.x, float(map_lower)  * (1.0/8.0) + refl_map_coords.y * (1.0/8.0))); //  -refl_map_coords / 8.0 + map_lower  * (1.0 / 8)));
		vec4 spec_refl_light_higher = texture(specular_env_tex, vec2(refl_map_coords.x, float(map_higher) * (1.0/8.0) + refl_map_coords.y * (1.0/8.0)));
		vec4 spec_refl_light = spec_refl_light_lower * (1.0 - map_t) + spec_refl_light_higher * map_t; // spectral radiance * 1.0e-9


		// Blend in reflection of cumulus clouds.  Skip cirrus clouds as an optimisation.
#if RENDER_SKY_AND_CLOUD_REFLECTIONS
		float cumulus_cloudfrac = 0.0;
		{
			float cumulus_ray_t = rayPlaneIntersect(pos_ws, reflected_dir_ws, 1000.0);
			if(cumulus_ray_t > 0.0)
			{
				vec3 hitpos = pos_ws + reflected_dir_ws * cumulus_ray_t;
				vec2 p = hitpos.xy * 0.0001;
				p.x += time * 0.002;

				vec2 cumulus_coords = vec2(p.x * 2.0 + 2.3453, p.y * 2.0 + 1.4354);

				float cumulus_val = max(0.f, fbmMix(cumulus_coords) - 0.3f);

				float dist_factor = 1.f - smoothstep(80000.0, 160000.0, cumulus_ray_t);

				cumulus_cloudfrac = dist_factor * cumulus_val;
			}
		}

		float cloudfrac = cumulus_cloudfrac;
		vec4 cloudcol = sun_and_sky_av_spec_rad;
		spec_refl_light = mix(spec_refl_light, cloudcol, max(0.f, cloudfrac));
#endif // RENDER_SKY_AND_CLOUD_REFLECTIONS

#if ORDER_INDEPENDENT_TRANSPARENCY
		vec4 transmission_col = vec4(0.6f) + 0.4f * MAT_UNIFORM.diffuse_colour; // Desaturate transmission colour a bit.
#else
		vec4 transmission_col = MAT_UNIFORM.diffuse_colour;
#endif		

		float spec_refl_cos_theta = abs(dot(frag_to_cam, unit_normal_cs));
		float spec_refl_fresnel = fresnelApprox(spec_refl_cos_theta, ior);

		float sun_vis_factor = 1.0f; // TODO: use shadow mapping to compute this.
		vec4 sun_light = sun_spec_rad_times_solid_angle * sun_vis_factor;

	
		col = spec_refl_light * spec_refl_fresnel + 
			sun_light * sun_specular + 
			local_light_radiance + // Reflected light from local light sources.
			emission_col;

#if ORDER_INDEPENDENT_TRANSPARENCY
		float T = 1.f - spec_refl_fresnel; // transmittance
		transmittance_out = transmission_col * T;

		// For computing the average transmittance (used for order-independent transparency), don't take spec_refl_fresnel into account so much, or our glass will be too dark at grazing angles.
		float use_T = 0.8f + T * 0.2f;
		vec4 use_transmittance = transmission_col * use_T;

		av_transmittance_out = (use_transmittance.r + use_transmittance.g + use_transmittance.b) * (1.0 / 3.0);
#else
		col += transmission_col * 0.5 * sun_and_sky_av_spec_rad;

		alpha = spec_refl_fresnel + sun_specular;
		col.xyz *= alpha; // To apply an alpha factor to the source colour if desired, we can just multiply by alpha in the fragment shader.
#endif
	}

	col *= 3.0; // tone-map

#if ORDER_INDEPENDENT_TRANSPARENCY
	
	#if DO_POST_PROCESSING
	accum_out = vec4(col.xyz, 1.0);
	#else
	accum_out = vec4(toNonLinear(col.xyz), 1.0);
	#endif
#else // else if !ORDER_INDEPENDENT_TRANSPARENCY:

	#if DO_POST_PROCESSING
	colour_out = vec4(col.xyz, alpha);
	#else
	colour_out = vec4(toNonLinear(col.xyz), alpha);
	#endif
#endif


#if DRAW_PLANAR_UV_GRID
	float du_dx = abs(dFdx(use_texture_coords.x));
	float du_dy = abs(dFdy(use_texture_coords.x));

	float dv_dx = abs(dFdx(use_texture_coords.y));
	float dv_dy = abs(dFdy(use_texture_coords.y));

	float a = max(du_dx, du_dy);
	float b = max(dv_dx, dv_dy);

	float border_w_u = max(0.01f, a * 0.5f);
	float border_w_v = max(0.01f, b * 0.5f);
	if(fract(use_texture_coords.x) < border_w_u || fract(use_texture_coords.x) >= (1 - border_w_u) ||
		fract(use_texture_coords.y) < border_w_v || fract(use_texture_coords.y) >= (1 - border_w_v))
		accum_out = vec4(0.2f, 0.8f, 0.54f, 1.f);
#endif
}