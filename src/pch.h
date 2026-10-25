#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// SKSE::log functions are structs (to capture std::source_location), so every
// log call creates an unnamed temporary. Code analysis rule es.84 flags that.
#pragma warning(disable : 26444)

namespace logs = SKSE::log;
