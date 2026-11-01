#include "TestFramework.hpp"
#include "qoh99/DirectPlaySystem.hpp"
#include "qoh99/VanillaNetProtocol.hpp"

using namespace qoh99;
using namespace qoh99::vnet;

TEST(protocol_wire_sizes) {
    CHECK_EQ(sizeof(InputMsg), 12u);
    CHECK_EQ(sizeof(ScalarMsg), 12u);
    CHECK_EQ(sizeof(ConfigMsgHeader), 32u);
    CHECK_EQ(kConfigWireSize, 4232u);
    CHECK_EQ(kConfigBlockOff, 3412u);
    CHECK_EQ(kConfigBlockSize, 0x334u);
}

TEST(protocol_type0_layout) {
    InputMsg m = makeInputMsg(0xF000F, input_mask::kSysConfirm, 42, -12, 300);
    CHECK_EQ(m.type, (std::uint32_t)MSG_INPUT);
    CHECK_EQ(NetInputPacker::unpackLogical(m.packedInput), 0xF000Fu);
    CHECK_EQ(NetInputPacker::unpackSeq(m.packedInput), 42u);
    CHECK_EQ(NetInputPacker::unpackSys(m.packedInput), 1u);
    CHECK_EQ(m.syncPosX, -12);
    CHECK_EQ(m.syncPosY, 300);
}

TEST(protocol_scalar_dispatch_effects) {
    BufferMemoryView mem;
    VanillaDirectPlayDispatcher d(mem);

    d.applyScalar({ MSG_SEED, 0xDEADBEEF, 0 });
    CHECK_EQ(mem.get<std::uint32_t>(addr::kRngSeedGameplay), 0xDEADBEEFu);
    CHECK_EQ(mem.get<std::uint32_t>(addr::kRngSeedCosmetic), 0xDEADBEEFu);

    d.applyScalar({ MSG_SIDE, 2, 0 });
    CHECK_EQ(d.localSide(), 2);

    d.applyScalar({ MSG_SEQ, 77, 0 });
    CHECK_EQ(d.frameSeq(), 77u);

    d.applyScalar({ MSG_READY, 0, 0 });
    CHECK(d.ready());

    d.applyScalar({ MSG_PARAM5, 0, 5 });
    CHECK_EQ(mem.get<std::uint32_t>(addr::kNetParam5), 5u);
}

TEST(protocol_input_enqueue_parallel_rings_without_seq_filter) {
    BufferMemoryView mem;
    VanillaDirectPlayDispatcher d(mem);
    mem.set<std::uint32_t>(addr::kNetFrameSeq, 100);

    InputMsg first = makeInputMsg(1, 0, 99, -12, 300);
    InputMsg second = makeInputMsg(2, 0, 101, 40, -50);
    InputMsg third = makeInputMsg(3, 0, 102, 60, 70);
    CHECK(d.applyInput(first));
    CHECK(d.applyInput(second));
    CHECK(d.applyInput(third));
    CHECK_EQ(d.ringCount(), 3u);
    CHECK_EQ(d.ringSlot(0), first.packedInput);
    CHECK_EQ(d.ringSlot(1), second.packedInput);
    CHECK_EQ(d.ringSlot(2), third.packedInput);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosXRing + 0), -12);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosXRing + 2), 40);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosXRing + 4), 60);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosYRing + 0), 300);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosYRing + 2), -50);
    CHECK_EQ(mem.get<std::int16_t>(addr::kNetSyncPosYRing + 4), 70);
    CHECK(!d.applyInput(first));
}

TEST(protocol_rollback_relevance_covers_all_types) {
    for (std::uint32_t t = 0; t < MSG_TYPE_COUNT; ++t) {
        MsgRollback r = rollbackRelevance(t);
        (void)r;
        if (t != MSG_PARAM5)
            CHECK(rollbackRelevance(t) != MsgRollback::Unknown);
    }
    CHECK(rollbackRelevance(MSG_INPUT) == MsgRollback::InputFeed);
    CHECK(rollbackRelevance(MSG_SEED) == MsgRollback::SeedSync);
    CHECK(rollbackRelevance(MSG_CONFIG) == MsgRollback::BootConfig);
}

TEST(protocol_host_boot_gate) {
    BufferMemoryView mem;
    DirectPlaySystem dp(mem);
    mem.set<int>(addr::kNetIsNetworkSession, 1);
    mem.set<int>(addr::kNetClientGotFlag, 1);
    mem.set<int>(addr::kNetReadyFlag, 0);
    CHECK(dp.hostShouldSendBoot());
    mem.set<int>(addr::kNetReadyFlag, 1);
    CHECK(!dp.hostShouldSendBoot());
}
