#pragma once

namespace Hooks
{
	// Replaces BSCompoundFrustum::GetActivePlaneState and SetActivePlaneState
	// and installs the experimental map performance hooks.
	// Returns false if the original code does not look as expected.
	bool Install();

	// Must be called after kDataLoaded, when the UI event sources exist.
	void RegisterEventSinks();
}
