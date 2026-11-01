#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class AudioSystem {
public:
    explicit AudioSystem(MemoryView& m) : m_(m) {}

    // OS handles: never save or restore these.
    GameAddr primaryBuffer() const {
        return m_.get<GameAddr>(addr::kDsoundBufferA);
    }
    GameAddr streamSecondaryBuffer() const {
        return m_.get<GameAddr>(addr::kDsoundBufferB);
    }
    GameAddr dsoundBufferA() const { return primaryBuffer(); }
    GameAddr dsoundBufferB() const { return streamSecondaryBuffer(); }

    static constexpr GameAddr kSfxShutdownVa  = 0x45F360;
    static constexpr GameAddr kSfxInitVa      = 0x45F3E0;
    static constexpr GameAddr kSfxPrimaryInitVa = 0x45F480;
    static constexpr GameAddr kSfxUnloadVa    = 0x45F6D0;
    static constexpr GameAddr kSfxLoadVa      = 0x45F760;
    static constexpr GameAddr kSfxDuplicatePlayVa = 0x45F9C0;
    static constexpr GameAddr kSfxBasePlayVa  = 0x45FB20;
    static constexpr GameAddr kSfxStopVa      = 0x45FB60;
    static constexpr GameAddr kSfxStatusVa    = 0x45FBB0;
    static constexpr GameAddr kStreamThreadVa = 0x460090;
    static constexpr GameAddr kStreamTeardownVa = 0x4601D0;
    static constexpr GameAddr kStreamCreateVa = 0x460300;
    static constexpr GameAddr kBgmSetVa       = 0x46FBC0;

    static constexpr GameAddr kDirectSoundPtrVa = 0x4AAAB4;
    static constexpr GameAddr kPrimaryBufferPtrVa = 0x4AAAE4;
    static constexpr GameAddr kDuplicateSampleMapVa = 0x4AAB00;
    static constexpr GameAddr kStreamThreadHandleVa = 0x4AAC00;
    static constexpr GameAddr kStreamThreadIdVa = 0x4AAC08;
    static constexpr GameAddr kDuplicateBufferTableVa = 0x4AAC0C;
    static constexpr GameAddr kBaseBufferTableVa = 0x4AAD14;
    static constexpr GameAddr kStatusScratchVa = 0x4AB114;
    static constexpr GameAddr kStreamBufferPtrVa = 0x4AB118;
    static constexpr GameAddr kStreamNotifyPtrVa = 0x4AB11C;

    // Hybrid/physical BGM state; do not snapshot native handles/cursors.
    static constexpr GameAddr kBgmCompletionVa = 0x4A7490;
    static constexpr GameAddr kBgmCurrentTrackVa = 0x4A7494;
    static constexpr GameAddr kBgmBackendVa = 0x4A7498;
    static constexpr GameAddr kBgmFlagsVa = 0x4AB790;
    static constexpr GameAddr kBgmWavePlayTypeVa = 0x4AB89C;
    static constexpr GameAddr kBgmMidiPathVa = 0x617900;
    static constexpr GameAddr kBgmSplitPhaseVa = 0x6248CC;

    static constexpr GameAddr kSeMovieMciOpenVa = 0x474B80;
    static constexpr GameAddr kSeMidiOpenVa   = 0x474DD0;
    static constexpr GameAddr kSeMidiPlayVa   = 0x474E40;
    static constexpr GameAddr kSeMidiSeekVa   = 0x474E90;
    static constexpr GameAddr kSeMidiCloseVa  = 0x474EE0;
    static constexpr GameAddr kSeMciAliasCloseVa = 0x474F30;
    static constexpr GameAddr kSeCdPlayVa     = 0x474FA0;
    static constexpr GameAddr kSeCdStopVa     = 0x475000;
    static constexpr GameAddr kSeCdCloseVa    = 0x475030;

    static constexpr GameAddr kLegacyMovieMciOpenVa = 0x474BA0;
    static constexpr GameAddr kLegacyMidiOpenVa  = 0x474DF0;
    static constexpr GameAddr kLegacyMidiPlayVa  = 0x474E60;
    static constexpr GameAddr kLegacyMidiSeekVa  = 0x474EB0;
    static constexpr GameAddr kLegacyMidiCloseVa = 0x474F00;
    static constexpr GameAddr kLegacyMciAliasCloseVa = 0x474F50;
    static constexpr GameAddr kLegacyCdPlayVa    = 0x474FC0;
    static constexpr GameAddr kLegacyCdStopVa    = 0x475020;
    static constexpr GameAddr kLegacyCdCloseVa   = 0x475050;

private:
    MemoryView& m_;
};

}
