#include "pch.h"

namespace
{
	// Alle späteren Hooks beruhen auf dem Code-Layout von 1.5.97
	// (Stack-Rahmen, Offsets innerhalb von Funktionen). Auf jeder anderen
	// Laufzeit bleibt das Plugin deshalb inaktiv, statt blind zu patchen.
	bool IsSupportedRuntime()
	{
		const auto version = REL::Module::get().version();
		if (version != SKSE::RUNTIME_SSE_1_5_97) {
			logs::error("Laufzeit {} wird nicht unterstuetzt, Plugin bleibt inaktiv.", version.string("."));
			return false;
		}
		return true;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logs::info("OcclusionLimitFix laedt...");

	if (!IsSupportedRuntime()) {
		return true;
	}

	// Hier kommen die Hooks hin, sobald RE-Auftrag 1 Puffergroesse
	// und Aufrufer von BSCompoundFrustum::GetActivePlaneState (ID 75075) liefert.
	logs::info("Laufzeit 1.5.97 erkannt. Noch keine Hooks installiert.");
	return true;
}
