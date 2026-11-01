#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class TimingSystem {
public:
    explicit TimingSystem(MemoryView& m) : m_(m) {}

    static constexpr GameAddr kPacingVa   = fnaddr::kFramePacing;
    static constexpr GameAddr kMsgPumpVa  = fnaddr::kMessagePump;   // DO NOT HOOK

    struct WallClockRegion { GameAddr base; std::uint32_t size; };
    // These regions hold the timeGetTime pacing rings and must never be saved or restored.
    static const WallClockRegion* wallClockRegions(std::size_t& count) {
        static const WallClockRegion r[] = {
            { 0x4ABA2C, 0x40 },
            { 0x4ABD30, 0x40 },
            { 0x4ABDC4, 0x40 },
        };
        count = 3;
        return r;
    }

private:
    MemoryView& m_;
};

}
