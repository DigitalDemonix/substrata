#include "AddObjectPreviewWidget.h"


#include "PlayerPhysics.h"
#include "../dll/include/IndigoMesh.h"
#include "../indigo/TextureServer.h"
#include "../indigo/globals.h"
#include "../graphics/Map2D.h"
#include "../graphics/imformatdecoder.h"
#include "../graphics/ImageMap.h"
#include "../maths/vec3.h"
#include "../maths/GeometrySampling.h"
#include "../utils/Lock.h"
#include "../utils/Mutex.h"
#include "../utils/Clock.h"
#include "../utils/Timer.h"
#include "../utils/Platform.h"
#include "../utils/FileUtils.h"
#include "../utils/Reference.h"
#include "../utils/StringUtils.h"
#include "../utils/CameraController.h"
#include "../utils/TaskManager.h"
#include <QtGui/QMouseEvent>
#include <set>
#include <stack>
#include <algorithm>


// https://wiki.qt.io/How_to_use_OpenGL_Core_Profile_with_Qt
// https://developer.apple.com/opengl/capabilities/GLInfo_1085_Core.html
static QGLFormat makeFormat()
{
	// We need to request a 'core' profile.  Otherwise on OS X, we get an OpenGL 2.1 interface, whereas we require a v3+ interface.
	QGLFormat format;
	// We need to request version 3.2 (or above?) on OS X, otherwise we get legacy version 2.
#ifdef OSX
	format.setVersion(3, 2);
#endif
	format.setProfile(QGLFormat::CoreProfile);
	format.setSampleBuffers(true);
	return format;
}


AddObjectPreviewWidget::AddObjectPreviewWidget(QWidget *parent)
:	QGLWidget(makeFormat(), parent)
{
	viewport_aspect_ratio = 1;

	OpenGLEngineSettings settings;
	settings.shadow_mapping = true;
	opengl_engine = new OpenGLEngine(settings);

	viewport_w = viewport_h = 100;

	// Needed to get keyboard events.
	setFocusPolicy(Qt::StrongFocus);
}


AddObjectPreviewWidget::~AddObjectPreviewWidget()
{
	// Make context current as we destroy the opengl enegine.
	this->makeCurrent();
	opengl_engine = NULL;
}


void AddObjectPreviewWidget::resizeGL(int width_, int height_)
{
	viewport_w = width_;
	viewport_h = height_;

	glViewport(0, 0, width_, height_);

	viewport_aspect_ratio = (double)width_ / (double)height_;
}


void AddObjectPreviewWidget::initializeGL()
{
	opengl_engine->initialise(
		//"n:/indigo/trunk/opengl/shaders" // shader dir
		"./shaders" // shader dir
	);
	if(!opengl_engine->initSucceeded())
	{
		conPrint("AddObjectPreviewWidget opengl_engine init failed: " + opengl_engine->getInitialisationErrorMsg());
	}


	cam_phi = 0;
	cam_theta = 1.3f;
	cam_dist = 3;
	cam_target_pos = Vec4f(0,0,1,1);

	// Add env mat
	{
		OpenGLMaterial env_mat;
		env_mat.albedo_tex_path = "sky.png";
		env_mat.tex_matrix = Matrix2f(-1 / Maths::get2Pi<float>(), 0, 0, 1 / Maths::pi<float>());
		buildMaterial(env_mat);

		opengl_engine->setEnvMat(env_mat);
	}


	// Make axis arrows
	{
		GLObjectRef arrow = opengl_engine->makeArrowObject(Vec4f(0,0,0,1), Vec4f(1,0,0,1), Colour4f(0.6, 0.2, 0.2, 1.f), 1.f);
		opengl_engine->addObject(arrow);
	}
	{
		GLObjectRef arrow = opengl_engine->makeArrowObject(Vec4f(0,0,0,1), Vec4f(0,1,0,1), Colour4f(0.2, 0.6, 0.2, 1.f), 1.f);
		opengl_engine->addObject(arrow);
	}
	{
		GLObjectRef arrow = opengl_engine->makeArrowObject(Vec4f(0,0,0,1), Vec4f(0,0,1,1), Colour4f(0.2, 0.2, 0.6, 1.f), 1.f);
		opengl_engine->addObject(arrow);
	}


	target_marker_ob = opengl_engine->makeAABBObject(cam_target_pos + Vec4f(0,0,0,0), cam_target_pos + Vec4f(0.03f, 0.03f, 0.03f, 0.f), Colour4f(0.6f, 0.6f, 0.2f, 1.f));
	opengl_engine->addObject(target_marker_ob);


	/*
		Load a ground plane into the GL engine
	*/
	if(true)
	{
		Indigo::MeshRef mesh = new Indigo::Mesh();
		mesh->addVertex(Indigo::Vec3f(-1, -1, 0), Indigo::Vec3f(0,0,1));
		mesh->addVertex(Indigo::Vec3f( 1, -1, 0), Indigo::Vec3f(0,0,1));
		mesh->addVertex(Indigo::Vec3f( 1,  1, 0), Indigo::Vec3f(0,0,1));
		mesh->addVertex(Indigo::Vec3f(-1,  1, 0), Indigo::Vec3f(0,0,1));
			
		mesh->num_uv_mappings = 1;
		mesh->addUVs(Indigo::Vector<Indigo::Vec2f>(1, Indigo::Vec2f(0,0)));
		mesh->addUVs(Indigo::Vector<Indigo::Vec2f>(1, Indigo::Vec2f(1,0)));
		mesh->addUVs(Indigo::Vector<Indigo::Vec2f>(1, Indigo::Vec2f(1,1)));
		mesh->addUVs(Indigo::Vector<Indigo::Vec2f>(1, Indigo::Vec2f(0,1)));
			
		uint32 indices[] = {0, 1, 2, 3};
		mesh->addQuad(indices, indices, 0);

		mesh->endOfModel();

		GLObjectRef ob = new GLObject();
		ob->materials.resize(1);
		ob->materials[0].albedo_rgb = Colour3f(0.7f, 0.7f, 0.7f);
		ob->materials[0].phong_exponent = 10.f;

		ob->ob_to_world_matrix.setToTranslationMatrix(0,0,0);
		ob->mesh_data = OpenGLEngine::buildIndigoMesh(mesh);

		opengl_engine->addObject(ob);
	}
}


void AddObjectPreviewWidget::paintGL()
{
	const Matrix4f T = Matrix4f::translationMatrix(0.f, cam_dist, 0.f);
	const Matrix4f z_rot = Matrix4f::rotationMatrix(Vec4f(0,0,1,0), cam_phi);
	const Matrix4f x_rot = Matrix4f::rotationMatrix(Vec4f(1,0,0,0), -(cam_theta - Maths::pi_2<float>()));
	const Matrix4f rot = x_rot * z_rot;
	const Matrix4f world_to_camera_space_matrix = T * rot * Matrix4f::translationMatrix(-cam_target_pos);

	const float sensor_width = 0.035f;
	const float lens_sensor_dist = 0.03f;
	const float render_aspect_ratio = viewport_aspect_ratio;

	opengl_engine->setViewportAspectRatio(viewport_aspect_ratio, viewport_w, viewport_h);
	opengl_engine->setMaxDrawDistance(100.f);
	opengl_engine->setCameraTransform(world_to_camera_space_matrix, sensor_width, lens_sensor_dist, render_aspect_ratio);
	opengl_engine->draw();
}


void AddObjectPreviewWidget::addObject(const Reference<GLObject>& object)
{
	this->makeCurrent();

	// Build materials
	for(size_t i=0; i<object->materials.size(); ++i)
		buildMaterial(object->materials[i]);

	opengl_engine->addObject(object);
}


void AddObjectPreviewWidget::addOverlayObject(const Reference<OverlayObject>& object)
{
	this->makeCurrent();

	buildMaterial(object->material);

	opengl_engine->addOverlayObject(object);
}


void AddObjectPreviewWidget::setEnvMat(OpenGLMaterial& mat)
{
	this->makeCurrent();

	buildMaterial(mat);
	opengl_engine->setEnvMat(mat);
}


void AddObjectPreviewWidget::buildMaterial(OpenGLMaterial& opengl_mat)
{
	try
	{
		if(!opengl_mat.albedo_tex_path.empty() && opengl_mat.albedo_texture.isNull()) // If texture not already loaded:
		{
			std::string use_path;
			try
			{
				use_path = FileUtils::getActualOSPath(opengl_mat.albedo_tex_path);
			}
			catch(FileUtils::FileUtilsExcep&)
			{
				use_path = opengl_mat.albedo_tex_path;
			}

			Reference<Map2D> tex = this->texture_server_ptr->getTexForPath(indigo_base_dir, use_path);
			unsigned int tex_xres = tex->getMapWidth();
			unsigned int tex_yres = tex->getMapHeight();

			if(tex.isType<ImageMapUInt8>())
			{
				ImageMapUInt8* imagemap = static_cast<ImageMapUInt8*>(tex.getPointer());

				if(imagemap->getN() == 3)
				{
					Reference<OpenGLTexture> opengl_tex = new OpenGLTexture();
					opengl_tex->load(tex_xres, tex_yres, imagemap->getData(), opengl_engine, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE, 
						false // nearest filtering
					);
					opengl_mat.albedo_texture = opengl_tex;
				}
			}
		}
		//std::cout << "successfully loaded " << use_path << ", xres = " << tex_xres << ", yres = " << tex_yres << std::endl << std::endl;
	}
	catch(TextureServerExcep& e)
	{
		conPrint("Failed to load texture '" + opengl_mat.albedo_tex_path + "': " + e.what());
	}
	catch(ImFormatExcep& e)
	{
		conPrint("Failed to load texture '" + opengl_mat.albedo_tex_path + "': " + e.what());
	}
}


void AddObjectPreviewWidget::keyPressEvent(QKeyEvent* e)
{
}


void AddObjectPreviewWidget::keyReleaseEvent(QKeyEvent* e)
{
}


void AddObjectPreviewWidget::mousePressEvent(QMouseEvent* e)
{
	mouse_move_origin = QCursor::pos();
}


void AddObjectPreviewWidget::showEvent(QShowEvent* e)
{
	emit widgetShowSignal();
}


void AddObjectPreviewWidget::mouseMoveEvent(QMouseEvent* e)
{
	Qt::MouseButtons mb = e->buttons();

	// Get new mouse position, movement vector and set previous mouse position to new.
	QPoint new_pos = QCursor::pos();
	QPoint delta = new_pos - mouse_move_origin; 

	if(mb & Qt::LeftButton)
	{
		const float move_scale = 0.005f;
		cam_phi += delta.x() * move_scale;
		cam_theta = myClamp<float>(cam_theta - (float)delta.y() * move_scale, 0.01f, Maths::pi<float>() - 0.01f);
	}

	printVar(cam_phi);
	printVar(cam_theta);

	if((mb & Qt::MiddleButton) || (mb & Qt::RightButton))
	{
		const float move_scale = 0.005f;

		const Vec4f forwards = GeometrySampling::dirForSphericalCoords(-cam_phi + Maths::pi_2<float>(), Maths::pi<float>() - cam_theta);
		const Vec4f right = normalise(crossProduct(forwards, Vec4f(0,0,1,0)));
		const Vec4f up = crossProduct(right, forwards);
		//const Vec4f right(1,0,0,0);
		//const Vec4f up(0,0,1,0);

		cam_target_pos += right * -(float)delta.x() * move_scale + up * (float)delta.y() * move_scale;
		
		conPrint("forwards: " + forwards.toStringNSigFigs(3));
		conPrint("right: " + right.toStringNSigFigs(3));
		conPrint("up: " + up.toStringNSigFigs(3));
		conPrint("cam_target_pos: " + cam_target_pos.toStringNSigFigs(3));

		target_marker_ob->ob_to_world_matrix.setColumn(3, cam_target_pos);
		opengl_engine->updateObjectTransformData(*target_marker_ob);
	}

	//if(mb & Qt::RightButton || mb & Qt::LeftButton || mb & Qt::MidButton)
	//	emit cameraUpdated();

	mouse_move_origin = new_pos;
}


void AddObjectPreviewWidget::wheelEvent(QWheelEvent* ev)
{
	// Make change proportional to distance value.
	// Mouse wheel scroll up reduces distance.
	cam_dist = myClamp<float>(cam_dist - (cam_dist * ev->delta() * 0.002f), 0.01f, 100.f);

	ev->accept(); // We want to kill the event now.
	this->setFocus(); // otherwise this loses focus for some reason.
}
