#include "qoh99/DirectPlaySystem.hpp"

namespace qoh99 {

void VanillaDirectPlayDispatcher::applyScalar(const vnet::ScalarMsg& msg) {
    switch (msg.type) {
        case vnet::MSG_SEQ:
            m_.set<std::uint32_t>(addr::kNetFrameSeq, msg.field1);
            break;
        case vnet::MSG_SEED:
            m_.set<std::uint32_t>(addr::kRngSeedGameplay, msg.field1);
            m_.set<std::uint32_t>(addr::kRngSeedCosmetic, msg.field1);
            break;
        case vnet::MSG_SIDE:
            m_.set<int>(addr::kNetLocalSide, static_cast<int>(msg.field1));
            break;
        case vnet::MSG_PARAM5:
            m_.set<std::uint32_t>(addr::kNetParam5, msg.field2);
            break;
        case vnet::MSG_READY:
            m_.set<int>(addr::kNetReadyFlag, 1);
            break;
        default:
            break;
    }
}

bool VanillaDirectPlayDispatcher::applyInput(const vnet::InputMsg& msg) {
    std::uint32_t count = ringCount();
    if (count >= kRingDepth)
        return false;
    m_.set<std::uint32_t>(addr::kNetInputRing + 4u * count, msg.packedInput);
    m_.set<std::int16_t>(addr::kNetSyncPosXRing + 2u * count, msg.syncPosX);
    m_.set<std::int16_t>(addr::kNetSyncPosYRing + 2u * count, msg.syncPosY);
    m_.set<std::uint32_t>(addr::kNetInputRingCount, count + 1);
    return true;
}

}
