#include "Hooks.h"

#include <REL/Module.h>
#include <REL/Version.h>

#include <SKSE/API.h>
#include <SKSE/Interfaces.h>
#include <SKSE/Logger.h>

#include <spdlog/common.h>

namespace
{
	// Diagnostics (runtime, trampoline usage, plane set maxima) are written at debug level.
	// Set to true to see them. Planned: read this from an INI.
	constexpr bool kVerboseLogging = false;
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, {
		.logLevel = kVerboseLogging ? spdlog::level::debug : spdlog::level::info,
		.trampoline = true,
		.trampolineSize = 28,  // two write_branch<5> calls, 14 bytes each
		});
	logs::debug("OcclusionLimitFix loading...");
	logs::debug("Runtime: {}", REL::Module::get().version().string("."));

	if (!Hooks::Install()) {
		logs::warn("OcclusionLimitFix is inactive, the game behaves as without the plugin.");
	}
	return true;
}
