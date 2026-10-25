#pragma once

#include <spdlog/common.h>

namespace Settings
{
	inline spdlog::level::level_enum logLevel = spdlog::level::info;
	inline bool                      invalidLogLevel = false;
	inline bool                      skipOcclusionOnMap = false;

	// Reads Data/SKSE/Plugins/OcclusionLimitFix.ini. A missing file or key keeps the default.
	// Called before SKSE::Init, so it must not log.
	void Load();
}
