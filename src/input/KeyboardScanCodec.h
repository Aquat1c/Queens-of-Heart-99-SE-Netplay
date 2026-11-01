#pragma once

#include <cstdint>

namespace qinput {

struct RawKeyboardScan {
    uint16_t makeCode = 0;
    bool e0 = false;
    bool e1 = false;
};

struct DikResolution {
    bool valid = false;
    uint8_t dik = 0;
};

DikResolution dikFromRawScan(const RawKeyboardScan& scan);

const char* dikDisplayLabel(uint8_t dik);

bool dikIsExtended(uint8_t dik);

}
