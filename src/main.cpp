#include "CrashFix.h"
#include "MapOcclusion.h"
#include "OccluderBudget.h"
#include "Settings.h"

#include <REL/Module.h>

#include <SKSE/API.h>
#include <SKSE/Logger.h>
#include <SKSE/Trampoline.h>

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	Settings::Load();

	// 14 bytes per hook target: 2 for the crash fix, 3 for the map option, 1 for the occluder budget
	const std::size_t trampolineSize = static_cast<size_t>(2 * 14) + (Settings::skipOcclusionOnMap ? 3 * 14 : 0) + (Settings::maxActiveOccluders > 0 ? 14 : 0);
	SKSE::Init(a_skse, {
		.logLevel = Settings::logLevel,
		.trampoline = true,
		.trampolineSize = trampolineSize,
	});

	if (Settings::invalidLogLevel) {
		logs::warn("Unknown sLogLevel in OcclusionLimitFix.ini, using info.");
	}
	logs::debug("Runtime {}, bSkipOcclusionOnMap = {}, iMaxActiveOccluders = {}",
		REL::Module::get().version().string("."), Settings::skipOcclusionOnMap, Settings::maxActiveOccluders);

	if (!CrashFix::Install()) {
		logs::warn("Crash fix inactive, the game behaves as without the plugin. Experimental options are disabled too.");
		return true;
	}

	// The experimental options need addresses the VR address library does not have.
	if (REL::Module::IsVR()) {
		if (Settings::skipOcclusionOnMap || Settings::maxActiveOccluders > 0) {
			logs::warn("Skyrim VR: experimental options are not available, only the crash fix is active.");
		}
		logs::debug("Trampoline: {} of {} bytes used", SKSE::GetTrampoline().allocated_size(), SKSE::GetTrampoline().capacity());
		return true;
	}

	if (Settings::maxActiveOccluders > 0) {
		OccluderBudget::Install();
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
