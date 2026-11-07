// Offline test for the SoH RetroAchievements PoC.
// Uses SoH's real RAMemory.cpp and RAHttp.cpp against the live RA set 10113,
// with a synthetic gSaveContext/gPlayState. Award, leaderboard and ping
// requests are blocked so nothing is ever unlocked on the account.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <rc_client.h>
// Internal header, only to flip allow_leaderboards_in_softcore for testing
#include "../src/rc_client_internal.h"

#include "soh/Network/RetroAchievements/RAHttp.h"
#include "soh/Network/RetroAchievements/RAMemory.h"

extern "C" {
#include "z64.h"
SaveContext gSaveContext;
PlayState* gPlayState = nullptr;
ActiveSequence gActiveSeqs[4];
u16 D_8011E1C0;
static s16 sTimerNext, sTimerState, sSubTimerNext, sSubTimerState;
void Interface_GetTimerCounters(s16* a, s16* b, s16* c, s16* d) {
    *a = sTimerNext; *b = sTimerState; *c = sSubTimerNext; *d = sSubTimerState;
}
}

static PlayState sPlay;
static rc_client_t* sClient;
static bool sDone = false;
static int sResult = 0;
static std::vector<std::string> sTriggered;
static int sBlocked = 0;
static std::vector<std::string> sUnlockedBefore;
static std::vector<std::string> sLbStarted;

static uint32_t RC_CCONV Read(uint32_t a, uint8_t* b, uint32_t n, rc_client_t*) {
    return RAMemory::Read(a, b, n);
}

static void RC_CCONV Call(const rc_api_request_t* req, rc_client_server_callback_t cb, void* data, rc_client_t*) {
    std::string post = req->post_data ? req->post_data : "";
    if (post.find("r=awardachievement") != std::string::npos || post.find("r=submitlbentry") != std::string::npos ||
        post.find("r=ping") != std::string::npos) {
        sBlocked++;
        rc_api_server_response_t r{};
        r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR; // never leaves the process
        cb(&r, data);
        return;
    }
    RAHttp::Request(req->url, post, req->content_type ? req->content_type : "", [cb, data](const RAHttp::Response& resp) {
        rc_api_server_response_t r{};
        r.body = resp.body.empty() ? nullptr : resp.body.c_str();
        r.body_length = resp.body.size();
        r.http_status_code = resp.statusCode;
        cb(&r, data);
    });
}

static void RC_CCONV Event(const rc_client_event_t* e, rc_client_t*) {
    if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) {
        sTriggered.push_back(e->achievement->title);
        printf("  TRIGGERED: %s\n", e->achievement->title);
    } else if (e->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED) {
        sLbStarted.push_back(e->leaderboard->title);
        printf("  LEADERBOARD STARTED: %s\n", e->leaderboard->title);
    }
}

static void RC_CCONV Log(const char* m, const rc_client_t*) {
    printf("  [rc] %s\n", m);
}

static void RC_CCONV Done(int result, const char* err, rc_client_t*, void*) {
    sResult = result;
    if (result != RC_OK) {
        printf("  error %d: %s\n", result, err ? err : "");
    }
    sDone = true;
}

static bool Wait() {
    sDone = false;
    for (int i = 0; i < 300 && !sDone; i++) {
        RAHttp::DispatchCompleted();
        rc_client_idle(sClient);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return sDone && sResult == RC_OK;
}

static void Frames(int n) {
    for (int i = 0; i < n; i++) {
        RAMemory::Refresh();
        rc_client_do_frame(sClient);
    }
}

static const rc_client_achievement_t* Find(const char* title) {
    rc_client_achievement_list_t* list =
        rc_client_create_achievement_list(sClient, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    const rc_client_achievement_t* found = nullptr;
    for (uint32_t b = 0; b < list->num_buckets; b++)
        for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++)
            if (strcmp(list->buckets[b].achievements[i]->title, title) == 0)
                found = list->buckets[b].achievements[i];
    rc_client_destroy_achievement_list(list);
    return found;
}

static void Expect(const char* title) {
    bool fired = false, before = false;
    for (auto& t : sTriggered) fired |= (t == title);
    for (auto& t : sUnlockedBefore) before |= (t == title);
    const char* result = fired ? "PASS (triggered)"
                         : before ? "SKIP (already unlocked on the account, so rcheevos doesn't evaluate it)"
                                  : "FAIL (did not trigger)";
    printf("%-22s %s\n", title, result);
}

int main(int argc, char** argv) {
    std::ifstream creds(argv[1]);
    std::string user, key, token;
    std::getline(creds, user);
    std::getline(creds, key);
    std::getline(creds, token);

    RAHttp::SetUserAgent("ShipOfHarkinian/PoC-test (macOS)");
    sClient = rc_client_create(Read, Call);
    rc_client_enable_logging(sClient, RC_CLIENT_LOG_LEVEL_WARN, Log);
    rc_client_set_event_handler(sClient, Event);
    rc_client_set_hardcore_enabled(sClient, 0);
    // Leaderboards only run in hardcore by default. Allow them here so their start
    // conditions can be tested; submissions are blocked in Call() either way.
    sClient->state.allow_leaderboards_in_softcore = 1;

    printf("Login...\n");
    rc_client_begin_login_with_token(sClient, user.c_str(), token.c_str(), Done, nullptr);
    if (!Wait()) return 1;
    printf("  ok: %s\n", rc_client_get_user_info(sClient)->display_name);

    printf("Load game...\n");
    rc_client_begin_load_game(sClient, "5bd1fe107bf8106b2ab6650abecd54d6", Done, nullptr);
    if (!Wait()) return 1;
    const rc_client_game_t* g = rc_client_get_game_info(sClient);
    rc_client_user_game_summary_t s{};
    rc_client_get_user_game_summary(sClient, &s);
    printf("  ok: game %u \"%s\", %u core achievements, %u unlocked, %u unsupported\n", g->id, g->title,
           s.num_core_achievements, s.num_unlocked_achievements, s.num_unsupported_achievements);

    for (const char* t : {"Plenty of Hearts", "Heat Resistant Tunic", "Rise and Shine!!", "Hero of Time", "Rolling Thunder", "Damsel in Distress"}) {
        const rc_client_achievement_t* a = Find(t);
        if (a && a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) {
            sUnlockedBefore.push_back(t);
        }
    }
    // Baseline state on a save file, in Kokiri Forest
    memset(&gSaveContext, 0, sizeof(gSaveContext));
    memset(&sPlay, 0, sizeof(sPlay));
    gPlayState = &sPlay;
    gSaveContext.linkAge = 1;
    gSaveContext.healthCapacity = 0x30;
    gSaveContext.inventory.gsTokens = 7;
    gSaveContext.inventory.equipment = 0x0101; // Kokiri sword + Kokiri tunic
    sPlay.sceneNum = 0x55;
    Frames(10);

    char rp[256];
    rc_client_get_rich_presence_message(sClient, rp, sizeof(rp));
    printf("\nRich presence: %s\n", rp);
    printf("  expected:    Young Link is in Kokiri Forest [❤️3] [🕷️7/100]\n\n");

    // Plenty of Hearts: 0x11A5FC goes 304 -> 320
    gSaveContext.healthCapacity = 304;
    Frames(5);
    gSaveContext.healthCapacity = 320;
    Frames(2);

    // Heat Resistant Tunic: Goron Tunic bit (equipment bit 9) appears
    Frames(3);
    gSaveContext.inventory.equipment |= 1 << 9;
    Frames(2);

    // Rise and Shine!!: in Hyrule Castle (95), eventChkInf[1] bit 3 then bit 4
    sPlay.sceneNum = 95;
    gSaveContext.eventChkInf[1] = 0x08;
    Frames(5);
    gSaveContext.eventChkInf[1] = 0x18;
    Frames(2);

    // Hero of Time: Temple of Time (67) with text 0x7054 showing
    sPlay.sceneNum = 67;
    Frames(5);
    sPlay.msgCtx.textId = 28756;
    Frames(2);

    // ---- New mappings: music, cutscene text, HUD timer ----
    sPlay.msgCtx.textId = 0;
    gSaveContext.health = 0x30;

    // Rolling Thunder: scene 19, boss music 38, then music 27 -> 33, no damage
    sPlay.sceneNum = 19;
    gActiveSeqs[0].prevSeqId = 38;
    Frames(5);
    gActiveSeqs[0].prevSeqId = 27;
    Frames(5);
    gActiveSeqs[0].prevSeqId = 33;
    Frames(2);

    // Damsel in Distress: Temple of Time, Ocarina of Time in slot 7, flags, cutscene text 0x2009
    sPlay.sceneNum = 67;
    gSaveContext.inventory.items[7] = 8;
    gSaveContext.eventChkInf[8] |= 0x05;
    Frames(5);
    D_8011E1C0 = 8201;
    Frames(2);

    // Dampe's Race leaderboard: Graveyard (0x53) -> 0x48, HUD timer counter 20 -> 19
    sPlay.sceneNum = 0x53;
    Frames(5);
    sPlay.sceneNum = 0x48;
    sTimerNext = 20;
    Frames(3);
    sTimerNext = 19;
    Frames(2);

    Expect("Rolling Thunder");
    Expect("Damsel in Distress");
    bool lb = false;
    for (auto& t : sLbStarted) lb |= (t == "Dampes Race");
    printf("%-22s %s\n", "[LB] Dampes Race", lb ? "PASS (started)" : "FAIL (did not start)");
    Expect("Plenty of Hearts");
    Expect("Heat Resistant Tunic");
    Expect("Rise and Shine!!");
    Expect("Hero of Time");
    printf("\nUnmapped addresses read: %u\nBlocked award/ping requests: %d\n", RAMemory::UnmappedReadCount(), sBlocked);

    rc_client_unload_game(sClient);
    rc_client_destroy(sClient);
    return 0;
}
