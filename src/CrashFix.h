#pragma once

namespace CrashFix
{
	enum class Result
	{
		kInstalled,    // OLF replacement is active
		kOtherPlugin,  // another plugin already redirected both functions, e.g. Creation Engine Tweaks
		kFailed        // unexpected code, nothing patched
	};

	// Replaces BSCompoundFrustum::GetActivePlaneState and SetActivePlaneState, which overflow
	// a 256-entry stack buffer with more than 255 occluder plane sets.
	Result Install();
}
