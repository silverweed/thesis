#include "endpoint_xplatform.hpp"
#include "logging.hpp"
#include <cstring>
#include <iostream>
#ifndef _WIN32
#	include <cerrno>
#endif

bool xplatSocketInit()
{
#ifdef _WIN32
	WSADATA wsaData;
	return WSAStartup(MAKEWORD(1, 1), &wsaData) == 0;
#else
	return true;
#endif
}

bool xplatSocketCleanup()
{
#ifdef _WIN32
	return WSACleanup() == 0;
#else
	return true;
#endif
}

int xplatSockClose(socket_t sock)
{
	int status = 0;
#ifdef _WIN32
	// This may fail if the socket is UDP, just don't care
	status = shutdown(sock, SD_BOTH);
	if (status == 0 || WSAGetLastError() == WSAENOTCONN) {
		status = closesocket(sock);
		logging::info("Socket ", sock, " shut down.");
#else
	// This may fail if the socket is UDP, just don't care
	status = shutdown(sock, SHUT_RDWR);
	if (status == 0 || errno == ENOTCONN) {
		char buf[256];
		while (read(sock, buf, 256) > 0) {
		}
		status = close(sock);
		logging::info("Socket ", sock, " shut down.");
#endif
	} else
		logging::warn("Error shutting down the socket: ", xplatGetErrorString(), " (", xplatGetError(), ")");

	return status;
}

const char* xplatGetErrorString()
{
	return std::strerror(xplatGetError());
}

int xplatGetError()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}
