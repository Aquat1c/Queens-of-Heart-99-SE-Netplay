#pragma once
#include <cstdint>
#include "AddressMap.hpp"
#include "InputSystem.hpp"

namespace qoh99 { namespace vnet {

enum MsgType : std::uint32_t {
    MSG_INPUT  = 0,
    MSG_SEQ    = 1,
    MSG_SEED   = 2,
    MSG_SIDE   = 3,
    MSG_CHAT   = 4,
    MSG_PARAM5 = 5,
    MSG_CONFIG = 6,
    MSG_READY  = 7,
    MSG_TYPE_COUNT = 8
};

#pragma pack(push, 1)

// Type 0 is 12 bytes on the wire. The pos-sync words are sender fighter +136/+140, stored by the receiver into word_66EE10/word_66EE18.
struct InputMsg {
    std::uint32_t type;
    std::uint32_t packedInput;
    std::int16_t  syncPosX;
    std::int16_t  syncPosY;
};
static_assert(sizeof(InputMsg) == 12, "wire size");

struct ScalarMsg {
    std::uint32_t type;
    std::uint32_t field1;
    std::uint32_t field2;
};
static_assert(sizeof(ScalarMsg) == 12, "wire size");

// Type 6 is 4232 bytes on the wire: block20 -> unk_616750, 0x334 bytes at wire offset 3412 -> dword_617498, bytes 32..3411 unmapped.
struct ConfigMsgHeader {
    std::uint32_t type;
    std::uint32_t cfg40;        // Stored to *(matchConfigPtr + 40): char/stage.
    std::uint32_t cfg48;        // Stored to *(matchConfigPtr + 48): rounds-to-win.
    std::uint8_t  block20[20];
};
static_assert(sizeof(ConfigMsgHeader) == 32, "header size");

constexpr std::uint32_t kConfigWireSize  = 4232;
constexpr std::uint32_t kConfigBlockOff  = 3412;
constexpr std::uint32_t kConfigBlockSize = 0x334;
static_assert(kConfigBlockOff + kConfigBlockSize == kConfigWireSize,
              "0x334 block is the wire tail");

#pragma pack(pop)

enum class MsgRollback { InputFeed, SeqBasis, SeedSync, SideAssign,
                         None, Unknown, BootConfig, BootGate };

inline MsgRollback rollbackRelevance(std::uint32_t type) {
    switch (type) {
        case MSG_INPUT:  return MsgRollback::InputFeed;
        case MSG_SEQ:    return MsgRollback::SeqBasis;
        case MSG_SEED:   return MsgRollback::SeedSync;
        case MSG_SIDE:   return MsgRollback::SideAssign;
        case MSG_CHAT:   return MsgRollback::None;
        case MSG_PARAM5: return MsgRollback::Unknown;
        case MSG_CONFIG: return MsgRollback::BootConfig;
        case MSG_READY:  return MsgRollback::BootGate;
        default:         return MsgRollback::Unknown;
    }
}

constexpr std::uint32_t qohPackGameplayInputForNet(std::uint32_t logical,
                                                   std::uint32_t sysWord,
                                                   std::uint32_t frameSeq) {
    return NetInputPacker::pack(logical, sysWord, frameSeq);
}
constexpr std::uint32_t qohUnpackGameplayInputFromNet(std::uint32_t packed) {
    return NetInputPacker::unpackLogical(packed);
}

inline InputMsg makeInputMsg(std::uint32_t logical, std::uint32_t sysWord,
                             std::uint32_t seq, std::int16_t posX, std::int16_t posY) {
    return { MSG_INPUT, qohPackGameplayInputForNet(logical, sysWord, seq), posX, posY };
}

}}
