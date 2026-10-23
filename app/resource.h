// =============================================================================
// SuVtitle - app/resource.h   (resource ids + THE app's version)
//
// Modelled 2026-10-23 on AudioPB's ui/resource.h (641f1aa): one icon is the whole inventory -- the FIRST icon resource
// of an exe is also what Explorer shows for the file, so IDI_APPICON serves the window class, the taskbar, Alt-Tab
// and the file. The version lives HERE because everything reads it from here (the .rc's VERSIONINFO, Explorer's
// Properties > Details; one day the installer): one truth, bumped in one place. SuVtitle starts at 1.0.0 (S10).
// =============================================================================
#pragma once
#define IDI_APPICON 101

#define SUV_VERSION_MAJOR 1
#define SUV_VERSION_MINOR 0
#define SUV_VERSION_PATCH 0
#define SUV_VERSION_STR   "1.0.0"
