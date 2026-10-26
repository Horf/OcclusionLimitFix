#include "CrashFix.h"
#include "MapOcclusion.h"
#include "Settings.h"

#include <REL/Module.h>

#include <SKSE/API.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	Settings::Load();

	// 14 bytes per hook target: 2 for the crash fix, 3 for the map option
	const std::size_t trampolineSize = static_cast<size_t>(2 * 14) + (Settings::skipOcclusionOnMap ? 3 * 14 : 0);
	SKSE::Init(a_skse, {
		.logLevel = Settings::logLevel,
		.trampoline = true,
		.trampolineSize = trampolineSize,
	});

	if (REL::Module::IsVR()) {
		logs::warn("Skyrim VR is not supported, plugin inactive.");
		return true;
	}

	if (Settings::invalidLogLevel) {
		logs::warn("Unknown sLogLevel in OcclusionLimitFix.ini, using info.");
	}
	logs::debug("Runtime {}, bSkipOcclusionOnMap = {}", REL::Module::get().version().string("."), Settings::skipOcclusionOnMap);

	if (!CrashFix::Install()) {
		logs::warn("Crash fix inactive, the game behaves as without the plugin.");
	}

	if (Settings::skipOcclusionOnMap && MapOcclusion::Install()) {
		SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
				MapOcclusion::RegisterEventSinks();
			}
		});
	}

	logs::debug("Trampoline: {} of {} bytes used", SKSE::GetTrampoline().allocated_size(), SKSE::GetTrampoline().capacity());
	return true;
}
