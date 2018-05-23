#pragma once

#include <string>
#include <vector>
#include "logging.hpp"

inline bool startsWith(const std::string& haystack, const std::string& needle) {
	return haystack.substr(0, needle.size()) == needle;
}

/** Reads a file into memory. The memory is allocated as a vector<char> and returned. */
std::vector<char> readFileIntoMemory(const char *path);

/** Reads a file into a provided buffer.
 *  The load will fail if given buffer length is less than required.
 *  @return -1 in case of failure, else the number of bytes loaded.
 */
std::size_t readFileIntoMemory(const char *path, void *buffer, std::size_t bufsize);

void dumpBytes(const void *buffer, std::size_t count, std::size_t maxCount = 50, LogLevel lv = LOGLV_VERBOSE);
