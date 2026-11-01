#include "replay_hud.h"
#include <cstring>

int main() {
    struct Case { uint8_t pause, menu; const char* expected; };
    const Case cases[] = {
        {0x39, 0xD2, "Press Space to resume"},
        {0x19, 0xD2, "Press P to resume"},
        {0xCD, 0xD2, "Press Right to resume"},
        {0, 0xD2, "Press Insert > Keys > Resume"},
        {0, 0, "Replay pause unbound - configure in launcher"},
    };
    int failed = 0;
    for (const auto& test : cases) {
        char hint[96]{};
        FormatReplayResumeHint(hint, sizeof(hint), test.pause, test.menu);
        if (std::strcmp(hint, test.expected) != 0) {
            std::printf("FAIL expected '%s', got '%s'\n", test.expected, hint);
            ++failed;
        }
    }
    std::printf("Replay HUD: %d failed\n", failed);
    return failed == 0 ? 0 : 1;
}
