/*=====================================================================
MakeHypercardTextureTask.h
--------------------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#pragma once


#include <Task.h>
#include <ThreadMessage.h>
#include <string>
#include <vector>
class MainWindow;
class WinterShaderEvaluator;
class OpenGLEngine;


class HypercardTexMadeMessage : public ThreadMessage
{
public:
	std::string hypercard_content;
	Reference<WinterShaderEvaluator> script_evaluator;
};


/*=====================================================================
MakeHypercardTextureTask
------------------------

=====================================================================*/
class MakeHypercardTextureTask : public glare::Task
{
public:
	MakeHypercardTextureTask();
	virtual ~MakeHypercardTextureTask();

	virtual void run(size_t thread_index);

	Reference<OpenGLEngine> opengl_engine;
	MainWindow* main_window;
	std::string hypercard_content;
};