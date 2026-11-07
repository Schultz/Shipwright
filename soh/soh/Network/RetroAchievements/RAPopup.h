#pragma once

#ifdef __cplusplus

#include <cstdint>
#include <string>

// Trophy-style unlock popups with the achievement's RetroAchievements badge.
//
// One banner at a time slides in at the top right of the game window, holds,
// then slides out; later popups queue behind it. Badge images are downloaded
// from RetroAchievements, decoded with stb_image and uploaded as ImGui
// textures. Prefetch them when the set loads so they're ready on unlock.
namespace RAPopup {

// Registers the overlay window. Call once after the GUI exists.
void Init();

// Starts downloading a badge if it isn't cached yet
void PrefetchBadge(const std::string& url);

struct Popup {
    std::string headline;    // e.g. "Achievement Unlocked"
    std::string title;       // achievement title
    std::string description; // achievement description
    uint32_t points = 0;     // 0 hides the points
    std::string badgeUrl;    // RA badge image, may be empty
    bool gold = true;        // gold frame for unlocks, silver for informational popups
};

void Show(const Popup& popup);

} // namespace RAPopup

#endif
