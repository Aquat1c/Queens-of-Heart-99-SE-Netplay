#pragma once
#include "AddressMap.hpp"
#include "GameplayRelevance.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class HudSystem {
public:
    explicit HudSystem(MemoryView& m) : m_(m) {}

    struct FeederRegion {
        const char* name;
        GameAddr base;
        std::uint32_t size;
        GameplayRelevance relevance;
    };
    static const FeederRegion* feederRegions(std::size_t& count) {
        static const FeederRegion r[] = {
            { "fps_counter_digit", 0x49B520, 0x10, GameplayRelevance::RenderCosmetic },
            { "camera_zoom_visual", 0x49B530, 0x8, GameplayRelevance::RenderCosmetic },
            { "round_intro_control", 0x49B538, 0xC, GameplayRelevance::DeterministicGameplay },
            { "finish_banner_descriptor", 0x49B548, 0x18, GameplayRelevance::RenderCosmetic },
            { "hud_display_head", 0x87D000, 0x37C4, GameplayRelevance::RenderCosmetic },
            { "hud_display_mid",  0x8807C8, 0x1C4,  GameplayRelevance::RenderCosmetic },
            { "hud_display_tail", 0x880990, 0x670,  GameplayRelevance::RenderCosmetic },
            { "hud_counters", 0x8A5C00, 0x200,  GameplayRelevance::DeterministicGameplay },
        };
        count = 8;
        return r;
    }

    int timerDigits() const { return m_.get<int>(addr::kHudTimerDigits); }
    int comboDigits() const { return m_.get<int>(addr::kHudComboDigits); }

private:
    MemoryView& m_;
};

}
