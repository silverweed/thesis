#include "server_tcp.hpp"
#include "blocking_queue.hpp"
#include "logging.hpp"
#include "server.hpp"
#include "server_resources.hpp"
#include "tcp_messages.hpp"
#include "tcp_serialize.hpp"
#include "xplatform.hpp"
#include <array>
#include <chrono>

using namespace logging;

struct TcpMsg {
	TcpMsgType type;
	/** Currently only used by REQ_MODEL */
	uint16_t payload;
};

static BlockingQueue<TcpMsg> gMsgRecvQueue;
static std::chrono::time_point<std::chrono::steady_clock> gLatestPing;

static void genUpdateLists(Server& server)
{
	// Regenerate lists of stuff to send
	{
		std::lock_guard<std::mutex> lock{ server.toClient.updates.mtx };
		server.toClient.updates.persistent.clear();
	}
	auto& toSend = server.networkThreads.tcpActive->resourcesToSend;

	{
		std::lock_guard<std::mutex> lock{ server.networkThreads.tcpActive->mtx };
		for (const auto& light : server.resources.pointLights)
			toSend.pointLights.emplace(light);
	}
}

static void loadAndEnqueueModel(Server& server, unsigned n)
{
	static const std::array<std::string, 4> modelList = { "/models/sponza/sponza.dae",
		"/models/nanosuit/nanosuit.obj",
		"/models/cat/cat.obj",
		"/models/wall/wall2.obj" };

	info("loadAndSendModel(", n, ")");

	if (n >= modelList.size()) {
		warn("Received a REQ_MODEL (", n, "), but models are only ", modelList.size(), "!");
		return;
	}

	const auto& path = modelList[n];
	const auto modelSid = sid((server.cwd + xplatPath(path.c_str()).c_str()));
	if (server.stuffSent.has(modelSid, modelSid))
		return;

	Model model;
	if (!loadSingleModel(server, path, &model))
		return;

	// Note: tcpActive->mtx is already locked by us
	server.networkThreads.tcpActive->resourcesToSend.models.emplace(model);
}

static bool expectTCPMsg(TcpMsgType type)
{
	return gMsgRecvQueue.pop_or_wait().type == type;
}

static bool tcp_connectionPrelude(socket_t clientSocket)
{
	// Connection prelude (one-time stuff)

	// Perform handshake
	if (!expectTCPMsg(TcpMsgType::HELO))
		return false;

	if (!sendTCPMsg(clientSocket, TcpMsgType::HELO_ACK))
		return false;

	// Wait for ready signal from client
	if (!expectTCPMsg(TcpMsgType::READY))
		return false;

	return true;
}

/** @return Number of bytes sent, or -1 */
static int64_t
	batch_sendTexture(socket_t clientSocket, Server& server, const std::string& texName, shared::TextureFormat fmt)
{
	if (texName.length() == 0)
		return 0;

	const auto texSid = sid(texName);
	if (server.stuffSent.has(texSid, texSid))
		return 0;

	info("* sending texture ", texName);

	std::size_t bytesSent;
	bool ok = sendTexture(clientSocket, server.resources, texName, fmt, &bytesSent);
	if (!ok) {
		err("batch_sendTexture: failed");
		return -1;
	}

	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return -1;
	}

	server.stuffSent.insert(texSid, texSid);

	return static_cast<int64_t>(bytesSent);
}

/** Send material (along with textures used by it) */
static bool batch_sendMaterial(socket_t clientSocket,
	Server& server,
	/* inout */ std::unordered_set<std::pair<std::string, shared::TextureFormat>>& texturesToSend,
	const Material& mat)
{
	// Don't send the same material twice
	if (server.stuffSent.has(mat.name, mat.name))
		return true;

	debug("sending new material ", mat.name);

	bool ok = sendMaterial(clientSocket, mat);
	if (!ok) {
		err("Failed sending material");
		return false;
	}

	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return false;
	}

	// Send textures later, after geometry
	texturesToSend.emplace(mat.diffuseTex, shared::TextureFormat::RGBA);
	texturesToSend.emplace(mat.specularTex, shared::TextureFormat::GREY);
	texturesToSend.emplace(mat.normalTex, shared::TextureFormat::RGBA);

	server.stuffSent.insert(mat.name, mat.name);

	return true;
}

/** Send model (along with materials used by it) */
static bool batch_sendModel(socket_t clientSocket,
	Server& server,
	/* inout */ std::unordered_set<std::pair<std::string, shared::TextureFormat>>& texturesToSend,
	const Model& model)
{
	if (server.stuffSent.has(model.name, model.name))
		return true;

	bool ok = sendModel(clientSocket, model);
	if (!ok) {
		err("Failed sending model");
		return false;
	}

	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return false;
	}

	info("model.materials = ", model.data->materials.size());
	for (const auto& mat : model.data->materials) {
		if (!batch_sendMaterial(clientSocket, server, texturesToSend, mat))
			return false;
	}

	server.stuffSent.insert(model.name, model.name);

	return true;
}

static bool
	batch_sendShaders(socket_t clientSocket, ServerResources& resources, const char* baseName, uint8_t shaderStage)
{
	bool ok = sendShader(clientSocket,
		resources,
		(std::string{ baseName } + ".vert.spv").c_str(),
		shaderStage,
		shared::ShaderStage::VERTEX);
	if (!ok) {
		err("Failed sending shader");
		return false;
	}

	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return false;
	}

	ok = sendShader(clientSocket,
		resources,
		(std::string{ baseName } + ".frag.spv").c_str(),
		shaderStage,
		shared::ShaderStage::FRAGMENT);
	if (!ok) {
		err("Failed sending shader");
		return false;
	}

	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return false;
	}

	return true;
}

static bool batch_sendPointLight(socket_t clientSocket, Server& server, const shared::PointLight& light)
{
	if (server.stuffSent.has(light.name, light.name))
		return true;

	bool ok = sendPointLight(clientSocket, light);
	if (!ok) {
		err("Failed sending point light");
		return false;
	}
	ok = expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK);
	if (!ok) {
		warn("Not received RSRC_EXCHANGE_ACK!");
		return false;
	}

	server.stuffSent.insert(light.name, light.name);

	return true;
}

static bool sendResourceBatch(socket_t clientSocket,
	Server& server,
	const ResourceBatch& batch,
	TexturesQueue& texturesQueue)
{
	std::unordered_set<std::pair<std::string, shared::TextureFormat>> texturesToSend;
	std::unordered_set<StringId> materialsSent;

	info("Sending ", batch.models.size(), " models");
	for (const auto& model : batch.models) {
		// This will also send dependent materials
		if (!batch_sendModel(clientSocket, server, texturesToSend, model))
			return false;

		texturesQueue.insert(texturesToSend.begin(), texturesToSend.end());

		// After sending model base info, schedule its geometry to be streamed and add it
		// to the scene (so its transform will be sent too)
		{
			std::lock_guard<std::mutex> lock{ server.toClient.modelsToSendMtx };
			server.toClient.modelsToSend.emplace_back(model);
		}

		auto node = server.scene.addNode(model.name, NodeType::MODEL, Transform{});
		// Make Sponza static (FIXME: ugly)
		if (node->name == sid((server.cwd + xplatPath("/models/sponza/sponza.dae")).c_str()))
			node->flags |= (1 << NODE_FLAG_STATIC);
	}

	// Send lights
	for (const auto& light : batch.pointLights) {
		if (!batch_sendPointLight(clientSocket, server, light))
			return false;
	}

	// Send shaders (and unload them immediately after)
	// const std::array<const char*, 3> shadersToSend = { "shaders/gbuffer", "shaders/skybox",
	// "shaders/composition"
	// }; for (unsigned i = 0; i < shadersToSend.size(); ++i) { if (!batch_sendShaders(clientSocket,
	// resources, shadersToSend[i], i)) return false;
	//}

	if (!sendTCPMsg(clientSocket, TcpMsgType::END_RSRC_EXCHANGE))
		return false;

	info("Done sending data");

	return true;
}

///////////////////

TcpActiveThread::TcpActiveThread(Server& server, Endpoint& ep)
	: server{ server }
	, ep{ ep }
{
	thread = std::thread{ &TcpActiveThread::tcpActiveTask, this };
	xplatSetThreadName(thread, "TcpActive");
}

TcpActiveThread::~TcpActiveThread()
{
	if (thread.joinable()) {
		info("Joining Tcp Active thread...");
		thread.join();
		info("Joined Tcp Active thread.");
	}
}

void TcpActiveThread::tcpActiveTask()
{
	info("Listening...");
	// One client at a time
	if (::listen(ep.socket, 1) != 0) {
		err("Error listening: ", xplatGetErrorString(), " (", xplatGetError(), ")");
		return;
	}

	while (ep.connected) {
		sockaddr_in clientAddr;
		socklen_t clientLen = sizeof(sockaddr_in);

		info("Accepting...");
		auto clientSocket = ::accept(ep.socket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
		if (clientSocket == -1) {
			if (ep.connected) {
				err("Error: couldn't accept connection.");
				closeEndpoint(ep);
				break;
			}
			continue;
		}

		info("Accepted connection from ", inet_ntoa(clientAddr.sin_addr));

		// Start receiving thread
		server.networkThreads.tcpRecv =
			std::make_unique<TcpReceiveThread>(server, server.endpoints.reliable, clientSocket);

		genUpdateLists(server);

		if (!tcp_connectionPrelude(clientSocket))
			dropClient(clientSocket);

		const char* readableAddr = inet_ntoa(clientAddr.sin_addr);

		connectToClient(clientSocket, readableAddr);

		if (!sendTCPMsg(clientSocket, TcpMsgType::READY)) {
			info("TCP: Dropping client ", readableAddr);
			dropClient(clientSocket);
		}

		if (!msgLoop(clientSocket)) {
			info("TCP: Dropping client ", readableAddr);
			dropClient(clientSocket);
		}
	}

	info("tcpActiveTask: ended.");
}

void TcpActiveThread::connectToClient(socket_t clientSocket, const char* clientAddr)
{
	// Start keepalive listening thread
	server.networkThreads.keepalive =
		std::make_unique<KeepaliveListenThread>(server, server.endpoints.reliable, clientSocket);

	// Starts UDP loops and send ready to client
	server.endpoints.udpActive =
		startEndpoint(clientAddr, cfg::UDP_SERVER_TO_CLIENT_PORT, Endpoint::Type::ACTIVE, SOCK_DGRAM);
	server.networkThreads.udpActive = std::make_unique<UdpActiveThread>(server, server.endpoints.udpActive);
	server.endpoints.udpPassive =
		startEndpoint(ep.ip.c_str(), cfg::UDP_CLIENT_TO_SERVER_PORT, Endpoint::Type::PASSIVE, SOCK_DGRAM);
	server.networkThreads.udpPassive = std::make_unique<UdpPassiveThread>(server, server.endpoints.udpPassive);
}

bool TcpActiveThread::msgLoop(socket_t clientSocket)
{
	while (ep.connected) {
		std::unique_lock<std::mutex> ulk{ mtx };
		cv.wait(ulk, [this]() {
			return !ep.connected || !server.networkThreads.tcpRecv->clientConnected ||
			       !server.networkThreads.keepalive->clientConnected || resourcesToSend.size() > 0 ||
			       gMsgRecvQueue.size() > 0 || server.toClient.texturesQueue.size() > 0;
		});

		if (!ep.connected || !server.networkThreads.keepalive->clientConnected ||
			!server.networkThreads.tcpRecv->clientConnected)
			return false;

		{
			// Check for REQ_MODEL
			TcpMsg msg;
			while (gMsgRecvQueue.try_pop(msg)) {
				if (msg.type != TcpMsgType::REQ_MODEL)
					continue;

				loadAndEnqueueModel(server, msg.payload);
			}
		}

		if (resourcesToSend.size() > 0) {
			if (!sendTCPMsg(clientSocket, TcpMsgType::START_RSRC_EXCHANGE))
				return false;

			if (!expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK))
				return false;

			info("Send ResourceBatch");
			if (!sendResourceBatch(clientSocket, server, resourcesToSend, server.toClient.texturesQueue)) {
				err("Failed to send ResourceBatch");
				return false;
			}

			resourcesToSend.clear();
		}

		while (server.toClient.updates.persistent.size() == 0 && server.toClient.texturesQueue.size() > 0) {

			int64_t totBytesSent = 0;
			constexpr int64_t MIN_BYTES_PER_BATCH = megabytes(1);

			if (!sendTCPMsg(clientSocket, TcpMsgType::START_RSRC_EXCHANGE))
				return false;

			if (!expectTCPMsg(TcpMsgType::RSRC_EXCHANGE_ACK))
				return false;

			for (auto tex_it = server.toClient.texturesQueue.begin();
				tex_it != server.toClient.texturesQueue.end();) {
				totBytesSent += batch_sendTexture(clientSocket, server, tex_it->first, tex_it->second);

				if (totBytesSent < 0)
					return false;

				tex_it = server.toClient.texturesQueue.erase(tex_it);

				if (totBytesSent > MIN_BYTES_PER_BATCH)
					break;
			}

			if (!sendTCPMsg(clientSocket, TcpMsgType::END_RSRC_EXCHANGE))
				return false;
		}
	}

	return false;
}

void TcpActiveThread::dropClient(socket_t clientSocket)
{
	info("Dropping client");

	// Send disconnect message
	sendTCPMsg(clientSocket, TcpMsgType::DISCONNECT);

	info("Closing passiveEP");
	closeEndpoint(server.endpoints.udpPassive);
	server.networkThreads.udpPassive.reset(nullptr);

	info("Closing activeEP");
	closeEndpoint(server.endpoints.udpActive);
	server.networkThreads.udpActive.reset(nullptr);

	server.networkThreads.tcpRecv->clientConnected = false;
	server.networkThreads.keepalive.reset(nullptr);

	xplatSockClose(clientSocket);
	server.networkThreads.tcpRecv->clientConnected = false;
	server.networkThreads.tcpRecv.reset(nullptr);

	server.scene.clear();
}
///////////

KeepaliveListenThread::KeepaliveListenThread(Server& server, const Endpoint& ep, socket_t clientSocket)
	: ServerSlaveThread{ server, ep, clientSocket }
{
	thread = std::thread{ &KeepaliveListenThread::keepaliveListenTask, this };
	xplatSetThreadName(thread, "KeepaliveListen");
}

KeepaliveListenThread::~KeepaliveListenThread()
{
	if (thread.joinable()) {
		info("Joining keepaliveThread...");
		cv.notify_all();
		thread.join();
		info("Joined keepaliveThread.");
	}
}

/* This task listens for keepalives and updates `latestPing` with the current time every time it receives one.
 */
void KeepaliveListenThread::keepaliveListenTask()
{
	constexpr auto interval = std::chrono::seconds{ cfg::SERVER_KEEPALIVE_INTERVAL_SECONDS };

	while (ep.connected && clientConnected) {
		std::unique_lock<std::mutex> ulk{ mtx };
		if (cv.wait_for(ulk, interval) == std::cv_status::no_timeout) {
			break;
		}

		// Verify the client has pinged us within our sleep time
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - gLatestPing) > interval) {
			// drop the client
			err("Keepalive timeout.");
			break;
		}
	}
	clientConnected = false;
	if (server.networkThreads.tcpActive)
		server.networkThreads.tcpActive->cv.notify_one();
}

///////////////
TcpReceiveThread::TcpReceiveThread(Server& server, const Endpoint& ep, socket_t clientSocket)
	: ServerSlaveThread{ server, ep, clientSocket }
{
	thread = std::thread{ &TcpReceiveThread::receiveTask, this };
	xplatSetThreadName(thread, "TcpReceive");
}

TcpReceiveThread::~TcpReceiveThread()
{
	if (thread.joinable()) {
		info("Joining ReceiveThread...");
		thread.join();
		info("Joined ReceiveThread.");
	}
}

void TcpReceiveThread::receiveTask()
{
	info("Started receiveTask");

	if (gMsgRecvQueue.capacity() == 0)
		gMsgRecvQueue.reserve(256);
	else
		gMsgRecvQueue.clear();

	int failCount = 0;
	constexpr int MAX_FAIL_COUNT = 10;

	while (ep.connected && clientConnected) {
		std::array<uint8_t, 3> packet;
		packet.fill(0);
		TcpMsgType type;
		if (receiveTCPMsg(clientSocket, packet.data(), packet.size(), type)) {
			failCount = 0;
			switch (type) {
			case TcpMsgType::DISCONNECT:
				info("Received DISCONNECT from client.");
				goto exit;
			case TcpMsgType::KEEPALIVE:
				gLatestPing = std::chrono::steady_clock::now();
				break;
			default: {
				debug("pushing msg ", type);
				TcpMsg msg;
				msg.type = type;
				if (type == TcpMsgType::REQ_MODEL) {
					msg.payload = *reinterpret_cast<uint16_t*>(packet.data() + 1);
				}
				gMsgRecvQueue.push(msg);
				if (server.networkThreads.tcpActive)
					server.networkThreads.tcpActive->cv.notify_one();
			} break;
			}
		} else {
			if (++failCount == MAX_FAIL_COUNT)
				break;
		}
	}
exit:
	clientConnected = false;
	if (server.networkThreads.tcpActive)
		server.networkThreads.tcpActive->cv.notify_one();
}

