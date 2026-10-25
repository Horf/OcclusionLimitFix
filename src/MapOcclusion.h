#pragma once

namespace MapOcclusion
{
	// Experimental: skips occluder tests while the map is open (1.5.97 only for now).
	// Seen from above, occluders hide almost nothing, but the engine still tests every
	// object against every occluder on the main thread.
	bool Install();

	// Call after kDataLoaded, when the UI event sources exist.
	void RegisterEventSinks();
}
