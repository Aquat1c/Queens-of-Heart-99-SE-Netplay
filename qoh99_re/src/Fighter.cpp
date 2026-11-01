#include "qoh99/Fighter.hpp"
#include "qoh99/Confidence.hpp"

namespace qoh99 {

struct FighterFieldInfo {
    const char* name;
    std::uint32_t offset;
    std::uint32_t size;
    Confidence conf;
};

static const FighterFieldInfo kFields[] = {
    { "actionState",    fighter_field::kActionState,    4, Confidence::StrongLikely },
    { "actionFrame",    fighter_field::kActionFrame,    4, Confidence::StrongLikely },
    { "actionLockEnd",  fighter_field::kActionLockEnd,  4, Confidence::RuntimeVerified },
    { "animFrame",      fighter_field::kAnimFrame,      4, Confidence::StrongLikely },
    { "nextAction",     fighter_field::kNextAction,     4, Confidence::StrongLikely },
    { "transitionFlag", fighter_field::kTransitionFlag, 4, Confidence::StrongLikely },
    { "prevAction",     fighter_field::kPrevAction,     4, Confidence::StrongLikely },
    { "dispatchAction", fighter_field::kDispatchAction, 4, Confidence::StrongLikely },
    { "initStatus",     fighter_field::kInitStatus,     4, Confidence::StrongLikely },
    { "posXSyncWord",   fighter_field::kPosXSyncWord,   2, Confidence::StaticAnalysis },
    { "posYSyncWord",   fighter_field::kPosYSyncWord,   2, Confidence::StaticAnalysis },
    { "moveGateByte",   fighter_field::kMoveGateByte,   1, Confidence::StaticAnalysis },
    { "childCmdPtr",    fighter_field::kChildCmdPtr,    4, Confidence::StaticAnalysis },
    { "facing",         fighter_field::kFacing,         1, Confidence::StaticAnalysis },
    { "exGaugeOwner",   fighter_field::kExGaugeOwner,   1, Confidence::StaticAnalysis },
    { "playerIndex",    fighter_field::kPlayerIndex,    1, Confidence::StaticAnalysis },
    { "charOrType",     fighter_field::kCharOrType,     1, Confidence::StrongLikely },
    { "comOwnership",   fighter_field::kComOwnership,   4, Confidence::StaticAnalysis },
    { "pairedPtr",      fighter_field::kPairedPtr,      4, Confidence::StrongLikely },
};

const FighterFieldInfo* fighterFields(std::size_t& count) {
    count = sizeof(kFields) / sizeof(kFields[0]);
    return kFields;
}

}
