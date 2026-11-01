#include "qoh99/AudioSystem.hpp"

namespace qoh99 {

static_assert(AudioSystem::kSfxInitVa == 0x45F3E0, "DirectSoundCreate site");
static_assert(AudioSystem::kStreamThreadVa == 0x460090, "stream thread");
static_assert(AudioSystem::kBgmSetVa == 0x46FBC0, "high-level BGM command seam");
static_assert(AudioSystem::kSeMidiOpenVa == 0x474DD0,
              "QOH99 SE MIDI helper, not GAME.EXE/AVI address");

}
