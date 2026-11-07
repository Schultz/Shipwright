#ifdef ENABLE_RETROACHIEVEMENTS

#include "RAMemory.h"

#include <cstring>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>

extern "C" {
#include "z64.h"
#include "variables.h"
extern PlayState* gPlayState;
extern ActiveSequence gActiveSeqs[4];
extern u16 D_8011E1C0; // decomp: sCurTextId, text of the current cutscene textbox
void Interface_GetTimerCounters(s16* timerNextSecond, s16* timerState, s16* subTimerNextSecond, s16* subTimerState);
}

namespace {

constexpr uint32_t RDRAM_SIZE = 0x800000;

// NTSC-U 1.0 addresses (offsets from 0x80000000), from the zeldaret/oot decomp
constexpr uint32_t N64_SAVE_CONTEXT = 0x11A5D0;
constexpr uint32_t N64_SAVE_CONTEXT_SIZE = 0x1428;
constexpr uint32_t N64_PLAY_STATE = 0x1C84A0;
constexpr uint32_t N64_ACTIVE_SEQS = 0x124C00;      // gActiveSeqs[4], 0x264 bytes each
constexpr uint32_t N64_CUR_TEXT_ID = 0x0EFCD0;      // sCurTextId (z_demo.c)
constexpr uint32_t N64_INTERFACE_TIMERS = 0x11BF20; // Interface_Draw timer statics (z_parameter.c .bss)

std::vector<uint8_t> sRam(RDRAM_SIZE, 0);
std::vector<bool> sMapped(RDRAM_SIZE, false);
std::unordered_set<uint32_t> sLoggedUnmapped;
bool sLayoutBuilt = false;
// While true, Put* calls only record which addresses are mapped
bool sMarkingLayout = false;

// N64 address -> position in RA's little-endian word view
inline uint32_t Swizzle(uint32_t n64Address) {
    return n64Address ^ 3;
}

void Put8(uint32_t n64Address, uint8_t value) {
    uint32_t i = Swizzle(n64Address);
    if (sMarkingLayout) {
        sMapped[i] = true;
        return;
    }
    sRam[i] = value;
}

void Put16(uint32_t n64Address, uint16_t value) {
    Put8(n64Address, value >> 8);
    Put8(n64Address + 1, value & 0xFF);
}

void Put32(uint32_t n64Address, uint32_t value) {
    Put16(n64Address, value >> 16);
    Put16(n64Address + 2, value & 0xFFFF);
}

// Writes a scalar of any width in N64 byte order
template <typename T> void Put(uint32_t n64Address, T value) {
    static_assert(std::is_arithmetic_v<T>, "Put only handles scalars");
    if constexpr (sizeof(T) == 1) {
        Put8(n64Address, static_cast<uint8_t>(value));
    } else if constexpr (sizeof(T) == 2) {
        Put16(n64Address, static_cast<uint16_t>(value));
    } else {
        uint32_t raw;
        std::memcpy(&raw, &value, sizeof(raw)); // floats keep their bit pattern
        Put32(n64Address, raw);
    }
}

// Writes the first `count` elements of an array. SoH widened some arrays, so
// the N64 element count is passed explicitly rather than taken from the type.
template <typename T> void PutArray(uint32_t n64Address, const T* values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        Put(n64Address + static_cast<uint32_t>(i * sizeof(T)), values[i]);
    }
}

void PutItemEquips(uint32_t base, const ItemEquips& e) {
    PutArray(base + 0x00, e.buttonItems, 4);  // SoH: 8 for D-pad equips
    PutArray(base + 0x04, e.cButtonSlots, 3); // SoH: 7 for D-pad equips
    Put(base + 0x08, e.equipment);
}

void PutSaveContext() {
    const uint32_t b = N64_SAVE_CONTEXT;
    const SaveContext& s = gSaveContext;

    Put(b + 0x0000, s.entranceIndex);
    Put(b + 0x0004, s.linkAge);
    Put(b + 0x0008, s.cutsceneIndex);
    Put(b + 0x000C, s.dayTime);
    Put(b + 0x0010, s.nightFlag);
    Put(b + 0x0014, s.totalDays);
    Put(b + 0x0018, s.bgsDayCount);
    PutArray(b + 0x001C, reinterpret_cast<const uint8_t*>(s.newf), 6);
    Put(b + 0x0022, s.deaths);
    PutArray(b + 0x0024, s.playerName, 8);
    Put(b + 0x002C, s.n64ddFlag);
    Put(b + 0x002E, s.healthCapacity);
    Put(b + 0x0030, s.health);
    Put(b + 0x0032, s.magicLevel);
    Put(b + 0x0033, s.magic);
    Put(b + 0x0034, s.rupees);
    Put(b + 0x0036, s.swordHealth);
    Put(b + 0x0038, s.naviTimer);
    Put(b + 0x003A, s.isMagicAcquired);
    Put(b + 0x003C, s.isDoubleMagicAcquired);
    Put(b + 0x003D, s.isDoubleDefenseAcquired);
    Put(b + 0x003E, s.bgsFlag);
    Put(b + 0x003F, s.ocarinaGameRoundNum);
    PutItemEquips(b + 0x0040, s.childEquips);
    PutItemEquips(b + 0x004A, s.adultEquips);
    Put(b + 0x0054, s.unk_54);
    Put(b + 0x0066, s.savedSceneNum);
    PutItemEquips(b + 0x0068, s.equips);

    // Inventory
    const uint32_t inv = b + 0x0074;
    const Inventory& i = s.inventory;
    PutArray(inv + 0x00, i.items, 24);
    PutArray(inv + 0x18, i.ammo, 16);
    Put(inv + 0x28, i.equipment);
    Put(inv + 0x2C, i.upgrades);
    Put(inv + 0x30, i.questItems);
    PutArray(inv + 0x34, i.dungeonItems, 20);
    PutArray(inv + 0x48, i.dungeonKeys, 19);
    Put(inv + 0x5B, i.defenseHearts);
    Put(inv + 0x5C, i.gsTokens);

    // Permanent scene flags, 124 scenes x 0x1C bytes
    for (uint32_t scene = 0; scene < 124; scene++) {
        const uint32_t f = b + 0x00D4 + scene * 0x1C;
        const SavedSceneFlags& sf = s.sceneFlags[scene];
        Put(f + 0x00, sf.chest);
        Put(f + 0x04, sf.swch);
        Put(f + 0x08, sf.clear);
        Put(f + 0x0C, sf.collect);
        Put(f + 0x10, sf.unk);
        Put(f + 0x14, sf.rooms);
        Put(f + 0x18, sf.floors);
    }

    // Farore's Wind
    const uint32_t fw = b + 0x0E64;
    Put(fw + 0x00, s.fw.pos.x);
    Put(fw + 0x04, s.fw.pos.y);
    Put(fw + 0x08, s.fw.pos.z);
    Put(fw + 0x0C, s.fw.yaw);
    Put(fw + 0x10, s.fw.playerParams);
    Put(fw + 0x14, s.fw.entranceIndex);
    Put(fw + 0x18, s.fw.roomIndex);
    Put(fw + 0x1C, s.fw.set);
    Put(fw + 0x20, s.fw.tempSwchFlags);
    Put(fw + 0x24, s.fw.tempCollectFlags);

    PutArray(b + 0x0E9C, s.gsFlags, 6);
    PutArray(b + 0x0EB8, s.highScores, 7);
    PutArray(b + 0x0ED4, s.eventChkInf, 14);
    PutArray(b + 0x0EF0, s.itemGetInf, 4);
    PutArray(b + 0x0EF8, s.infTable, 30);
    Put(b + 0x0F38, s.worldMapAreaData);
    Put(b + 0x0F40, s.scarecrowLongSongSet);
    Put(b + 0x12C5, s.scarecrowSpawnSongSet);

    // Epona
    Put(b + 0x1348, s.horseData.scene);
    Put(b + 0x134A, s.horseData.pos.x);
    Put(b + 0x134C, s.horseData.pos.y);
    Put(b + 0x134E, s.horseData.pos.z);
    Put(b + 0x1350, s.horseData.angle);

    Put(b + 0x1352, s.checksum);
    Put(b + 0x1354, s.fileNum);
    Put(b + 0x135C, s.gameMode);
    Put(b + 0x1360, s.sceneLayer);
    Put(b + 0x1364, s.respawnFlag);

    // Respawn points, 3 x 0x1C bytes
    for (uint32_t r = 0; r < RESPAWN_MODE_MAX; r++) {
        const uint32_t rp = b + 0x1368 + r * 0x1C;
        const RespawnData& d = s.respawn[r];
        Put(rp + 0x00, d.pos.x);
        Put(rp + 0x04, d.pos.y);
        Put(rp + 0x08, d.pos.z);
        Put(rp + 0x0C, d.yaw);
        Put(rp + 0x0E, d.playerParams);
        Put(rp + 0x10, d.entranceIndex);
        Put(rp + 0x12, d.roomIndex);
        Put(rp + 0x13, d.data);
        Put(rp + 0x14, d.tempSwchFlags);
        Put(rp + 0x18, d.tempCollectFlags);
    }

    Put(b + 0x13BC, s.entranceSpeed);
    Put(b + 0x13C0, s.entranceSound);
    Put(b + 0x13C3, s.retainWeatherMode);
    Put(b + 0x13C4, s.dogParams);
    Put(b + 0x13C6, s.textTriggerFlags);
    Put(b + 0x13C7, s.showTitleCard);
    Put(b + 0x13C8, s.nayrusLoveTimer);
    Put(b + 0x13CC, s.rupeeAccumulator);
    Put(b + 0x13CE, s.timerState);
    Put(b + 0x13D0, s.timerSeconds);
    Put(b + 0x13D2, s.subTimerState);
    Put(b + 0x13D4, s.subTimerSeconds);
    PutArray(b + 0x13D6, s.timerX, 2);
    PutArray(b + 0x13DA, s.timerY, 2);
    Put(b + 0x13E0, s.seqId);
    Put(b + 0x13E1, s.natureAmbienceId);
    PutArray(b + 0x13E2, s.buttonStatus, 5); // SoH: 9 for D-pad equips
    Put(b + 0x13E7, s.forceRisingButtonAlphas);
    Put(b + 0x13E8, s.nextHudVisibilityMode);
    Put(b + 0x13EA, s.hudVisibilityMode);
    Put(b + 0x13EC, s.hudVisibilityModeTimer);
    Put(b + 0x13EE, s.prevHudVisibilityMode);
    Put(b + 0x13F0, s.magicState);
    Put(b + 0x13F2, s.prevMagicState);
    Put(b + 0x13F4, s.magicCapacity);
    Put(b + 0x13F6, s.magicFillTarget);
    Put(b + 0x13F8, s.magicTarget);
    PutArray(b + 0x13FA, s.eventInf, 4);
    Put(b + 0x1402, s.mapIndex);
    Put(b + 0x1404, s.minigameState);
    Put(b + 0x1406, s.minigameScore);
    Put(b + 0x1409, s.language);
    Put(b + 0x140A, s.audioSetting);
    Put(b + 0x140C, s.zTargetSetting);
    Put(b + 0x140E, s.forcedSeqId);
    Put(b + 0x1410, s.cutsceneTransitionControl);
    Put(b + 0x1412, s.nextCutsceneIndex);
    Put(b + 0x1414, s.cutsceneTrigger);
    Put(b + 0x1415, s.chamberCutsceneNum);
    Put(b + 0x1416, s.nextDayTime);
    Put(b + 0x1418, s.transFadeDuration);
    Put(b + 0x1419, s.transWipeSpeed);
    Put(b + 0x141A, s.skyboxTime);
    Put(b + 0x141C, s.dogIsLost);
    Put(b + 0x141D, s.nextTransitionType);
    Put(b + 0x1420, s.worldMapArea);
    Put(b + 0x1422, s.sunsSongState);
    Put(b + 0x1424, s.healthAccumulator);
}

// PlayState fields the achievement set reads. Outside of gameplay there is no
// PlayState, so these read as zero, which is also what the set expects from an
// N64 sitting on the title screen.
void PutPlayState() {
    const uint32_t p = N64_PLAY_STATE;
    const PlayState* play = gPlayState;

    Put<uint32_t>(p + 0x0009C, play ? play->state.frames : 0);         // RA 0x1C853C, scene timer
    Put<int16_t>(p + 0x000A4, play ? play->sceneNum : 0);              // RA 0x1C8546
    Put<uint8_t>(p + 0x01C27, play ? play->actorCtx.lensActive : 0);   // RA 0x1CA0C4

    // Current scene's flags (switch, chest, clear, collect), RA 0x1CA1C8...
    const uint32_t flags = p + 0x01C24 + 0x104;
    Put<uint32_t>(flags + 0x00, play ? play->actorCtx.flags.swch : 0);
    Put<uint32_t>(flags + 0x04, play ? play->actorCtx.flags.tempSwch : 0);
    Put<uint32_t>(flags + 0x08, play ? play->actorCtx.flags.unk0 : 0);
    Put<uint32_t>(flags + 0x0C, play ? play->actorCtx.flags.unk1 : 0);
    Put<uint32_t>(flags + 0x10, play ? play->actorCtx.flags.chest : 0);
    Put<uint32_t>(flags + 0x14, play ? play->actorCtx.flags.clear : 0);
    Put<uint32_t>(flags + 0x18, play ? play->actorCtx.flags.tempClear : 0);
    Put<uint32_t>(flags + 0x1C, play ? play->actorCtx.flags.collect : 0);
    Put<uint32_t>(flags + 0x20, play ? play->actorCtx.flags.tempCollect : 0);
    Put<uint16_t>(p + 0x103D0, play ? play->msgCtx.textId : 0);        // RA 0x1D8872
    Put<int8_t>(p + 0x11E5C, play ? play->shootingGalleryStatus : 0);  // RA 0x1DA2FF
    Put<int8_t>(p + 0x11E5D, play ? play->bombchuBowlingStatus : 0);   // RA 0x1DA2FE
}

// Globals outside SaveContext and PlayState. Addresses come from the
// zeldaret/oot NTSC 1.0 build's linker map.
void PutGlobals() {
    // Music: the set checks gActiveSeqs[0].prevSeqId to detect boss fights
    // (RA 0x124E54). SoH stores the vanilla seqId here even when the audio
    // shuffler plays a replacement, so these achievements survive shuffling.
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t seq = N64_ACTIVE_SEQS + i * 0x264;
        Put(seq + 0x254, gActiveSeqs[i].seqId);
        Put(seq + 0x256, gActiveSeqs[i].prevSeqId);
    }

    // Text of the cutscene textbox, RA 0x0EFCD2 (song lessons, spiritual stones)
    Put(N64_CUR_TEXT_ID, D_8011E1C0);

    // Frames until the HUD timer's next second, RA 0x11BF22 (race leaderboards)
    s16 timerNextSecond, timerState, subTimerNextSecond, subTimerState;
    Interface_GetTimerCounters(&timerNextSecond, &timerState, &subTimerNextSecond, &subTimerState);
    Put(N64_INTERFACE_TIMERS + 0, timerNextSecond);
    Put(N64_INTERFACE_TIMERS + 2, timerState);
    Put(N64_INTERFACE_TIMERS + 4, subTimerNextSecond);
    Put(N64_INTERFACE_TIMERS + 6, subTimerState);
}

void BuildLayout() {
    // Run the writers once in marking mode to learn which bytes are mapped
    sMarkingLayout = true;
    PutSaveContext();
    PutPlayState();
    PutGlobals();
    sMarkingLayout = false;
    sLayoutBuilt = true;

    size_t mapped = 0;
    for (bool m : sMapped) {
        mapped += m;
    }
    SPDLOG_INFO("[RetroAchievements] Virtual N64 RAM maps {} bytes (SaveContext is {} bytes)", mapped,
                N64_SAVE_CONTEXT_SIZE);
}

} // namespace

namespace RAMemory {

void Refresh() {
    if (!sLayoutBuilt) {
        BuildLayout();
    }
    PutSaveContext();
    PutPlayState();
    PutGlobals();
}

uint32_t Read(uint32_t address, uint8_t* buffer, uint32_t numBytes) {
    if (address >= RDRAM_SIZE) {
        return 0;
    }
    if (address + numBytes > RDRAM_SIZE) {
        numBytes = RDRAM_SIZE - address;
    }

    std::memcpy(buffer, &sRam[address], numBytes);

    if (sLayoutBuilt) {
        for (uint32_t i = 0; i < numBytes; i++) {
            uint32_t a = address + i;
            if (!sMapped[a] && sLoggedUnmapped.insert(a).second) {
                SPDLOG_WARN("[RetroAchievements] Unmapped read at RA address 0x{:06X} (N64 0x{:06X})", a, a ^ 3);
            }
        }
    }
    return numBytes;
}

uint32_t UnmappedReadCount() {
    return static_cast<uint32_t>(sLoggedUnmapped.size());
}

} // namespace RAMemory

#endif // ENABLE_RETROACHIEVEMENTS
