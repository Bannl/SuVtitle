// =============================================================================
// SuVtitle - engine/EngineVersion.h   (SuVtitleEngine.exe's version + resource ids)
//
// New with the split (2026-10-23). ONE place for the engine's version: engine.rc's VERSIONINFO (Explorer's
// Properties > Details) and main_translator.cpp (the usage line, the log's start line) both include it.
// It follows SuVtitle's own version (S10: 1.0.0, app/resource.h's SUV_VERSION_*), bumped with it.
// Plain #defines and include guards, no #pragma: rc.exe reads this file too.
// =============================================================================
#ifndef SUVTITLE_ENGINE_VERSION_H
#define SUVTITLE_ENGINE_VERSION_H

#define IDI_ENGINEICON 101

#define SUVTITLE_ENGINE_VERSION_MAJOR 1
#define SUVTITLE_ENGINE_VERSION_MINOR 0
#define SUVTITLE_ENGINE_VERSION_PATCH 0
#define SUVTITLE_ENGINE_VERSION_STR   "1.0.0"

#endif
