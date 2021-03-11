/*=====================================================================
WorkerThread.h
--------------
Copyright Glare Technologies Limited 2018 -
=====================================================================*/
#pragma once


#include <MessageableThread.h>
#include <Platform.h>
#include <MyThread.h>
#include <EventFD.h>
#include <MySocket.h>
#include <string>
class WorkUnit;
class PrintOutput;
class ThreadMessageSink;
class Server;


/*=====================================================================
WorkerThread
------------
This thread runs on the server, and handles communication with a single client.
=====================================================================*/
class WorkerThread : public MessageableThread
{
public:
	// May throw glare::Exception from constructor if EventFD init fails.
	WorkerThread(int thread_id, const Reference<SocketInterface>& socket, Server* server);
	virtual ~WorkerThread();

	virtual void doRun();

	std::string connected_world_name;

	void enqueueDataToSend(const std::string& data); // threadsafe

private:
	void sendGetFileMessageIfNeeded(const std::string& resource_URL);
	void handleResourceUploadConnection();
	void handleResourceDownloadConnection();
	void handleScreenshotBotConnection();

	ThreadSafeQueue<std::string> data_to_send;

	Reference<SocketInterface> socket;
	Server* server;
	EventFD event_fd;	
};
