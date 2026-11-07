# RetroAchievements in Shipwright: integration plan

Status: exploratory. Nothing here is committed or approved by RetroAchievements.
Date: 2026-11-05

## PoC status (2026-11-05)

Built and passing offline tests on macOS (arm64). Not yet played in-game. Works with any non-Master Quest extraction (see "Game identification").

Code, all behind `-DENABLE_RETROACHIEVEMENTS=ON` (default OFF):

| File | Contents |
|---|---|
| `soh/soh/Network/RetroAchievements/RAMemory.cpp` | Virtual NTSC-U 1.0 RDRAM. All of SaveContext plus scene ID, scene timer, text ID, lens, scene flags, shooting gallery and bombchu status, music, cutscene text and HUD timer counters |
| `soh/src/code/z_parameter.c` | Moves the four HUD timer statics out of `Interface_Draw` to file scope and adds `Interface_GetTimerCounters()`. Behavior is unchanged |
| `soh/soh/Network/RetroAchievements/RAPopup.cpp` | Trophy-style popup at the top right: RA badge image, headline, title, description, points, chime. Badges are prefetched at set load, decoded with stb_image and uploaded through `Fast3dGui::LoadTextureFromResource`. The `GuiTexture` must be built with `ResourceInitData`, because `~IResource` logs `InitData->Path` and crashes on a default-constructed one |
| `soh/soh/Network/RetroAchievements/RAHttp.cpp` | libcurl worker thread. Responses run on the game thread |
| `soh/soh/Network/RetroAchievements/RetroAchievements.cpp` | rc_client setup, token login, game load by hash, per-frame `rc_client_do_frame` from `OnGameFrameUpdate`, unlock popups, menu page, replay mode (rcheevos encore mode, `gRemote.RetroAchievements.Encore`), test mode (rcheevos spectator mode, `gRemote.RetroAchievements.TestMode`: achievements trigger locally, no session, unlock, leaderboard or ping request is sent) |
| `soh/soh/SohGui/SohMenuNetwork.cpp` | Network > RetroAchievements page |
| `soh/CMakeLists.txt` | rcheevos v12.5.0 through FetchContent, built as a static lib. System libcurl |

Findings from building it:

- RA's N64 memory is RDRAM stored as little-endian 32-bit words. RA address `A` is N64 byte `A ^ 3`. Health, rupees, scene ID, inventory, file number and game mode all line up under that rule.
- rcheevos needs `src/rhash/md5.c` even without ROM hashing. StormLib (via libultraship) also defines `md5_init` with a different struct. Without renaming rcheevos' copy (`md5_init=rc_md5_init` and so on), the linker silently resolves to StormLib's.
- The SoH user agent is `ShipOfHarkinian/<major.minor.patch> (<OS>) rcheevos/12.5`. RA accepted softcore login and game load with it.

Offline test (`RA/poc-test/build_and_run.sh <creds>`). It runs the real `RAMemory.cpp` and `RAHttp.cpp` against the live set with a synthetic `gSaveContext` and `PlayState`. Award, leaderboard and ping requests are blocked, so nothing unlocks on the account.

| Check | Result |
|---|---|
| Token login, load set 10113 by hash | 97 core achievements, 0 unsupported |
| Rich presence | `Young Link is in Kokiri Forest [❤️3] [🕷️7/100]`, matches expected |
| Plenty of Hearts (`healthCapacity`, 16-bit) | Pass |
| Heat Resistant Tunic (byte within `inventory.equipment`) | Pass |
| Hero of Time (scene ID plus `msgCtx.textId`) | Pass |
| Rise and Shine!! (event flags) | Skipped, already unlocked on the test account |
| Rolling Thunder (music, `gActiveSeqs[0].prevSeqId`) | Pass |
| Damsel in Distress (cutscene text, `sCurTextId`) | Pass |
| Dampé's Race leaderboard start (HUD timer counter) | Pass, with softcore leaderboards forced on in the test |

Coverage: 87 of the set's 100 achievement entries and 10 of 13 leaderboards now read only mapped memory. 28 unmapped addresses remain: Link's actor (tier 3), the actor heap (tier 4), the constant `0x000000` the set uses with AddSource, and `0x1D8871`, one byte past `msgCtx.textId`.

rcheevos only runs leaderboards in hardcore. The softcore PoC never starts them in-game. The flag that allows them in softcore (`allow_leaderboards_in_softcore`) is internal, used by RAIntegration, and the test sets it through the internal header.

### Globals found in the NTSC 1.0 map

The zeldaret/oot decomp builds NTSC 1.0 byte-for-byte. Its compressed checksum is RA's hash, `5bd1fe107bf8106b2ab6650abecd54d6`. The linker map from that build is saved as `RA/oot-ntsc-1.0.map`.

| RA address | N64 symbol | SoH source | Used by |
|---|---|---|---|
| `0x124E54` | `gActiveSeqs[0].prevSeqId` (`0x80124C00 + 0x256`) | `gActiveSeqs[0].prevSeqId`. SoH stores the vanilla seqId even when the audio shuffler plays a replacement | 20 boss and minigame achievements |
| `0x0EFCD2` | `sCurTextId` (`0x800EFCD0`, `z_demo.c`) | `D_8011E1C0`, the older decomp name, set in `Cutscene_Command_Textbox` | Entrusted Mission, Damsel in Distress, Musician |
| `0x11BF22`, `0x11BF26` | `Interface_Draw` statics in `z_parameter.c` `.bss` at `0x8011BF20`: `sTimerNextSecondTimer`, `sTimerStateTimer`, `sSubTimerNextSecondTimer`, `sSubTimerStateTimer` | Same statics, moved to file scope | 6 race and minigame leaderboards |

The map lists named symbols only. The timer statics are unnamed function-local `.bss`. They were found by taking `z_map_exp.o`'s `.bss` size (0x10, after `gMapData` at `0x8011BF10`), then reading `z_parameter.o`'s relocations: only `Interface_Draw` touches `.bss+0..+6`, and its static declarations follow that order.

Next steps for the PoC:

1. Play-test in SoH and record a few unlocks.
2. Tier 3: Link's actor fields, plus the animation table. The map has the asset symbols needed.
3. ~~Windows: WinHTTP backend for `RAHttp`.~~ Done.

## Goal

Let SoH players earn achievements from the existing N64 set for Ocarina of Time (RA game 10113). SoH does not get a new set of its own.

RA's standalone policy says "Decompilations, recompilations, and other unofficial ports are not eligible for standalone sets." This plan doesn't ask for one. SoH reports unlocks against the same set emulator players use, so no duplicate set exists.

## How it works

rcheevos does the RA-specific work. SoH supplies two callbacks.

| Piece | Owner | Job |
|---|---|---|
| `rc_client` (rcheevos) | RA library | Login, game load by hash, logic checks each frame, unlocks, leaderboards, rich presence |
| `server_call` callback | SoH | Sends the HTTP POST that rc_client built and hands back the response |
| `read_memory` callback | SoH | Returns what NTSC-U 1.0 RDRAM would hold at an address, built from SoH's live state |
| RAIntegration | RA, Windows only | "The DLL responsible to integrate emulators with RetroAchievements.org." Handles the server connection and adds RA's menu and developer toolkit |

rcheevos doesn't open network connections itself. Its README says "rcheevos does *not* provide HTTP network connections." rc_client builds every request and hands it to the `server_call` callback.

On Windows there's a second option. `rc_client_begin_load_raintegration(client, dir, HWND, ...)` (`rc_client_raintegration.h`) loads the RAIntegration DLL, which then handles the connection and adds RA's menu (memory inspector, achievement editor, code notes) to the app's window. RA set developers expect that toolkit, so the Windows build should support it. The header only exists under `_WIN32` and takes an `HWND`, so macOS and Linux need the plain rc_client path with SoH's own HTTP callback. The plan builds that path first because it works everywhere, then adds RAIntegration loading on Windows.

The `read_memory` callback is the real work. Achievement logic reads N64 addresses like `0x11A606` (rupees). SoH has no N64 RAM, so the callback looks up which SoH variable lives at that address and returns its bytes in N64 byte order.

## What the existing set needs

From `analysis_10113.txt` (main set, 97 achievements, 113 entries counting 13 leaderboards and one marker):

| Tier | Memory read | Entries | Work |
|---|---|---|---|
| 1 | SaveContext only | 6 | Field table |
| 2 | SaveContext plus fixed globals | 90 | Field table plus about 10 globals |
| 3 | Player actor fields | 8 | Fixed offsets plus an animation lookup table |
| 4 | Fixed actor-heap addresses | 8 | One shim per achievement |

Globals outside SaveContext that tier 2 reads:

| N64 addr | Meaning | SoH source |
|---|---|---|
| `0x1C8546` | Scene ID | `play->sceneNum` (`include/z64.h:1421`) |
| `0x1D8872` | Current text box ID | `play->msgCtx.textId` |
| `0x124E54` | Active BGM sequence ID | Audio seq player state. Must report the vanilla ID, not a shuffled replacement |
| `0x0EFCD2` | Dialogue/item-get ID | To identify from the NTSC 1.0 symbol map |
| `0x11BF22`, `0x11BF26` | Frame timers | To identify |
| `0x1CA0C4` | Lens of Truth active | PlayState field |
| `0x1CA1C8`, `0x1CA1C9` | Temp scene flags | PlayState field |
| `0x1DA2FE`, `0x1DA2FF` | Bombchu bowling state | PlayState field |

20 achievements detect boss fights by the music changing, not by boss HP. The music ID has to be the vanilla sequence even if a player has the audio shuffler configured. Hardcore mode turns the shuffler off anyway (see below).

The Glitch Showcase subset (game 17444) leans much harder on tier 4 (18 of 44 entries). Leave it out of scope.

## Phases

### Phase 0: proof of concept, then ask RA

Build a working demo before contacting RA. A demo answers "can a port run the existing set faithfully?" with evidence instead of a proposal, and RA's own docs say a proof of concept carries more weight with the admins.

Before starting, check 2Ship2Harkinian (the Majora's Mask port) for RA work in progress. A Shipwright maintainer said SoH would likely port whatever 2Ship does.

PoC scope, kept small on purpose:

- Softcore only. Any rcheevos client can earn softcore unlocks, so no approval is needed to test against the live set.
- macOS first (the dev machine), using rc_client with a libcurl `server_call` callback. That same code builds on Linux and Windows.
- Game identification by SoH's detected version: N64 1.0 (US/JP) exact, other vanilla versions mapped, Master Quest excluded.
- Translation of SaveContext plus the tier 2 globals. That covers 96 of 113 entries.
- A minimal ImGui window for login and status, and unlock popups through `soh/soh/Notification`.
- Unmapped-read logging on, so the demo shows which addresses it can't answer yet.

PoC deliverables:

- A recording of several achievements unlocking in SoH. Pick a mix: a save-only one ("Plenty of Hearts"), a text-ID one ("Entrusted Mission"), and a music-cued boss one ("Hero of Time" or "Dark Lord").
- A side-by-side check against an emulator showing the same unlocks.
- Rich presence showing the right scene on the player's RA profile.
- This plan and the tier analysis, updated with what the PoC found.

Then send RAdmin the demo and a short note:

- SoH runs the existing set 10113 through rc_client. No new set, so nothing duplicates the N64 set.
- How the game is identified (see "Game identification" below).
- How hardcore restrictions would be enforced (see "Hardcore mode" below).
- What's left: tiers 3 and 4, and the Windows RAIntegration path.

Their answer decides whether hardcore is ever possible. Phases 1 through 5 below go past the PoC into a full implementation.

### Phase 1: plumbing

- Add rcheevos the way Shipwright adds other third-party code. Submodules are only for HarbourMasters' own projects (`libultraship`, `torch`). Small third-party libraries use `FetchContent` pinned to a tag or commit: `spdlog` at `v1.16.0` (`CMakeLists.txt:224`), `dr_libs` at a commit hash (`soh/CMakeLists.txt:293`). System libraries come from vcpkg on Windows (`CMakeLists.txt:106`) and `find_package` on macOS and Linux.
  - rcheevos: `FetchContent_Declare` in `soh/CMakeLists.txt` next to `dr_libs`, `GIT_TAG v12.5.0` (released 2026-09-14). rcheevos ships no `CMakeLists.txt`, so define a static library target from its `src/` files after fetching it.
  - libcurl: `find_package(CURL)` on macOS and Linux. Windows uses WinHTTP, so the vcpkg list doesn't change.
- Write the `server_call` callback. SoH has no HTTP client today. Anchor and CrowdControl use SDL2_net for raw TCP, which can't do HTTPS, and the only `curl` in the tree runs at build time to fetch a controller database (`soh/CMakeLists.txt:756`). Put a small `HttpPost(url, body, userAgent, callback)` interface in front of these platform backends:
  - macOS: system libcurl, which ships with the OS. This is the PoC backend and adds no new dependency.
  - Linux: libcurl (`libcurl-dev`).
  - Windows: WinHTTP, which ships with the OS, or RAIntegration.dll when present.
- Don't copy RetroArch's HTTP stack. Its `server_call` (`RetroArch/cheevos/cheevos_client.c:356`) runs on RetroArch's task queue and `libretro-common/net/net_http.c`. HTTPS there comes from `net_socket_ssl_retro.c`, a new (2026) in-house TLS 1.2 client with a bundled CA list. The files are MIT, but bringing it over means taking a large part of libretro-common and owning that TLS code.
- Do use RetroArch's `cheevos/cheevos.c` as the reference for rc_client usage: the memory-read callback, `rc_client_do_frame` versus `rc_client_idle`, switching into hardcore and its reset, user agent format, and event handling. It bundles rcheevos 12.4.0 in `deps/rcheevos`.
- On Windows, call `rc_client_begin_load_raintegration` at startup. If the DLL is present, it takes over the connection and adds RA's menu to the window. If it's missing, fall back to the libcurl callback.
- Run HTTP on a worker thread. rc_client is asynchronous and expects the callback to complete later.
- Store the login token in SoH's config, never the password. Use `rc_client_begin_login_with_token` after the first login.
- Add a settings window with login, logout, hardcore toggle and status.
- Show unlock and leaderboard notifications through the existing `soh/soh/Notification` system.

### Phase 2: game identification

rc_client loads a game by ROM hash, and SoH ships no ROM. SoH knows which ROM the player extracted from (`ResourceMgr_GetGameVersion`, CRCs in `soh/soh/GameVersions.h`).

The achievement logic never reads the ROM, though. RAMemory builds the NTSC 1.0 RAM layout from SoH's own state, and SoH runs the same game code whichever ROM supplied the assets. Version checks in SoH's game code only branch three ways:

| Check | Where | Affects what the set reads? |
|---|---|---|
| Master Quest | Scene tables (`z_scene_table.c`), map markers, title logo | Yes. Different dungeons, and RA has separate MQ sets |
| PAL region | File select, message boxes, language defaults, HUD layout, credits | No, UI and text only |
| GameCube / JP version | Title logo (`z_en_mag.c`) | No |

So the PoC uses a mapping table (`ROM_TABLE` in `RetroAchievements.cpp`):

- N64 NTSC 1.0 (`0xEC7011B7`) is an exact match. SoH's version ID is the ROM header CRC, which is the same for US 1.0 and JP 1.0. Both are on RA's hash list for set 10113 (`5BD1FE107BF8106B2AB6650ABECD54D6` and `9f04c8e68534b870f707c247fa4b50fc`), so either cartridge is covered. SoH can't tell which it was, so it reports the US hash.
- Every other vanilla version (N64 1.1/1.2, PAL N64, GameCube US/JP/JP CE/PAL, debug, iQue) is mapped to the same set and reports the US 1.0 hash. The menu shows it as "mapped".
- Master Quest versions are unsupported. If an MQ archive is loaded next to a vanilla one and the current save is MQ, frame processing pauses.

Questions for RA:

- Are they fine with mapped versions reporting the NTSC 1.0 hash? If not, the table can drop to exact matches only, with no other code change.
- If RA wants proof the player owns the ROM, hash it during extraction with rcheevos' `rc_hash` and save the result next to the `.o2r`.

### Phase 3: the RAM translation layer (tiers 1 and 2)

- Build a field table from the zeldaret/oot decomp's NTSC-U 1.0 build: every SaveContext field with its N64 offset, size and the matching SoH field.
- Don't `memcpy` SaveContext. SoH changed its layout. For example, `buttonItems` grew from 4 to 8 and `cButtonSlots` from 3 to 7 for D-pad equips (`include/z64save.h:65`), and `ShipSaveContextData ship` was added at the end. Every field after a change sits at a different offset in SoH than on the N64.
- Byte-swap to big-endian on every read.
- For fields SoH widened, return only the vanilla part (the first 4 button items, for example).
- Map the ~10 globals from the tier 2 table.
- Return 0 for addresses with no mapping and log them in debug builds. That log is how unmapped reads in new or updated sets get found.

rc_client calls `read_memory` many times per frame, so lookups have to be fast. A sorted table with binary search, or a flat page array indexed by `address >> 8`, is enough.

Call `rc_client_do_frame` once per game logic frame, from the `OnGameFrameUpdate` hook (`GameInteractor_HookTable.h:13`). Don't call it once per rendered frame, because frame interpolation renders extra frames that aren't logic frames. When the game is paused, call `rc_client_idle` instead.

Exit criteria for this phase:

- The tier 1 and tier 2 achievements unlock in SoH under the same conditions as in an emulator.
- Rich presence shows the right location and status.

### Phase 4: Link's actor (tier 3)

- Map Link's position (the set reads the high half of the Y float at `0x1DAA5A`), the item in hand (`0x1DAB77`) and the animation (`0x1DABDC`) to fields on SoH's `Player` struct. Player sits at a fixed address on NTSC 1.0, `0x1DAA30`.
- Animation needs a lookup table. The set compares the low 16 bits of the N64 segment address of Link's current animation (for example `11664` = `0x2D90`). SoH's animations are loaded resources, so build a table from resource path to the vanilla N64 offset using the decomp's asset XML. Only the handful of animations the set checks are needed at first.

### Phase 5: actor-heap shims (tier 4)

Each of these achievements reads an address where the N64 happened to put a specific actor. Write one shim per case: find the matching actor in SoH (by actor ID and spawn order) and return its field when the set reads that address.

| Achievement | N64 addresses | What it reads |
|---|---|---|
| Spider Net | `0x1E8416` | Queen Gohma state |
| Bombalicious | `0x1ECBAA` | King Dodongo fire-breath timer |
| Drunken Master | `0x1E71FC` | Phantom Ganon related |
| Cuccos' Revenge | `0x1F1C40`..`0x1F4B86`, stride `0x7E0` | Seven Kakariko cuccos' anger state |
| Dangerously Delicious | actor heap | To check |
| LB Epona Race, Slingshot Sniper, Bow Sniper | `0x1E3324`, `0x1E3326`, `0x1F00E4` | Minigame actors |

These only line up if SoH spawns actors in the same order the N64 does. Test every one of them against an emulator.

### Phase 6: hardcore mode

RA's hardcore compliance rules ban save states, rewind, slow motion, frame advance, cheats that change gameplay, memory editors, debuggers and recorded-input playback. Hardcore also needs a unique, stable user agent and at least six months of public releases before RA will review it.

In SoH, hardcore mode would:

- Block hardcore while any cvar under `CVAR_PREFIX_CHEAT` is set. Anything under `CVAR_PREFIX_ENHANCEMENT` or `CVAR_PREFIX_RANDOMIZER_*` is blocked unless it's on an allow-list (`soh/soh/cvar_prefixes.h`).
- Start from a blank list of allowed enhancements. Add only ones that don't change gameplay, like UI, controls remapping and graphics options.
- Turn off save states (`soh/soh/Enhancements/savestates.cpp`), the debug console, actor viewer and other debugger windows, boot commands and the audio shuffler.
- Turn off Anchor and CrowdControl, since both change the game from outside.
- Require a game reset when switching into hardcore. rc_client handles the hardcore flag. SoH does the reset.
- Use the user agent `ShipOfHarkinian/<version> (<OS>) rcheevos/<version>`.

Softcore allows everything, matching how emulators behave.

### Phase 7: testing

Run SoH and an emulator side by side with the same inputs. For every achievement and leaderboard in set 10113, check that both unlock at the same moment. The rcheevos repo includes test helpers for checking logic against recorded memory.

For tiers 3 and 4, also dump the RAM translation layer's view of each address the set reads next to emulator RAM, and diff them over a play session. Any address that differs is a bug in the translation layer.

## Open questions

- Will RA accept a hash derived from SoH's version detection, or require the player's real ROM hash?
- Will RA accept a client that answers memory reads by translation instead of emulation? No doc covers this case.
- What happens when the set's author changes the logic? New addresses fall through to the "unmapped read" log. Someone has to watch for that.
- Master Quest has its own RA sets on GameCube hashes. SoH supports MQ ROMs, so it's a candidate for a later phase.

## Files in this folder

| File | Contents |
|---|---|
| `p10113.json`, `p17444.json` | Full achievement and leaderboard logic for the main set and the Glitch Showcase subset |
| `g10113.json`, `g17444.json` | Web API metadata (titles, unlock counts, badges) |
| `h10113.json` | ROM hashes RA accepts for set 10113 |
| `notes10113.json` | 159 RA code notes describing N64 RAM addresses |
| `analyze.py` | Parser that sorts every memory read by region |
| `analysis_10113.txt`, `analysis_17444.txt` | Parser output |
| `oot-ntsc-1.0.map` | Linker map from the zeldaret/oot NTSC 1.0 build. Exact RAM addresses for every named symbol |
| `poc-test/` | Offline test against the live set (`build_and_run.sh <creds>`) |

## Sources

- RA standalone support policy: https://docs.retroachievements.org/general/standalone-support.html
- RA hardcore compliance requirements: https://docs.retroachievements.org/general/hardcore-compliance-requirements.html
- rcheevos: https://github.com/RetroAchievements/rcheevos
- rc_client integration guide: https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration
- RAIntegration: https://github.com/RetroAchievements/RAIntegration
- Shipwright issue #5069: https://github.com/HarbourMasters/Shipwright/issues/5069
- RA forum, SoH support thread: https://retroachievements.org/forums/topic/24586
