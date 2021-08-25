/*=====================================================================
WorkerThread.cpp
------------------
Copyright Glare Technologies Limited 2018 -
=====================================================================*/
#include "WorkerThread.h"


#include "ServerWorldState.h"
#include "Server.h"
#include "Screenshot.h"
#include "SubEthTransaction.h"
#include "../shared/Protocol.h"
#include "../shared/UID.h"
#include "../shared/WorldObject.h"
#include <vec3.h>
#include <ConPrint.h>
#include <Clock.h>
#include <AESEncryption.h>
#include <SHA256.h>
#include <Base64.h>
#include <Exception.h>
#include <MySocket.h>
#include <URL.h>
#include <Lock.h>
#include <StringUtils.h>
#include <CryptoRNG.h>
#include <SocketBufferOutStream.h>
#include <PlatformUtils.h>
#include <KillThreadMessage.h>
#include <Parser.h>
#include <FileUtils.h>
#include <MemMappedFile.h>


static const bool VERBOSE = false;
static const int MAX_STRING_LEN = 10000;


WorkerThread::WorkerThread(int thread_id_, const Reference<SocketInterface>& socket_, Server* server_)
:	socket(socket_),
	server(server_)
{
	//if(VERBOSE) print("event_fd.efd: " + toString(event_fd.efd));
}


WorkerThread::~WorkerThread()
{
}


void WorkerThread::sendGetFileMessageIfNeeded(const std::string& resource_URL)
{
	if(!ResourceManager::isValidURL(resource_URL))
		throw glare::Exception("Invalid URL: '" + resource_URL + "'");

	try
	{
		URL parsed_url = URL::parseURL(resource_URL);

		// If this is a web URL, then we don't need to get it from the client.
		if(parsed_url.scheme == "http" || parsed_url.scheme == "https")
			return;
	}
	catch(glare::Exception&)
	{}

	// See if we have this file on the server already
	{
		const std::string path = server->world_state->resource_manager->pathForURL(resource_URL);
		if(FileUtils::fileExists(path))
		{
			// Check hash?
			conPrint("resource file with URL '" + resource_URL + "' already present on disk.");
		}
		else
		{
			conPrint("resource file with URL '" + resource_URL + "' not present on disk, sending get file message to client.");

			// We need the file from the client.
			// Send the client a 'get file' message
			SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
			packet.writeUInt32(Protocol::GetFile);
			packet.writeStringLengthFirst(resource_URL);

			this->enqueueDataToSend(packet);
		}
	}
}


static void writeErrorMessageToClient(SocketInterfaceRef& socket, const std::string& msg)
{
	SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
	packet.writeUInt32(Protocol::ErrorMessageID);
	packet.writeStringLengthFirst(msg);
	socket->writeData(packet.buf.data(), packet.buf.size());
}


// Enqueues packet to WorkerThreads to send to clients connected to the server.
static void enqueuePacketToBroadcast(const SocketBufferOutStream& packet_buffer, Server* server)
{
	assert(packet_buffer.buf.size() > 0);
	if(packet_buffer.buf.size() > 0)
	{
		Lock lock(server->worker_thread_manager.getMutex());
		for(auto i = server->worker_thread_manager.getThreads().begin(); i != server->worker_thread_manager.getThreads().end(); ++i)
		{
			assert(dynamic_cast<WorkerThread*>(i->getPointer()));
			static_cast<WorkerThread*>(i->getPointer())->enqueueDataToSend(packet_buffer);
		}
	}
}


void WorkerThread::handleResourceUploadConnection()
{
	conPrint("handleResourceUploadConnection()");

	try
	{
		const std::string username = socket->readStringLengthFirst(MAX_STRING_LEN);
		const std::string password = socket->readStringLengthFirst(MAX_STRING_LEN);

		conPrint("\tusername: '" + username + "'");

		UserRef client_user;
		{
			Lock lock(server->world_state->mutex);
			auto res = server->world_state->name_to_users.find(username);
			if(res != server->world_state->name_to_users.end())
			{
				User* user = res->second.getPointer();
				if(user->isPasswordValid(password))
					client_user = user; // Password is valid, log user in.
			}
		}

		if(client_user.isNull())
		{
			conPrint("\tLogin failed.");
			socket->writeUInt32(Protocol::LogInFailure);
			socket->writeStringLengthFirst("Login failed.");
			return;
		}


		const std::string URL = socket->readStringLengthFirst(MAX_STRING_LEN);

		conPrint("\tURL: '" + URL + "'");

		/*if(!ResourceManager::isValidURL(URL))
		{
		conPrint("Invalid URL '" + URL + "'");
		throw glare::Exception("Invalid URL '" + URL + "'");
		}*/

		// See if we have a resource in the ResourceManager already
		ResourceRef resource = server->world_state->resource_manager->getOrCreateResourceForURL(URL); // Will create a new Resource ob if not already inserted.
		if(resource->owner_id == UserID::invalidUserID())
		{
			// No such resource existed before, client may create this resource.
		}
		else // else if resource already existed:
		{
			if(resource->owner_id != client_user->id) // If this resource already exists and was created by someone else:
			{
				socket->writeUInt32(Protocol::NoWritePermissions);
				socket->writeStringLengthFirst("Not allowed to upload resource to URL '" + URL + ", someone else created a resource at this URL already.");
				return;
			}
		}
		
		// resource->setState(Resource::State_Transferring); // Don't set this (for now) or we will have to handle changing it on exceptions below.


		const uint64 file_len = socket->readUInt64();
		conPrint("\tfile_len: " + toString(file_len) + " B");
		if(file_len == 0)
		{
			socket->writeUInt32(Protocol::InvalidFileSize);
			socket->writeStringLengthFirst("Invalid file len of zero.");
			return;
		}

		// TODO: cap length in a better way
		if(file_len > 1000000000)
		{
			socket->writeUInt32(Protocol::InvalidFileSize);
			socket->writeStringLengthFirst("uploaded file too large.");
			return;
		}

		// Otherwise upload is allowed:
		socket->writeUInt32(Protocol::UploadAllowed);

		std::vector<uint8> buf(file_len);
		socket->readData(buf.data(), file_len);

		conPrint("\tReceived file with URL '" + URL + "' from client. (" + toString(file_len) + " B)");

		// Save to disk
		const std::string local_path = server->world_state->resource_manager->pathForURL(URL);

		conPrint("\tWriting to disk at '" + local_path + "'...");

		FileUtils::writeEntireFile(local_path, (const char*)buf.data(), buf.size());

		conPrint("\tWritten to disk.");

		resource->owner_id = client_user->id;
		resource->setState(Resource::State_Present);
		server->world_state->markAsChanged();


		// Send NewResourceOnServer message to connected clients
		{
			SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
			packet.writeUInt32(Protocol::NewResourceOnServer);
			packet.writeStringLengthFirst(URL);

			enqueuePacketToBroadcast(packet, server);
		}

		// Connection will be closed by the client after the client has uploaded the file.  Wait for the connection to close.
		//socket->waitForGracefulDisconnect();
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Resource upload client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}
}


void WorkerThread::handleResourceDownloadConnection()
{
	conPrint("handleResourceDownloadConnection()");

	try
	{

		while(1)
		{
			const uint32 msg_type = socket->readUInt32();
			if(msg_type == Protocol::GetFiles)
			{
				conPrint("------GetFiles-----");

				const uint64 num_resources = socket->readUInt64();
				conPrint("\tnum_resources requested: " + toString(num_resources));

				for(size_t i=0; i<num_resources; ++i)
				{
					const std::string URL = socket->readStringLengthFirst(MAX_STRING_LEN);

					conPrint("\tRequested URL: '" + URL + "'");

					if(!ResourceManager::isValidURL(URL))
					{
						conPrint("\tRequested URL was invalid.");
						socket->writeUInt32(1); // write error msg to client
					}
					else
					{
						conPrint("\tRequested URL was valid.");

						const ResourceRef resource = server->world_state->resource_manager->getExistingResourceForURL(URL);
						if(resource.isNull() || (resource->getState() != Resource::State_Present))
						{
							conPrint("\tRequested URL was not present on disk.");
							socket->writeUInt32(1); // write error msg to client
						}
						else
						{
							const std::string local_path = resource->getLocalPath();

							conPrint("\tlocal path: '" + local_path + "'");

							try
							{
								// Load resource off disk
								MemMappedFile file(local_path);
								conPrint("\tSending file to client.");
								socket->writeUInt32(0); // write OK msg to client
								socket->writeUInt64(file.fileSize()); // Write file size
								socket->writeData(file.fileData(), file.fileSize()); // Write file data

								conPrint("\tSent file '" + local_path + "' to client. (" + toString(file.fileSize()) + " B)");
							}
							catch(glare::Exception& e)
							{
								conPrint("\tException while trying to load file for URL: " + e.what());

								socket->writeUInt32(1); // write error msg to client
							}
						}
					}
				}
			}
			else
			{
				conPrint("handleResourceDownloadConnection(): Unhandled msg type: " + toString(msg_type));
				return;
			}
		}
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Resource download client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}
}


void WorkerThread::handleScreenshotBotConnection()
{
	conPrint("handleScreenshotBotConnection()");

	// TODO: authentication

	try
	{
		while(1)
		{
			// Poll server state for a screenshot request
			ScreenshotRef screenshot;

			{ // lock scope
				Lock lock(server->world_state->mutex);

				server->world_state->last_screenshot_bot_contact_time = TimeStamp::currentTime();

				// Find first screenshot in ScreenshotState_notdone state.  NOTE: slow linear scan.
				for(auto it = server->world_state->screenshots.begin(); it != server->world_state->screenshots.end(); ++it)
				{
					if(it->second->state == Screenshot::ScreenshotState_notdone)
					{
						screenshot = it->second;
						break;
					}
				}
			} // End lock scope

			if(screenshot.nonNull()) // If there is a screenshot to take:
			{
				socket->writeUInt32(Protocol::ScreenShotRequest);

				socket->writeDouble(screenshot->cam_pos.x);
				socket->writeDouble(screenshot->cam_pos.y);
				socket->writeDouble(screenshot->cam_pos.z);
				socket->writeDouble(screenshot->cam_angles.x);
				socket->writeDouble(screenshot->cam_angles.y);
				socket->writeDouble(screenshot->cam_angles.z);
				socket->writeInt32(screenshot->width_px);
				socket->writeInt32(screenshot->highlight_parcel_id);

				// Read response
				const uint32 result = socket->readUInt32();
				if(result == Protocol::ScreenShotSucceeded)
				{
					// Read screenshot data
					const uint64 data_len = socket->readUInt64();
					if(data_len > 100000000) // ~100MB
						throw glare::Exception("data_len was too large");

					conPrint("Receiving screenshot of " + toString(data_len) + " B");
					std::vector<uint8> data(data_len);
					socket->readData(data.data(), data_len);

					conPrint("Received screenshot of " + toString(data_len) + " B");

					// Generate random path
					const int NUM_BYTES = 16;
					uint8 pathdata[NUM_BYTES];
					CryptoRNG::getRandomBytes(pathdata, NUM_BYTES);
					const std::string screenshot_filename = "screenshot_" + StringUtils::convertByteArrayToHexString(pathdata, NUM_BYTES) + ".jpg";
					const std::string screenshot_path = server->screenshot_dir + "/" + screenshot_filename;

					// Save screenshot to path
					FileUtils::writeEntireFile(screenshot_path, data);

					conPrint("Saved to disk at " + screenshot_path);

					screenshot->state = Screenshot::ScreenshotState_done;
					screenshot->local_path = screenshot_path;

					server->world_state->markAsChanged();
				}
				else
					throw glare::Exception("Client reported screenshot taking failed.");
			}
			else
			{
				// There is no current screenshot request, sleep for a while
				PlatformUtils::Sleep(10000);
			}
		}
	}
	catch(glare::Exception& e)
	{
		conPrint("handleScreenshotBotConnection: glare::Exception: " + e.what());
	}
	catch(std::exception& e)
	{
		conPrint(std::string("handleScreenshotBotConnection: Caught std::exception: ") + e.what());
	}
}


void WorkerThread::handleEthBotConnection()
{
	conPrint("handleEthBotConnection()");

	try
	{
		// Do authentication
		const std::string password = socket->readStringLengthFirst(10000);
		if(SHA256::hash(password) != StringUtils::convertHexToBinary("9bd7674cb1e7ec496f88b31264aaa3ff75ce9d60aabc5e6fd0f8e7ba8a27f829")) // See ethBotTests().
			throw glare::Exception("Invalid password");
			
		while(1)
		{
			// Poll server state for a request
			SubEthTransactionRef trans;
			uint64 largest_nonce_used = 0; 
			{ // lock scope
				Lock lock(server->world_state->mutex);

				server->world_state->last_eth_bot_contact_time = TimeStamp::currentTime();

				// Find first transction in New state.  NOTE: slow linear scan.
				for(auto it = server->world_state->sub_eth_transactions.begin(); it != server->world_state->sub_eth_transactions.end(); ++it)
				{
					if(it->second->state == SubEthTransaction::State_New)
					{
						trans = it->second;
						break;
					}
				}

				// Work out nonce to use for this transaction.  First, work out largest nonce used for succesfully submitted transactions
				for(auto it = server->world_state->sub_eth_transactions.begin(); it != server->world_state->sub_eth_transactions.end(); ++it)
				{
					if(it->second->state == SubEthTransaction::State_Completed)
						largest_nonce_used = myMax(largest_nonce_used, it->second->nonce);
				}
			} // End lock scope

			const uint64 MIN_NONCE = 2; // To reflect any existing transactions on account
			const uint64 next_nonce = myMax(MIN_NONCE, largest_nonce_used) + 1;

			if(trans.nonNull()) // If there is a transaction to submit:
			{
				socket->writeUInt32(Protocol::SubmitEthTransactionRequest);

				trans->nonce = next_nonce; // Update transaction nonce
				trans->submitted_time = TimeStamp::currentTime();
				
				writeToStream(*trans, *socket);

				// Read response
				const uint32 result = socket->readUInt32();
				if(result == Protocol::EthTransactionSubmitted)
				{
					const UInt256 transaction_hash = readUInt256FromStream(*socket);

					conPrint("Transaction was submitted.");

					trans->state = SubEthTransaction::State_Completed; // State_Submitted;
					trans->transaction_hash = transaction_hash;

					// Mark parcel as minted as an NFT
					{ // lock scope
						Lock lock(server->world_state->mutex);

						auto parcel_res = server->world_state->getRootWorldState()->parcels.find(trans->parcel_id);
						if(parcel_res != server->world_state->getRootWorldState()->parcels.end())
						{
							Parcel* parcel = parcel_res->second.ptr();
							parcel->nft_status = Parcel::NFTStatus_MintedNFT;
						}
					} // End lock scope

				}
				else if(result == Protocol::EthTransactionSubmissionFailed)
				{
					conPrint("Transaction submission failed.");

					trans->state = SubEthTransaction::State_Submitted;
					trans->transaction_hash = UInt256(0);
					trans->submission_error_message = socket->readStringLengthFirst(10000);
				}
				else
					throw glare::Exception("Client reported transaction submission failed.");

				server->world_state->markAsChanged();
			}
			else
			{
				// There is no current transaction to process, sleep for a while
				PlatformUtils::Sleep(10000);
			}
		}
	}
	catch(glare::Exception& e)
	{
		conPrint("handleEthBotConnection: glare::Exception: " + e.what());
	}
	catch(std::exception& e)
	{
		conPrint(std::string("handleEthBotConnection: Caught std::exception: ") + e.what());
	}
}


static bool objectIsInParcelOwnedByLoggedInUser(const WorldObject& ob, const User& user, ServerWorldState& world_state)
{
	assert(user.id.valid());

	for(auto& it : world_state.parcels)
	{
		const Parcel* parcel = it.second.ptr();
		if((parcel->owner_id == user.id) && parcel->pointInParcel(ob.pos))
			return true;
	}

	return false;
}


// NOTE: world state mutex should be locked before calling this method.
static bool userHasObjectWritePermissions(const WorldObject& ob, const User& user, const std::string& connected_world_name, ServerWorldState& world_state)
{
	if(user.id.valid())
	{
		return (user.id == ob.creator_id) || // If the user created/owns the object
			isGodUser(user.id) || // or if the user is the god user (id 0)
			user.name == "lightmapperbot" || // lightmapper bot has full write permissions for now.
			((connected_world_name != "") && (user.name == connected_world_name)) || // or if this is the user's personal world
			objectIsInParcelOwnedByLoggedInUser(ob, user, world_state); // Can modify objects owned by other people if they are in parcels you own.
	}
	else
		return false;
}


void WorkerThread::doRun()
{
	PlatformUtils::setCurrentThreadNameIfTestsEnabled("WorkerThread");

	ServerAllWorldsState* world_state = server->world_state.getPointer();

	UID client_avatar_uid(0);
	Reference<User> client_user; // Will be a null reference if client is not logged in, otherwise will refer to the user account the client is logged in to.
	Reference<ServerWorldState> cur_world_state; // World the client is connected to.
	bool logged_in_user_is_lightmapper_bot = false; // Just for updating the last_lightmapper_bot_contact_time.

	try
	{
		// Read hello bytes
		const uint32 hello = socket->readUInt32();
		printVar(hello);
		if(hello != Protocol::CyberspaceHello)
			throw glare::Exception("Received invalid hello message (" + toString(hello) + ") from client.");
		
		// Write hello response
		socket->writeUInt32(Protocol::CyberspaceHello);

		// Read protocol version
		const uint32 client_version = socket->readUInt32();
		printVar(client_version);
		if(client_version < Protocol::CyberspaceProtocolVersion)
		{
			socket->writeUInt32(Protocol::ClientProtocolTooOld);
			socket->writeStringLengthFirst("Sorry, your client protocol version (" + toString(client_version) + ") is too old, require version " + 
				toString(Protocol::CyberspaceProtocolVersion) + ".  Please update your client at substrata.info.");
		}
		else if(client_version > Protocol::CyberspaceProtocolVersion)
		{
			socket->writeUInt32(Protocol::ClientProtocolTooNew);
			socket->writeStringLengthFirst("Sorry, your client protocol version (" + toString(client_version) + ") is too new, require version " + 
				toString(Protocol::CyberspaceProtocolVersion) + ".  Please use an older client.");
		}
		else
		{
			socket->writeUInt32(Protocol::ClientProtocolOK);
		}

		const uint32 connection_type = socket->readUInt32();
	
		if(connection_type == Protocol::ConnectionTypeUploadResource)
		{
			handleResourceUploadConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeDownloadResources)
		{
			handleResourceDownloadConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeScreenshotBot)
		{
			handleScreenshotBotConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeEthBot)
		{
			handleEthBotConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeUpdates)
		{
			// Read name of world to connect to
			const std::string world_name = socket->readStringLengthFirst(1000);
			this->connected_world_name = world_name;

			{
				Lock lock(world_state->mutex);
				// Create world if didn't exist before.
				// TODO: do this here? or restrict possible world names to those of users etc..?
				if(world_state->world_states[world_name].isNull())
					world_state->world_states[world_name] = new ServerWorldState();
				cur_world_state = world_state->world_states[world_name];
			}

			// Write avatar UID assigned to the connected client.
			client_avatar_uid = world_state->getNextAvatarUID();
			writeToStream(client_avatar_uid, *socket);

			// Send TimeSyncMessage packet to client
			{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
				packet.writeUInt32(Protocol::TimeSyncMessage);
				packet.writeDouble(server->getCurrentGlobalTime());
				socket->writeData(packet.buf.data(), packet.buf.size());
			}

			// Send all current avatar state data to client
			{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);

				{ // Lock scope
					Lock lock(world_state->mutex);
					for(auto it = cur_world_state->avatars.begin(); it != cur_world_state->avatars.end(); ++it)
					{
						const Avatar* avatar = it->second.getPointer();

						// Write AvatarIsHere message
						packet.writeUInt32(Protocol::AvatarIsHere);
						writeToNetworkStream(*avatar, packet);
					}
				} // End lock scope

				socket->writeData(packet.buf.data(), packet.buf.size());
			}

			// Send all current object data to client
			/*{
				Lock lock(world_state->mutex);
				for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
				{
					const WorldObject* ob = it->second.getPointer();

					// Send ObjectCreated packet
					SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
					packet.writeUInt32(Protocol::ObjectCreated);
					ob->writeToNetworkStream(packet);
					socket->writeData(packet.buf.data(), packet.buf.size());
				}
			}*/

			// Send all current parcel data to client
			{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);

				{ // Lock scope
					Lock lock(world_state->mutex);
					for(auto it = cur_world_state->parcels.begin(); it != cur_world_state->parcels.end(); ++it)
					{
						const Parcel* parcel = it->second.getPointer();

						// Send ParcelCreated message
						packet.writeUInt32(Protocol::ParcelCreated);
						writeToNetworkStream(*parcel, packet);
					}
				} // End lock scope

				socket->writeData(packet.buf.data(), packet.buf.size());
			}

			// Send a message saying we have sent all initial state
			/*{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
				packet.writeUInt32(Protocol::InitialStateSent);
				socket->writeData(packet.buf.data(), packet.buf.size());
			}*/
		}

		assert(cur_world_state.nonNull());


		socket->setNoDelayEnabled(true); // We want to send out lots of little packets with low latency.  So disable Nagle's algorithm, e.g. send coalescing.

		
		while(1) // write to / read from socket loop
		{
			// See if we have any pending data to send in the data_to_send queue, and if so, send all pending data.
			if(VERBOSE) conPrint("WorkerThread: checking for pending data to send...");

			// We don't want to do network writes while holding the data_to_send_mutex.  So copy to temp_data_to_send.
			{
				Lock lock(data_to_send_mutex);
				temp_data_to_send = data_to_send;
				data_to_send.clear();
			}

			if(temp_data_to_send.nonEmpty() && (connection_type == Protocol::ConnectionTypeUpdates))
			{
				socket->writeData(temp_data_to_send.data(), temp_data_to_send.size());
				temp_data_to_send.clear();
			}


			if(logged_in_user_is_lightmapper_bot)
				server->world_state->last_lightmapper_bot_contact_time = TimeStamp::currentTime(); // bit of a hack


#if defined(_WIN32) || defined(OSX)
			if(socket->readable(0.05)) // If socket has some data to read from it:
#else
			if(socket->readable(event_fd)) // Block until either the socket is readable or the event fd is signalled, which means we have data to write.
#endif
			{
				// Read msg type
				const uint32 msg_type = socket->readUInt32();
				switch(msg_type)
				{
				case Protocol::AvatarTransformUpdate:
					{
						//conPrint("AvatarTransformUpdate");
						const UID avatar_uid = readUIDFromStream(*socket);
						const Vec3d pos = readVec3FromStream<double>(*socket);
						const Vec3f rotation = readVec3FromStream<float>(*socket);
						const uint32 anim_state = socket->readUInt32();

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->pos = pos;
								avatar->rotation = rotation;
								avatar->anim_state = anim_state;
								avatar->transform_dirty = true;

								//conPrint("updated avatar transform");
							}
						}
						break;
					}
				case Protocol::AvatarFullUpdate:
					{
						conPrint("Protocol::AvatarFullUpdate");
						const UID avatar_uid = readUIDFromStream(*socket);

						Avatar temp_avatar;
						readFromNetworkStreamGivenUID(*socket, temp_avatar); // Read message data before grabbing lock

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->copyNetworkStateFrom(temp_avatar);
								avatar->other_dirty = true;


								// Store avatar settings in the user data
								if(client_user.nonNull())
								{
									client_user->avatar_settings = avatar->avatar_settings;

									server->world_state->markAsChanged(); // TODO: only do this if avatar settings actually changed.

									conPrint("Updated user avatar settings.  model_url: " + client_user->avatar_settings.model_url);
								}

								//conPrint("updated avatar transform");
							}
						}

						if(!temp_avatar.avatar_settings.model_url.empty())
							sendGetFileMessageIfNeeded(temp_avatar.avatar_settings.model_url);

						// Process resources
						std::set<std::string> URLs;
						temp_avatar.getDependencyURLSetForAllLODLevels(URLs);
						for(auto it = URLs.begin(); it != URLs.end(); ++it)
							sendGetFileMessageIfNeeded(*it);

						break;
					}
				case Protocol::CreateAvatar:
					{
						conPrint("received Protocol::CreateAvatar");
						// Note: name will come from user account
						// will use the client_avatar_uid that we assigned to the client
						
						Avatar temp_avatar;
						temp_avatar.uid = readUIDFromStream(*socket); // Will be replaced.
						readFromNetworkStreamGivenUID(*socket, temp_avatar); // Read message data before grabbing lock

						temp_avatar.name = client_user.isNull() ? "Anonymous" : client_user->name;

						const UID use_avatar_uid = client_avatar_uid;
						temp_avatar.uid = use_avatar_uid;

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(use_avatar_uid);
							if(res == cur_world_state->avatars.end())
							{
								// Avatar for UID not already created, create it now.
								AvatarRef avatar = new Avatar();
								avatar->uid = use_avatar_uid;
								avatar->copyNetworkStateFrom(temp_avatar);
								avatar->state = Avatar::State_JustCreated;
								avatar->other_dirty = true;
								cur_world_state->avatars.insert(std::make_pair(use_avatar_uid, avatar));

								conPrint("created new avatar");
							}
						}

						if(!temp_avatar.avatar_settings.model_url.empty())
							sendGetFileMessageIfNeeded(temp_avatar.avatar_settings.model_url);

						// Process resources
						std::set<std::string> URLs;
						temp_avatar.getDependencyURLSetForAllLODLevels(URLs);
						for(auto it = URLs.begin(); it != URLs.end(); ++it)
							sendGetFileMessageIfNeeded(*it);

						conPrint("New Avatar creation: username: '" + temp_avatar.name + "', model_url: '" + temp_avatar.avatar_settings.model_url + "'");

						break;
					}
				case Protocol::AvatarDestroyed:
					{
						conPrint("AvatarDestroyed");
						const UID avatar_uid = readUIDFromStream(*socket);

						// Mark avatar as dead
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->state = Avatar::State_Dead;
								avatar->other_dirty = true;
							}
						}
						break;
					}
				case Protocol::ObjectTransformUpdate:
					{
						//conPrint("ObjectTransformUpdate");
						const UID object_uid = readUIDFromStream(*socket);
						const Vec3d pos = readVec3FromStream<double>(*socket);
						const Vec3f axis = readVec3FromStream<float>(*socket);
						const float angle = socket->readFloat();

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to modify an object.");
						}
						else
						{
							std::string err_msg_to_client;
							// Look up existing object in world state
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									if(!userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state))
										err_msg_to_client = "You must be the owner of this object to change it.";
									else
									{
										ob->pos = pos;
										ob->axis = axis;
										ob->angle = angle;
										ob->from_remote_transform_dirty = true;
										cur_world_state->dirty_from_remote_objects.insert(ob);
									}

									//conPrint("updated object transform");
								}
							} // End lock scope

							if(!err_msg_to_client.empty())
								writeErrorMessageToClient(socket, err_msg_to_client);
						}

						break;
					}
				case Protocol::ObjectFullUpdate:
					{
						//conPrint("ObjectFullUpdate");
						const UID object_uid = readUIDFromStream(*socket);

						WorldObject temp_ob;
						readFromNetworkStreamGivenUID(*socket, temp_ob); // Read rest of ObjectFullUpdate message.

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to modify an object.");
						}
						else
						{
							// Look up existing object in world state
							bool send_must_be_owner_msg = false;
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									if(!userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state))
									{
										send_must_be_owner_msg = true;
									}
									else
									{
										ob->copyNetworkStateFrom(temp_ob);

										ob->from_remote_other_dirty = true;
										cur_world_state->dirty_from_remote_objects.insert(ob);

										// Process resources
										std::set<std::string> URLs;
										ob->getDependencyURLSetForAllLODLevels(URLs);
										for(auto it = URLs.begin(); it != URLs.end(); ++it)
											sendGetFileMessageIfNeeded(*it);
									}
								}
							} // End lock scope

							if(send_must_be_owner_msg)
								writeErrorMessageToClient(socket, "You must be the owner of this object to change it.");
						}
						break;
					}
				case Protocol::ObjectLightmapURLChanged:
					{
						//conPrint("ObjectLightmapURLChanged");
						const UID object_uid = readUIDFromStream(*socket);
						const std::string new_lightmap_url = socket->readStringLengthFirst(10000);

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();
					
								ob->lightmap_url = new_lightmap_url;

								ob->from_remote_lightmap_url_dirty = true;
								cur_world_state->dirty_from_remote_objects.insert(ob);
							}
						}
						break;
					}
				case Protocol::ObjectModelURLChanged:
					{
						//conPrint("ObjectModelURLChanged");
						const UID object_uid = readUIDFromStream(*socket);
						const std::string new_model_url = socket->readStringLengthFirst(10000);

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();
					
								ob->model_url = new_model_url;

								ob->from_remote_model_url_dirty = true;
								cur_world_state->dirty_from_remote_objects.insert(ob);
							}
						}
						break;
					}
				case Protocol::ObjectFlagsChanged:
					{
						//conPrint("ObjectFlagsChanged");
						const UID object_uid = readUIDFromStream(*socket);
						const uint32 flags = socket->readUInt32();

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();

								ob->flags = flags; // Copy flags

								ob->from_remote_flags_dirty = true;
								cur_world_state->dirty_from_remote_objects.insert(ob);
							}
						}
						break;
					}
				case Protocol::CreateObject: // Client wants to create an object
					{
						conPrint("CreateObject");

						WorldObjectRef new_ob = new WorldObject();
						new_ob->uid = readUIDFromStream(*socket); // Read dummy UID
						readFromNetworkStreamGivenUID(*socket, *new_ob);

						conPrint("model_url: '" + new_ob->model_url + "', pos: " + new_ob->pos.toString());

						// If client is not logged in, refuse object creation.
						if(client_user.isNull())
						{
							conPrint("Creation denied, user was not logged in.");
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::ErrorMessageID);
							packet.writeStringLengthFirst("You must be logged in to create an object.");
							socket->writeData(packet.buf.data(), packet.buf.size());
						}
						else
						{
							new_ob->creator_id = client_user->id;
							new_ob->created_time = TimeStamp::currentTime();
							new_ob->creator_name = client_user->name;

							std::set<std::string> URLs;
							new_ob->getDependencyURLSetForAllLODLevels(URLs);
							for(auto it = URLs.begin(); it != URLs.end(); ++it)
								sendGetFileMessageIfNeeded(*it);

							// Look up existing object in world state
							{
								::Lock lock(world_state->mutex);

								// Object for UID not already created, create it now.
								new_ob->uid = world_state->getNextObjectUID();
								new_ob->state = WorldObject::State_JustCreated;
								new_ob->from_remote_other_dirty = true;
								cur_world_state->dirty_from_remote_objects.insert(new_ob);
								cur_world_state->objects.insert(std::make_pair(new_ob->uid, new_ob));
							}
						}

						break;
					}
				case Protocol::DestroyObject: // Client wants to destroy an object.
					{
						conPrint("DestroyObject");
						const UID object_uid = readUIDFromStream(*socket);

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to destroy an object.");
						}
						else
						{
							bool send_must_be_owner_msg = false;
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									const bool have_delete_perms = userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state);
									if(!have_delete_perms)
										send_must_be_owner_msg = true;
									else
									{
										// Mark object as dead
										ob->state = WorldObject::State_Dead;
										ob->from_remote_other_dirty = true;
										cur_world_state->dirty_from_remote_objects.insert(ob);
									}
								}
							} // End lock scope

							if(send_must_be_owner_msg)
								writeErrorMessageToClient(socket, "You must be the owner of this object to destroy it.");
						}
						break;
					}
				case Protocol::GetAllObjects: // Client wants to get all objects in world
				{
					conPrint("GetAllObjects");

					SocketBufferOutStream temp_buf(SocketBufferOutStream::DontUseNetworkByteOrder);

					{
						Lock lock(world_state->mutex);
						for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
						{
							const WorldObject* ob = it->second.getPointer();

							// Send ObjectInitialSend message
							temp_buf.writeUInt32(Protocol::ObjectInitialSend);
							ob->writeToNetworkStream(temp_buf);
						}
					}

					temp_buf.writeUInt32(Protocol::AllObjectsSent); // Terminate the buffer with a AllObjectsSent message.

					socket->writeData(temp_buf.buf.data(), temp_buf.buf.size());

					break;
				}
				case Protocol::QueryObjects: // Client wants to query objects in certain grid cells
				{
					const uint32 num_cells = socket->readUInt32();
					if(num_cells > 100000)
						throw glare::Exception("QueryObjects: too many cells: " + toString(num_cells));

					//conPrint("QueryObjects, num_cells=" + toString(num_cells));

					//conPrint("QueryObjects: num_cells " + toString(num_cells));
					
					// Read cell coords from network and make AABBs for cells
					js::Vector<js::AABBox, 16> cell_aabbs(num_cells);
					for(uint32 i=0; i<num_cells; ++i)
					{
						const int x = socket->readInt32();
						const int y = socket->readInt32();
						const int z = socket->readInt32();

						//if(i < 10)
						//	conPrint("cell " + toString(i) + " coords: " + toString(x) + ", " + toString(y) + ", " + toString(z));

						const float CELL_WIDTH = 200.f; // NOTE: has to be the same value as in gui_client/ProximityLoader.cpp.

						cell_aabbs[i] = js::AABBox(
							Vec4f(0,0,0,1) + Vec4f((float)x,     (float)y,     (float)z,     0)*CELL_WIDTH,
							Vec4f(0,0,0,1) + Vec4f((float)(x+1), (float)(y+1), (float)(z+1), 0)*CELL_WIDTH
						);
					}


					SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
					int num_obs_written = 0;

					{ // Lock scope
						Lock lock(world_state->mutex);
						for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
						{
							const WorldObject* ob = it->second.ptr();

							// See if the object is in any of the cell AABBs
							bool in_cell = false;
							for(uint32 i=0; i<num_cells; ++i)
								if(cell_aabbs[i].contains(ob->pos.toVec4fPoint()))
								{
									in_cell = true;
									break;
								}

							if(in_cell)
							{
								// Send ObjectInitialSend packet
								packet.writeUInt32(Protocol::ObjectInitialSend);
								ob->writeToNetworkStream(packet);
								num_obs_written++;
							}
						}
					} // End lock scope

					socket->writeData(packet.buf.data(), packet.buf.size()); // Write data to network

					//conPrint("Sent back info on " + toString(num_obs_written) + " object(s)");

					break;
				}
				case Protocol::QueryParcels:
					{
						conPrint("QueryParcels");
						// Send all current parcel data to client
						SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
						packet.writeUInt32(Protocol::ParcelList); // Write message ID
						
						{
							Lock lock(world_state->mutex);
							packet.writeUInt64(cur_world_state->parcels.size()); // Write num parcels
							for(auto it = cur_world_state->parcels.begin(); it != cur_world_state->parcels.end(); ++it)
								writeToNetworkStream(*it->second, packet); // Write parcel
						}

						socket->writeData(packet.buf.data(), packet.buf.size()); // Send the data
						break;
					}
				case Protocol::ParcelFullUpdate: // Client wants to update a parcel
					{
						conPrint("ParcelFullUpdate");
						const ParcelID parcel_id = readParcelIDFromStream(*socket);

						// Look up existing parcel in world state
						{
							bool read = false;

							// Only allow updating of parcels is this is a website connection.
							const bool have_permissions = false;// connection_type == Protocol::ConnectionTypeWebsite;

							if(have_permissions)
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->parcels.find(parcel_id);
								if(res != cur_world_state->parcels.end())
								{
									// TODO: Check if this client has permissions to update the parcel information.

									Parcel* parcel = res->second.getPointer();
									readFromNetworkStreamGivenID(*socket, *parcel);
									read = true;
									parcel->from_remote_dirty = true;
								}
							}

							// Make sure we have read the whole pracel from the network stream
							if(!read)
							{
								Parcel dummy;
								readFromNetworkStreamGivenID(*socket, dummy);
							}
						}
						break;
					}
				case Protocol::ChatMessageID:
					{
						//const std::string name = socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string msg = socket->readStringLengthFirst(MAX_STRING_LEN);

						conPrint("Received chat message: '" + msg + "'");

						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to chat.");
						}
						else
						{
							// Enqueue chat messages to worker threads to send
							// Send ChatMessageID packet
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::ChatMessageID);
							packet.writeStringLengthFirst(client_user->name);
							packet.writeStringLengthFirst(msg);

							enqueuePacketToBroadcast(packet, server);
						}
						break;
					}
				case Protocol::UserSelectedObject:
					{
						//conPrint("Received UserSelectedObject msg.");

						const UID object_uid = readUIDFromStream(*socket);

						// Send message to connected clients
						{
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::UserSelectedObject);
							writeToStream(client_avatar_uid, packet);
							writeToStream(object_uid, packet);

							enqueuePacketToBroadcast(packet, server);
						}
						break;
					}
				case Protocol::UserDeselectedObject:
					{
						//conPrint("Received UserDeselectedObject msg.");

						const UID object_uid = readUIDFromStream(*socket);

						// Send message to connected clients
						{
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::UserDeselectedObject);
							writeToStream(client_avatar_uid, packet);
							writeToStream(object_uid, packet);

							enqueuePacketToBroadcast(packet, server);
						}
						break;
					}
				case Protocol::LogInMessage: // Client wants to log in.
					{
						conPrint("LogInMessage");

						const std::string username = socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string password = socket->readStringLengthFirst(MAX_STRING_LEN);

						conPrint("username: '" + username + "'");
						
						bool logged_in = false;
						{
							Lock lock(world_state->mutex);
							auto res = world_state->name_to_users.find(username);
							if(res != world_state->name_to_users.end())
							{
								User* user = res->second.getPointer();
								const bool password_valid = user->isPasswordValid(password);
								conPrint("password_valid: " + boolToString(password_valid));
								if(password_valid)
								{
									// Password is valid, log user in.
									client_user = user;

									logged_in = true;
								}
							}
						}

						conPrint("logged_in: " + boolToString(logged_in));
						if(logged_in)
						{
							if(username == "lightmapperbot")
								logged_in_user_is_lightmapper_bot = true;

							// Send logged-in message to client
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::LoggedInMessageID);
							writeToStream(client_user->id, packet);
							packet.writeStringLengthFirst(username);
							writeToStream(client_user->avatar_settings, packet);

							socket->writeData(packet.buf.data(), packet.buf.size());
						}
						else
						{
							// Login failed.  Send error message back to client
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::ErrorMessageID);
							packet.writeStringLengthFirst("Login failed: username or password incorrect.");
							socket->writeData(packet.buf.data(), packet.buf.size());
						}
					
						break;
					}
				case Protocol::LogOutMessage: // Client wants to log out.
					{
						conPrint("LogOutMessage");

						client_user = NULL; // Mark the client as not logged in.

						// Send logged-out message to client
						SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
						packet.writeUInt32(Protocol::LoggedOutMessageID);
						socket->writeData(packet.buf.data(), packet.buf.size());
						
						break;
					}
				case Protocol::SignUpMessage:
					{
						conPrint("SignUpMessage");

						const std::string username = socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string email    = socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string password = socket->readStringLengthFirst(MAX_STRING_LEN);

						try
						{

							conPrint("username: '" + username + "', email: '" + email + "'");

							bool signed_up = false;

							std::string msg_to_client;
							if(username.size() < 3)
								msg_to_client = "Username is too short, must have at least 3 characters";
							else
							{
								if(password.size() < 6)
									msg_to_client = "Password is too short, must have at least 6 characters";
								else
								{
									Lock lock(world_state->mutex);
									auto res = world_state->name_to_users.find(username);
									if(res == world_state->name_to_users.end())
									{
										Reference<User> new_user = new User();
										new_user->id = UserID((uint32)world_state->name_to_users.size());
										new_user->created_time = TimeStamp::currentTime();
										new_user->name = username;
										new_user->email_address = email;

										// We need a random salt for the user.
										uint8 random_bytes[32];
										CryptoRNG::getRandomBytes(random_bytes, 32); // throws glare::Exception

										std::string user_salt;
										Base64::encode(random_bytes, 32, user_salt); // Convert random bytes to base-64.

										new_user->password_hash_salt = user_salt;
										new_user->hashed_password = User::computePasswordHash(password, user_salt);

										// Add new user to world state
										world_state->user_id_to_users.insert(std::make_pair(new_user->id, new_user));
										world_state->name_to_users   .insert(std::make_pair(username,     new_user));
										world_state->markAsChanged(); // Mark as changed so gets saved to disk.

										client_user = new_user; // Log user in as well.
										signed_up = true;
									}
								}
							}

							conPrint("signed_up: " + boolToString(signed_up));
							if(signed_up)
							{
								conPrint("Sign up successful");
								// Send signed-up message to client
								SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
								packet.writeUInt32(Protocol::SignedUpMessageID);
								writeToStream(client_user->id, packet);
								packet.writeStringLengthFirst(username);
								socket->writeData(packet.buf.data(), packet.buf.size());
							}
							else
							{
								conPrint("Sign up failed.");

								// signup failed.  Send error message back to client
								SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
								packet.writeUInt32(Protocol::ErrorMessageID);
								packet.writeStringLengthFirst(msg_to_client);
								socket->writeData(packet.buf.data(), packet.buf.size());
							}
						}
						catch(glare::Exception& e)
						{
							conPrint("Sign up failed, internal error: " + e.what());

							// signup failed.  Send error message back to client
							SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
							packet.writeUInt32(Protocol::ErrorMessageID);
							packet.writeStringLengthFirst("Signup failed: internal error.");
							socket->writeData(packet.buf.data(), packet.buf.size());
						}

						break;
					}
				case Protocol::RequestPasswordReset:
					{
						conPrint("RequestPasswordReset");

						const std::string email    = socket->readStringLengthFirst(MAX_STRING_LEN);

						// NOTE: This stuff is done via the website now instead.

						//conPrint("email: " + email);
						//
						//// TEMP: Send password reset email in this thread for now. 
						//// TODO: move to another thread (make some kind of background task?)
						//{
						//	Lock lock(world_state->mutex);
						//	for(auto it = world_state->user_id_to_users.begin(); it != world_state->user_id_to_users.end(); ++it)
						//		if(it->second->email_address == email)
						//		{
						//			User* user = it->second.getPointer();
						//			try
						//			{
						//				user->sendPasswordResetEmail();
						//				world_state->markAsChanged(); // Mark as changed so gets saved to disk.
						//				conPrint("Sent user password reset email to '" + email + ", username '" + user->name + "'");
						//			}
						//			catch(glare::Exception& e)
						//			{
						//				conPrint("Sending password reset email failed: " + e.what());
						//			}
						//		}
						//}
					
						break;
					}
				case Protocol::ChangePasswordWithResetToken:
					{
						conPrint("ChangePasswordWithResetToken");
						
						const std::string email			= socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string reset_token	= socket->readStringLengthFirst(MAX_STRING_LEN);
						const std::string new_password	= socket->readStringLengthFirst(MAX_STRING_LEN);

						// NOTE: This stuff is done via the website now instead.
						// 
						//conPrint("email: " + email);
						//conPrint("reset_token: " + reset_token);
						////conPrint("new_password: " + new_password);
						//
						//{
						//	Lock lock(world_state->mutex);
						//
						//	// Find user with the given email address:
						//	for(auto it = world_state->user_id_to_users.begin(); it != world_state->user_id_to_users.end(); ++it)
						//		if(it->second->email_address == email)
						//		{
						//			User* user = it->second.getPointer();
						//			const bool reset = user->resetPasswordWithToken(reset_token, new_password);
						//			if(reset)
						//			{
						//				world_state->markAsChanged(); // Mark as changed so gets saved to disk.
						//				conPrint("User password successfully updated.");
						//			}
						//		}
						//}

						break;
					}
				default:			
					{
						//conPrint("Unknown message id: " + toString(msg_type));
						throw glare::Exception("Unknown message id: " + toString(msg_type));
					}
				}
			}
			else
			{
#if defined(_WIN32) || defined(OSX)
#else
				if(VERBOSE) conPrint("WorkerThread: event FD was signalled.");

				// The event FD was signalled, which means there is some data to send on the socket.
				// Reset the event fd by reading from it.
				event_fd.read();

				if(VERBOSE) conPrint("WorkerThread: event FD has been reset.");
#endif
			}
		}
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Updates client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}

	// Mark avatar corresponding to client as dead
	if(cur_world_state.nonNull())
	{
		Lock lock(world_state->mutex);
		if(cur_world_state->avatars.count(client_avatar_uid) == 1)
		{
			cur_world_state->avatars[client_avatar_uid]->state = Avatar::State_Dead;
			cur_world_state->avatars[client_avatar_uid]->other_dirty = true;
		}
	}
}


void WorkerThread::enqueueDataToSend(const std::string& data)
{
	if(VERBOSE) conPrint("WorkerThread::enqueueDataToSend(), data: '" + data + "'");

	// Append data to data_to_send
	if(!data.empty())
	{
		Lock lock(data_to_send_mutex);
		const size_t write_i = data_to_send.size();
		data_to_send.resize(write_i + data.size());
		std::memcpy(&data_to_send[write_i], data.data(), data.size());
	}

	event_fd.notify();
}


void WorkerThread::enqueueDataToSend(const SocketBufferOutStream& packet) // threadsafe
{
	// Append data to data_to_send
	if(!packet.buf.empty())
	{
		Lock lock(data_to_send_mutex);
		const size_t write_i = data_to_send.size();
		data_to_send.resize(write_i + packet.buf.size());
		std::memcpy(&data_to_send[write_i], packet.buf.data(), packet.buf.size());
	}

	event_fd.notify();
}