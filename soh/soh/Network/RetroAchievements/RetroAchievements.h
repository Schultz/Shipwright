#pragma once

#ifdef __cplusplus

// RetroAchievements support (proof of concept, softcore only).
//
// SoH runs the existing N64 achievement set for Ocarina of Time through
// rcheevos' rc_client. Achievement logic reads N64 RAM, which RAMemory builds
// from SoH's live state. See RA/INTEGRATION_PLAN.md at the repo root.
namespace RetroAchievements {

// Draws the Settings > RetroAchievements menu page
void DrawMenu();

} // namespace RetroAchievements

#endif
