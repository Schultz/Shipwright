#ifdef ENABLE_RETROACHIEVEMENTS

#include "RetroAchievements.h"
#include "RAHttp.h"
#include "RAMemory.h"
#include "RAPopup.h"

#include <string>
#include <imgui.h>
#include <rc_client.h>
#include <spdlog/spdlog.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/ShipInit.hpp"
#include "soh/GameVersions.h"

extern "C" {
#include "variables.h"
}

#define CVAR_RA(var) CVAR_REMOTE("RetroAchievements." var)

namespace {

// RetroAchievements' MD5 for "Legend of Zelda, The - Ocarina of Time (USA).z64",
// the NTSC-U 1.0 ROM. It identifies game 10113 on the server.
constexpr const char* NTSC_US_10_HASH = "5bd1fe107bf8106b2ab6650abecd54d6";

// Which extracted ROMs can run set 10113.
//
// The achievement logic never reads the ROM. RAMemory builds the NTSC 1.0 RAM
// layout from SoH's own state, and SoH runs the same game code whichever ROM
// the assets came from. Outside Master Quest, version checks in SoH only change
// UI and text (PAL file select, message boxes, title logo). So every vanilla
// version gets the same set, and the ROM only decides which hash is reported.
//
// SoH identifies a ROM by its header CRC, which is the same for US 1.0 and
// JP 1.0 (also for 1.1 and 1.2). Both 1.0 ROMs are on RA's list for set 10113,
// so that ID is an exact match either way.
enum class RomMatch { Exact, Mapped, Unsupported };

struct RomEntry {
    uint32_t version;
    const char* name;
    RomMatch match;
};

constexpr RomEntry ROM_TABLE[] = {
    { OOT_NTSC_US_10, "N64 NTSC 1.0 (US/JP)", RomMatch::Exact },
    { OOT_NTSC_US_11, "N64 NTSC 1.1", RomMatch::Mapped },
    { OOT_NTSC_US_12, "N64 NTSC 1.2", RomMatch::Mapped },
    { OOT_PAL_10, "N64 PAL 1.0", RomMatch::Mapped },
    { OOT_PAL_11, "N64 PAL 1.1", RomMatch::Mapped },
    { OOT_NTSC_US_GC, "GameCube NTSC US", RomMatch::Mapped },
    { OOT_NTSC_JP_GC, "GameCube NTSC JP", RomMatch::Mapped },
    { OOT_NTSC_JP_GC_CE, "GameCube NTSC JP (Collector's Edition)", RomMatch::Mapped },
    { OOT_PAL_GC, "GameCube PAL", RomMatch::Mapped },
    { OOT_PAL_GC_DBG1, "GameCube PAL Debug", RomMatch::Mapped },
    { OOT_PAL_GC_DBG2, "GameCube PAL Debug", RomMatch::Mapped },
    { OOT_IQUE_TW, "iQue (TW)", RomMatch::Mapped },
    { OOT_IQUE_CN, "iQue (CN)", RomMatch::Mapped },
    // Master Quest has different dungeons and its own RA sets
    { OOT_NTSC_US_MQ, "GameCube Master Quest US", RomMatch::Unsupported },
    { OOT_NTSC_JP_MQ, "GameCube Master Quest JP", RomMatch::Unsupported },
    { OOT_PAL_MQ, "GameCube Master Quest PAL", RomMatch::Unsupported },
    { OOT_PAL_GC_MQ_DBG, "GameCube Master Quest Debug", RomMatch::Unsupported },
};

rc_client_t* sClient = nullptr;
std::string sStatus = "Not logged in";
std::string sRomStatus;
bool sBusy = false;

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// Picks the best loaded archive: an exact match first, then a mapped one.
// Returns nullptr when only Master Quest or unknown versions are loaded.
const RomEntry* FindUsableRom() {
    const RomEntry* best = nullptr;
    for (uint32_t i = 0; i < ResourceMgr_GetNumGameVersions(); i++) {
        uint32_t version = ResourceMgr_GetGameVersion(i);
        for (const RomEntry& entry : ROM_TABLE) {
            if (entry.version != version || entry.match == RomMatch::Unsupported) {
                continue;
            }
            if (best == nullptr || (entry.match == RomMatch::Exact && best->match != RomMatch::Exact)) {
                best = &entry;
            }
        }
    }
    return best;
}

// ---- rc_client callbacks ----

uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t*) {
    return RAMemory::Read(address, buffer, numBytes);
}

void RC_CCONV ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callbackData,
                         rc_client_t*) {
    RAHttp::Request(request->url, request->post_data ? request->post_data : "",
                    request->content_type ? request->content_type : "", [callback, callbackData](const auto& r) {
                        rc_api_server_response_t response{};
                        response.body = r.body.empty() ? nullptr : r.body.c_str();
                        response.body_length = r.body.size();
                        response.http_status_code = r.statusCode;
                        callback(&response, callbackData);
                    });
}

void RC_CCONV LogMessage(const char* message, const rc_client_t*) {
    SPDLOG_INFO("[rcheevos] {}", message);
}

void RC_CCONV HandleEvent(const rc_client_event_t* event, rc_client_t* client) {
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            RAPopup::Show({
                .headline = rc_client_get_spectator_mode_enabled(client) ? "Achievement Unlocked (test, not sent)"
                                                                         : "Achievement Unlocked",
                .title = event->achievement->title,
                .description = event->achievement->description ? event->achievement->description : "",
                .points = event->achievement->points,
                .badgeUrl = event->achievement->badge_url ? event->achievement->badge_url : "",
            });
            SPDLOG_INFO("[RetroAchievements] Unlocked {} \"{}\"", event->achievement->id, event->achievement->title);
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
            Notification::Emit({ .prefix = "Leaderboard started:", .message = event->leaderboard->title });
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
            Notification::Emit({ .prefix = "Leaderboard failed:", .message = event->leaderboard->title });
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
            Notification::Emit({
                .prefix = "Leaderboard submitted:",
                .message = event->leaderboard->title,
                .suffix = event->leaderboard->tracker_value ? event->leaderboard->tracker_value : "",
            });
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED: {
            const rc_client_game_t* game = rc_client_get_game_info(client);
            RAPopup::Show({
                .headline = "Set Complete",
                .title = game->title,
                .description = "Every achievement in the set is unlocked",
                .badgeUrl = game->badge_url ? game->badge_url : "",
            });
            break;
        }
        case RC_CLIENT_EVENT_SERVER_ERROR:
            SPDLOG_ERROR("[RetroAchievements] Server error: {}", event->server_error->error_message);
            Notification::Emit({ .prefix = "RetroAchievements:", .message = event->server_error->error_message });
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            Notification::Emit({ .message = "RetroAchievements: unlock pending, reconnecting..." });
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            Notification::Emit({ .message = "RetroAchievements: reconnected" });
            break;
        default:
            break;
    }
}

// ---- Session flow: login -> load game ----

void RC_CCONV OnGameLoaded(int result, const char* error, rc_client_t* client, void*) {
    sBusy = false;
    if (result != RC_OK) {
        sStatus = std::string("Game load failed: ") + (error ? error : "unknown error");
        SPDLOG_ERROR("[RetroAchievements] {}", sStatus);
        return;
    }

    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    sStatus = std::string("Playing ") + game->title;
    SPDLOG_INFO("[RetroAchievements] Loaded game {} \"{}\": {}/{} unlocked", game->id, game->title,
                summary.num_unlocked_achievements, summary.num_core_achievements);
    RAPopup::Show({
        .headline = "RetroAchievements",
        .title = game->title,
        .description = std::to_string(summary.num_unlocked_achievements) + "/" +
                       std::to_string(summary.num_core_achievements) + " unlocked - softcore" +
                       (rc_client_get_encore_mode_enabled(client) ? ", replay mode" : "") +
                       (rc_client_get_spectator_mode_enabled(client) ? ", TEST MODE" : ""),
        .badgeUrl = game->badge_url ? game->badge_url : "",
        .gold = false,
    });

    // Download every badge now so unlock popups show their image immediately
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++) {
            const char* url = list->buckets[b].achievements[i]->badge_url;
            if (url != nullptr) {
                RAPopup::PrefetchBadge(url);
            }
        }
    }
    rc_client_destroy_achievement_list(list);
}

// Shows a popup for a random achievement in the set, to preview the style
void PreviewPopup() {
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        sClient, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    std::vector<const rc_client_achievement_t*> all;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++) {
            all.push_back(list->buckets[b].achievements[i]);
        }
    }
    if (!all.empty()) {
        const rc_client_achievement_t* a = all[rand() % all.size()];
        RAPopup::Show({
            .headline = "Achievement Unlocked (preview)",
            .title = a->title,
            .description = a->description ? a->description : "",
            .points = a->points,
            .badgeUrl = a->badge_url ? a->badge_url : "",
        });
    }
    rc_client_destroy_achievement_list(list);
}

void LoadGame() {
    const RomEntry* rom = FindUsableRom();
    if (rom == nullptr) {
        sStatus = "No supported ROM: achievements need a non-Master Quest extraction";
        sRomStatus.clear();
        return;
    }
    sRomStatus =
        std::string(rom->name) + (rom->match == RomMatch::Exact ? " (exact match)" : " (mapped to the NTSC 1.0 set)");
    SPDLOG_INFO("[RetroAchievements] Using {}", sRomStatus);
    sBusy = true;
    sStatus = "Loading achievements...";
    rc_client_begin_load_game(sClient, NTSC_US_10_HASH, OnGameLoaded, nullptr);
}

void RC_CCONV OnLoggedIn(int result, const char* error, rc_client_t* client, void*) {
    sBusy = false;
    if (result != RC_OK) {
        sStatus = std::string("Login failed: ") + (error ? error : "unknown error");
        SPDLOG_ERROR("[RetroAchievements] {}", sStatus);
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) {
            CVarClear(CVAR_RA("Token"));
            SaveCVars();
        }
        return;
    }

    const rc_client_user_t* user = rc_client_get_user_info(client);
    // Keep the token, never the password
    CVarSetString(CVAR_RA("Username"), user->username);
    CVarSetString(CVAR_RA("Token"), user->token);
    SaveCVars();
    sStatus = std::string("Logged in as ") + user->display_name;
    SPDLOG_INFO("[RetroAchievements] {}", sStatus);
    LoadGame();
}

void CreateClient() {
    if (sClient != nullptr) {
        return;
    }
    sClient = rc_client_create(ReadMemory, ServerCall);
    rc_client_enable_logging(sClient, RC_CLIENT_LOG_LEVEL_INFO, LogMessage);
    rc_client_set_event_handler(sClient, HandleEvent);
    // Proof of concept: softcore only, until hardcore restrictions exist
    rc_client_set_hardcore_enabled(sClient, 0);
    // Encore (replay) mode: already-unlocked achievements stay active and pop again. Re-earned
    // unlocks still go to the server, which keeps the original unlock date.
    rc_client_set_encore_mode_enabled(sClient, CVarGetInteger(CVAR_RA("Encore"), 0));
    // Test mode (rcheevos spectator mode): achievements trigger and pop locally, but no unlock or
    // leaderboard submission is sent. Safe with debug saves, the save editor and cheats.
    rc_client_set_spectator_mode_enabled(sClient, CVarGetInteger(CVAR_RA("TestMode"), 0));

    char clause[64];
    rc_client_get_user_agent_clause(sClient, clause, sizeof(clause));
#if defined(__APPLE__)
    const char* os = "macOS";
#elif defined(_WIN32)
    const char* os = "Windows";
#else
    const char* os = "Linux";
#endif
    RAHttp::SetUserAgent(std::string("ShipOfHarkinian/") + std::to_string(gBuildVersionMajor) + "." +
                         std::to_string(gBuildVersionMinor) + "." + std::to_string(gBuildVersionPatch) + " (" + os +
                         ") " + clause);
}

void LoginWithPassword(const std::string& username, const std::string& password) {
    CreateClient();
    sBusy = true;
    sStatus = "Logging in...";
    rc_client_begin_login_with_password(sClient, username.c_str(), password.c_str(), OnLoggedIn, nullptr);
}

void Logout() {
    if (sClient != nullptr) {
        rc_client_unload_game(sClient);
        rc_client_logout(sClient);
    }
    CVarClear(CVAR_RA("Token"));
    SaveCVars();
    sStatus = "Not logged in";
}

// ---- Per-frame work ----

void OnGameFrame() {
    RAHttp::DispatchCompleted();
    if (sClient == nullptr) {
        return;
    }
    // A Master Quest save plays different dungeons than the set was written for.
    // That only happens with an MQ archive loaded next to a vanilla one.
    if (rc_client_is_game_loaded(sClient) && !ResourceMgr_IsGameMasterQuest()) {
        RAMemory::Refresh();
        rc_client_do_frame(sClient);
    } else {
        rc_client_idle(sClient);
    }
}

void Init() {
    RAPopup::Init();
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnGameFrame);

    std::string username = CVarGetString(CVAR_RA("Username"), "");
    std::string token = CVarGetString(CVAR_RA("Token"), "");
    if (!username.empty() && !token.empty()) {
        CreateClient();
        sBusy = true;
        sStatus = "Logging in...";
        rc_client_begin_login_with_token(sClient, username.c_str(), token.c_str(), OnLoggedIn, nullptr);
    }
}

RegisterShipInitFunc sInit(Init);

} // namespace

namespace RetroAchievements {

void DrawMenu() {
    ImGui::TextWrapped("Earn achievements from the RetroAchievements set for Ocarina of Time (N64) while playing "
                       "SoH. Proof of concept: softcore only. Works with any non-Master Quest extraction.");
    ImGui::Spacing();
    ImGui::Text("Status: %s", sStatus.c_str());
    if (!sRomStatus.empty()) {
        ImGui::Text("ROM: %s", sRomStatus.c_str());
    }
    if (sClient != nullptr && rc_client_is_game_loaded(sClient) && ResourceMgr_IsGameMasterQuest()) {
        ImGui::TextWrapped("Paused: this save is Master Quest, which the set doesn't cover.");
    }

    const bool loggedIn = sClient != nullptr && rc_client_get_user_info(sClient) != nullptr;

    if (!loggedIn) {
        static char username[64] = "";
        static char password[128] = "";
        if (username[0] == '\0') {
            std::string saved = CVarGetString(CVAR_RA("Username"), "");
            snprintf(username, sizeof(username), "%s", saved.c_str());
        }
        ImGui::BeginDisabled(sBusy);
        ImGui::InputText("Username##RA", username, sizeof(username));
        ImGui::InputText("Password##RA", password, sizeof(password), ImGuiInputTextFlags_Password);
        if (ImGui::Button("Log in##RA") && username[0] != '\0' && password[0] != '\0') {
            LoginWithPassword(username, password);
            // The password is only needed for this request
            memset(password, 0, sizeof(password));
        }
        ImGui::EndDisabled();
        return;
    }

    const rc_client_user_t* user = rc_client_get_user_info(sClient);
    ImGui::Text("User: %s (%u softcore points)", user->display_name, user->score_softcore);

    // rc_client only reads encore mode when a game loads, so toggling it reloads the set
    bool encore = CVarGetInteger(CVAR_RA("Encore"), 0) != 0;
    ImGui::BeginDisabled(sBusy);
    if (ImGui::Checkbox("Replay mode (encore)##RA", &encore)) {
        CVarSetInteger(CVAR_RA("Encore"), encore ? 1 : 0);
        SaveCVars();
        rc_client_set_encore_mode_enabled(sClient, encore ? 1 : 0);
        if (rc_client_is_game_loaded(sClient)) {
            rc_client_unload_game(sClient);
            LoadGame();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Already-unlocked achievements can be earned again, with popups.\n"
                          "Your RetroAchievements profile keeps the original unlock dates.");
    }

    bool testMode = CVarGetInteger(CVAR_RA("TestMode"), 0) != 0;
    ImGui::BeginDisabled(sBusy);
    if (ImGui::Checkbox("Test mode (don't submit unlocks)##RA", &testMode)) {
        CVarSetInteger(CVAR_RA("TestMode"), testMode ? 1 : 0);
        SaveCVars();
        if (testMode) {
            // Takes effect at the next unlock, no reload needed
            rc_client_set_spectator_mode_enabled(sClient, 1);
        } else if (rc_client_is_game_loaded(sClient)) {
            // Test mode enabled at load skips the server session and stays locked on until the set
            // reloads. Unloading releases the lock; reload with a real session.
            rc_client_unload_game(sClient);
            rc_client_set_spectator_mode_enabled(sClient, 0);
            LoadGame();
        } else {
            rc_client_set_spectator_mode_enabled(sClient, 0);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Achievements still trigger and pop up in-game, but nothing is sent to RetroAchievements.\n"
                          "Use it with debug saves, the save editor or cheats.\n"
                          "Turning it off reloads the achievement set.");
    }
    if (rc_client_get_spectator_mode_enabled(sClient)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "TEST MODE: unlocks are not sent to RetroAchievements");
    }

    bool sound = CVarGetInteger(CVAR_RA("Sound"), 1) != 0;
    if (ImGui::Checkbox("Unlock sound##RA", &sound)) {
        CVarSetInteger(CVAR_RA("Sound"), sound ? 1 : 0);
        SaveCVars();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!rc_client_is_game_loaded(sClient));
    if (ImGui::Button("Preview popup##RA")) {
        PreviewPopup();
    }
    ImGui::EndDisabled();

    if (rc_client_is_game_loaded(sClient)) {
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(sClient, &summary);
        ImGui::Text("Unlocked: %u / %u", summary.num_unlocked_achievements, summary.num_core_achievements);
        ImGui::Text("Not supported by this build yet: %u", summary.num_unsupported_achievements);

        char presence[256];
        if (rc_client_get_rich_presence_message(sClient, presence, sizeof(presence)) > 0) {
            ImGui::TextWrapped("Rich presence: %s", presence);
        }
        ImGui::Text("Unmapped RAM addresses read: %u", RAMemory::UnmappedReadCount());
    }

    if (ImGui::Button("Log out##RA")) {
        Logout();
    }
}

} // namespace RetroAchievements

#endif // ENABLE_RETROACHIEVEMENTS
