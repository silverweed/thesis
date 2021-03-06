#include "bandwidth_limiter.hpp"
#include "config.hpp"
#include "geom_update.hpp"
#include "hashing.hpp"
#include "logging.hpp"
#include "model.hpp"
#include "server.hpp"
#include "server_appstage.hpp"
#include "server_tcp.hpp"
#include "server_udp.hpp"
#include "units.hpp"
#include "xplatform.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace logging;
using namespace std::literals::chrono_literals;

static constexpr std::size_t MEMSIZE = megabytes(128);
static constexpr auto CLIENT_UPDATE_TIME = std::chrono::milliseconds{ 33 };

bool gMoveObjects = true;
bool gChangeLights = true;

struct MainArgs {
	std::string ip = "127.0.0.1";
	float limitBytesPerSecond = -1;
	int nLights = 10;
};

static void parseArgs(int argc, char** argv, MainArgs& args);
static std::vector<shared::PointLight> createLights(int n);

int main(int argc, char** argv)
{
	MainArgs args = {};
	parseArgs(argc, argv, args);

	std::cerr << "Debug level = " << static_cast<int>(gDebugLv) << "\n";

	/// Initial setup
	if (!xplatSocketInit()) {
		err("Failed to initialize sockets.");
		return EXIT_FAILURE;
	}
	if (args.limitBytesPerSecond >= 0) {
		info("Limiting bandwidth to ", args.limitBytesPerSecond, " bytes/s");
		gBandwidthLimiter.setSendLimit(args.limitBytesPerSecond);
		// gBandwidthLimiter.setMaxQueueingDelay(std::chrono::milliseconds{ 50 });
		gBandwidthLimiter.start();
	}

	Server server{ MEMSIZE };
	server.cwd = xplatGetCwd();

	const auto atExit = [&server]() {
		// debug("Sockets:\nudpActive: ",
		// server.endpoints.udpActive.socket,
		//"\nudpPassive: ",
		// server.endpoints.udpPassive.socket,
		//"\nreliable: ",
		// server.endpoints.reliable.socket,
		//"\nclient: ",
		// server.networkThreads.keepalive->clientSocket);
		// "Ensure" we close the sockets even if we terminate abruptly
		gBandwidthLimiter.stop();
		server.closeNetwork();
		if (xplatSocketCleanup())
			info("Successfully cleaned up sockets.");
		else
			warn("Error cleaning up sockets: ", xplatGetErrorString());
		std::exit(0);
	};

	if (!xplatEnableExitHandler()) {
		err("Failed to enable exit handler!");
		return EXIT_FAILURE;
	}
	xplatSetExitHandler(atExit);

	{
		// Add lights
		const auto lights = createLights(args.nLights);
		server.resources.pointLights.insert(server.resources.pointLights.end(), lights.begin(), lights.end());

		for (const auto& light : server.resources.pointLights) {
			server.scene.addNode(light.name, NodeType::POINT_LIGHT, Transform{});
		}
	}

	/// Start TCP socket and wait for connections
	server.endpoints.reliable =
		startEndpoint(args.ip.c_str(), cfg::RELIABLE_PORT, Endpoint::Type::PASSIVE, SOCK_STREAM);
	if (!xplatIsValidSocket(server.endpoints.reliable.socket)) {
		err("Failed to listen on ", args.ip, ":", cfg::RELIABLE_PORT, ": quitting.");
		return 1;
	}
	server.networkThreads.tcpActive = std::make_unique<TcpActiveThread>(server, server.endpoints.reliable);

	info("Started appstage");
	appstageLoop(server);
	atExit();
}

void parseArgs(int argc, char** argv, MainArgs& args)
{
	const auto usage = [argv]() {
		std::cerr << "Usage: " << argv[0] << " [-v[vvv...]] [-n (no colored logs)] [-b (max bytes per second)]"
			  << " [-m (don't move objects)] [-l (don't change lights)] [-k (n dyn lights)]\n";
		std::exit(EXIT_FAILURE);
	};

	int i = 1;
	int posArgs = 0;
	while (i < argc) {
		if (strlen(argv[i]) < 2) {
			std::cerr << "Invalid flag -.\n";
			std::exit(EXIT_FAILURE);
		}
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
			case 'v': {
				int lv = 1;
				unsigned j = 2;
				while (j < strlen(argv[i]) && argv[i][j] == 'v') {
					++lv;
					++j;
				}
				gDebugLv = static_cast<LogLevel>(lv);
			} break;
			case 'n':
				gColoredLogs = false;
				break;
			case 'b': {
				if (i == argc - 1) {
					usage();
				}
				args.limitBytesPerSecond = std::atof(argv[i + 1]);
				++i;
			} break;

			case 'm':
				gMoveObjects = false;
				break;

			case 'l':
				gChangeLights = false;
				break;

			case 'k':
				if (i == argc - 1) {
					usage();
				}
				args.nLights = std::atoi(argv[i + 1]);
				++i;
				break;
			default:
				usage();
			}
		} else {
			switch (posArgs++) {
			case 0:
				args.ip = std::string{ argv[i] };
				break;
			default:
				break;
			}
		}
		++i;
	}
}

std::vector<shared::PointLight> createLights(int n)
{
	std::vector<shared::PointLight> lights;
	lights.reserve(n);

	for (int i = 0; i < n; ++i) {
		shared::PointLight light;
		light.name = sid(std::string{ "Light " } + std::to_string(i));
		light.color = glm::vec3{ 1.0, 1.0, 1.0 };
		light.attenuation = 0.5;
		lights.emplace_back(light);
	}

	return lights;
}
