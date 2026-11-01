#pragma once

#include "../../spectator/SpectatorConnectedArchiveV4.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qclient::spectator {

enum class SpectatorReplaySaveState : uint8_t {
    Disabled,
    WaitingForPerGameStart,
    BackfillingRetainedRange,
    CapturingLiveTail,
    Finalizing,
    FailedCurrentGame,
};

enum class SpectatorReplaySaveFailureV4 : uint8_t {
    None,
    UserDisabled,
    MissingPrefix,
    StreamFailed,
    RangeConflict,
    DigestMismatch,
    WorkerLiability,
    SinkBeginFailed,
    SinkAppendFailed,
    SinkFinalizeFailed,
    SetSealedBeforeGameEnd,
};

enum class SpectatorReplaySaveOutcomeKindV4 : uint8_t {
    Saved,
    IncompleteNoSave,
    Failed,
};

struct SpectatorReplaySaveOutcomeV4 {
    qreplay::GameIdentityV2 game{};
    SpectatorReplaySaveOutcomeKindV4 kind =
        SpectatorReplaySaveOutcomeKindV4::IncompleteNoSave;
    SpectatorReplaySaveFailureV4 failure =
        SpectatorReplaySaveFailureV4::None;
    uint32_t recordCount = 0u;
    std::array<uint8_t, 32> canonicalGameDigest{};
};

class SpectatorReplaySaveSinkV4 {
public:
    virtual ~SpectatorReplaySaveSinkV4() = default;
    virtual bool begin(
        const qreplay::ConfirmedGameStartV2& start) = 0;
    virtual bool append(
        const qreplay::CanonicalTickBundleV2& tick) = 0;
    virtual bool palette(const qreplay::ReplayPaletteV2& palette) {
        (void)palette;
        return false;
    }
    virtual bool finalize(
        const qreplay::ConfirmedGameEndV2& end) = 0;
    virtual void abort(SpectatorReplaySaveFailureV4 reason) = 0;
};

struct SpectatorReplaySaveConfigV4 {
    uint32_t maxPendingItems =
        qnetplay::spectator::kMaxConnectedUnackedItemsV4;
    uint64_t maxPendingBytes =
        qnetplay::spectator::kMaxConnectedUnackedBytesV4;
    size_t maxOutcomes = 4096u;
};

class SpectatorReplaySaveAdapterV4 {
public:
    SpectatorReplaySaveAdapterV4(
        SpectatorReplaySaveSinkV4* sink,
        const SpectatorReplaySaveConfigV4& config = {});

    bool attach(
        const qnetplay::spectator::SpectatorConnectedArchiveV4* archive);
    void setEnabled(bool enabled);

    size_t pump(size_t maxPublications = 64u);
    void notifyStreamFailure();

    SpectatorReplaySaveState state() const { return state_; }
    bool enabled() const { return enabled_; }
    uint32_t readItemSequence() const { return readItemSequence_; }
    const std::vector<SpectatorReplaySaveOutcomeV4>& outcomes() const {
        return outcomes_;
    }

private:
    struct Candidate {
        bool active = false;
        qreplay::GameIdentityV2 game{};
        uint32_t startRecordCursor = 0u;
        uint32_t nextRecordCursor = 0u;
        uint32_t nextEventSequence = 0u;
        uint64_t rollingDigest = 0u;
        qreplay::CanonicalGameDigestV2 canonical{};
    };

    SpectatorReplaySaveSinkV4* sink_ = nullptr;
    SpectatorReplaySaveConfigV4 config_{};
    const qnetplay::spectator::SpectatorConnectedArchiveV4* archive_ =
        nullptr;
    SpectatorReplaySaveState state_ =
        SpectatorReplaySaveState::Disabled;
    bool enabled_ = false;
    bool skipFailedGame_ = false;
    uint32_t readItemSequence_ = 0u;
    Candidate candidate_{};
    std::vector<SpectatorReplaySaveOutcomeV4> outcomes_;

    bool selectRetainedRange();
    bool alreadyFinished(const qreplay::GameIdentityV2& game) const;
    bool beginCandidate(
        const qnetplay::spectator::ConnectedBeginGameBundleV4& begin);
    bool appendRecord(
        const qnetplay::spectator::ConnectedConfirmedRecordV4& record);
    bool finishCandidate(
        const qnetplay::spectator::ConnectedEndGameBundleV4& end);
    void failCurrent(
        SpectatorReplaySaveFailureV4 failure,
        SpectatorReplaySaveOutcomeKindV4 kind =
            SpectatorReplaySaveOutcomeKindV4::Failed);
    void publishOutcome(const SpectatorReplaySaveOutcomeV4& outcome);
};

}
