// Fighter object size is 0x5FC (1532); anything past +0x5FC is an adjacent object or pool, not a fighter field.
// The child command object is at fighter+164.
#pragma once
#include <cstdint>
#include <cstddef>

namespace qoh99 { namespace fld {

constexpr size_t kFighterSize     = 0x5FC;

constexpr size_t kFi_ChildPtr     = 164;     // Points to the command/input-history object.
constexpr size_t kFi_Facing       = 170;     // A value of 1 swaps LEFT(0x40000) and RIGHT(0x80000).
constexpr size_t kFi_PlayerIndex  = 172;     // Index into dword_4A37C4[], which selects the input source.
constexpr size_t kFi_CharId       = 174;
constexpr size_t kFi_MoveSel      = 0xAE;    // The byte at +174 gates moves (==25 or ==1) during match init.
constexpr size_t kFi_ControlLock  = 1400;    // Nonzero forces input recording to 0.
constexpr size_t kFi_PairedPtr    = 1436;

constexpr size_t kCmd_DirRing     = 8;
constexpr size_t kCmd_DirHold     = 136;
constexpr size_t kCmd_DirHead     = 264;
constexpr size_t kCmd_BtnRing     = 268;
constexpr size_t kCmd_BtnHold     = 332;
constexpr size_t kCmd_BtnHead     = 396;
constexpr size_t kCmd_MinSize     = 0x1B4;
constexpr size_t kCmd_SaveSize    = 0x200;

// Direction ring values: 0x10000/0x20000 are the vertical pair and 0x40000/0x80000 are the horizontal pair.

}}
