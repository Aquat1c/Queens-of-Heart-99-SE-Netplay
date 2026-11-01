#include "ReplayIntegrity.h"

namespace qreplay {
namespace {

struct Crc32cTable {
    uint32_t entry[256];

    Crc32cTable() {
        constexpr uint32_t kReflectedPoly = 0x82F63B78u;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t crc = i;
            for (unsigned bit = 0; bit < 8u; ++bit)
                crc = (crc >> 1) ^ (kReflectedPoly & (0u - (crc & 1u)));
            entry[i] = crc;
        }
    }
};

const Crc32cTable& crcTable() {
    static const Crc32cTable table;
    return table;
}

uint32_t crcAdvance(uint32_t state, const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0)
        return state;
    const Crc32cTable& table = crcTable();
    for (size_t i = 0; i < len; ++i)
        state = table.entry[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    return state;
}

const uint32_t kSha256Constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

uint32_t rotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

void writeBe32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

constexpr uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

constexpr uint8_t kFrameDomainTag = 0x01u;
constexpr uint8_t kEventDomainTag = 0x02u;

void mix8(uint64_t& hash, uint8_t value) {
    hash ^= static_cast<uint64_t>(value);
    hash *= kFnvPrime;
}

void mix16(uint64_t& hash, uint16_t value) {
    for (unsigned i = 0; i < 2u; ++i)
        mix8(hash, static_cast<uint8_t>(value >> (i * 8u)));
}

void mix32(uint64_t& hash, uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i)
        mix8(hash, static_cast<uint8_t>(value >> (i * 8u)));
}

void mix64(uint64_t& hash, uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i)
        mix8(hash, static_cast<uint8_t>(value >> (i * 8u)));
}

uint32_t asUnsigned32(int32_t value) {
    return value >= 0
        ? static_cast<uint32_t>(value)
        : 0xFFFFFFFFu - static_cast<uint32_t>(-(value + 1));
}

void mixLifecycle(uint64_t& hash, const LifecycleTag& tag) {
    mix32(hash, tag.scene);
    mix32(hash, tag.requestedScene);
    mix32(hash, tag.sceneThreeState);
    mix32(hash, tag.matchState);
    mix32(hash, tag.roundState);
    mix32(hash, tag.layoutGeneration);
}

uint64_t avalanche(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

}

uint32_t crc32c(const uint8_t* data, size_t len, uint32_t seed) {
    return ~crcAdvance(~seed, data, len);
}

void Crc32c::reset() { state_ = 0xFFFFFFFFu; }

void Crc32c::update(const uint8_t* data, size_t len) {
    state_ = crcAdvance(state_, data, len);
}

uint32_t Crc32c::value() const { return ~state_; }

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    state_[0] = 0x6A09E667u;
    state_[1] = 0xBB67AE85u;
    state_[2] = 0x3C6EF372u;
    state_[3] = 0xA54FF53Au;
    state_[4] = 0x510E527Fu;
    state_[5] = 0x9B05688Cu;
    state_[6] = 0x1F83D9ABu;
    state_[7] = 0x5BE0CD19u;
    for (size_t i = 0; i < sizeof(buffer_); ++i)
        buffer_[i] = 0u;
    buffered_ = 0;
    totalBytes_ = 0;
    finished_ = false;
    digest_.fill(0u);
}

void Sha256::compress(const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16u; ++i)
        w[i] = readBe32(block + i * 4u);
    for (unsigned i = 16u; i < 64u; ++i) {
        const uint32_t s0 =
            rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 =
            rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (unsigned i = 0; i < 64u; ++i) {
        const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kSha256Constants[i] + w[i];
        const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (finished_ || data == nullptr || len == 0)
        return;
    totalBytes_ += static_cast<uint64_t>(len);
    size_t offset = 0;
    if (buffered_ != 0) {
        while (offset < len && buffered_ < 64u)
            buffer_[buffered_++] = data[offset++];
        if (buffered_ < 64u)
            return;
        compress(buffer_);
        buffered_ = 0;
    }
    while (len - offset >= 64u) {
        compress(data + offset);
        offset += 64u;
    }
    while (offset < len)
        buffer_[buffered_++] = data[offset++];
}

void Sha256::finish(std::array<uint8_t, 32>& out) {
    if (!finished_) {
        const uint64_t bitLength = totalBytes_ * 8ull;
        uint8_t padding[72];
        for (size_t i = 0; i < sizeof(padding); ++i)
            padding[i] = 0u;
        padding[0] = 0x80u;
        const size_t remainder = static_cast<size_t>(totalBytes_ % 64ull);
        const size_t padBytes = remainder < 56u
            ? (56u - remainder)
            : (120u - remainder);
        for (unsigned i = 0; i < 8u; ++i) {
            padding[padBytes + i] =
                static_cast<uint8_t>(bitLength >> ((7u - i) * 8u));
        }
        const uint64_t savedTotal = totalBytes_;
        update(padding, padBytes + 8u);
        totalBytes_ = savedTotal;
        for (unsigned i = 0; i < 8u; ++i)
            writeBe32(digest_.data() + i * 4u, state_[i]);
        finished_ = true;
    }
    out = digest_;
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
    Sha256 hasher;
    hasher.update(data, len);
    std::array<uint8_t, 32> digest{};
    hasher.finish(digest);
    return digest;
}

std::string toHex(const std::array<uint8_t, 32>& digest) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    text.resize(digest.size() * 2u);
    for (size_t i = 0; i < digest.size(); ++i) {
        text[i * 2u] = kDigits[(digest[i] >> 4) & 0x0Fu];
        text[i * 2u + 1u] = kDigits[digest[i] & 0x0Fu];
    }
    return text;
}

bool fromHex(const std::string& text, std::array<uint8_t, 32>& out) {
    if (text.size() != 64u)
        return false;
    std::array<uint8_t, 32> decoded{};
    for (size_t i = 0; i < decoded.size(); ++i) {
        uint8_t value = 0u;
        for (unsigned nibble = 0; nibble < 2u; ++nibble) {
            const char ch = text[i * 2u + nibble];
            uint8_t digit = 0u;
            if (ch >= '0' && ch <= '9')
                digit = static_cast<uint8_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                digit = static_cast<uint8_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                digit = static_cast<uint8_t>(ch - 'A' + 10);
            else
                return false;
            value = static_cast<uint8_t>((value << 4) | digit);
        }
        decoded[i] = value;
    }
    out = decoded;
    return true;
}

uint64_t advanceReplayDigest(uint64_t previous, const ReplayFrameV1& frame) {
    uint64_t hash = kFnvOffsetBasis;
    mix64(hash, previous);
    mix8(hash, kFrameDomainTag);
    mix32(hash, frame.timelineFrame);
    mix8(hash, static_cast<uint8_t>(frame.inputDomain));
    mix8(hash, frame.activeLaneMask);
    for (size_t lane = 0; lane < kSourceLanes; ++lane)
        mix16(hash, frame.actions[lane]);
    mix32(hash, frame.routeEpoch);
    mix64(hash, frame.preStateHash);
    mix64(hash, frame.postStateHash);
    mixLifecycle(hash, frame.lifecycleBefore);
    mixLifecycle(hash, frame.lifecycleAfter);
    mix32(hash, asUnsigned32(frame.dispatcherResult));
    return avalanche(hash);
}

uint64_t advanceReplayDigest(uint64_t previous, const ReplayEvent& event) {
    uint64_t hash = kFnvOffsetBasis;
    mix64(hash, previous);
    mix8(hash, kEventDomainTag);
    mix32(hash, event.timelineFrame);
    mix16(hash, static_cast<uint16_t>(event.kind));
    mix32(hash, event.matchId);
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        mix8(hash, event.roster[slot].nativeCharacterId);
        mix8(hash, event.roster[slot].paletteIndex);
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        mix8(hash, event.selectorProof[slot].cursorCell);
        mix8(hash, event.selectorProof[slot].alternateFlag);
        mix8(hash, event.selectorProof[slot].confirmState);
    }
    mix8(hash, event.selectorProofMask);
    mix32(hash, event.rosterEpoch);
    mix64(hash, event.valueA);
    mix64(hash, event.valueB);
    mix32(hash, event.valueC);
    mix16(hash, static_cast<uint16_t>(event.terminalReason));
    mix64(hash, event.stateProof);
    return avalanche(hash);
}

namespace {

struct FnvSinkV2 {
    uint64_t& hash;
    void u8(uint8_t value) { mix8(hash, value); }
    void u16(uint16_t value) { mix16(hash, value); }
    void u32(uint32_t value) { mix32(hash, value); }
    void u64(uint64_t value) { mix64(hash, value); }
    void i32(int32_t value) { mix32(hash, asUnsigned32(value)); }
    void bytes(const uint8_t* data, size_t count) {
        for (size_t i = 0u; i < count; ++i) mix8(hash, data[i]);
    }
};

struct ShaSinkV2 {
    Sha256& sha;

    void u8(uint8_t value) { sha.update(&value, 1u); }
    void u16(uint16_t value) {
        uint8_t bytes[2];
        for (unsigned i = 0u; i < 2u; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
        sha.update(bytes, sizeof(bytes));
    }
    void u32(uint32_t value) {
        uint8_t bytes[4];
        for (unsigned i = 0u; i < 4u; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
        sha.update(bytes, sizeof(bytes));
    }
    void u64(uint64_t value) {
        uint8_t bytes[8];
        for (unsigned i = 0u; i < 8u; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
        sha.update(bytes, sizeof(bytes));
    }
    void i32(int32_t value) { u32(asUnsigned32(value)); }
    void bytes(const uint8_t* data, size_t count) {
        sha.update(data, count);
    }
};

template <typename Sink>
void feedSimulationContractV2(
    Sink& sink, const SimulationContractV2& contract) {
    sink.bytes(
        contract.executableSha256.data(),
        contract.executableSha256.size());
    sink.u64(contract.nativeBuildIdentity);
    sink.u64(contract.runtimeSimulationId);
    sink.u64(contract.stateRegionSchemaFingerprint);
    sink.u64(contract.stateHashProjectionId);
    sink.u64(contract.stateHashAlgorithmId);
    sink.bytes(
        contract.contentManifestRoot.data(),
        contract.contentManifestRoot.size());
    sink.u64(contract.bootstrapSchemaId);
    sink.u64(contract.eventSchemaId);
    sink.u64(contract.characterCatalogId);
    sink.u64(contract.terminalResultSchemaId);
    sink.u32(contract.compactActionDomainVersion);
}

template <typename Sink>
void feedCanonicalManifestV2(Sink& sink, const GameManifestV2& manifest) {
    feedSimulationContractV2(sink, manifest.contract);
    sink.u8(static_cast<uint8_t>(manifest.bootstrapKind));
    sink.u8(manifest.vsRow);
    sink.u8(manifest.activeLaneMask);
    sink.u8(manifest.activeFighterMask);
    sink.u8(manifest.cpuMask);
    sink.u8(static_cast<uint8_t>(manifest.cpuContract));
    for (uint8_t route : manifest.slotToSourceRoute) sink.u8(route);
    for (const SourceLaneDescriptorV2& lane : manifest.lanes)
        sink.u8(lane.active ? 1u : 0u);
    for (const FighterSlotDescriptorV2& slot : manifest.slots) {
        sink.u8(static_cast<uint8_t>(slot.ownership));
        sink.u8(slot.side);
        sink.u8(slot.identity.nativeCharacterId);
        sink.u8(slot.identity.paletteIndex);
    }
    sink.u32(manifest.routeEpoch);
    sink.u32(manifest.rosterEpoch);
    sink.u32(manifest.layoutGeneration);
    sink.u32(manifest.stage.nativeStageId);
    sink.u8(manifest.stage.verified ? 1u : 0u);
    sink.u64(manifest.stage.catalogEntryId);
}

template <typename Sink>
void feedDeterministicStartV2(
    Sink& sink, const DeterministicGameStartV2& start,
    bool includeInitialRollingDigest) {
    sink.u32(start.configuration.timerMode);
    sink.u32(start.configuration.aiDifficulty);
    sink.u32(start.configuration.lifeMultiplier);
    sink.u32(start.configuration.roundsToWinMinusOne);
    sink.u32(start.rng.gameplay);
    sink.u32(start.rng.cosmetic);
    sink.u32(start.rng.selector);
    sink.u16(start.fpu.x87ControlWord);
    sink.u32(start.fpu.mxcsr);
    for (uint32_t codePage : start.nativeDisplay.codePages)
        sink.u32(codePage);
    for (const auto& name : start.nativeDisplay.names) {
        sink.bytes(
            reinterpret_cast<const uint8_t*>(name.data()), name.size());
    }
    sink.u32(start.initialPhase);
    sink.u64(start.bootstrapFingerprint);
    sink.u64(start.frameZeroPreStateHash);
    if (includeInitialRollingDigest)
        sink.u64(start.initialRollingDigest);
}

template <typename Sink>
void feedLifecycleV2(Sink& sink, const GameLifecycleV2& lifecycle) {
    sink.u32(lifecycle.scene);
    sink.u32(lifecycle.requestedScene);
    sink.i32(lifecycle.matchSub);
    sink.u32(lifecycle.sceneThreeState);
    sink.u32(lifecycle.roundState);
    sink.u32(lifecycle.roundCounter);
    sink.u32(lifecycle.resultCounter);
    sink.i32(lifecycle.winnerSlot);
    sink.u8(lifecycle.roundTallies[0]);
    sink.u8(lifecycle.roundTallies[1]);
    sink.u32(lifecycle.routeEpoch);
    sink.u32(lifecycle.rosterEpoch);
    sink.u32(lifecycle.layoutGeneration);
}

template <typename Sink>
void feedFrameV2(Sink& sink, const ConfirmedGameFrameV2& frame) {
    sink.u32(frame.replayFrame);
    sink.u8(static_cast<uint8_t>(frame.core.inputDomain));
    sink.u8(frame.core.activeLaneMask);
    for (uint16_t action : frame.core.actions) sink.u16(action);
    feedLifecycleV2(sink, frame.core.before);
    feedLifecycleV2(sink, frame.core.after);
    sink.i32(frame.core.dispatcherResult);
    sink.u64(frame.core.preStateHash);
    sink.u64(frame.core.postStateHash);
}

template <typename Sink>
void feedEventV2(Sink& sink, const ConfirmedGameEventV2& event) {
    sink.u32(event.replayFrame);
    sink.u32(event.eventSequence);
    sink.u16(static_cast<uint16_t>(event.kind));
    sink.u8(static_cast<uint8_t>(event.phase));
    sink.u32(event.transitionSiteId);
    sink.u8(event.roundOrdinal);
    sink.u8(event.winningSideMask);
    sink.u8(event.roundTallies[0]);
    sink.u8(event.roundTallies[1]);
    sink.i32(event.winnerSlot);
    sink.u16(event.playerOneDelay);
    sink.u16(event.playerTwoDelay);
    sink.u16(event.rollbackWindow);
    sink.u32(event.averageRttMicros);
    sink.u32(event.jitterMicros);
    sink.u32(event.lossPermille);
    sink.u64(event.stateProof);
}

template <typename Sink>
void feedEndV2(Sink& sink, const ConfirmedGameEndV2& end) {
    sink.u16(static_cast<uint16_t>(end.reason));
    sink.u32(end.nextRecord);
    sink.u8(end.winningSideMask);
    sink.u8(end.roundTallies[0]);
    sink.u8(end.roundTallies[1]);
    sink.i32(end.winnerSlot);
    sink.u64(end.postTailStateHash);
    sink.u64(end.finalRollingDigest);
}

void feedDomain(Sha256& sha, const char* domain) {
    size_t length = 0u;
    while (domain[length] != '\0') ++length;
    sha.update(reinterpret_cast<const uint8_t*>(domain), length);
    const uint8_t terminator = 0u;
    sha.update(&terminator, 1u);
}

bool digestIsZero(const std::array<uint8_t, 32>& digest) {
    for (uint8_t byte : digest)
        if (byte != 0u) return false;
    return true;
}

}

uint64_t initialReplayDigestV2(const ConfirmedGameStartV2& start) {
    uint64_t hash = kFnvOffsetBasis;
    FnvSinkV2 sink{hash};
    static const uint8_t kDomain[] = {
        'Q','9','9','R','-','T','I','M','E','L','I','N','E','-','V','2'
    };
    sink.bytes(kDomain, sizeof(kDomain));
    sink.u8(0x20u);
    feedCanonicalManifestV2(sink, start.manifest);
    feedDeterministicStartV2(sink, start.start, false);
    return avalanche(hash);
}

uint64_t advanceReplayDigestV2(
    uint64_t previous, const ConfirmedGameFrameV2& frame) {
    uint64_t hash = kFnvOffsetBasis;
    FnvSinkV2 sink{hash};
    sink.u64(previous);
    sink.u8(0x21u);
    feedFrameV2(sink, frame);
    return avalanche(hash);
}

uint64_t advanceReplayDigestV2(
    uint64_t previous, const ConfirmedGameEventV2& event) {
    if (!isCanonicalGameEventV2(event.kind)) return previous;
    uint64_t hash = kFnvOffsetBasis;
    FnvSinkV2 sink{hash};
    sink.u64(previous);
    sink.u8(0x22u);
    feedEventV2(sink, event);
    return avalanche(hash);
}

bool CanonicalGameDigestV2::begin(const ConfirmedGameStartV2& start) {
    if (started_ || !isValidGameManifestV2(start.manifest)
        || !isValidDeterministicGameStartV2(start.start)) {
        return false;
    }
    const uint64_t genesis = initialReplayDigestV2(start);
    if (start.start.initialRollingDigest != genesis) return false;
    sha_.reset();
    feedDomain(sha_, "Q99R-CANONICAL-GAME-V2");
    ShaSinkV2 sink{sha_};
    sink.u8(0x20u);
    feedCanonicalManifestV2(sink, start.manifest);
    feedDeterministicStartV2(sink, start.start, true);
    started_ = true;
    finished_ = false;
    nextRecord_ = 0u;
    rollingDigest_ = genesis;
    return true;
}

bool CanonicalGameDigestV2::append(const ConfirmedGameEventV2& event) {
    if (!started_ || finished_ || !isValidConfirmedGameEventV2(event))
        return false;
    const uint64_t expected = advanceReplayDigestV2(rollingDigest_, event);
    if (event.rollingDigest != expected) return false;
    if (isCanonicalGameEventV2(event.kind)) {
        ShaSinkV2 sink{sha_};
        sink.u8(0x22u);
        feedEventV2(sink, event);
    }
    rollingDigest_ = expected;
    return true;
}

bool CanonicalGameDigestV2::append(const ConfirmedGameFrameV2& frame) {
    if (!started_ || finished_ || frame.replayFrame != nextRecord_
        || !isValidConfirmedGameFrameCoreV2(
            frame.core, frame.core.activeLaneMask)) {
        return false;
    }
    const uint64_t expected = advanceReplayDigestV2(rollingDigest_, frame);
    if (frame.rollingDigest != expected) return false;
    ShaSinkV2 sink{sha_};
    sink.u8(0x21u);
    feedFrameV2(sink, frame);
    rollingDigest_ = expected;
    ++nextRecord_;
    return true;
}

bool CanonicalGameDigestV2::finish(
    const ConfirmedGameEndV2& end, std::array<uint8_t, 32>& out) {
    if (!started_ || finished_ || !isValidConfirmedGameEndV2(end)
        || end.nextRecord != nextRecord_
        || end.finalRollingDigest != rollingDigest_) {
        return false;
    }
    ShaSinkV2 sink{sha_};
    sink.u8(0x23u);
    feedEndV2(sink, end);
    std::array<uint8_t, 32> digest{};
    sha_.finish(digest);
    if (!digestIsZero(end.canonicalGameDigest)
        && digest != end.canonicalGameDigest) {
        return false;
    }
    out = digest;
    finished_ = true;
    return true;
}

}
