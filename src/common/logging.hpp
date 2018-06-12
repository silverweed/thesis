#pragma once

#include <iostream>

enum LogLevel {
	LOGLV_NONE = 0,
	LOGLV_ERR = 1,
	LOGLV_WARN = 2,
	LOGLV_INFO = 3,
	LOGLV_DEBUG = 4,
	LOGLV_VERBOSE = 5,
	LOGLV_UBER_VERBOSE = 6,
};

extern LogLevel gDebugLv;
extern bool gColoredLogs;

namespace logging {

constexpr auto C_RED = "\033[1;31m";
constexpr auto C_YELLOW = "\033[0;33m";
constexpr auto C_NONE = "\033[0m";

inline void log(LogLevel debugLv, bool breakLine)
{
	if (gColoredLogs)
		std::cerr << C_NONE;
	if (gDebugLv >= debugLv && breakLine)
		std::cerr << "\n";
}

template <typename Arg, typename... Args>
inline void log(LogLevel debugLv, bool breakLine, Arg&& arg, Args&&... args)
{
	if (gDebugLv < debugLv)
		return;
	std::cerr << arg;
	log(debugLv, breakLine, std::forward<Args>(args)...);
}

template <typename... Args>
inline void err(Args&&... args)
{
	log(LOGLV_ERR, true, gColoredLogs ? C_RED : "", "[E] ", std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(Args&&... args)
{
	log(LOGLV_WARN, true, gColoredLogs ? C_YELLOW : "", "[W] ", std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(Args&&... args)
{
	log(LOGLV_INFO, true, "[I] ", std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(Args&&... args)
{
	log(LOGLV_DEBUG, true, "[D] ", std::forward<Args>(args)...);
}

template <typename... Args>
inline void verbose(Args&&... args)
{
	log(LOGLV_VERBOSE, true, "[V] ", std::forward<Args>(args)...);
}

template <typename... Args>
inline void uberverbose(Args&&... args)
{
	log(LOGLV_UBER_VERBOSE, true, "[U] ", std::forward<Args>(args)...);
}

}   // end namespace logging
