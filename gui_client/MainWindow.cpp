/*=====================================================================
MainWindow.cpp
--------------
Copyright Glare Technologies Limited 2023 -
=====================================================================*/


#ifdef _MSC_VER // Qt headers suppress some warnings on Windows, make sure the warning suppression doesn't propagate to our code. See https://bugreports.qt.io/browse/QTBUG-26877
#pragma warning(push, 0) // Disable warnings
#endif
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "AboutDialog.h"
#include "ClientThread.h"
#include "GoToPositionDialog.h"
#include "LogWindow.h"
#include "UserDetailsWidget.h"
#include "AvatarSettingsDialog.h"
#include "AddObjectDialog.h"
#include "AddVideoDialog.h"
#include "MainOptionsDialog.h"
#include "FindObjectDialog.h"
#include "ListObjectsNearbyDialog.h"
#include "ModelLoading.h"
#include "TestSuite.h"
#include "ThreadMessages.h"
#include "TerrainSystem.h"
#include "GuiClientApplication.h"
#include "LoginDialog.h"
#include "SignUpDialog.h"
#include "GoToParcelDialog.h"
#include "QSettingsStore.h"
#include "ResetPasswordDialog.h"
#include "ChangePasswordDialog.h"
#include "ClientUDPHandlerThread.h"
#include "URLWidget.h"
#include "URLWhitelist.h"
#include "URLParser.h"
#include "WinterShaderEvaluator.h"
#include "CEF.h"
#include "../shared/Protocol.h"
#include "../shared/Version.h"
#include "../shared/LODGeneration.h"
#include "../shared/ImageDecoding.h"
#include "../shared/MessageUtils.h"
#include "../shared/FileTypes.h"
#include "../shared/WorldSettings.h"
#include <QtCore/QProcess>
#include <QtCore/QMimeData>
#include <QtCore/QSettings>
#include <QtWidgets/QApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsTextItem>
#include <QtWidgets/QMessageBox>
#include <QtGui/QImageWriter>
#include <QtWidgets/QErrorMessage>
#include <QtWidgets/QSplashScreen>
#include <QtGui/QPainter>
#include <QtCore/QTimer>
#include "../qt/QtUtils.h"
#ifdef _MSC_VER
#pragma warning(pop) // Re-enable warnings
#endif
#include "../maths/Quat.h"
#include "../maths/GeometrySampling.h"
#include "../utils/Clock.h"
#include "../utils/Timer.h"
#include "../utils/PlatformUtils.h"
#include "../utils/ConPrint.h"
#include "../utils/Exception.h"
#include "../utils/TaskManager.h"
#include "../utils/SocketBufferOutStream.h"
#include "../utils/StringUtils.h"
#include "../utils/FileUtils.h"
#include "../utils/FileChecksum.h"
#include "../utils/Parser.h"
#include "../utils/ContainerUtils.h"
#include "../utils/Base64.h"
#include "../utils/OpenSSL.h"
#include "../utils/ShouldCancelCallback.h"
#include "../utils/FileInStream.h"
#include "../utils/FileOutStream.h"
#include "../utils/BufferOutStream.h"
#include "../utils/IncludeXXHash.h"
#include "../utils/RuntimeCheck.h"
#include "../networking/Networking.h"
#include "../graphics/ImageMap.h"
#include "../graphics/FormatDecoderGLTF.h"
#include "../dll/IndigoStringUtils.h"
#include "../dll/include/IndigoException.h"
#include "../indigo/TextureServer.h"
#include "../graphics/PNGDecoder.h"
#include "../graphics/jpegdecoder.h"
#if defined(_WIN32)
#include "../video/WMFVideoReader.h"
#endif
#include "../direct3d/Direct3DUtils.h"
#include <Escaping.h>
#include <clocale>
#include <tls.h>
#include "superluminal/PerformanceAPI.h"
#if BUGSPLAT_SUPPORT
#include <BugSplat.h>
#endif

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#else
#include <signal.h>
#endif
#include <OpenGLEngineTests.h>

#include <tracy/Tracy.hpp>


// If we are building on Windows, and we are not in Release mode (e.g. BUILD_TESTS is enabled), then make sure the console window is shown.
#if defined(_WIN32) && defined(BUILD_TESTS)
#pragma comment(linker, "/SUBSYSTEM:CONSOLE")
#endif


static const Colour4f PARCEL_OUTLINE_COLOUR    = Colour4f::fromHTMLHexString("f09a13"); // orange


MainWindow::MainWindow(const std::string& base_dir_path_, const std::string& appdata_path_, const ArgumentParser& args, QWidget* parent)
:	base_dir_path(base_dir_path_),
	appdata_path(appdata_path_),
	parsed_args(args),
	QMainWindow(parent),
	need_help_info_dock_widget_position(false),
	log_window(NULL),
	in_CEF_message_loop(false),
	should_close(false),
	closing(false),
	main_timer_id(0),
	gui_client(base_dir_path_, appdata_path_, args),
	run_as_screenshot_slave(false),
	taking_map_screenshot(false),
	test_screenshot_taking(false),
	done_screenshot_setup(false),
	running_destructor(false),
	scratch_packet(SocketBufferOutStream::DontUseNetworkByteOrder),
	settings_store(NULL)
{
	//QGamepadManager::instance();

	ui = new Ui::MainWindow();
	ui->setupUi(this);

	setAcceptDrops(true);

	update_ob_editor_transform_timer = new QTimer(this);
	update_ob_editor_transform_timer->setSingleShot(true);
	connect(update_ob_editor_transform_timer, SIGNAL(timeout()), this, SLOT(updateObjectEditorObTransformSlot()));

	// Add dock widgets to Window menu
	ui->menuWindow->addSeparator();
	ui->menuWindow->addAction(ui->editorDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->materialBrowserDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->environmentDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->worldSettingsDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->chatDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->helpInfoDockWidget->toggleViewAction());
	//ui->menuWindow->addAction(ui->indigoViewDockWidget->toggleViewAction());
	ui->menuWindow->addAction(ui->diagnosticsDockWidget->toggleViewAction());

	settings = new QSettings("Glare Technologies", "Cyberspace");

	credential_manager.loadFromSettings(*settings);

	if(args.isArgPresent("--no_MDI"))
		ui->glWidget->allow_multi_draw_indirect = false;
	if(args.isArgPresent("--no_bindless"))
		ui->glWidget->allow_bindless_textures = false;

	ui->glWidget->setBaseDir(base_dir_path, /*print output=*/this, settings);
	ui->objectEditor->base_dir_path = base_dir_path;
	ui->objectEditor->settings = settings;

	// Add a spacer to right-align the UserDetailsWidget (see http://www.setnode.com/blog/right-aligning-a-button-in-a-qtoolbar/)
	QWidget* spacer = new QWidget();
	spacer->setMinimumWidth(60);
	spacer->setMaximumWidth(60);
	//spacer->setGeometry(QRect()
	//spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ui->toolBar->addWidget(spacer);

	url_widget = new URLWidget(this);
	ui->toolBar->addWidget(url_widget);

	user_details = new UserDetailsWidget(this);
	ui->toolBar->addWidget(user_details);

	// Make it so the toolbar can't be hidden, as it's confusing for users when it disappears.
	ui->toolBar->toggleViewAction()->setEnabled(false);
	
	
	// Open Log File
	//const std::string logfile_path = FileUtils::join(this->appdata_path, "log.txt");
	//this->logfile.open(StringUtils::UTF8ToPlatformUnicodeEncoding(logfile_path).c_str(), std::ios_base::out);
	//if(!logfile.good())
	//	conPrint("WARNING: Failed to open log file at '" + logfile_path + "' for writing.");
	//logfile << "============================= Cyberspace Started =============================" << std::endl;
	//logfile << Clock::getAsciiTime() << std::endl;

	

	// Create the LogWindow early so we can log stuff to it.
	log_window = new LogWindow(this, settings);


	// Since we use a perspective projection matrix with infinite far distance, use a large max drawing distance.
	ui->glWidget->max_draw_dist = 100000;

	// Restore main window geometry and state
	this->restoreGeometry(settings->value("mainwindow/geometry").toByteArray());

	if(!this->restoreState(settings->value("mainwindow/windowState").toByteArray()))
	{
		// State was not restored.  This will be the case for new Substrata installs.
		// Hide some dock widgets to provide a slightly simpler user experience.
		// Hide everything but chat and help-info dock widgets.
		this->ui->editorDockWidget->hide();
		this->ui->materialBrowserDockWidget->hide();
		this->ui->environmentDockWidget->hide();
		this->ui->worldSettingsWidget->hide();
		this->ui->diagnosticsDockWidget->hide();
		this->ui->worldSettingsDockWidget->hide();
	}

	ui->worldSettingsWidget->init(this);

	ui->objectEditor->init();

	ui->diagnosticsWidget->init(settings);
	connect(ui->diagnosticsWidget, SIGNAL(settingsChangedSignal()), this, SLOT(diagnosticsWidgetChanged()));
	connect(ui->diagnosticsWidget, SIGNAL(reloadTerrainSignal()), this, SLOT(diagnosticsReloadTerrain()));

	ui->environmentOptionsWidget->init(settings);
	connect(ui->environmentOptionsWidget, SIGNAL(settingChanged()), this, SLOT(environmentSettingChangedSlot()));

	connect(ui->chatPushButton, SIGNAL(clicked()), this, SLOT(sendChatMessageSlot()));
	connect(ui->chatMessageLineEdit, SIGNAL(returnPressed()), this, SLOT(sendChatMessageSlot()));
	connect(ui->glWidget, SIGNAL(mouseClicked(QMouseEvent*)), this, SLOT(glWidgetMouseClicked(QMouseEvent*)));
	connect(ui->glWidget, SIGNAL(mousePressed(QMouseEvent*)), this, SLOT(glWidgetMousePressed(QMouseEvent*)));
	connect(ui->glWidget, SIGNAL(mouseDoubleClickedSignal(QMouseEvent*)), this, SLOT(glWidgetMouseDoubleClicked(QMouseEvent*)));
	connect(ui->glWidget, SIGNAL(mouseMoved(QMouseEvent*)), this, SLOT(glWidgetMouseMoved(QMouseEvent*)));
	connect(ui->glWidget, SIGNAL(keyPressed(QKeyEvent*)), this, SLOT(glWidgetKeyPressed(QKeyEvent*)));
	connect(ui->glWidget, SIGNAL(keyReleased(QKeyEvent*)), this, SLOT(glWidgetkeyReleased(QKeyEvent*)));
	connect(ui->glWidget, SIGNAL(focusOutSignal()), this, SLOT(glWidgetFocusOut()));
	connect(ui->glWidget, SIGNAL(mouseWheelSignal(QWheelEvent*)), this, SLOT(glWidgetMouseWheelEvent(QWheelEvent*)));
	connect(ui->glWidget, SIGNAL(cameraUpdated()), this, SLOT(cameraUpdated()));
	connect(ui->glWidget, SIGNAL(viewportResizedSignal(int, int)), this, SLOT(glWidgetViewportResized(int, int)));
	connect(ui->glWidget, SIGNAL(copyShortcutActivated()), this, SLOT(on_actionCopy_Object_triggered()));
	connect(ui->glWidget, SIGNAL(pasteShortcutActivated()), this, SLOT(on_actionPaste_Object_triggered()));
	connect(ui->objectEditor, SIGNAL(objectTransformChanged()), this, SLOT(objectTransformEditedSlot()));
	connect(ui->objectEditor, SIGNAL(objectChanged()), this, SLOT(objectEditedSlot()));
	connect(ui->objectEditor, SIGNAL(bakeObjectLightmap()), this, SLOT(bakeObjectLightmapSlot()));
	connect(ui->objectEditor, SIGNAL(bakeObjectLightmapHighQual()), this, SLOT(bakeObjectLightmapHighQualSlot()));
	connect(ui->objectEditor, SIGNAL(removeLightmapSignal()), this, SLOT(removeLightmapSignalSlot()));
	connect(ui->objectEditor, SIGNAL(posAndRot3DControlsToggled()), this, SLOT(posAndRot3DControlsToggledSlot()));
	connect(ui->parcelEditor, SIGNAL(parcelChanged()), this, SLOT(parcelEditedSlot()));
	connect(ui->worldSettingsWidget, SIGNAL(settingsAppliedSignal()), this, SLOT(worldSettingsAppliedSlot()));
	connect(user_details, SIGNAL(logInClicked()), this, SLOT(on_actionLogIn_triggered()));
	connect(user_details, SIGNAL(logOutClicked()), this, SLOT(on_actionLogOut_triggered()));
	connect(user_details, SIGNAL(signUpClicked()), this, SLOT(on_actionSignUp_triggered()));
	connect(url_widget, SIGNAL(URLChanged()), this, SLOT(URLChangedSlot()));


#if !defined(_WIN32)
	// On Windows, Windows will execute substrata.exe with the -linku argument, so we don't need this technique.
	QDesktopServices::setUrlHandler("sub", /*receiver=*/this, /*method=*/"handleURL");
#endif
}


static std::string computeWindowTitle()
{
	return "Substrata v" + ::cyberspace_version;
}


static const char* default_help_info_message = "Use the W/A/S/D keys and arrow keys to move and look around.\n"
	"Click and drag the mouse on the 3D view to look around.\n"
	"Space key: jump\n"
	"Double-click an object to select it.";


void MainWindow::startMainTimer()
{
	// Stop previous timer, if it exists.
	if(main_timer_id != 0)
		killTimer(main_timer_id);

	int use_interval = 1; // in milliseconds
	const bool limit_FPS = settings->value(MainOptionsDialog::limitFPSKey(), /*default val=*/false).toBool();
	if(limit_FPS)
	{
		const int max_FPS = myClamp(settings->value(MainOptionsDialog::FPSLimitKey(), /*default val=*/60).toInt(), 15, 1000);
		use_interval = (int)(1000.0 / max_FPS);
	}

#ifdef OSX
	// Set to at least 17ms due to this issue on Mac OS: https://bugreports.qt.io/browse/QTBUG-60346
	use_interval = myMax(use_interval, 17); 
#endif

	main_timer_id = startTimer(use_interval);
}


void MainWindow::initialise()
{
	setWindowTitle(QtUtils::toQString(computeWindowTitle()));

	ui->materialBrowserDockWidgetContents->init(this, this->base_dir_path, this->appdata_path, gui_client.texture_server, /*print output=*/this);
	connect(ui->materialBrowserDockWidgetContents, SIGNAL(materialSelected(const std::string&)), this, SLOT(materialSelectedInBrowser(const std::string&)));

	ui->objectEditor->setControlsEnabled(false);
	ui->parcelEditor->hide();

	startMainTimer();
	
	ui->infoDockWidget->setTitleBarWidget(new QWidget());
	ui->infoDockWidget->hide();

	setUIForSelectedObject();

	// Update help text
	this->ui->helpInfoLabel->setText(default_help_info_message);

	if(!settings->contains("mainwindow/geometry"))
		need_help_info_dock_widget_position = true;

	connect(this->ui->indigoViewDockWidget, SIGNAL(visibilityChanged(bool)), this, SLOT(onIndigoViewDockWidgetVisibilityChanged(bool)));

#if INDIGO_SUPPORT
#else
	this->ui->indigoViewDockWidget->hide();
#endif

	lightmap_flag_timer = new QTimer(this);
	lightmap_flag_timer->setSingleShot(true);
	connect(lightmap_flag_timer, SIGNAL(timeout()), this, SLOT(sendLightmapNeededFlagsSlot()));


	std::string cache_dir = appdata_path;
	if(settings->value(MainOptionsDialog::useCustomCacheDirKey(), /*default value=*/false).toBool())
	{
		const std::string custom_cache_dir = QtUtils::toStdString(settings->value(MainOptionsDialog::customCacheDirKey()).toString());
		if(!custom_cache_dir.empty()) // Don't use custom cache dir if it's the empty string (e.g. not set to something valid)
			cache_dir = custom_cache_dir;
	}

	settings_store = new QSettingsStore(settings);

	gui_client.initialise(cache_dir, this, settings_store, this);

#ifdef _WIN32
	// Create a GPU device.  Needed to get hardware accelerated video decoding.
	Direct3DUtils::createGPUDeviceAndMFDeviceManager(d3d_device, device_manager);
#endif //_WIN32

	
	//for(int i=0; i<4; ++i)
	//	this->footstep_sources.push_back(audio_engine.addSourceFromSoundFile("D:\\audio\\sound_effects\\footstep_mono" + toString(i) + ".wav"));


	if(run_as_screenshot_slave)
	{
		conPrint("Waiting for screenshot command connection...");
		MySocketRef listener = new MySocket();
		listener->bindAndListen(34534);

		screenshot_command_socket = listener->acceptConnection(); // Blocks
		screenshot_command_socket->setUseNetworkByteOrder(false);
		conPrint("Got screenshot command connection.");
	}
}


void MainWindow::afterGLInitInitialise()
{
	if(settings->value("mainwindow/showParcels", QVariant(false)).toBool())
	{
		ui->actionShow_Parcels->setChecked(true);
		gui_client.addParcelObjects();
	}

	if(settings->value("mainwindow/flyMode", QVariant(false)).toBool())
	{
		ui->actionFly_Mode->setChecked(true);
		gui_client.player_physics.setFlyModeEnabled(true);
	}

	gui_client.cam_controller.setThirdPersonEnabled(settings->value("mainwindow/thirdPersonCamera", /*default val=*/false).toBool());
	ui->actionThird_Person_Camera->setChecked(settings->value("mainwindow/thirdPersonCamera", /*default val=*/false).toBool());

	//OpenGLEngineTests::doTextureLoadingTests(*ui->glWidget->opengl_engine);

#ifdef _WIN32
	// Prepare for D3D interoperability with opengl
	//wgl_funcs.init();
	//
	//interop_device_handle = wgl_funcs.wglDXOpenDeviceNV(d3d_device.ptr); // prepare for interoperability with opengl
	//if(interop_device_handle == 0)
	//	throw glare::Exception("wglDXOpenDeviceNV failed.");
#endif

	const auto device_pixel_ratio = ui->glWidget->devicePixelRatio(); // For retina screens this is 2, meaning the gl viewport width is in physical pixels, which have twice the density of qt pixel coordinates.

	const bool show_minimap = MainOptionsDialog::getShowMinimap(this->settings);
	gui_client.afterGLInitInitialise((double)device_pixel_ratio, show_minimap, ui->glWidget->opengl_engine);




	// Do auto-setting of graphics options, if they have not been set.  Otherwise apply MSAA setting.
	if(!settings->contains(MainOptionsDialog::MSAAKey())) // If the MSAA key has not been set:
	{
		const bool is_retina = device_pixel_ratio > 1;

		// We won't use MSAA by default in two cases:
		// 1) Intel drivers - which implies an integrated Intel GPU which is probably not very powerful.
		// 2) A retina monitor - the majority of which correspond to Mac laptops, which will look hopefully look alright without MSAA, and run slowly with MSAA.
		const bool is_Intel = ui->glWidget->opengl_engine->openglDriverVendorIsIntel();
		const bool no_MSAA = is_Intel || is_retina;
		const bool MSAA = !no_MSAA;
		//ui->glWidget->opengl_engine->setMSAAEnabled(MSAA);

		settings->setValue(MainOptionsDialog::MSAAKey(), MSAA); // Save MSAA setting

		settings->setValue(MainOptionsDialog::BloomKey(), MSAA); // Use the same decision for bloom

		logMessage("Auto-setting MSAA: is_retina: " + boolToString(is_retina) + ", is_Intel: " + boolToString(is_Intel) + ", MSAA: " + boolToString(MSAA));
	}
	else
	{
		// Else MSAA setting is present.
		const bool MSAA = settings->value(MainOptionsDialog::MSAAKey(), /*default=*/true).toBool();
		logMessage("Setting MSAA to " + boolToString(MSAA));
		//ui->glWidget->opengl_engine->setMSAAEnabled(MSAA);
	}
}


MainWindow::~MainWindow()
{
	running_destructor = true; // Set this to not append log messages during destruction, causes assert failure in Qt.

	delete settings_store;

#if !defined(_WIN32)
	QDesktopServices::unsetUrlHandler("sub"); // Remove 'this' as an URL handler.
#endif

	//ui->glWidget->makeCurrent(); // This crashes on Mac

#ifdef _WIN32
	//if(this->interop_device_handle)
	//{
	//	const BOOL res = wgl_funcs.wglDXCloseDeviceNV(this->interop_device_handle); // close interoperability with opengl
	//	assertOrDeclareUsed(res);
	//}
#endif

	// Free direct3d device and device manager
#ifdef _WIN32
	device_manager.release();
	d3d_device.release();
#endif

	delete ui;
}


void MainWindow::closeEvent(QCloseEvent* event)
{
	// Don't try and close everything down while we're in the message loop in the chromium embedded framework (CEF) code,
	// because that will try and close CEF down, which leads to problems.
	// Instead set a flag (should_close), and close the mainwindow when we're back in the main message loop and not the CEF loop.
	if(in_CEF_message_loop)
	{
		should_close = true;
		event->ignore();
		return;
	}

	ui->glWidget->makeCurrent();

	// Save main window geometry and state.  See http://doc.qt.io/archives/qt-4.8/qmainwindow.html#saveState
	settings->setValue("mainwindow/geometry", saveGeometry());
	settings->setValue("mainwindow/windowState", saveState());


	gui_client.shutdown();


	this->opengl_engine = NULL;
	ui->glWidget->shutdown(); // Shuts down OpenGL Engine.

	if(log_window) log_window->close();

	in_CEF_message_loop = true;
	CEF::shutdownCEF();
	in_CEF_message_loop = false;

	this->closing = true;
	QMainWindow::closeEvent(event);
}


void MainWindow::onIndigoViewDockWidgetVisibilityChanged(bool visible)
{
	/*conPrint("--------------------------------------- MainWindow::onIndigoViewDockWidgetVisibilityChanged (visible: " + boolToString(visible) + ") --------------");
	if(visible)
	{
		this->ui->indigoView->initialise(this->base_dir_path);

		if(this->world_state.nonNull())
		{
			Lock lock(this->world_state->mutex);
			this->ui->indigoView->addExistingObjects(*this->world_state, *this->resource_manager);
		}
	}
	else
	{
		this->ui->indigoView->shutdown();
	}*/
}


// Some resources, such as MP4 videos, shouldn't be downloaded fully before displaying, but instead can be streamed and displayed when only part of the stream is downloaded.
/*static bool shouldStreamResourceViaHTTP(const std::string& url)
{
	// On Windows, use WMF's http reading to stream videos (until we get the custom byte stream working)
#ifdef _WIN32
	return ::hasExtensionStringView(url, "mp4");
#else
	return false; // On Mac/linux, we'll use the ResourceIODeviceWrapper for QMediaPlayer, so we don't need to use http.
#endif
}*/


//bool MainWindow::isAudioProcessed(const std::string& url) const
//{
//	Lock lock(audio_processed_mutex);
//	return audio_processed.count(url) > 0;
//}


void MainWindow::logMessage(const std::string& msg) // Append to LogWindow log display
{
	//this->logfile << msg << "\n";
	if(this->log_window && !running_destructor)
		this->log_window->appendLine(msg);
}


void MainWindow::logAndConPrintMessage(const std::string& msg) // Print to stdout and append to LogWindow log display
{
	conPrint(msg);
	//this->logfile << msg << "\n";
	if(this->log_window && !running_destructor)
		this->log_window->appendLine(msg);
}


void MainWindow::print(const std::string& s) // Print a message and a newline character.
{
	logMessage(s);
}


void MainWindow::printStr(const std::string& s) // Print a message without a newline character.
{
	logMessage(s);
}


static const size_t MAX_NUM_NOTIFICATIONS = 5;


void MainWindow::showErrorNotification(const std::string& message)
{
	QLabel* label = new QLabel(ui->notificationContainer);
	label->setText(QtUtils::toQString(message));
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);

	label->setStyleSheet("QLabel { padding: 6px; background-color : rgb(255, 200, 200); }");

	ui->notificationContainer->layout()->addWidget(label);

	Notification n;
	n.creation_time = Clock::getTimeSinceInit();
	n.label = label;
	notifications.push_back(n);

	if(notifications.size() == 1)
		ui->infoDockWidget->show();
	else if(notifications.size() > MAX_NUM_NOTIFICATIONS)
	{
		// Remove the first notification
		Notification& notification = notifications.front();
		ui->notificationContainer->layout()->removeWidget(notification.label);
		notification.label->deleteLater();
		notifications.pop_front(); // remove from list
	}
}


void MainWindow::showInfoNotification(const std::string& message)
{
	QLabel* label = new QLabel(ui->notificationContainer);
	label->setText(QtUtils::toQString(message));
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);

	label->setStyleSheet("QLabel { padding: 6px; background-color : rgb(239, 228, 176); }");

	ui->notificationContainer->layout()->addWidget(label);

	Notification n;
	n.creation_time = Clock::getTimeSinceInit();
	n.label = label;
	notifications.push_back(n);

	if(notifications.size() == 1)
		ui->infoDockWidget->show();
	else if(notifications.size() > MAX_NUM_NOTIFICATIONS)
	{
		// Remove the first notification
		Notification& notification = notifications.front();
		ui->notificationContainer->layout()->removeWidget(notification.label);
		notification.label->deleteLater();
		notifications.pop_front(); // remove from list
	}
}


void MainWindow::setTextAsNotLoggedIn()
{
	user_details->setTextAsNotLoggedIn();
}


void MainWindow::setTextAsLoggedIn(const std::string& username)
{
	user_details->setTextAsLoggedIn(username);
}


void MainWindow::updateWorldSettingsControlsEditable()
{
	ui->worldSettingsWidget->updateControlsEditable();
}


void MainWindow::updateWorldSettingsUIFromWorldSettings()
{
	this->ui->worldSettingsWidget->setFromWorldSettings(gui_client.connected_world_settings); // Update UI
}


bool MainWindow::diagnosticsVisible()
{
	return ui->diagnosticsDockWidget->isVisible();
}


bool MainWindow::showObAABBsEnabled()
{
	 return ui->diagnosticsWidget->showObAABBsCheckBox->isChecked();
}


bool MainWindow::showPhysicsObOwnershipEnabled()
{
	return ui->diagnosticsWidget->showPhysicsObOwnershipCheckBox->isChecked();
}


bool MainWindow::showVehiclePhysicsVisEnabled()
{
	return ui->diagnosticsWidget->showVehiclePhysicsVisCheckBox->isChecked();
}


void MainWindow::writeTransformMembersToObject(WorldObject& ob)
{
	ui->objectEditor->writeTransformMembersToObject(ob);
}


void MainWindow::objectLastModifiedUpdated(const WorldObject& ob)
{
	 ui->objectEditor->objectLastModifiedUpdated(ob);
}


void MainWindow::objectModelURLUpdated(const WorldObject& ob)
{
	ui->objectEditor->objectModelURLUpdated(ob);
}


void MainWindow::objectLightmapURLUpdated(const WorldObject& ob)
{
	ui->objectEditor->objectLightmapURLUpdated(ob);
}


void MainWindow::showEditorDockWidget()
{
	ui->editorDockWidget->show(); // Show the object editor dock widget if it is hidden.
}


void MainWindow::setObjectEditorControlsEditable(bool editable)
{
	ui->objectEditor->setControlsEditable(editable);
}


void MainWindow::setObjectEditorFromOb(const WorldObject& ob, int selected_mat_index, bool ob_in_editing_users_world)
{
	ui->objectEditor->setFromObject(ob, selected_mat_index, ob_in_editing_users_world);
}


int MainWindow::getSelectedMatIndex()
{
	return ui->objectEditor->getSelectedMatIndex();
}


void MainWindow::objectEditorToObject(WorldObject& ob)
{
	ui->objectEditor->toObject(ob); // Sets changed_flags on object as well.
}


void MainWindow::objectEditorObjectPickedUp()
{
	ui->objectEditor->objectPickedUp();
}


void MainWindow::objectEditorObjectDropped()
{
	 ui->objectEditor->objectDropped();
}


bool MainWindow::snapToGridCheckBoxChecked()
{
	return ui->objectEditor->snapToGridCheckBox->isChecked();
}


double MainWindow::gridSpacing()
{
	return  ui->objectEditor->gridSpacingDoubleSpinBox->value();
}


bool MainWindow::posAndRot3DControlsEnabled()
{
	return ui->objectEditor->posAndRot3DControlsEnabled();
}


void MainWindow::showObjectEditor()
{
	ui->parcelEditor->hide();
	ui->objectEditor->show();
}


void MainWindow::setObjectEditorEnabled(bool enabled)
{
	ui->objectEditor->setEnabled(enabled);
}


struct AvatarNameInfo
{
	std::string name;
	Colour3f colour;

	inline bool operator < (const AvatarNameInfo& other) const { return name < other.name; }
};


void MainWindow::appendChatMessage(const std::string& msg)
{
	ui->chatMessagesTextEdit->append(QtUtils::toQString(msg));
}


void MainWindow::clearChatMessages()
{
	ui->chatMessagesTextEdit->clear();
}


bool MainWindow::isShowParcelsEnabled() const
{
	return ui->actionShow_Parcels->isChecked();
}


void MainWindow::updateOnlineUsersList() // Works off world state avatars.
{
	if(gui_client.world_state.isNull())
		return;

	std::vector<AvatarNameInfo> names;
	{
		Lock lock(gui_client.world_state->mutex);
		for(auto entry : gui_client.world_state->avatars)
		{
			AvatarNameInfo info;
			info.name   = entry.second->name;
			info.colour = entry.second->name_colour;
			names.push_back(info);
		}
	}

	std::sort(names.begin(), names.end());

	// Combine names into a single string, while escaping any HTML chars.
	QString s;
	for(size_t i=0; i<names.size(); ++i)
	{
		s += QtUtils::toQString("<span style=\"color:rgb(" + toString(names[i].colour.r * 255) + ", " + toString(names[i].colour.g * 255) + ", " + toString(names[i].colour.b * 255) + ")\">");
		s += QtUtils::toQString(names[i].name).toHtmlEscaped() + "</span>" + ((i + 1 < names.size()) ? "<br/>" : "");
	}

	ui->onlineUsersTextEdit->setHtml(s);
}


void MainWindow::showHTMLMessageBox(const std::string& title, const std::string& msg)
{
	QMessageBox msgBox;
	msgBox.setTextFormat(Qt::RichText);
	msgBox.setWindowTitle(QtUtils::toQString(title));
	msgBox.setText(QtUtils::toQString(msg));
	msgBox.exec();
}

void MainWindow::showPlainTextMessageBox(const std::string& title, const std::string& msg)
{
	QMessageBox msgBox;
	msgBox.setWindowTitle(QtUtils::toQString(title));
	msgBox.setText(QtUtils::toQString(msg));
	msgBox.exec();
}


// Set up for screenshot-bot type screenshot
void MainWindow::setUpForScreenshot()
{
	if(taking_map_screenshot)
		gui_client.removeParcelObjects();

	// Highlight requested parcel_id
	if(screenshot_highlight_parcel_id != -1)
	{
		Lock lock(gui_client.world_state->mutex);

		gui_client.addParcelObjects();

		auto res = gui_client.world_state->parcels.find(ParcelID(screenshot_highlight_parcel_id));
		if(res != gui_client.world_state->parcels.end())
		{
			// Deselect any existing gl objects
			ui->glWidget->opengl_engine->deselectAllObjects();

			gui_client.selected_parcel = res->second;
			ui->glWidget->opengl_engine->selectObject(gui_client.selected_parcel->opengl_engine_ob);
			ui->glWidget->opengl_engine->setSelectionOutlineColour(PARCEL_OUTLINE_COLOUR);
			ui->glWidget->opengl_engine->setSelectionOutlineWidth(6.0f);
		}
	}

	ui->glWidget->take_map_screenshot = taking_map_screenshot;

	gui_client.gesture_ui.destroy();

	gui_client.minimap.destroy();

	opengl_engine->getCurrentScene()->cloud_shadows = false;

	done_screenshot_setup = true;
}


static ImageMapUInt8Ref convertQImageToImageMapUInt8(const QImage& qimage)
{
	// NOTE: Qt-saved images were doing weird things with parcel border alpha.  Just copy to an ImageMapUInt8 and do the image saving ourselves.
	const int W = qimage.width();
	const int H = qimage.height();
	ImageMapUInt8Ref map = new ImageMapUInt8(W, H, 3);

	const int px_byte_stride = qimage.depth() / 8; // depth = bits per pixel

	// Copy to ImageMapUInt8
	for(int y=0; y<H; ++y)
	{
		const uint8* scanline = qimage.constScanLine(y);

		for(int x=0; x<W; ++x)
		{
			const QRgb src_px = *(const QRgb*)(scanline + px_byte_stride * x);
			uint8* dst_px = map->getPixel(x, y);
			dst_px[0] = (uint8)qRed(src_px);
			dst_px[1] = (uint8)qGreen(src_px);
			dst_px[2] = (uint8)qBlue(src_px);
		}
	}
	
	return map;
}


// For screenshot bot
void MainWindow::saveScreenshot() // Throws glare::Exception on failure
{
	conPrint("Taking screenshot");

#if QT_VERSION_MAJOR >= 6
	QImage framebuffer = ui->glWidget->grabFramebuffer();
#else
	QImage framebuffer = ui->glWidget->grabFrameBuffer();
#endif

	const int target_viewport_w = taking_map_screenshot ? (screenshot_width_px * 2) : (650 * 2); // Existing screenshots are 650 px x 437 px.
	const int target_viewport_h = taking_map_screenshot ? (screenshot_width_px * 2) : (437 * 2); 
	
	if(framebuffer.width() != target_viewport_w)
		throw glare::Exception("saveScreenshot(): framebuffer width was incorrect: actual: " + toString(framebuffer.width()) + ", target: " + toString(target_viewport_w));
	if(framebuffer.height() != target_viewport_h)
		throw glare::Exception("saveScreenshot(): framebuffer height was incorrect: actual: " + toString(framebuffer.height()) + ", target: " + toString(target_viewport_h));
	
	QImage scaled_img = framebuffer.scaledToWidth(screenshot_width_px, Qt::SmoothTransformation);

	// NOTE: Qt-saved images were doing weird things with parcel border alpha.  Just copy to an ImageMapUInt8 and do the image saving ourselves.
	ImageMapUInt8Ref map = convertQImageToImageMapUInt8(scaled_img);

	JPEGDecoder::save(map, screenshot_output_path, JPEGDecoder::SaveOptions(/*quality=*/95));
}


static Vec2f GLCoordsForGLWidgetPos(MainWindow* main_window, const Vec2f widget_pos)
{
	const int vp_width  = main_window->ui->glWidget->opengl_engine->getViewPortWidth();
	const int vp_height = main_window->ui->glWidget->opengl_engine->getViewPortHeight();

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	const double device_pixel_ratio = main_window->ui->glWidget->devicePixelRatio(); // For retina screens this is 2, meaning the gl viewport width is in physical pixels, of which have twice the density of qt pixel coordinates.
	const int use_vp_width  = (int)(vp_width  / device_pixel_ratio);
	const int use_vp_height = (int)(vp_height / device_pixel_ratio);
#else
	const int device_pixel_ratio = main_window->ui->glWidget->devicePixelRatio(); // For retina screens this is 2, meaning the gl viewport width is in physical pixels, of which have twice the density of qt pixel coordinates.
	const int use_vp_width  = vp_width  / device_pixel_ratio;
	const int use_vp_height = vp_height / device_pixel_ratio;
#endif

	return Vec2f(
		 (widget_pos.x - use_vp_width /2) / (use_vp_width /2),
		-(widget_pos.y - use_vp_height/2) / (use_vp_height/2)
	);
}


void MainWindow::timerEvent(QTimerEvent* event)
{
	PERFORMANCEAPI_INSTRUMENT("timerEvent");
	ZoneScoped; // Tracy profiler

	if(closing || in_CEF_message_loop)
		return;

	// We don't want to do the closeEvent stuff in the CEF message loop.  
	// If we got a close event in there, handle it now when we're in the main message loop, and not the CEF message loop.
	assert(!in_CEF_message_loop);
	if(should_close)
	{
		should_close = false;
		this->close();
		return;
	}

	Timer timerEvent_timer;

	in_CEF_message_loop = true;
	CEF::doMessageLoopWork();
	in_CEF_message_loop = false;


	ui->glWidget->makeCurrent(); // Need to make this gl widget context current, before we execute OpenGL calls in processLoading.


	const QPoint mouse_point = ui->glWidget->mapFromGlobal(QCursor::pos());

	MouseCursorState mouse_cursor_state;
	mouse_cursor_state.cursor_pos = Vec2i(mouse_point.x(), mouse_point.y());
	mouse_cursor_state.gl_coords =  GLCoordsForGLWidgetPos(this, Vec2f((float)mouse_point.x(), (float)mouse_point.y()));

	// NOTE: Stupid qt: QApplication::keyboardModifiers() doesn't update properly when just CTRL is pressed/released, without any other events.
	// So use GetAsyncKeyState on Windows, since it actually works.
#if defined(_WIN32)
	const bool ctrl_key_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool alt_key_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; // alt = VK_MENU
#else
	const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
	const bool ctrl_key_down = (modifiers & Qt::ControlModifier) != 0;
	const bool alt_key_down  = (modifiers & Qt::AltModifier)     != 0;
#endif
	mouse_cursor_state.alt_key_down = alt_key_down;
	mouse_cursor_state.ctrl_key_down = ctrl_key_down;
	gui_client.timerEvent(mouse_cursor_state);

	updateDiagnostics();
	
	updateStatusBar();

	runScreenshotCode();
	
	const double cur_time = Clock::getTimeSinceInit(); // Used for animation, interpolation etc..

	//------------- Check to see if we should remove any old notifications ------------
	const double notification_display_time = 5;
	for(auto it = notifications.begin(); it != notifications.end();)
	{
		if(cur_time >  it->creation_time + notification_display_time)
		{
			// Remove the notification
			ui->notificationContainer->layout()->removeWidget(it->label);
			it->label->deleteLater();
			it = notifications.erase(it); // remove from list

			// Make the info dock widget resize.  See https://stackoverflow.com/a/30472749/7495926
			QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
			ui->infoDockWidget->resize(ui->infoDockWidget->sizeHint());

			// Hide the info dock widget if there are no remaining widgets.
			if(notifications.empty())
				ui->infoDockWidget->hide();
		}		
		else
			++it;
	}


	// Update URL Bar
	// NOTE: use doubleToStringNDecimalPlaces instead of doubleToStringMaxNDecimalPlaces, as the latter is distracting due to flickering URL length when moving.
	if(this->url_widget->shouldBeUpdated())
		this->url_widget->setURL("sub://" + gui_client.server_hostname + "/" + gui_client.server_worldname +
			"?x=" + doubleToStringNDecimalPlaces(gui_client.cam_controller.getFirstPersonPosition().x, 1) + 
			"&y=" + doubleToStringNDecimalPlaces(gui_client.cam_controller.getFirstPersonPosition().y, 1) +
			"&z=" + doubleToStringNDecimalPlaces(gui_client.cam_controller.getFirstPersonPosition().z, 1));

	const QPoint gl_pos = ui->glWidget->mapToGlobal(QPoint(200, 10));
	if(ui->infoDockWidget->geometry().topLeft() != gl_pos)
	{
		// conPrint("Positioning ui->infoDockWidget at " + toString(gl_pos.x()) + ", " + toString(gl_pos.y()));
		ui->infoDockWidget->setGeometry(gl_pos.x(), gl_pos.y(), 300, 1);
	}
	

	if(need_help_info_dock_widget_position)
	{
		// Position near bottom right corner of glWidget.
		ui->helpInfoDockWidget->setGeometry(QRect(ui->glWidget->mapToGlobal(ui->glWidget->geometry().bottomRight() + QPoint(-320, -120)), QSize(300, 100)));
		need_help_info_dock_widget_position = false;
	}

	last_timerEvent_CPU_work_elapsed = timerEvent_timer.elapsed();

	/*if(last_timerEvent_CPU_work_elapsed > 0.010)
	{
		logMessage("=============Long frame==================");
		logMessage("Frame CPU time: " + doubleToStringNSigFigs(last_timerEvent_CPU_work_elapsed * 1.0e3, 4) + " ms");
		logMessage("loading time: " + doubleToStringNSigFigs(frame_loading_time * 1.0e3, 4) + " ms");
		for(size_t i=0; i<loading_times.size(); ++i)
			logMessage("\t" + loading_times[i]);
		//conPrint("\tprocessing animated textures took " + doubleToStringNSigFigs(animated_tex_time * 1.0e3, 4) + " ms");
	}*/

	ui->glWidget->makeCurrent();

	//Timer timer;
	{
		Timer timer2;
		PERFORMANCEAPI_INSTRUMENT("updateGL()");
		ZoneScopedN("updateGL"); // Tracy profiler
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
		ui->glWidget->update();
#else
		ui->glWidget->updateGL();
#endif
		//if(timer.elapsed() > 0.020)
		//	conPrint(doubleToStringNDecimalPlaces(Clock::getTimeSinceInit(), 3) + ": updateGL() took " + timer.elapsedStringNSigFigs(4));
		this->last_updateGL_time = timer2.elapsed();
	}
}


void MainWindow::updateDiagnostics()
{
	if(ui->diagnosticsDockWidget->isVisible() && (gui_client.num_frames_since_fps_timer_reset == 1))
	{
		ZoneScopedN("diagnostics"); // Tracy profiler

		//const double fps = num_frames / (double)fps_display_timer.elapsed();
		
		const bool do_graphics_diagnostics = ui->diagnosticsWidget->graphicsDiagnosticsCheckBox->isChecked();
		const bool do_physics_diagnostics = ui->diagnosticsWidget->physicsDiagnosticsCheckBox->isChecked();

		const std::string msg = gui_client.getDiagnosticsString(do_graphics_diagnostics, do_physics_diagnostics, last_timerEvent_CPU_work_elapsed, last_updateGL_time);

		// Don't update diagnostics string when part of it is selected, so user can actually copy it.
		if(!ui->diagnosticsWidget->diagnosticsTextEdit->textCursor().hasSelection())
			ui->diagnosticsWidget->diagnosticsTextEdit->setPlainText(QtUtils::toQString(msg));
	}
}


void MainWindow::runScreenshotCode()
{
	if(run_as_screenshot_slave || test_screenshot_taking)
	{
		if(screenshot_output_path.empty()) // If we don't have a screenshot command we are currently executing:
		{
			try
			{
				if(test_screenshot_taking || screenshot_command_socket->readable(/*timeout (s)=*/0.01))
				{
					conPrint("Reading command from screenshot_command_socket etc...");
					const std::string command = test_screenshot_taking ? "takescreenshot" : screenshot_command_socket->readStringLengthFirst(1000);
					conPrint("Read screenshot command: " + command);
					if(command == "takescreenshot")
					{
						if(test_screenshot_taking)
						{
							screenshot_campos = Vec3d(0, -1, 100);
							screenshot_camangles = Vec3d(0, 2.5f, 0); // (heading, pitch, roll).
							screenshot_width_px = 1024;
							screenshot_highlight_parcel_id = 10;
							screenshot_output_path = "test_screenshot.jpg";

							screenshot_ortho_sensor_width_m = 100;
							taking_map_screenshot = true;
						}
						else
						{
							screenshot_campos.x = screenshot_command_socket->readDouble();
							screenshot_campos.y = screenshot_command_socket->readDouble();
							screenshot_campos.z = screenshot_command_socket->readDouble();
							screenshot_camangles.x = screenshot_command_socket->readDouble();
							screenshot_camangles.y = screenshot_command_socket->readDouble();
							screenshot_camangles.z = screenshot_command_socket->readDouble();
							screenshot_width_px = screenshot_command_socket->readInt32();
							screenshot_highlight_parcel_id = screenshot_command_socket->readInt32();
							screenshot_output_path = screenshot_command_socket->readStringLengthFirst(1000);
							taking_map_screenshot = false;
						}
					}
					else if(command == "takemapscreenshot")
					{
						int tile_x, tile_y, tile_z;
						if(test_screenshot_taking)
						{
							tile_x = 0;
							tile_y = 0;
							tile_z = 7;
							screenshot_output_path = "test_screenshot.jpg";
						}
						else
						{
							tile_x = screenshot_command_socket->readInt32();
							tile_y = screenshot_command_socket->readInt32();
							tile_z = screenshot_command_socket->readInt32();
							screenshot_output_path = screenshot_command_socket->readStringLengthFirst(1000);
						}

						const int TILE_WIDTH_PX = 256; // Works the easiest with leaflet.js
						const float TILE_WIDTH_M = 5120.f / (1 << tile_z);
						screenshot_campos = Vec3d(
							(tile_x + 0.5) * TILE_WIDTH_M,
							(tile_y + 0.5) * TILE_WIDTH_M,
							200.0
						);
						screenshot_camangles = Vec3d(
							0, // Heading
							3.14, // pitch
							0 // roll
						);
						screenshot_ortho_sensor_width_m = TILE_WIDTH_M;
						screenshot_width_px = TILE_WIDTH_PX;
						screenshot_highlight_parcel_id = -1;
						taking_map_screenshot = true;
					}
					else if(command == "quit")
					{
						conPrint("Received quit command, exiting...");
						exit(1);
					}
					else
						throw glare::Exception("received invalid screenshot command.");
				}
			}
			catch(glare::Exception& e)
			{
				conPrint("Excep while reading screenshot command from screenshot_command_socket: " + e.what() + ", exiting!");
				//QMessageBox msgBox;
				//msgBox.setWindowTitle("Error");
				//msgBox.setText(QtUtils::toQString("Excep while reading screenshot command from screenshot_command_socket: " + e.what()));
				//msgBox.exec();
				exit(1);
			}
		}
	}
	if(!screenshot_output_path.empty() && gui_client.world_state.nonNull())
	{
		if(!screenshot_output_path.empty()) // If we are in screenshot-taking mode:
		{
			gui_client.cam_controller.setPosition(screenshot_campos);
			gui_client.cam_controller.setAngles(screenshot_camangles);
			gui_client.player_physics.setPosition(screenshot_campos);

			// Enable fly mode so we don't just fall to the ground
			ui->actionFly_Mode->setChecked(true);
			gui_client.player_physics.setFlyModeEnabled(true);
			gui_client.cam_controller.setThirdPersonEnabled(false);
			ui->actionThird_Person_Camera->setChecked(false);
		}

		size_t num_obs;
		{
			Lock lock(gui_client.world_state->mutex);
			num_obs = gui_client.world_state->objects.size();
		}

		const bool map_screenshot = taking_map_screenshot;//parsed_args.isArgPresent("--takemapscreenshot");

		ui->glWidget->take_map_screenshot = map_screenshot;
		ui->glWidget->screenshot_ortho_sensor_width_m = screenshot_ortho_sensor_width_m;

		const size_t num_model_and_tex_tasks = gui_client.load_item_queue.size() + gui_client.model_and_texture_loader_task_manager.getNumUnfinishedTasks() + gui_client.model_loaded_messages_to_process.size();

		if(time_since_last_waiting_msg.elapsed() > 2.0)
		{
			conPrint("---------------Waiting for loading to be done for screenshot ---------------");
			printVar(num_obs);
			printVar(num_model_and_tex_tasks);
			printVar(gui_client.num_non_net_resources_downloading);
			printVar(gui_client.num_net_resources_downloading);

			time_since_last_waiting_msg.reset();
		}

		const bool loaded_all =
			(time_since_last_screenshot.elapsed() > 4.0) && // Bit of a hack to allow time for the shadow mapping to render properly
			(num_obs > 0 || total_timer.elapsed() >= 15) && // Wait until we have downloaded some objects from the server, or (if the world is empty) X seconds have elapsed.
			(total_timer.elapsed() >= 8) && // Bit of a hack to allow time for the shadow mapping to render properly, also for the initial object query responses to arrive
			(num_model_and_tex_tasks == 0) &&
			(gui_client.num_non_net_resources_downloading == 0) &&
			(gui_client.num_net_resources_downloading == 0) &&
			(gui_client.terrain_system.nonNull() && gui_client.terrain_system->isTerrainFullyBuilt());

		if(loaded_all)
		{
			if(!done_screenshot_setup)
			{
				conPrint("Setting up for screenshot...");

				ui->editorDockWidget->hide();
				ui->chatDockWidget->hide();
				ui->diagnosticsDockWidget->hide();

				const int target_viewport_w = map_screenshot ? (screenshot_width_px * 2) : (650 * 2); // Existing screenshots are 650 px x 437 px.
				const int target_viewport_h = map_screenshot ? (screenshot_width_px * 2) : (437 * 2);

				conPrint("Setting geometry size...");
				// Make the gl widget a certain size so that the screenshot size / aspect ratio is consistent.
				ui->glWidget->setGeometry(0, 0, target_viewport_w, target_viewport_h);
				setUpForScreenshot();
			}
			else
			{
				try
				{
					saveScreenshot();

					// Reset screenshot state
					screenshot_output_path.clear();
					done_screenshot_setup = false;

					time_since_last_screenshot.reset();

					if(screenshot_command_socket.nonNull())
					{
						screenshot_command_socket->writeInt32(0); // Write success msg
						screenshot_command_socket->writeStringLengthFirst("Success!");
					}
				}
				catch(glare::Exception& e)
				{
					conPrint("Excep while saving screenshot: " + e.what());

					// Reset screenshot state
					screenshot_output_path.clear();
					done_screenshot_setup = false;

					time_since_last_screenshot.reset();

					if(screenshot_command_socket.nonNull())
					{
						screenshot_command_socket->writeInt32(1); // Write failure msg
						screenshot_command_socket->writeStringLengthFirst("Exception encountered: " + e.what());
					}
				}
			}
		}
	}
}


static std::string printableServerURL(const std::string& hostname, const std::string& userpath)
{
	if(userpath.empty())
		return hostname;
	else
		return hostname + "/" + userpath;
}


void MainWindow::updateStatusBar()
{
	ZoneScoped; // Tracy profiler

	std::string status;
	switch(gui_client.connection_state)
	{
	case GUIClient::ServerConnectionState_NotConnected:
		status += "Not connected to server.";
		break;
	case GUIClient::ServerConnectionState_Connecting:
		status += "Connecting to " + printableServerURL(gui_client.server_hostname, gui_client.server_worldname) + "...";
		break;
	case GUIClient::ServerConnectionState_Connected:
		status += "Connected to " + printableServerURL(gui_client.server_hostname, gui_client.server_worldname);
		break;
	}

	const int total_num_non_net_resources_downloading = (int)gui_client.num_non_net_resources_downloading + (int)gui_client.download_queue.size();
	if(total_num_non_net_resources_downloading > 0)
		status += " | Downloading " + toString(total_num_non_net_resources_downloading) + ((total_num_non_net_resources_downloading == 1) ? " resource..." : " resources...");

	if(gui_client.num_net_resources_downloading > 0)
		status += " | Downloading " + toString(gui_client.num_net_resources_downloading) + ((gui_client.num_net_resources_downloading == 1) ? " web resource..." : " web resources...");

	if(gui_client.num_resources_uploading > 0)
		status += " | Uploading " + toString(gui_client.num_resources_uploading) + ((gui_client.num_resources_uploading == 1) ? " resource..." : " resources...");

	const size_t num_model_and_tex_tasks = gui_client.load_item_queue.size() + gui_client.model_and_texture_loader_task_manager.getNumUnfinishedTasks() + (gui_client.model_loaded_messages_to_process.size() + gui_client.texture_loaded_messages_to_process.size());
	if(num_model_and_tex_tasks > 0)
		status += " | Loading " + toString(num_model_and_tex_tasks) + ((num_model_and_tex_tasks == 1) ? " model or texture..." : " models and textures...");

	this->statusBar()->showMessage(QtUtils::toQString(status));
}


static void enqueueMessageToSend(ClientThread& client_thread, SocketBufferOutStream& packet)
{
	MessageUtils::updatePacketLengthField(packet);

	client_thread.enqueueDataToSend(packet.buf);
}


void MainWindow::on_actionAvatarSettings_triggered()
{
	AvatarSettingsDialog dialog(this->base_dir_path, this->settings, gui_client.texture_server, gui_client.resource_manager);
	const int res = dialog.exec();
	ui->glWidget->makeCurrent();// Change back from the dialog GL context to the mainwindow GL context.

	if((res == QDialog::Accepted) && dialog.loaded_mesh.nonNull()) //  loaded_object.nonNull()) // If the dialog was accepted, and we loaded something:
	{
		try
		{
			if(!gui_client.logged_in_user_id.valid())
				throw glare::Exception("You must be logged in to set your avatar model");

			std::string mesh_URL;
			if(dialog.result_path != "")
			{
				// If the user selected a mesh that is not a bmesh, convert it to bmesh
				std::string bmesh_disk_path = dialog.result_path;
				if(!hasExtension(dialog.result_path, "bmesh"))
				{
					// Save as bmesh in temp location
					bmesh_disk_path = PlatformUtils::getTempDirPath() + "/temp.bmesh";

					BatchedMesh::WriteOptions write_options;
					write_options.compression_level = 9; // Use a somewhat high compression level, as this mesh is likely to be read many times, and only encoded here.
					// TODO: show 'processing...' dialog while it compresses and saves?
					dialog.loaded_mesh->writeToFile(bmesh_disk_path, write_options);
				}
				else
				{
					bmesh_disk_path = dialog.result_path;
				}

				// Compute hash over model
				const uint64 model_hash = FileChecksum::fileChecksum(bmesh_disk_path);

				const std::string original_filename = FileUtils::getFilename(dialog.result_path); // Use the original filename, not 'temp.igmesh'.
				mesh_URL = ResourceManager::URLForNameAndExtensionAndHash(original_filename, ::getExtension(bmesh_disk_path), model_hash); // ResourceManager::URLForPathAndHash(igmesh_disk_path, model_hash);

				// Copy model to local resources dir.  UploadResourceThread will read from here.
				gui_client.resource_manager->copyLocalFileToResourceDir(bmesh_disk_path, mesh_URL);
			}

			const Vec3d cam_angles = gui_client.cam_controller.getAngles();
			Avatar avatar;
			avatar.uid = gui_client.client_avatar_uid;
			avatar.pos = Vec3d(gui_client.cam_controller.getFirstPersonPosition());
			avatar.rotation = Vec3f(0, (float)cam_angles.y, (float)cam_angles.x);
			avatar.name = gui_client.logged_in_user_name;
			avatar.avatar_settings.model_url = mesh_URL;
			avatar.avatar_settings.pre_ob_to_world_matrix = dialog.pre_ob_to_world_matrix;
			avatar.avatar_settings.materials = dialog.loaded_materials;


			// Copy all dependencies (textures etc..) to resources dir.  UploadResourceThread will read from here.
			std::set<DependencyURL> paths;
			avatar.getDependencyURLSet(/*ob_lod_level=*/0, paths);
			for(auto it = paths.begin(); it != paths.end(); ++it)
			{
				const std::string path = it->URL;
				if(FileUtils::fileExists(path))
				{
					const uint64 hash = FileChecksum::fileChecksum(path);
					const std::string resource_URL = ResourceManager::URLForPathAndHash(path, hash);
					gui_client.resource_manager->copyLocalFileToResourceDir(path, resource_URL);
				}
			}

			// Convert texture paths on the object to URLs
			avatar.convertLocalPathsToURLS(*gui_client.resource_manager);

			if(!gui_client.task_manager)
				gui_client.task_manager = new glare::TaskManager("mainwindow general task manager", myClamp<size_t>(PlatformUtils::getNumLogicalProcessors() / 2, 1, 8)), // Currently just used for LODGeneration::generateLODTexturesForMaterialsIfNotPresent().

			// Generate LOD textures for materials, if not already present on disk.
			LODGeneration::generateLODTexturesForMaterialsIfNotPresent(avatar.avatar_settings.materials, *gui_client.resource_manager, *gui_client.task_manager);

			// Send AvatarFullUpdate message to server
			MessageUtils::initPacket(gui_client.scratch_packet, Protocol::AvatarFullUpdate);
			writeAvatarToNetworkStream(avatar, gui_client.scratch_packet);

			enqueueMessageToSend(*gui_client.client_thread, gui_client.scratch_packet);

			showInfoNotification("Updated avatar.");
		}
		catch(glare::Exception& e)
		{
			// Show error
			print(e.what());
			QErrorMessage m;
			m.showMessage(QtUtils::toQString(e.what()));
			m.exec();
		}
	}
}


void MainWindow::on_actionAddObject_triggered()
{
	const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f;

	// Check permissions
	bool ob_pos_in_parcel;
	const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
	if(!have_creation_perms)
	{
		if(ob_pos_in_parcel)
			showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
		else
			showErrorNotification("You can only create objects in a parcel that you have write permissions for.");
		return;
	}

	AddObjectDialog dialog(this->base_dir_path, this->settings, gui_client.texture_server, gui_client.resource_manager, 
#ifdef _WIN32
		this->device_manager.ptr
#else
		NULL
#endif
	);
	const int res = dialog.exec();
	ui->glWidget->makeCurrent(); // Change back from the dialog GL context to the mainwindow GL context.

	if((res == QDialog::Accepted) && !dialog.loaded_materials.empty()) // If dialog was accepted, and we loaded an object successfully in it:
	{
		try
		{
			const Vec3d adjusted_ob_pos = ob_pos + gui_client.cam_controller.getRightVec() * dialog.ob_cam_right_translation + gui_client.cam_controller.getUpVec() * dialog.ob_cam_up_translation; // Centre object in front of camera

			// Some mesh types have a rotation to bring them to our z-up convention.  Don't change the rotation on those.
			Vec3f axis(0, 0, 1);
			float angle = 0;
			if(dialog.axis == Vec3f(0, 0, 1))
			{
				// If we don't have a rotation to z-up, make object face camera.
				angle = Maths::roundToMultipleFloating((float)gui_client.cam_controller.getAngles().x - Maths::pi_2<float>(), Maths::pi_4<float>()); // Round to nearest 45 degree angle, facing camera.
			}
			else
			{
				axis = dialog.axis;
				angle = dialog.angle;
			}

			gui_client.createObject(
				dialog.result_path,
				dialog.loaded_mesh,
				dialog.loaded_mesh_is_image_cube,
				dialog.loaded_voxels,
				adjusted_ob_pos,
				dialog.scale,
				axis,
				angle,
				dialog.loaded_materials
			);
		}
		catch(glare::Exception& e)
		{
			// Show error
			print(e.what());
			QErrorMessage m;
			m.showMessage(QtUtils::toQString(e.what()));
			m.exec();
		}
	}
}


void MainWindow::on_actionAddHypercard_triggered()
{
	const float quad_w = 0.4f;
	const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f -
		gui_client.cam_controller.getUpVec() * quad_w * 0.5f -
		gui_client.cam_controller.getRightVec() * quad_w * 0.5f;

	// Check permissions
	bool ob_pos_in_parcel;
	const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
	if(!have_creation_perms)
	{
		if(ob_pos_in_parcel)
			showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
		else
			showErrorNotification("You can only create hypercards in a parcel that you have write permissions for.");
		return;
	}

	WorldObjectRef new_world_object = new WorldObject();
	new_world_object->uid = UID(0); // Will be set by server
	new_world_object->object_type = WorldObject::ObjectType_Hypercard;
	new_world_object->pos = ob_pos;
	new_world_object->axis = Vec3f(0, 0, 1);
	new_world_object->angle = Maths::roundToMultipleFloating((float)gui_client.cam_controller.getAngles().x - Maths::pi_2<float>(), Maths::pi_4<float>()); // Round to nearest 45 degree angle.
	new_world_object->scale = Vec3f(0.4f);
	new_world_object->content = "Select the object \nto edit this text";
	new_world_object->setAABBOS(js::AABBox(Vec4f(0,0,0,1), Vec4f(1,0,1,1)));

	// Send CreateObject message to server
	{
		MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
		new_world_object->writeToNetworkStream(scratch_packet);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}

	showInfoNotification("Added hypercard.");
}


void MainWindow::on_actionAdd_Spotlight_triggered()
{
	const float quad_w = 0.4f;
	const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f -
		gui_client.cam_controller.getUpVec() * quad_w * 0.5f -
		gui_client.cam_controller.getRightVec() * quad_w * 0.5f;

	// Check permissions
	bool ob_pos_in_parcel;
	const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
	if(!have_creation_perms)
	{
		if(ob_pos_in_parcel)
			showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
		else
			showErrorNotification("You can only create spotlights in a parcel that you have write permissions for.");
		return;
	}

	WorldObjectRef new_world_object = new WorldObject();
	new_world_object->uid = UID(0); // Will be set by server
	new_world_object->object_type = WorldObject::ObjectType_Spotlight;
	new_world_object->pos = ob_pos;
	new_world_object->axis = Vec3f(0, 0, 1);
	new_world_object->angle = 0;
	new_world_object->scale = Vec3f(1.f);

	// Emitting material
	new_world_object->materials.push_back(new WorldMaterial());
	new_world_object->materials.back()->emission_lum_flux_or_lum = 100000.f;

	// Spotlight housing material
	new_world_object->materials.push_back(new WorldMaterial());

	const float fixture_w = 0.1;
	const js::AABBox aabb_os = js::AABBox(Vec4f(-fixture_w/2, -fixture_w/2, 0,1), Vec4f(fixture_w/2,  fixture_w/2, 0,1));
	new_world_object->setAABBOS(aabb_os);


	// Send CreateObject message to server
	{
		MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
		new_world_object->writeToNetworkStream(scratch_packet);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}

	showInfoNotification("Added spotlight.");
}


void MainWindow::on_actionAdd_Web_View_triggered()
{
	const float quad_w = 0.4f;
	const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f -
		gui_client.cam_controller.getUpVec() * quad_w * 0.5f -
		gui_client.cam_controller.getRightVec() * quad_w * 0.5f;

	// Check permissions
	bool ob_pos_in_parcel;
	const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
	if(!have_creation_perms)
	{
		if(ob_pos_in_parcel)
			showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
		else
			showErrorNotification("You can only create web views in a parcel that you have write permissions for.");
		return;
	}

	WorldObjectRef new_world_object = new WorldObject();
	new_world_object->uid = UID(0); // Will be set by server
	new_world_object->object_type = WorldObject::ObjectType_WebView;
	new_world_object->pos = ob_pos;
	new_world_object->axis = Vec3f(0, 0, 1);
	new_world_object->angle = Maths::roundToMultipleFloating((float)gui_client.cam_controller.getAngles().x - Maths::pi_2<float>(), Maths::pi_4<float>()); // Round to nearest 45 degree angle.
	new_world_object->scale = Vec3f(/*width=*/1.f, /*depth=*/0.02f, /*height=*/1080.f / 1920.f);
	new_world_object->max_model_lod_level = 0;

	new_world_object->target_url = "https://substrata.info/"; // Use a default URL - indicates to users how to set the URL.

	new_world_object->materials.resize(2);
	new_world_object->materials[0] = new WorldMaterial();
	new_world_object->materials[0]->colour_rgb = Colour3f(1.f);
	new_world_object->materials[1] = new WorldMaterial();

	const js::AABBox aabb_os = gui_client.image_cube_shape.getAABBOS();
	new_world_object->setAABBOS(aabb_os);


	// Send CreateObject message to server
	{
		MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
		new_world_object->writeToNetworkStream(scratch_packet);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}

	showInfoNotification("Added web view.");
}


void MainWindow::on_actionAdd_Video_triggered()
{
	try
	{
		const float quad_w = 0.4f;
		const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f -
			gui_client.cam_controller.getUpVec() * quad_w * 0.5f -
			gui_client.cam_controller.getRightVec() * quad_w * 0.5f;

		// Check permissions
		bool ob_pos_in_parcel;
		const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
		if(!have_creation_perms)
		{
			if(ob_pos_in_parcel)
				showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
			else
				showErrorNotification("You can only create videos in a parcel that you have write permissions for.");
			return;
		}

		AddVideoDialog dialog(this->settings, gui_client.resource_manager, 
#ifdef _WIN32
			this->device_manager.ptr
#else
			NULL
#endif
		);
		const int res = dialog.exec();

		if(res == QDialog::Accepted)
		{
			std::string use_URL;
			if(dialog.wasResultLocalPath())
			{
				if(dialog.getVideoLocalPath() == "")
					return;

				// Copy model to local resources dir if not already there.  UploadResourceThread will read from here.
				use_URL = gui_client.resource_manager->copyLocalFileToResourceDirIfNotPresent(dialog.getVideoLocalPath());
			}
			else
			{
				if(dialog.getVideoURL() == "")
					return;

				use_URL = dialog.getVideoURL();
			}

			WorldObjectRef new_world_object = new WorldObject();
			new_world_object->uid = UID(0); // Will be set by server
			new_world_object->object_type = WorldObject::ObjectType_Video;
			new_world_object->pos = ob_pos;
			new_world_object->axis = Vec3f(0, 0, 1);
			new_world_object->angle = Maths::roundToMultipleFloating((float)gui_client.cam_controller.getAngles().x - Maths::pi_2<float>(), Maths::pi_4<float>()); // Round to nearest 45 degree angle.
			new_world_object->scale = Vec3f(/*width=*/1.f, /*depth=*/0.02f, /*height=*/(float)dialog.video_height / dialog.video_width); // NOTE: matches Youtube aspect ratio of 16:9.
			new_world_object->max_model_lod_level = 0;

			BitUtils::setBit(new_world_object->flags, WorldObject::VIDEO_AUTOPLAY);

			new_world_object->materials.resize(2);
			new_world_object->materials[0] = new WorldMaterial();
			new_world_object->materials[0]->colour_rgb = Colour3f(0.f);
			new_world_object->materials[0]->emission_lum_flux_or_lum = 20000; // NOTE: this material data is generally not used in loadModelForObject() for ObjectType_Video.
			new_world_object->materials[0]->emission_rgb = Colour3f(1.f);
			new_world_object->materials[0]->emission_texture_url = use_URL; // Video URL is stored in emission_texture_url. (need to put here so it is a resource dependency)
			new_world_object->materials[1] = new WorldMaterial();

			const js::AABBox aabb_os = gui_client.image_cube_shape.getAABBOS();
			new_world_object->setAABBOS(aabb_os);


			// Send CreateObject message to server
			{
				MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
				new_world_object->writeToNetworkStream(scratch_packet);

				enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
			}

			showInfoNotification("Added video.");
		}
	}
	catch(glare::Exception& e)
	{
		// Show error
		print(e.what());
		QErrorMessage m;
		m.showMessage(QtUtils::toQString(e.what()));
		m.exec();
	}
}


void MainWindow::on_actionAdd_Audio_Source_triggered()
{
	try
	{
		const float quad_w = 0.0f;
		const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f -
			gui_client.cam_controller.getUpVec() * quad_w * 0.5f -
			gui_client.cam_controller.getRightVec() * quad_w * 0.5f;

		// Check permissions
		bool ob_pos_in_parcel;
		const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
		if(!have_creation_perms)
		{
			if(ob_pos_in_parcel)
				showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
			else
				showErrorNotification("You can only create audio sources in a parcel that you have write permissions for.");
			return;
		}

		const QString last_audio_dir = settings->value("mainwindow/lastAudioFileDir").toString();

		QFileDialog::Options options;
		QString selected_filter;
		const QString selected_filename = QFileDialog::getOpenFileName(this,
			tr("Select audio file..."),
			last_audio_dir,
			tr("Audio file (*.mp3 *.wav)"), // tr("Audio file (*.mp3 *.m4a *.aac *.wav)"),
			&selected_filter,
			options
		);

		if(selected_filename != "")
		{
			settings->setValue("mainwindow/lastAudioFileDir", QtUtils::toQString(FileUtils::getDirectory(QtUtils::toIndString(selected_filename))));

			const std::string path = QtUtils::toStdString(selected_filename);

			// Compute hash over audio file
			const uint64 audio_file_hash = FileChecksum::fileChecksum(path);

			const std::string audio_file_URL = ResourceManager::URLForPathAndHash(path, audio_file_hash);

			// Copy audio file to local resources dir.  UploadResourceThread will read from here.
			gui_client.resource_manager->copyLocalFileToResourceDir(path, audio_file_URL);

			const std::string model_obj_path = base_dir_path + "/resources/models/Capsule.obj";
			const std::string model_URL = "Capsule_obj_7611321750126528672.bmesh"; // NOTE: Assuming server already has capsule mesh as a resource

		
			WorldObjectRef new_world_object = new WorldObject();
			new_world_object->uid = UID(0); // Will be set by server
			new_world_object->object_type = WorldObject::ObjectType_Generic;
			new_world_object->pos = ob_pos;
			new_world_object->axis = Vec3f(0, 0, 1);
			new_world_object->angle = 0;
			new_world_object->scale = Vec3f(0.2f);
			new_world_object->model_url = model_URL;
			new_world_object->audio_source_url = audio_file_URL;
			new_world_object->materials.resize(1);
			new_world_object->materials[0] = new WorldMaterial();
			new_world_object->materials[0]->colour_rgb = Colour3f(1,0,0);

			// Set aabb_ws
			
			/*WorldObject loaded_object;
			BatchedMeshRef batched_mesh;
			ModelLoading::makeGLObjectForModelFile(task_manager, model_obj_path, batched_mesh, loaded_object);
			if(batched_mesh.nonNull())
			{
				const js::AABBox aabb_os = batched_mesh->aabb_os;
				new_world_object->aabb_ws = aabb_os.transformedAABB(obToWorldMatrix(*new_world_object));
			}*/

			const js::AABBox aabb_os(Vec4f(-0.25f, -0.25f, -0.5f, 1.0f), Vec4f(0.25f, 0.25f, 0.5f, 1.0f)); // AABB os of capsule.obj
			new_world_object->setAABBOS(aabb_os);


			// Send CreateObject message to server
			{
				MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
				new_world_object->writeToNetworkStream(scratch_packet);

				enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
			}

			showInfoNotification("Added audio source.");
		}
	}
	catch(glare::Exception& e)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Error");
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


void MainWindow::on_actionAdd_Voxels_triggered()
{
	// Offset down by 0.25 to allow for centering with voxel width of 0.5.
	const Vec3d ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * 2.0f - Vec3d(0.25, 0.25, 0.25);

	// Check permissions
	bool ob_pos_in_parcel;
	const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(ob_pos, ob_pos_in_parcel);
	if(!have_creation_perms)
	{
		if(ob_pos_in_parcel)
			showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
		else
			showErrorNotification("You can only create objects in a parcel that you have write permissions for.");
		return;
	}

	
	WorldObjectRef new_world_object = new WorldObject();
	new_world_object->uid = UID(0); // Will be set by server
	new_world_object->object_type = WorldObject::ObjectType_VoxelGroup;
	new_world_object->materials.resize(1);
	new_world_object->materials[0] = new WorldMaterial();
	new_world_object->pos = ob_pos;
	new_world_object->axis = Vec3f(0, 0, 1);
	new_world_object->angle = 0;
	new_world_object->scale = Vec3f(0.5f); // This will be the initial width of the voxels
	new_world_object->getDecompressedVoxels().push_back(Voxel(Vec3<int>(0, 0, 0), 0)); // Start with a single voxel.
	new_world_object->compressVoxels();
	new_world_object->setAABBOS(new_world_object->getDecompressedVoxelGroup().getAABB());

	// Send CreateObject message to server
	{
		MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
		new_world_object->writeToNetworkStream(scratch_packet);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}

	showInfoNotification("Voxel Object created.");

	// Deselect any currently selected object
	gui_client.deselectObject();
}


void MainWindow::on_actionCopy_Object_triggered()
{
	if(gui_client.selected_ob.nonNull())
	{
		QClipboard* clipboard = QGuiApplication::clipboard();
		QMimeData* mime_data = new QMimeData();

		BufferOutStream temp_buf;
		gui_client.selected_ob->writeToStream(temp_buf);

		mime_data->setData(/*mime-type:*/"x-substrata-object-binary", QByteArray((const char*)temp_buf.buf.data(), (int)temp_buf.buf.size()));
		clipboard->setMimeData(mime_data);
	}
}


void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if(event->mimeData()->hasImage() || event->mimeData()->hasUrls())
		event->acceptProposedAction();
}


void MainWindow::dropEvent(QDropEvent* event)
{
	handlePasteOrDropMimeData(event->mimeData());
}


void MainWindow::handlePasteOrDropMimeData(const QMimeData* mime_data)
{
	try
	{
		// const QStringList formats = mime_data->formats();
		// for(auto it = formats.begin(); it != formats.end(); ++it)
		// 	conPrint("Format: " + it->toStdString());

		if(mime_data)
		{
			if(mime_data->hasUrls())
			{
				const QList<QUrl> urls = mime_data->urls();

				std::string image_path_to_load;
				std::string model_path_to_load;
				for(auto it = urls.begin(); it != urls.end(); ++it)
				{
					const std::string url = it->toString().toStdString();
					if(hasPrefix(url, "file:///"))
					{
						const std::string path = eatPrefix(url, "file:///");

						if(FileUtils::fileExists(path))
						{
							if(ImageDecoding::hasSupportedImageExtension(path))
								image_path_to_load = path;

							if(ModelLoading::hasSupportedModelExtension(path))
								model_path_to_load = path;
						}
					}
				}

				if(!image_path_to_load.empty())
					gui_client.createImageObject(image_path_to_load);

				if(!model_path_to_load.empty())
					gui_client.createModelObject(model_path_to_load);

				if(image_path_to_load.empty() && model_path_to_load.empty())
					throw glare::Exception("Pasted files / URLs did not contain a supported image or model format.");
			}
			else if(mime_data->hasFormat("x-substrata-object-binary")) // Binary encoded substrata object, from a user copying a substrata object.
			{
				const QByteArray ob_data = mime_data->data("x-substrata-object-binary");

				// Copy QByteArray to BufferInStream
				BufferInStream in_stream_buf;
				in_stream_buf.buf.resize(ob_data.size());
				if(ob_data.size() > 0)
					std::memcpy(in_stream_buf.buf.data(), ob_data.data(), ob_data.size());

				try
				{
					// Deserialise object
					WorldObjectRef pasted_ob = new WorldObject();
					readWorldObjectFromStream(in_stream_buf, *pasted_ob);

					// Choose a position for the pasted object.
					Vec3d new_ob_pos;
					if(pasted_ob->pos.getDist(gui_client.cam_controller.getFirstPersonPosition()) > 50.0) // If the source object is far from the camera:
					{
						// Position pasted object in front of the camera.
						const float ob_w = pasted_ob->getAABBWSLongestLength();
						new_ob_pos = gui_client.cam_controller.getFirstPersonPosition() + gui_client.cam_controller.getForwardsVec() * myMax(2.f, ob_w * 2.0f);
					}
					else
					{
						// If the camera is near the source object, position pasted object besides the source object.
						// Translate along an axis depending on the camera viewpoint.
						Vec3d use_offset_vec;
						if(std::fabs(gui_client.cam_controller.getRightVec().x) > std::fabs(gui_client.cam_controller.getRightVec().y))
							use_offset_vec = (gui_client.cam_controller.getRightVec().x > 0) ? Vec3d(1,0,0) : Vec3d(-1,0,0);
						else
							use_offset_vec = (gui_client.cam_controller.getRightVec().y > 0) ? Vec3d(0,1,0) : Vec3d(0,-1,0);

						// We don't want to paste directly in the same place as another object (for example a previously pasted object), otherwise users can create duplicate objects by mistake and lose them.
						// So check if there is already an object there, and choose another position if so.
						new_ob_pos = pasted_ob->pos;
						for(int i=0; i<100; ++i)
						{
							const Vec3d tentative_pos = pasted_ob->pos + use_offset_vec * (i + 1) * 0.5f;
							if(!gui_client.isObjectWithPosition(tentative_pos))
							{
								new_ob_pos = tentative_pos;
								break;
							}
						}
					}

					pasted_ob->pos = new_ob_pos;
					pasted_ob->transformChanged();

					// Check permissions
					bool ob_pos_in_parcel;
					const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(new_ob_pos, ob_pos_in_parcel);
					if(!have_creation_perms)
					{
						if(ob_pos_in_parcel)
							showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
						else
							showErrorNotification("You can only create objects in a parcel that you have write permissions for.");
						return;
					}

					// Create object, by sending CreateObject message to server
					// Note that the recreated object will have a different ID than in the clipboard.
					{
						MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
						pasted_ob->writeToNetworkStream(scratch_packet);

						enqueueMessageToSend(*gui_client.client_thread, scratch_packet);

						showInfoNotification("Object pasted.");
					}
				}
				catch(glare::Exception& e)
				{
					conPrint("Error while reading object from clipboard: " + e.what());
				}
			}
			else if(mime_data->hasImage()) // Image data (for example from snip screen)
			{
				QImage image = qvariant_cast<QImage>(mime_data->imageData());

				const std::string temp_path = PlatformUtils::getTempDirPath() + "/temp.jpg";
				const bool res = image.save(QtUtils::toQString(temp_path), "JPG", 95);
				if(!res)
					throw glare::Exception("Failed to save image to disk.");

				gui_client.createImageObjectForWidthAndHeight(temp_path, image.width(), image.height(), /*has alpha=*/false);
			}
		}
	}
	catch(glare::Exception& e)
	{
		// Show error
		print(e.what());
		QErrorMessage m;
		m.showMessage(QtUtils::toQString(e.what()));
		m.exec();
	}
}


void MainWindow::on_actionPaste_Object_triggered()
{
	QClipboard* clipboard = QGuiApplication::clipboard();
	const QMimeData* mime_data = clipboard->mimeData();

	handlePasteOrDropMimeData(mime_data);
}


void MainWindow::on_actionCloneObject_triggered()
{
	if(gui_client.selected_ob.nonNull())
	{
		WorldObjectRef source_ob = gui_client.selected_ob;

		// Position cloned object besides the source object.
		// Translate along an axis depending on the camera viewpoint.
		Vec3d use_offset_vec;
		if(std::fabs(gui_client.cam_controller.getRightVec().x) > std::fabs(gui_client.cam_controller.getRightVec().y))
			use_offset_vec = (gui_client.cam_controller.getRightVec().x > 0) ? Vec3d(1,0,0) : Vec3d(-1,0,0);
		else
			use_offset_vec = (gui_client.cam_controller.getRightVec().y > 0) ? Vec3d(0,1,0) : Vec3d(0,-1,0);

		const Vec3d new_ob_pos = source_ob->pos + use_offset_vec;

		bool ob_pos_in_parcel;
		const bool have_creation_perms = gui_client.haveParcelObjectCreatePermissions(new_ob_pos, ob_pos_in_parcel);
		if(!have_creation_perms)
		{
			if(ob_pos_in_parcel)
				showErrorNotification("You do not have write permissions, and are not an admin for this parcel.");
			else
				showErrorNotification("You can only create objects in a parcel that you have write permissions for.");
			return;
		}

		WorldObjectRef new_world_object = new WorldObject();
		new_world_object->uid = UID(0); // Will be set by server
		new_world_object->object_type = source_ob->object_type;
		new_world_object->model_url = source_ob->model_url;
		new_world_object->script = source_ob->script;
		new_world_object->materials = source_ob->materials; // TODO: clone?
		new_world_object->content = source_ob->content;
		new_world_object->target_url = source_ob->target_url;
		new_world_object->pos = new_ob_pos;
		new_world_object->axis = source_ob->axis;
		new_world_object->angle = source_ob->angle;
		new_world_object->scale = source_ob->scale;
		new_world_object->flags = source_ob->flags;// | WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG; // Lightmaps need to be built for it.
		new_world_object->getDecompressedVoxels() = source_ob->getDecompressedVoxels();
		new_world_object->getCompressedVoxels() = source_ob->getCompressedVoxels();
		new_world_object->audio_source_url = source_ob->audio_source_url;
		new_world_object->audio_volume = source_ob->audio_volume;
		new_world_object->setAABBOS(source_ob->getAABBOS());

		new_world_object->max_model_lod_level = source_ob->max_model_lod_level;
		new_world_object->mass = source_ob->mass;
		new_world_object->friction = source_ob->friction;
		new_world_object->restitution = source_ob->restitution;


		// Send CreateObject message to server
		{
			MessageUtils::initPacket(scratch_packet, Protocol::CreateObject);
			new_world_object->writeToNetworkStream(scratch_packet);

			enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
		}

		// Deselect any currently selected object
		gui_client.deselectObject();

		showInfoNotification("Object cloned.");
	}
	else
	{
		QMessageBox msgBox;
		msgBox.setText("Please select an object before cloning.");
		msgBox.exec();
	}
}


void MainWindow::on_actionDeleteObject_triggered()
{
	if(gui_client.selected_ob.nonNull())
	{
		gui_client.deleteSelectedObject();
	}
}


void MainWindow::on_actionReset_Layout_triggered()
{
	ui->editorDockWidget->setFloating(false);
	this->addDockWidget(Qt::LeftDockWidgetArea, ui->editorDockWidget);
	ui->editorDockWidget->show();

	ui->chatDockWidget->setFloating(false);
	this->addDockWidget(Qt::RightDockWidgetArea, ui->chatDockWidget, Qt::Vertical);
	ui->chatDockWidget->show();

	ui->materialBrowserDockWidget->setFloating(false);
	this->addDockWidget(Qt::TopDockWidgetArea, ui->materialBrowserDockWidget, Qt::Horizontal);
	ui->materialBrowserDockWidget->show();

#if INDIGO_SUPPORT
	ui->indigoViewDockWidget->setFloating(false);
	this->addDockWidget(Qt::RightDockWidgetArea, ui->indigoViewDockWidget, Qt::Vertical);
	ui->indigoViewDockWidget->show();
#endif

	ui->helpInfoDockWidget->setFloating(true);
	ui->helpInfoDockWidget->show();
	// Position near bottom right corner of glWidget.
	ui->helpInfoDockWidget->setGeometry(QRect(ui->glWidget->mapToGlobal(ui->glWidget->geometry().bottomRight() + QPoint(-320, -120)), QSize(300, 100)));

	//this->addDockWidget(Qt::RightDockWidgetArea, ui->chatDockWidget, Qt::Vertical);
	//ui->chatDockWidget->show();

	ui->diagnosticsDockWidget->setFloating(false);
	this->addDockWidget(Qt::TopDockWidgetArea, ui->diagnosticsDockWidget, Qt::Horizontal);
	ui->diagnosticsDockWidget->show();


	// Enable tool bar
	ui->toolBar->setVisible(true);
}


void MainWindow::on_actionLogIn_triggered()
{
	if(gui_client.connection_state != GUIClient::ServerConnectionState_Connected)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Can't log in");
		msgBox.setText("You must be connected to a server to log in.");
		msgBox.exec();
		return;
	}

	LoginDialog dialog(settings, credential_manager, gui_client.server_hostname);
	const int res = dialog.exec();
	if(res == QDialog::Accepted)
	{
		const std::string username = QtUtils::toStdString(dialog.usernameLineEdit->text());
		const std::string password = QtUtils::toStdString(dialog.passwordLineEdit->text());

		//conPrint("username: " + username);
		//conPrint("password: " + password);
		//this->last_login_username = username;

		// Make LogInMessage packet and enqueue to send
		MessageUtils::initPacket(scratch_packet, Protocol::LogInMessage);
		scratch_packet.writeStringLengthFirst(username);
		scratch_packet.writeStringLengthFirst(password);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}
}


void MainWindow::on_actionLogOut_triggered()
{
	// Make message packet and enqueue to send
	MessageUtils::initPacket(scratch_packet, Protocol::LogOutMessage);
	enqueueMessageToSend(*gui_client.client_thread, scratch_packet);

	settings->setValue("LoginDialog/auto_login", false); // Don't log in automatically next start.
}


void MainWindow::on_actionSignUp_triggered()
{
	if(gui_client.connection_state != GUIClient::ServerConnectionState_Connected)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Can't sign up");
		msgBox.setText("You must be connected to a server to sign up.");
		msgBox.exec();
		return;
	}

	SignUpDialog dialog(settings, &credential_manager, gui_client.server_hostname);
	const int res = dialog.exec();
	if(res == QDialog::Accepted)
	{
		const std::string username = QtUtils::toStdString(dialog.usernameLineEdit->text());
		const std::string email    = QtUtils::toStdString(dialog.emailLineEdit->text());
		const std::string password = QtUtils::toStdString(dialog.passwordLineEdit->text());

		conPrint("username: " + username);
		conPrint("email:    " + email);
		conPrint("password: " + password);
		//this->last_login_username = username;

		// Make message packet and enqueue to send
		MessageUtils::initPacket(scratch_packet, Protocol::SignUpMessage);
		scratch_packet.writeStringLengthFirst(username);
		scratch_packet.writeStringLengthFirst(email);
		scratch_packet.writeStringLengthFirst(password);

		enqueueMessageToSend(*gui_client.client_thread, scratch_packet);
	}
}


void MainWindow::on_actionShow_Parcels_triggered()
{
	if(ui->actionShow_Parcels->isChecked())
	{
		gui_client.addParcelObjects();
	}
	else // Else if show parcels is now unchecked:
	{
		gui_client.removeParcelObjects();
	}

	settings->setValue("mainwindow/showParcels", QVariant(ui->actionShow_Parcels->isChecked()));
}


void MainWindow::on_actionFly_Mode_triggered()
{
	gui_client.player_physics.setFlyModeEnabled(ui->actionFly_Mode->isChecked());

	settings->setValue("mainwindow/flyMode", QVariant(ui->actionFly_Mode->isChecked()));
}


void MainWindow::on_actionThird_Person_Camera_triggered()
{
	settings->setValue("mainwindow/thirdPersonCamera", QVariant(ui->actionThird_Person_Camera->isChecked()));

	gui_client.thirdPersonCameraToggled(ui->actionThird_Person_Camera->isChecked());
}


void MainWindow::on_actionGoToMainWorld_triggered()
{
	gui_client.connectToServer("sub://" + gui_client.server_hostname);// this->server_hostname, "");
}


void MainWindow::on_actionGoToPersonalWorld_triggered()
{
	if(gui_client.logged_in_user_name != "")
	{
		gui_client.connectToServer("sub://" + gui_client.server_hostname + "/" + gui_client.logged_in_user_name); //  this->server_hostname, this->logged_in_user_name);
	}
	else
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Not logged in");
		msgBox.setText("You are not logged in, so we don't know your personal world name.  Please log in first.");
		msgBox.exec();
	}
}


void MainWindow::on_actionGo_to_CryptoVoxels_World_triggered()
{
	gui_client.connectToServer("sub://" + gui_client.server_hostname + "/cryptovoxels");//  this->server_hostname, "cryptovoxels");
}


void MainWindow::on_actionGo_to_Parcel_triggered()
{
	GoToParcelDialog d(this->settings);
	const int code = d.exec();
	if(code == QDialog::Accepted)
	{
		try
		{
			const int parcel_num = stringToInt(QtUtils::toStdString(d.parcelNumberLineEdit->text()));

			bool found = true;
			{
				Lock lock(gui_client.world_state->mutex);

				auto res = gui_client.world_state->parcels.find(ParcelID(parcel_num));
				if(res != gui_client.world_state->parcels.end())
				{
					const Parcel* parcel = res->second.ptr();

					gui_client.cam_controller.setPosition(parcel->getVisitPosition());
					gui_client.player_physics.setPosition(parcel->getVisitPosition());
				}
				else
					found = false;
			}

			if(!found)
			{
				QMessageBox msgBox;
				msgBox.setWindowTitle("Invalid parcel number");
				msgBox.setText("There is no parcel with that number.");
				msgBox.exec();
			}
		}
		catch(glare::Exception&)
		{
			QMessageBox msgBox;
			msgBox.setWindowTitle("Invalid parcel number");
			msgBox.setText("Please enter just a number.");
			msgBox.exec();
		}
	}
}


void MainWindow::on_actionGo_to_Position_triggered()
{
	GoToPositionDialog d(this->settings, gui_client.cam_controller.getFirstPersonPosition());
	const int code = d.exec();
	if(code == QDialog::Accepted)
	{
		const Vec3d pos(
			d.XDoubleSpinBox->value(),
			d.YDoubleSpinBox->value(),
			d.ZDoubleSpinBox->value()
		);
			
		gui_client.cam_controller.setPosition(pos);
		gui_client.player_physics.setPosition(pos);
	}
}


void MainWindow::on_actionSet_Start_Location_triggered()
{
	settings->setValue(MainOptionsDialog::startLocationURLKey(), QtUtils::toQString(this->url_widget->getURL()));
}


void MainWindow::on_actionGo_To_Start_Location_triggered()
{
	const std::string start_URL = QtUtils::toStdString(settings->value(MainOptionsDialog::startLocationURLKey()).toString());

	if(start_URL.empty())
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Invalid start location URL");
		msgBox.setText("You need to set a start location first with the 'Go > Set current location as start location' menu command.");
		msgBox.exec();
	}
	else
		visitSubURL(start_URL);
}


void MainWindow::on_actionFind_Object_triggered()
{
	FindObjectDialog d(this->settings);
	const int code = d.exec();
	if(code == QDialog::Accepted)
	{
		try
		{
			const int ob_id = stringToInt(QtUtils::toStdString(d.objectIDLineEdit->text()));

			bool found = true;
			{
				Lock lock(gui_client.world_state->mutex);

				auto res = gui_client.world_state->objects.find(UID(ob_id));
				if(res != gui_client.world_state->objects.end())
				{
					WorldObject* ob = res.getValue().ptr();

					gui_client.deselectObject();
					gui_client.selectObject(ob, /*selected_mat_index=*/0);
				}
				else
					found = false;
			}

			if(!found)
			{
				QMessageBox msgBox;
				msgBox.setWindowTitle("Invalid object id");
				msgBox.setText("There is no object with that id.");
				msgBox.exec();
			}
		}
		catch(glare::Exception&)
		{
			QMessageBox msgBox;
			msgBox.setWindowTitle("Invalid object id");
			msgBox.setText("Please enter just a number.");
			msgBox.exec();
		}
	}
}


void MainWindow::on_actionList_Objects_Nearby_triggered()
{
	ListObjectsNearbyDialog d(this->settings, gui_client.world_state.ptr(), gui_client.cam_controller.getPosition());
	const int code = d.exec();
	if(code == QDialog::Accepted)
	{
		const UID ob_id = d.getSelectedUID();
		if(ob_id.valid())
		{
			bool found = true;
			{
				Lock lock(gui_client.world_state->mutex);
				auto res = gui_client.world_state->objects.find(UID(ob_id));
				if(res != gui_client.world_state->objects.end())
				{
					WorldObject* ob = res.getValue().ptr();

					gui_client.deselectObject();
					gui_client.selectObject(ob, /*selected_mat_index=*/0);
				}
				else
					found = false;
			}

			if(!found)
			{
				QMessageBox msgBox;
				msgBox.setWindowTitle("Invalid object id");
				msgBox.setText("There is no object with that id.");
				msgBox.exec();
			}
		}
	}
}


void MainWindow::on_actionExport_view_to_Indigo_triggered()
{
	ui->indigoView->saveSceneToDisk();
}


void MainWindow::on_actionTake_Screenshot_triggered()
{
	gui_client.gesture_ui.setVisible(false); // Hide gesture UI
	gui_client.minimap.setVisible(false); // Hide minimap

	 // Remove any avatar markers from the HUD UI
	if(gui_client.world_state.nonNull())
	{
		Lock lock(gui_client.world_state->mutex);
		for(auto it = gui_client.world_state->avatars.begin(); it != gui_client.world_state->avatars.end(); ++it)
		{
			Avatar* avatar = it->second.ptr();
			gui_client.hud_ui.removeMarkerForAvatar(avatar);
		}
	}


	ui->glWidget->updateGL(); // Draw again now that the gesture UI and minimap are hidden.

#if QT_VERSION_MAJOR >= 6
	QImage framebuffer = ui->glWidget->grabFramebuffer();
#else
	QImage framebuffer = ui->glWidget->grabFrameBuffer();
#endif
	
	// NOTE: Qt-saved images were doing weird things with parcel border alpha.  Just copy to an ImageMapUInt8 and do the image saving ourselves.

	ImageMapUInt8Ref map = convertQImageToImageMapUInt8(framebuffer);


	const std::string path = this->appdata_path + "/screenshots/screenshot_" + toString((uint64)Clock::getSecsSince1970()) + ".png";
	try
	{
		FileUtils::createDirIfDoesNotExist(FileUtils::getDirectory(path));

		PNGDecoder::write(*map, path);
	
		showInfoNotification("Saved screenshot to " + path);
	}
	catch(glare::Exception& e)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Error");
		msgBox.setText(QtUtils::toQString("Saving screenshot to '" + path + "' failed: " + e.what()));
		msgBox.exec();
	}

	gui_client.gesture_ui.setVisible(true); // Restore showing gesture UI
	gui_client.minimap.setVisible(true); // Restore showing minimap
}


void MainWindow::on_actionShow_Screenshot_Folder_triggered()
{
	try
	{
		const std::string path = this->appdata_path + "/screenshots/";

		PlatformUtils::openFileBrowserWindowAtLocation(path);
	}
	catch(glare::Exception& e)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Error");
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


void MainWindow::on_actionAbout_Substrata_triggered()
{
	AboutDialog d(this, appdata_path);
	d.exec();
}


void MainWindow::on_actionOptions_triggered()
{
	const std::string prev_audio_input_dev_name = QtUtils::toStdString(settings->value(MainOptionsDialog::inputDeviceNameKey(), "Default").toString());

	MainOptionsDialog d(this->settings);
	const int code = d.exec();
	if(code == QDialog::Accepted)
	{
		const float dist = (float)settings->value(MainOptionsDialog::objectLoadDistanceKey(), /*default val=*/500.0).toDouble();
		gui_client.proximity_loader.setLoadDistance(dist);
		gui_client.load_distance = dist;
		gui_client.load_distance2 = dist*dist;

		//ui->glWidget->opengl_engine->setMSAAEnabled(settings->value(MainOptionsDialog::MSAAKey(), /*default val=*/true).toBool());

		startMainTimer(); // Restart main timer, as the timer interval depends on max FPS, whiich may have changed.

		// Create or destroy minimap if minimap option has changed.
		if(d.getShowMinimap(this->settings) && !gui_client.minimap.isCreated())
		{
			gui_client.minimap.create(opengl_engine, &gui_client, gui_client.gl_ui);
		}
		else if(!d.getShowMinimap(this->settings) && gui_client.minimap.isCreated())
		{
			gui_client.minimap.destroy();
		}
	}

	gui_client.mic_read_thread_manager.enqueueMessage(new InputVolumeScaleChangedMessage(
		settings->value(MainOptionsDialog::inputScaleFactorNameKey(), /*default val=*/100).toInt() * 0.01f // input_vol_scale_factor (note: stored in percent in settings)
	));

	// Restart mic read thread if audio input device changed.
	if(QtUtils::toStdString(settings->value(MainOptionsDialog::inputDeviceNameKey(), "Default").toString()) != prev_audio_input_dev_name)
	{
		gui_client.mic_read_thread_manager.killThreadsBlocking();

		Reference<glare::MicReadThread> mic_read_thread = new glare::MicReadThread(&gui_client.msg_queue, gui_client.udp_socket, gui_client.client_avatar_uid, gui_client.server_hostname, gui_client.server_UDP_port,
			MainOptionsDialog::getInputDeviceName(settings),
			MainOptionsDialog::getInputScaleFactor(settings), // input_vol_scale_factor
			&gui_client.mic_read_status
		);
		gui_client.mic_read_thread_manager.addThread(mic_read_thread);
	}
}


void MainWindow::on_actionUndo_triggered()
{
	try
	{
		WorldObjectRef ob = gui_client.undo_buffer.getUndoWorldObject();
		gui_client.applyUndoOrRedoObject(ob);
	}
	catch(glare::Exception& e)
	{
		conPrint("ERROR: Exception while trying to undo change: " + e.what());
	}
}


void MainWindow::on_actionRedo_triggered()
{
	try
	{
		WorldObjectRef ob = gui_client.undo_buffer.getRedoWorldObject();
		gui_client.applyUndoOrRedoObject(ob);
	}
	catch(glare::Exception& e)
	{
		conPrint("ERROR: Exception while trying to redo change: " + e.what());
	}
}


void MainWindow::on_actionShow_Log_triggered()
{
	this->log_window->show();
	this->log_window->raise();
}


void MainWindow::on_actionBake_Lightmaps_fast_for_all_objects_in_parcel_triggered()
{
	gui_client.bakeLightmapsForAllObjectsInParcel(WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG);
}


void MainWindow::on_actionBake_lightmaps_high_quality_for_all_objects_in_parcel_triggered()
{
	gui_client.bakeLightmapsForAllObjectsInParcel(WorldObject::HIGH_QUAL_LIGHTMAP_NEEDS_COMPUTING_FLAG);
}


void MainWindow::on_actionSummon_Bike_triggered()
{
	try
	{
		gui_client.summonBike();
	}
	catch(glare::Exception& e)
	{
		showErrorNotification(e.what());

		QMessageBox msgBox;
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


void MainWindow::on_actionSummon_Hovercar_triggered()
{
	try
	{
		gui_client.summonHovercar();
	}
	catch(glare::Exception& e)
	{
		showErrorNotification(e.what());

		QMessageBox msgBox;
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


void MainWindow::on_actionMute_Audio_toggled(bool checked)
{
	if(checked)
	{
		gui_client.audio_engine.setMasterVolume(0.f);
	}
	else
	{	
		gui_client.audio_engine.setMasterVolume(1.f);
	}
}


void MainWindow::diagnosticsWidgetChanged()
{
	opengl_engine->setDrawWireFrames(ui->diagnosticsWidget->showWireframesCheckBox->isChecked());
}


void MainWindow::diagnosticsReloadTerrain()
{
	if(gui_client.terrain_system.nonNull())
	{
		gui_client.terrain_system->shutdown();
		gui_client.terrain_system = NULL;
	}

	// Just leave terrain_system null, will be reinitialised in MainWindow::updateGroundPlane().
}


void MainWindow::sendChatMessageSlot()
{
	//conPrint("MainWindow::sendChatMessageSlot()");

	const std::string message = QtUtils::toIndString(ui->chatMessageLineEdit->text());

	// Make message packet and enqueue to send
	MessageUtils::initPacket(scratch_packet, Protocol::ChatMessageID);
	scratch_packet.writeStringLengthFirst(message);

	enqueueMessageToSend(*gui_client.client_thread, scratch_packet);

	ui->chatMessageLineEdit->clear();
}


void MainWindow::sendLightmapNeededFlagsSlot()
{
	gui_client.sendLightmapNeededFlagsSlot();
}


// Object transform has been edited, e.g. by the object editor.
void MainWindow::objectTransformEditedSlot()
{
	try
	{
		gui_client.objectTransformEdited();
	}
	catch(glare::Exception& e)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Error");
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


// Object property (that is not a transform property) has been edited, e.g. by the object editor.
void MainWindow::objectEditedSlot()
{
	try
	{
		gui_client.objectEdited();
	}
	catch(glare::Exception& e)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle("Error");
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


// Parcel has been edited, e.g. by the parcel editor.
void MainWindow::parcelEditedSlot()
{
	if(gui_client.selected_parcel.nonNull())
	{
		ui->parcelEditor->toParcel(*gui_client.selected_parcel);

		Lock lock(gui_client.world_state->mutex);
		//this->selected_parcel->from_local_other_dirty = true;
		gui_client.world_state->dirty_from_local_parcels.insert(gui_client.selected_parcel);
	}
}


void MainWindow::worldSettingsAppliedSlot()
{
	// NOTE: for now just wait for WorldSettingsUpdate to come from server, which will reload the terrain.
}


// An environment setting has been edited in the environment options dock widget
void MainWindow::environmentSettingChangedSlot()
{
	if(ui->glWidget->opengl_engine.nonNull())
	{
		const float theta = myClamp(::degreeToRad((float)ui->environmentOptionsWidget->sunThetaRealControl->value()), 0.01f, Maths::pi<float>() - 0.01f);
		const float phi   = ::degreeToRad((float)ui->environmentOptionsWidget->sunPhiRealControl->value());
		const Vec4f sundir = GeometrySampling::dirForSphericalCoords(phi, theta);

		opengl_engine->setSunDir(sundir);
	}
}


void MainWindow::bakeObjectLightmapSlot()
{
	if(gui_client.selected_ob.nonNull())
	{
		// Don't bake lightmaps for objects with sketal animation for now (creating second UV set removes joints and weights).
		const bool has_skeletal_anim = gui_client.selected_ob->opengl_engine_ob.nonNull() && gui_client.selected_ob->opengl_engine_ob->mesh_data.nonNull() &&
			!gui_client.selected_ob->opengl_engine_ob->mesh_data->animation_data.animations.empty();

		if(has_skeletal_anim)
		{
			showErrorNotification("You cannot currently bake lightmaps for objects with skeletal animation.");
		}
		else
		{
			gui_client.selected_ob->lightmap_baking = true;

			BitUtils::setBit(gui_client.selected_ob->flags, WorldObject::LIGHTMAP_NEEDS_COMPUTING_FLAG);
			gui_client.objs_with_lightmap_rebuild_needed.insert(gui_client.selected_ob);
			lightmap_flag_timer->start(/*msec=*/20); // Trigger sending update-lightmap update flag message later.
		}
	}
}


void MainWindow::bakeObjectLightmapHighQualSlot()
{
	if(gui_client.selected_ob.nonNull())
	{
		// Don't bake lightmaps for objects with sketal animation for now (creating second UV set removes joints and weights).
		const bool has_skeletal_anim = gui_client.selected_ob->opengl_engine_ob.nonNull() && gui_client.selected_ob->opengl_engine_ob->mesh_data.nonNull() &&
			!gui_client.selected_ob->opengl_engine_ob->mesh_data->animation_data.animations.empty();

		if(has_skeletal_anim)
		{
			showErrorNotification("You cannot currently bake lightmaps for objects with skeletal animation.");
		}
		else
		{
			gui_client.selected_ob->lightmap_baking = true;

			BitUtils::setBit(gui_client.selected_ob->flags, WorldObject::HIGH_QUAL_LIGHTMAP_NEEDS_COMPUTING_FLAG);
			gui_client.objs_with_lightmap_rebuild_needed.insert(gui_client.selected_ob);
			lightmap_flag_timer->start(/*msec=*/20); // Trigger sending update-lightmap update flag message later.
		}
	}
}


void MainWindow::removeLightmapSignalSlot()
{
	if(gui_client.selected_ob.nonNull())
	{
		gui_client.selected_ob->lightmap_url.clear();

		objectEditedSlot();
	}
}


void MainWindow::posAndRot3DControlsToggledSlot()
{
	gui_client.posAndRot3DControlsToggled(ui->objectEditor->posAndRot3DControlsEnabled());
	
	settings->setValue("objectEditor/show3DControlsCheckBoxChecked", ui->objectEditor->posAndRot3DControlsEnabled());
}


void MainWindow::materialSelectedInBrowser(const std::string& path)
{
	if(gui_client.selected_ob.nonNull())
	{
		const bool have_edit_permissions = gui_client.objectModificationAllowedWithMsg(*gui_client.selected_ob, "edit");
		if(have_edit_permissions)
			this->ui->objectEditor->materialSelectedInBrowser(path);
		else
			showErrorNotification("You do not have write permissions for this object, so you can't apply a material to it.");
	}
}


void MainWindow::URLChangedSlot()
{
	const std::string URL = this->url_widget->getURL();
	visitSubURL(URL);
}


void MainWindow::visitSubURL(const std::string& URL) // Visit a substrata 'sub://' URL.  Checks hostname and only reconnects if the hostname is different from the current one.
{
	try
	{
		gui_client.visitSubURL(URL);
	}
	catch(glare::Exception& e) // Handle URL parse failure
	{
		conPrint(e.what());
		QMessageBox msgBox;
		msgBox.setText(QtUtils::toQString(e.what()));
		msgBox.exec();
	}
}


static MouseButton fromQtMouseButton(Qt::MouseButton b)
{
	if(b == Qt::MouseButton::LeftButton)
		return MouseButton::Left;
	else if(b == Qt::MouseButton::RightButton)
		return MouseButton::Right;
	else if(b == Qt::MouseButton::MiddleButton)
		return MouseButton::Middle;
	else if(b == Qt::MouseButton::BackButton)
		return MouseButton::Back;
	else if(b == Qt::MouseButton::ForwardButton)
		return MouseButton::Forward;
	else
		return MouseButton::None;
}


static uint32 fromQtModifiers(Qt::KeyboardModifiers modifiers)
{
	const bool ctrl_key_down = (modifiers & Qt::ControlModifier) != 0;
	const bool alt_key_down  = (modifiers & Qt::AltModifier)     != 0;
	const bool shift_down    = (modifiers & Qt::ShiftModifier)   != 0;

	return 
		(ctrl_key_down ? Modifiers::Ctrl  : 0) |
		(alt_key_down  ? Modifiers::Alt   : 0) |
		(shift_down    ? Modifiers::Shift : 0);
}


void MainWindow::glWidgetMousePressed(QMouseEvent* e)
{
	const Vec2f widget_pos((float)e->pos().x(), (float)e->pos().y());

	MouseEvent mouse_event;
	mouse_event.cursor_pos = Vec2i(e->pos().x(), e->pos().y());
	mouse_event.gl_coords = GLCoordsForGLWidgetPos(this, widget_pos);
	mouse_event.button = fromQtMouseButton(e->button());
	mouse_event.modifiers = fromQtModifiers(e->modifiers());

	gui_client.mousePressed(mouse_event);
}


// This is emitted from GlWidget::mouseReleaseEvent().
void MainWindow::glWidgetMouseClicked(QMouseEvent* e)
{
	const Vec2f widget_pos((float)e->pos().x(), (float)e->pos().y());
	const Vec2f gl_coords = GLCoordsForGLWidgetPos(this, widget_pos);

	MouseEvent mouse_event;
	mouse_event.cursor_pos = Vec2i(e->pos().x(), e->pos().y());
	mouse_event.gl_coords = gl_coords;
	mouse_event.button = fromQtMouseButton(e->button());
	mouse_event.modifiers = fromQtModifiers(e->modifiers());

	gui_client.mouseClicked(mouse_event);

	if(mouse_event.accepted)
	{
		e->accept();
		return;
	}
}


void MainWindow::setUIForSelectedObject() // Enable/disable delete object action etc..
{
	const bool have_selected_ob = gui_client.selected_ob.nonNull();
	this->ui->actionCloneObject->setEnabled(have_selected_ob);
	this->ui->actionDeleteObject->setEnabled(have_selected_ob);
}


void MainWindow::startObEditorTimerIfNotActive()
{
	// Set a timer to call updateObjectEditorObTransformSlot() later. Not calling this every frame avoids stutters with webviews playing back videos interacting with Qt updating spinboxes.
	if(!update_ob_editor_transform_timer->isActive())
		update_ob_editor_transform_timer->start(/*msec=*/50);
}


void MainWindow::startLightmapFlagTimer()
{
	lightmap_flag_timer->start(/*msec=*/20); // Trigger sending update-lightmap update flag message later.
}


void MainWindow::setCamRotationOnMouseMoveEnabled(bool enabled)
{
	ui->glWidget->setCamRotationOnMouseMoveEnabled(enabled);
}


bool MainWindow::isCursorHidden()
{
	return ui->glWidget->isCursorHidden();
}


void MainWindow::hideCursor()
{
	ui->glWidget->hideCursor();
}


void MainWindow::setKeyboardCameraMoveEnabled(bool enabled)
{
	ui->glWidget->setKeyboardCameraMoveEnabled(enabled);
}


bool MainWindow::isKeyboardCameraMoveEnabled()
{
	return ui->glWidget->isKeyboardCameraMoveEnabled();
}


bool MainWindow::hasFocus()
{
	return ui->glWidget->hasFocus();
}


void MainWindow::setHelpInfoLabelToDefaultText()
{
	this->ui->helpInfoLabel->setText(default_help_info_message);
}


void MainWindow::setHelpInfoLabel(const std::string& text)
{
	this->ui->helpInfoLabel->setText(QtUtils::toQString(text));
}


void MainWindow::showParcelEditor()
{
	ui->objectEditor->hide();
	ui->parcelEditor->show();
}


void MainWindow::setParcelEditorForParcel(const Parcel& parcel)
{
	ui->parcelEditor->setFromParcel(parcel);
}


void MainWindow::setParcelEditorEnabled(bool b)
{
	ui->parcelEditor->setEnabled(b);
}


void MainWindow::enableThirdPersonCamera()
{
	ui->actionThird_Person_Camera->setChecked(true);
	ui->actionThird_Person_Camera->triggered(true); // Need to manually trigger the action.
}


void MainWindow::toggleFlyMode()
{
	ui->actionFly_Mode->toggle();
	ui->actionFly_Mode->triggered(ui->actionFly_Mode->isChecked()); // Need to manually emit triggered signal, toggle doesn't do it.
}


void MainWindow::toggleThirdPersonCameraMode()
{
	ui->actionThird_Person_Camera->toggle();
	ui->actionThird_Person_Camera->triggered(ui->actionThird_Person_Camera->isChecked()); // Need to manually emit triggered signal, toggle doesn't do it.
}


void MainWindow::enableThirdPersonCameraIfNotAlreadyEnabled()
{
	if(!ui->actionThird_Person_Camera->isChecked())
		ui->actionThird_Person_Camera->trigger();
}


void MainWindow::firstPersonCameraEnabled()
{
	ui->actionThird_Person_Camera->setChecked(false);
	ui->actionThird_Person_Camera->triggered(true); // Need to manually trigger the action.
}


void MainWindow::openURL(const std::string& URL)
{
	QDesktopServices::openUrl(QtUtils::toQString(URL));
}


Vec2i MainWindow::getMouseCursorWidgetPos()
{
	const QPoint mouse_point = ui->glWidget->mapFromGlobal(QCursor::pos());

	return Vec2i(mouse_point.x(), mouse_point.y());
}


std::string MainWindow::getUsernameForDomain(const std::string& domain)
{
	return credential_manager.getUsernameForDomain(domain);
}


std::string MainWindow::getDecryptedPasswordForDomain(const std::string& domain)
{
	return credential_manager.getDecryptedPasswordForDomain(domain);
}


bool MainWindow::inScreenshotTakingMode()
{
	return !screenshot_output_path.empty();
}


ImageMapUInt8Ref MainWindow::drawText(const std::string& text, int font_point_size)
{
	const QString qt_text = QtUtils::toQString(text);

	QFont system_font = QApplication::font();
	system_font.setPointSize(font_point_size);
	
	const QFontMetrics font_metrics(system_font);
	const QRect rect = font_metrics.boundingRect(qt_text);
	
	const int w_padding = (int)(font_metrics.height() * 0.5f);
	const int h_padding = (int)(font_metrics.height() * 0.5f);
	
	const int w_px = rect.width()          + w_padding * 2;
	const int h_px = font_metrics.height() + h_padding * 2;

	QImage image(w_px, h_px, QImage::Format_RGB888);
	image.fill(QColor(255, 255, 255));
	QPainter painter(&image);
	painter.setPen(QPen(QColor(150, 150, 150)));
	painter.setFont(system_font);
	painter.drawText(image.rect(), Qt::AlignCenter, qt_text);

	ImageMapUInt8Ref map = new ImageMapUInt8(w_px, h_px, 3);

	// Copy to the ImageMapUInt8 image
	for(int y=0; y<h_px; ++y)
		std::memcpy(map->getPixel(0, h_px - y - 1), image.scanLine(y), 3*w_px);

	return map;
}


void MainWindow::doObjectSelectionTraceForMouseEvent(QMouseEvent* e)
{
	const Vec2f widget_pos((float)e->pos().x(), (float)e->pos().y());
	const Vec2f gl_coords = GLCoordsForGLWidgetPos(this, widget_pos);

	MouseEvent mouse_event;
	mouse_event.cursor_pos = Vec2i(e->pos().x(), e->pos().y());
	mouse_event.gl_coords = gl_coords;
	mouse_event.button = fromQtMouseButton(e->button());
	mouse_event.modifiers = fromQtModifiers(e->modifiers());

	gui_client.doObjectSelectionTraceForMouseEvent(mouse_event);
}


void MainWindow::glWidgetMouseDoubleClicked(QMouseEvent* e)
{
	//conPrint("MainWindow::glWidgetMouseDoubleClicked()");

	doObjectSelectionTraceForMouseEvent(e);
}


void MainWindow::glWidgetMouseMoved(QMouseEvent* e)
{
	if(ui->glWidget->opengl_engine.isNull() || !ui->glWidget->opengl_engine->initSucceeded())
		return;

	const Vec2f widget_pos((float)e->pos().x(), (float)e->pos().y());
	const Vec2f gl_coords = GLCoordsForGLWidgetPos(this, widget_pos);

	MouseEvent mouse_event;
	mouse_event.cursor_pos = Vec2i(e->pos().x(), e->pos().y());
	mouse_event.gl_coords = gl_coords;
	mouse_event.modifiers = fromQtModifiers(e->modifiers());

	gui_client.mouseMoved(mouse_event);

	if(mouse_event.accepted)
	{
		e->accept();
		return;
	}
}


void MainWindow::updateObjectEditorObTransformSlot()
{
	if(gui_client.selected_ob.nonNull())
		ui->objectEditor->setTransformFromObject(*gui_client.selected_ob);
}


void setKeyEventFromQt(const QKeyEvent* e, KeyEvent& event_out)
{
	KeyEvent& ev = event_out;
	
	ev.key = Key::Key_None;
	ev.native_virtual_key = e->nativeVirtualKey();
	ev.text = QtUtils::toStdString(e->text());
	ev.modifiers = fromQtModifiers(e->modifiers());

	const int qt_key = e->key();

	// Just check for keys we use currently.  TODO: all keys
	if(qt_key == Qt::Key::Key_Escape)
		ev.key = Key::Key_Escape;
	else if(qt_key == Qt::Key::Key_Backspace)
		ev.key = Key::Key_Backspace;
	else if(qt_key == Qt::Key::Key_Delete)
		ev.key = Key::Key_Delete;
	else if(qt_key == Qt::Key::Key_Space)
		ev.key = Key::Key_Space;

	else if(qt_key == Qt::Key::Key_BracketLeft)
		ev.key = Key::Key_LeftBracket;
	else if(qt_key == Qt::Key::Key_BracketRight)
		ev.key = Key::Key_RightBracket;

	else if(qt_key == Qt::Key::Key_PageUp)
		ev.key = Key::Key_PageUp;
	else if(qt_key == Qt::Key::Key_PageDown)
		ev.key = Key::Key_PageDown;

	else if(qt_key == Qt::Key::Key_Equal)
		ev.key = Key::Key_Equals;
	else if(qt_key == Qt::Key::Key_Minus)
		ev.key = Key::Key_Minus;
	else if(qt_key == Qt::Key::Key_Plus)
		ev.key = Key::Key_Plus;

	else if(qt_key == Qt::Key::Key_Left)
		ev.key = Key::Key_Left;
	else if(qt_key == Qt::Key::Key_Right)
		ev.key = Key::Key_Right;
	else if(qt_key == Qt::Key::Key_Up)
		ev.key = Key::Key_Up;
	else if(qt_key == Qt::Key::Key_Down)
		ev.key = Key::Key_Down;

	// A-Z
	if(qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z)
		ev.key = (Key)((int)Key::Key_A + ((int)qt_key - (int)Qt::Key_A));

	// 0-9
	if(qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9)
		ev.key = (Key)((int)Key::Key_0 + ((int)qt_key - (int)Qt::Key_0));

	// F1-F12
	if(qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F12)
		ev.key = (Key)((int)Key::Key_F1 + ((int)qt_key - (int)Qt::Key_F1));
}


void MainWindow::glWidgetKeyPressed(QKeyEvent* e)
{
	KeyEvent key_event;
	setKeyEventFromQt(e, key_event);

	gui_client.keyPressed(key_event);
}


void MainWindow::glWidgetkeyReleased(QKeyEvent* e)
{
	KeyEvent key_event;
	setKeyEventFromQt(e, key_event);

	gui_client.keyReleased(key_event);
}


void MainWindow::glWidgetFocusOut()
{
	gui_client.focusOut();
}


void MainWindow::glWidgetMouseWheelEvent(QWheelEvent* e)
{
	const Vec2f widget_pos((float)e->pos().x(), (float)e->pos().y());
	const Vec2f gl_coords = GLCoordsForGLWidgetPos(this, widget_pos);

	MouseWheelEvent mouse_event;
	mouse_event.cursor_pos = Vec2i(e->pos().x(), e->pos().y());
	mouse_event.gl_coords = gl_coords;
	mouse_event.angle_delta = Vec2i(e->angleDelta().x(), e->angleDelta().y());
	mouse_event.modifiers = fromQtModifiers(e->modifiers());

	gui_client.onMouseWheelEvent(mouse_event);

	if(mouse_event.accepted)
	{
		e->accept();
		return;
	}
}


void MainWindow::glWidgetViewportResized(int w, int h)
{
	gui_client.viewportResized(w, h);
}


void MainWindow::cameraUpdated()
{
	// ui->indigoView->cameraUpdated(this->cam_controller);
}


void MainWindow::setGLWidgetContextAsCurrent()
{
	this->ui->glWidget->makeCurrent();
}


bool MainWindow::connectedToUsersPersonalWorldOrGodUser()
{
	return gui_client.connectedToUsersPersonalWorldOrGodUser();
}


QPoint MainWindow::getGlWidgetPosInGlobalSpace() const
{
	return this->ui->glWidget->mapToGlobal(this->ui->glWidget->pos());
}


// See https://doc.qt.io/qt-5/qdesktopservices.html, this slot should be called when the user clicks on a sub:// link somewhere in the system.
void MainWindow::handleURL(const QUrl &url)
{
	gui_client.connectToServer(QtUtils::toStdString(url.toString()));
}


void MainWindow::webViewDataLinkHovered(const QString& url)
{
	if(url.isEmpty())
	{
		ui->glWidget->setCursorIfNotHidden(Qt::ArrowCursor);
	}
	else
	{
		ui->glWidget->setCursorIfNotHidden(Qt::PointingHandCursor);
	}
}


// The mouse was double-clicked on a web-view object
void MainWindow::webViewMouseDoubleClicked(QMouseEvent* e)
{
	doObjectSelectionTraceForMouseEvent(e);
}


#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
typedef qintptr NativeEventArgType;
#else
typedef long NativeEventArgType;
#endif


// Override nativeEvent() so we can handle WM_COPYDATA messages from other Substrata processes.
// See https://www.programmersought.com/article/216036067/
bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, NativeEventArgType* result)
{
#if defined(_WIN32)
	if(event_type == "windows_generic_MSG")
	{
		MSG* msg = reinterpret_cast<MSG*>(message);

		if(msg->message == WM_COPYDATA)
		{
			COPYDATASTRUCT* copy_data = reinterpret_cast<COPYDATASTRUCT*>(msg->lParam);

			const std::string text_body((const char*)copy_data->lpData, (const char*)copy_data->lpData + copy_data->cbData);

			if(hasPrefix(text_body, "openSubURL:"))
			{
				const std::string url = eatPrefix(text_body, "openSubURL:");

				conPrint("Opening URL '" + url + "'...");

				gui_client.connectToServer(url);

				// Flash the taskbar icon, since the this window may not be visible.
				QApplication::alert(this);
			}
		}
	}
#endif

	return QWidget::nativeEvent(event_type, message, result); // Hand on to Qt processing
}


// If we receive a file-open event before the mainwindow has been created, (e.g. main_window is NULL), then store the url to use when the mainwindow is created.
// However if the mainwindow has already been created, call connectToServer on it.
// Note that this event will only be sent on Macs: "Note: This class is currently supported for macOS only."
// See https://www.programmersought.com/article/55521160114/ - "Use url scheme in mac os to evoke qt program and get startup parameters"
class OpenEventFilter : public QObject
{
public:
	OpenEventFilter() : main_window(NULL) {}

	virtual bool eventFilter(QObject* obj, QEvent* event)
	{
		if(event->type() == QEvent::FileOpen)
		{
			QFileOpenEvent* fileEvent = static_cast<QFileOpenEvent*>(event);
			if(!fileEvent->url().isEmpty())
			{
				const QString qurl = fileEvent->url().toString();

				if(main_window)
				{
					main_window->gui_client.connectToServer(QtUtils::toStdString(qurl));

					QApplication::alert(main_window); // Flash the taskbar icon, since the this window may not be visible.
				}
				else
					url = QtUtils::toStdString(qurl);
			}
			return true; // Should the event be filtered out, e.g. have we handled it?
		}
		else
		{
			return QObject::eventFilter(obj, event);
		}
	}

	MainWindow* main_window;
	std::string url;
};


// Enable bugsplat unless the DISABLE_BUGSPLAT env var is set to a non-zero value.
#ifdef BUGSPLAT_SUPPORT
static bool shouldEnableBugSplat()
{
	try
	{
		const std::string val = PlatformUtils::getEnvironmentVariable("DISABLE_BUGSPLAT");
		return val == "0";
	}
	catch(glare::Exception&)
	{
		return true;
	}
}
#endif

#ifndef FUZZING


int main(int argc, char *argv[])
{
#ifdef BUGSPLAT_SUPPORT
	if(shouldEnableBugSplat())
	{
		// BugSplat initialization.
		new MiniDmpSender(
			L"Substrata", // database
			L"Substrata", // app
			StringUtils::UTF8ToPlatformUnicodeEncoding(cyberspace_version).c_str(), // version
			NULL, // app identifier
			MDSF_USEGUARDMEMORY | MDSF_LOGFILE | MDSF_PREVENTHIJACKING // flags
		);

		// The following calls add support for collecting crashes for abort(), vectored exceptions, out of memory,
		// pure virtual function calls, and for invalid parameters for OS functions.
		// These calls should be used for each module that links with a separate copy of the CRT.
		SetGlobalCRTExceptionBehavior();
		SetPerThreadCRTExceptionBehavior(); // This call needed in each thread of your app
	}
#endif


	QApplication::setAttribute(Qt::AA_UseDesktopOpenGL); // See https://forum.qt.io/topic/73255/qglwidget-blank-screen-on-different-computer/7

	//QtWebEngine::initialize();
//	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
//	QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
//	QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
//	QtWebEngineQuick::initialize();

	// Note that this is deliberately constructed outside of the try..catch block below, because QErrorMessage crashes when displayed if
	// GuiClientApplication has been destroyed. (stupid qt).
	GuiClientApplication app(argc, argv);

	try
	{
		OpenEventFilter* open_even_filter = new OpenEventFilter();
		app.installEventFilter(open_even_filter);

#if defined(_WIN32)
		const bool com_init_success = WMFVideoReader::initialiseCOM();
#endif

		GUIClient::staticInit();

#if defined(_WIN32)
		// Initialize the Media Foundation platform.
		WMFVideoReader::initialiseWMF();
#endif

		std::string cyberspace_base_dir_path = PlatformUtils::getResourceDirectoryPath();
		std::string appdata_path = PlatformUtils::getOrCreateAppDataDirectory("Cyberspace");

		QDir::setCurrent(QtUtils::toQString(cyberspace_base_dir_path));
	
		conPrint("cyberspace_base_dir_path: " + cyberspace_base_dir_path);

		// Get a vector of the args.  Note that we will use app.arguments(), because it's the only way to get the args in Unicode in Qt.
		const QStringList arg_list = app.arguments();
		std::vector<std::string> args;
		for(int i = 0; i < arg_list.size(); ++i)
			args.push_back(QtUtils::toIndString(arg_list.at((int)i)));


		std::map<std::string, std::vector<ArgumentParser::ArgumentType> > syntax;
		syntax["--test"] = std::vector<ArgumentParser::ArgumentType>(); // Run unit tests
		syntax["-h"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify hostname to connect to
		syntax["-u"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify server URL to connect to
		syntax["-linku"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify server URL to connect to, when a user has clicked on a substrata URL hyperlink.
		syntax["--extractanims"] = std::vector<ArgumentParser::ArgumentType>(2, ArgumentParser::ArgumentType_string); // Extract animation data
		syntax["--screenshotslave"] = std::vector<ArgumentParser::ArgumentType>(); // Run GUI as a screenshot-taking slave.
		syntax["--testscreenshot"] = std::vector<ArgumentParser::ArgumentType>(); // Test screenshot taking
		syntax["--no_MDI"] = std::vector<ArgumentParser::ArgumentType>(); // Disable MDI in graphics engine
		syntax["--no_bindless"] = std::vector<ArgumentParser::ArgumentType>(); // Disable bindless textures in graphics engine

		if(args.size() == 3 && args[1] == "-NSDocumentRevisionsDebugMode")
			args.resize(1); // This is some XCode debugging rubbish, remove it

		ArgumentParser parsed_args(args, syntax);

		if(parsed_args.isArgPresent("--test"))
		{
			TestSuite::test();
			return 0;
		}

		// Extract animation data from a GLTF file.
		if(parsed_args.isArgPresent("--extractanims"))
		{
			// E.g. --extractanims "D:\models\readyplayerme_avatar_animation_17.glb" "D:\models\extracted_avatar_anim.bin"
			const std::string input_path  = parsed_args.getArgStringValue("--extractanims", 0);
			const std::string output_path = parsed_args.getArgStringValue("--extractanims", 1);

			// Extract anims
			GLTFLoadedData data;
			Reference<BatchedMesh> mesh = FormatDecoderGLTF::loadGLBFile(input_path, data);

			FileOutStream file(output_path);
			mesh->animation_data.writeToStream(file);

			return 0;
		}


		//std::string server_hostname = "substrata.info";
		//std::string server_userpath = "";
		std::string server_URL = "sub://substrata.info";
		bool server_URL_explicitly_specified = false;

		if(parsed_args.isArgPresent("-h"))
		{
			server_URL = "sub://" + parsed_args.getArgStringValue("-h");
			server_URL_explicitly_specified = true;
		}
			//server_hostname = parsed_args.getArgStringValue("-h");
		if(parsed_args.isArgPresent("-u"))
		{
			server_URL = parsed_args.getArgStringValue("-u");
			server_URL_explicitly_specified = true;
			//const std::string URL = parsed_args.getArgStringValue("-u");
			//try
			//{
			//	URLParseResults parse_res = URLParser::parseURL(URL);

			//	server_hostname = parse_res.hostname;
			//	server_userpath = parse_res.userpath;
			//}
			//catch(glare::Exception& e) // Handle URL parse failure
			//{
			//	QMessageBox msgBox;
			//	msgBox.setText(QtUtils::toQString(e.what()));
			//	msgBox.exec();
			//	return 1;
			//}
		}
		else if(parsed_args.isArgPresent("-linku"))
		{
#if defined(_WIN32)
			// If we already have a Substrata application open on this computer, we want to tell that one to go to the URL, instead of opening another Substrata.
			// Search for an already existing Window called "Substrata vx.y"
			// If it exists, send a Windows message to that process, telling it to open the URL, and return from this process.
			// TODO: work out how to do this on Mac and Linux, if it's needed.
			const std::string target_window_title = computeWindowTitle();
			const HWND target_hwnd = ::FindWindowA(NULL, target_window_title.c_str());
			if(target_hwnd != 0)
			{
				const std::string msg_body = "openSubURL:" + parsed_args.getArgStringValue("-linku");

				COPYDATASTRUCT copy_data;
				copy_data.dwData = 0;
				copy_data.cbData = (DWORD)msg_body.size(); // The size, in bytes, of the data pointed to by the lpData member.
				copy_data.lpData = (void*)msg_body.data(); // The data to be passed to the receiving application

				SendMessage(target_hwnd,
					WM_COPYDATA,
					NULL,
					(LPARAM)&copy_data
				);
				return 0;
			}
			else
			{
				server_URL = parsed_args.getArgStringValue("-linku");
				server_URL_explicitly_specified = true;
			}
#else
			server_URL = parsed_args.getArgStringValue("-linku");
			server_URL_explicitly_specified = true;
#endif
		}

		if(!open_even_filter->url.empty())
		{
			// If we have received a url from a file-open event on Mac:
			server_URL = open_even_filter->url; // Use it
			server_URL_explicitly_specified = true;
		}


		int app_exec_res;
		{ // Scope of MainWindow mw and textureserver.

			// Since we will be inserted textures based on URLs, the textures should be different if they have different paths, so we don't need to do path canonicalisation.
			TextureServer texture_server(/*use_canonical_path_keys=*/false);

			MainWindow mw(cyberspace_base_dir_path, appdata_path, parsed_args); // Creates GLWidget

			open_even_filter->main_window = &mw;

			mw.gui_client.texture_server = &texture_server;
			mw.ui->glWidget->texture_server_ptr = &texture_server; // Set texture server pointer before GlWidget::initializeGL() gets called, as it passes texture server pointer to the opengl engine.

			if(parsed_args.isArgPresent("--screenshotslave"))
				mw.run_as_screenshot_slave = true;

			if(parsed_args.isArgPresent("--testscreenshot"))
				mw.test_screenshot_taking = true;

			mw.initialise();

			mw.show();

			mw.raise();

			if(!mw.ui->glWidget->opengl_engine->initSucceeded())
			{
				const std::string msg = "OpenGL engine initialisation failed: " + mw.ui->glWidget->opengl_engine->getInitialisationErrorMsg();
				
				mw.logMessage(msg);
				
				QtUtils::showErrorMessageDialog(msg, &mw);
				return 1;
			}
			mw.opengl_engine = mw.ui->glWidget->opengl_engine;
			mw.gui_client.opengl_engine = mw.opengl_engine;

			mw.gui_client.cam_controller.setPosition(Vec3d(0,0,4.7));
			mw.ui->glWidget->setCameraController(&mw.gui_client.cam_controller);
			mw.gui_client.cam_controller.setMoveScale(0.3f);


			// If the user didn't explictly specify a URL (e.g. on the command line), and there is a valid start location URL setting, use it.
			if(!server_URL_explicitly_specified)
			{
				const std::string start_loc_URL_setting = QtUtils::toStdString(mw.settings->value(MainOptionsDialog::startLocationURLKey()).toString());
				if(!start_loc_URL_setting.empty())
					server_URL = start_loc_URL_setting;
			}

			mw.gui_client.connectToServer(server_URL);

			try
			{
				// Copy default avatar into resource dir
				{
					const std::string mesh_URL = "xbot_glb_10972822012543217816.glb";

					if(!mw.gui_client.resource_manager->isFileForURLPresent(mesh_URL))
					{
						mw.gui_client.resource_manager->copyLocalFileToResourceDir(cyberspace_base_dir_path + "/resources/xbot.glb", mesh_URL);
					}
				}
			}
			catch(glare::Exception& e)
			{
				conPrint(e.what());
				QMessageBox msgBox;
				msgBox.setText(QtUtils::toQString(e.what()));
				msgBox.exec();
				return 1;
			}

			mw.afterGLInitInitialise();

			app_exec_res = app.exec();

			open_even_filter->main_window = NULL;
		} // End scope of MainWindow mw and textureserver.

#if defined(_WIN32)
		WMFVideoReader::shutdownWMF();
#endif
		
		GUIClient::staticShutdown();

#if defined(_WIN32)
		if(com_init_success) WMFVideoReader::shutdownCOM();
#endif

		return app_exec_res;
	}
	catch(Indigo::IndigoException& e)
	{
		// Show error
		conPrint(toStdString(e.what()));
		QErrorMessage m;
		m.showMessage(QtUtils::toQString(e.what()));
		m.exec();
		return 1;
	}
	catch(glare::Exception& e)
	{
		// Show error
		conPrint(e.what());
		QErrorMessage m;
		m.showMessage(QtUtils::toQString(e.what()));
		m.exec();
		return 1;
	}
}


#endif // End #ifndef FUZZING
