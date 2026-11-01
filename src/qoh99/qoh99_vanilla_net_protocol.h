/* Every message begins with a dword `type`; sub_45D6B0 switches on it (0..7). */
#pragma once
#include <cstdint>
#include "qoh99_inputs.h"

namespace qoh99 { namespace vnet {

enum MsgType : uint32_t {
    MSG_INPUT  = 0,
    MSG_SEQ    = 1,
    MSG_SEED   = 2,
    MSG_SIDE   = 3,
    MSG_CHAT   = 4,
    MSG_PARAM5 = 5,
    MSG_CONFIG = 6,  // Type 6: match/char/stage config (see ConfigMsg), 4232 bytes.
    MSG_READY  = 7,
};

#pragma pack(push, 1)

// Type 0, wire size 12: field1 is the packed input word (qohPackGameplayInputForNet).
// The two sync words are the sender's fighter +136 and +140 words, stored by the receiver in word_66EE10[]/word_66EE18[] for desync detection.
struct InputMsg {
    uint32_t type;
    uint32_t packedInput;
    int16_t  syncPosX;
    int16_t  syncPosY;
};
static_assert(sizeof(InputMsg) == 12, "InputMsg must be 12 bytes on the wire");

// Types 1/2/3/5/7 share the 12-byte {type, field1, field2} shape; only some fields are used.
struct ScalarMsg {
    uint32_t type;
    uint32_t field1;
    uint32_t field2;
};

// type 6 - config. Receiver: *(cfgPtr+40)=field1, *(cfgPtr+48)=field2,
// copy 0x14 bytes -> unk_616750, copy 0x334 bytes (at wire off 3412) -> dword_617498.
struct ConfigHeader {
    uint32_t type;
    uint32_t cfg40;
    uint32_t cfg48;
    uint8_t  block20[20];
    // Wire offsets 32..3411 are opaque; block820[0x334] at wire offset 3412 is copied to dword_617498.
};
constexpr uint32_t kConfigWireSize   = 4232;
constexpr uint32_t kConfigBlockOff   = 3412;
constexpr uint32_t kConfigBlockSize  = 0x334;

#pragma pack(pop)

inline uint32_t buildInputWord(uint32_t logical, uint32_t frameSeq) {
    return qohPackGameplayInputForNet(logical, frameSeq);
}

}}
