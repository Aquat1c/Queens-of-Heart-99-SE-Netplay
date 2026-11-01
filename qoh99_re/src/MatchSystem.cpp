#include "qoh99/MatchSystem.hpp"

namespace qoh99 {

struct MatchSubHandler {
    int sub;
    GameAddr handlerVa;
    const char* name;
    Confidence conf;
};

static const MatchSubHandler kHandlers[] = {
    { -1, 0,        "roundIntro (inline: char load, then matchSub=0)", Confidence::RuntimeVerified },
    {  0, 0x421EE0, "qohGameplayTick",  Confidence::RuntimeVerified },
    {  1, 0x4047C0, "qohMatchSub1Tick (UNKNOWN phase)", Confidence::StrongLikely },
    {  2, 0x404F90, "qohMatchSub2Tick (UNKNOWN phase)", Confidence::StrongLikely },
    {  3, 0x4082F0, "qohMatchSub3Tick (UNKNOWN phase)", Confidence::StrongLikely },
    {  4, 0x4083D0, "qohMatchSub4Tick (UNKNOWN phase)", Confidence::StrongLikely },
    {  5, 0x408180, "qohRoundWinTick", Confidence::RuntimeVerified },
};

const MatchSubHandler* matchSubHandlers(std::size_t& count) {
    count = sizeof(kHandlers) / sizeof(kHandlers[0]);
    return kHandlers;
}

}
