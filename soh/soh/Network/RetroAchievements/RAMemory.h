#pragma once

#ifdef __cplusplus

#include <cstdint>

// Presents SoH's live game state to rcheevos as if it were the RDRAM of an
// NTSC-U 1.0 N64 running Ocarina of Time.
//
// Achievement logic on RetroAchievements is written against N64 RAM addresses.
// SoH has no N64 RAM, so once per frame RAMemory copies the fields that the
// achievement set reads into an 8 MB buffer at their NTSC-U 1.0 addresses,
// in N64 (big-endian) byte order.
//
// RetroAchievements reads N64 RAM as little-endian 32-bit words, the way
// emulators store it, so RA address A holds N64 byte A ^ 3. The buffer is kept
// in that order, which makes a read a plain copy.
namespace RAMemory {

// Copies SoH state into the virtual RAM. Call once per game frame, before
// rc_client_do_frame.
void Refresh();

// rc_client_read_memory_func_t body. Unmapped addresses read as zero and are
// logged once each.
uint32_t Read(uint32_t address, uint8_t* buffer, uint32_t numBytes);

// Number of distinct unmapped addresses the achievement set has read so far.
uint32_t UnmappedReadCount();

} // namespace RAMemory

#endif
