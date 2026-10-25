#include "Hooks.h"

#include <REL/Module.h>
#include <REL/Version.h>

#include <SKSE/API.h>
#include <SKSE/Logger.h>
#include <SKSE/Interfaces.h>

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// Measured with the "Trampoline: x of y" log line, trimmed afterwards
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 98 });
	logs::info("OcclusionLimitFix loading...");
	logs::info("Runtime: {}", REL::Module::get().version().string("."));
	logs::info("Module base: 0x{:X}", REL::Module::get().base());

	Hooks::Install();

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::RegisterEventSinks();
		}
	});

	return true;
}
