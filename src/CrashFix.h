#pragma once

namespace CrashFix
{
	// Replaces BSCompoundFrustum::GetActivePlaneState and SetActivePlaneState, which overflow
	// a 256-entry stack buffer with more than 255 occluder plane sets.
	bool Install();
}
