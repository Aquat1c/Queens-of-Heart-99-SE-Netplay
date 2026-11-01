#include "Log.h"

#include "input/InputWorkers.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace qinput {

namespace {

void recordLongestCall(WorkerTelemetry& telemetry, uint32_t startedMs,
                       NowMsFn nowMs) {
    if (nowMs == nullptr) return;
    const uint32_t elapsed = nowMs() - startedMs;
    uint32_t longest = telemetry.longestCallMs.load();
    while (elapsed > longest
           && !telemetry.longestCallMs.compare_exchange_weak(longest,
                                                             elapsed)) {
    }
}

}

KeyboardWorker::~KeyboardWorker() {
    requestStop();
    (void)join(2000);
}

bool KeyboardWorker::start(const KeyboardWorkerConfig& config) {
    if (running_.load() || config.backend == nullptr
        || config.nowMs == nullptr) {
        return false;
    }
    config_ = config;
    stop_.store(false);
    KeyboardSnapshotPayload pending{};
    pending.status = static_cast<uint32_t>(SnapshotStatus::PendingNeutral);
    pending.sampleTickMs = config_.nowMs();
    snapshot_.initialize(pending);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) return false;
    thread_ = CreateThread(nullptr, 0, &KeyboardWorker::threadEntry, this,
                           0, nullptr);
    if (thread_ == nullptr) {
        CloseHandle(static_cast<HANDLE>(wake_));
        wake_ = nullptr;
        return false;
    }
    SetThreadPriority(static_cast<HANDLE>(thread_),
                      THREAD_PRIORITY_BELOW_NORMAL);
    running_.store(true);
    return true;
}

void KeyboardWorker::requestStop() {
    stop_.store(true);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
}

bool KeyboardWorker::join(uint32_t deadlineMs) {
    if (thread_ == nullptr) return true;
    const DWORD waited = WaitForSingleObject(
        static_cast<HANDLE>(thread_), deadlineMs);
    if (waited != WAIT_OBJECT_0) return false;
    CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
    if (wake_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(wake_));
        wake_ = nullptr;
    }
    running_.store(false);
    return true;
}

unsigned long __stdcall KeyboardWorker::threadEntry(void* self) {
    static_cast<KeyboardWorker*>(self)->run();
    return 0;
}

void KeyboardWorker::run() {
    IKeyboardBackend* backend = config_.backend;
    bool created = false;
    uint8_t keys[256] = {};
    ++generation_;
    while (!stop_.load()) {
        const uint32_t startedMs = config_.nowMs();
        if (!created) {
            created = backend->create();
            recordLongestCall(telemetry_, startedMs, config_.nowMs);
            if (!created) {
                telemetry_.faults.fetch_add(1);
                KeyboardSnapshotPayload fault{};
                fault.status =
                    static_cast<uint32_t>(SnapshotStatus::Fault);
                fault.sampleTickMs = config_.nowMs();
                fault.sampleSerial = serial_;
                fault.workerEpoch = 1;
                fault.deviceGeneration = generation_;
                if (!snapshot_.publish(fault))
                    telemetry_.publishSkips.fetch_add(1);
                WaitForSingleObject(static_cast<HANDLE>(wake_), 250);
                continue;
            }
            ++generation_;
        }
        const bool polled = backend->poll(keys);
        recordLongestCall(telemetry_, startedMs, config_.nowMs);
        telemetry_.updates.fetch_add(1);
        KeyboardSnapshotPayload payload{};
        if (polled) {
            payload.status =
                static_cast<uint32_t>(SnapshotStatus::Connected);
            payload.sampleTickMs = config_.nowMs();
            payload.sampleSerial = ++serial_;
            for (int i = 0; i < 256; ++i) {
                if (keys[i] & 0x80u)
                    payload.keyBits[i >> 5] |= 1u << (i & 31);
            }
        } else {
            telemetry_.faults.fetch_add(1);
            backend->destroy();
            created = false;
            payload.status = static_cast<uint32_t>(SnapshotStatus::Fault);
            payload.sampleTickMs = config_.nowMs();
            payload.sampleSerial = serial_;
        }
        payload.workerEpoch = 1;
        payload.deviceGeneration = generation_;
        if (!snapshot_.publish(payload))
            telemetry_.publishSkips.fetch_add(1);
        WaitForSingleObject(static_cast<HANDLE>(wake_),
                            config_.updateIntervalMs);
    }
    backend->destroy();
}

ControllerWorker::~ControllerWorker() {
    requestStop();
    (void)join(2000);
}

bool ControllerWorker::start(const ControllerWorkerConfig& config) {
    if (running_.load() || config.backend == nullptr
        || config.nowMs == nullptr || config.matcher == nullptr) {
        return false;
    }
    config_ = config;
    stop_.store(false);
    refreshRequested_.store(true);
    ControllerSnapshotPayload pending{};
    pending.status = static_cast<uint32_t>(SnapshotStatus::PendingNeutral);
    pending.sampleTickMs = config_.nowMs();
    snapshot_.initialize(pending);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) return false;
    thread_ = CreateThread(nullptr, 0, &ControllerWorker::threadEntry,
                           this, 0, nullptr);
    if (thread_ == nullptr) {
        CloseHandle(static_cast<HANDLE>(wake_));
        wake_ = nullptr;
        return false;
    }
    SetThreadPriority(static_cast<HANDLE>(thread_),
                      THREAD_PRIORITY_BELOW_NORMAL);
    running_.store(true);
    return true;
}

void ControllerWorker::requestStop() {
    stop_.store(true);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
}

bool ControllerWorker::join(uint32_t deadlineMs) {
    if (thread_ == nullptr) return true;
    const DWORD waited = WaitForSingleObject(
        static_cast<HANDLE>(thread_), deadlineMs);
    if (waited != WAIT_OBJECT_0) return false;
    CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
    if (wake_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(wake_));
        wake_ = nullptr;
    }
    running_.store(false);
    return true;
}

void ControllerWorker::notifyDeviceChange() {
    refreshRequested_.store(true);
    if (wake_ != nullptr) SetEvent(static_cast<HANDLE>(wake_));
}

unsigned long __stdcall ControllerWorker::threadEntry(void* self) {
    static_cast<ControllerWorker*>(self)->run();
    return 0;
}

void ControllerWorker::publishState(SnapshotStatus status,
                                    const uint32_t* probes,
                                    bool heartbeat) {
    ControllerSnapshotPayload payload{};
    payload.status = static_cast<uint32_t>(status);
    if (heartbeat) {
        payload.sampleTickMs = config_.nowMs();
        payload.sampleSerial = ++serial_;
    } else {
        payload.sampleTickMs = config_.nowMs() - 0x40000000u;
        payload.sampleSerial = serial_;
    }
    payload.workerEpoch = 1;
    payload.deviceGeneration = generation_;
    if (probes != nullptr) {
        for (uint32_t i = 0;
             i < config_.probeCount && i < kMaxControllerProbes; ++i) {
            payload.probe[i] = probes[i];
        }
    }
    if (!snapshot_.publish(payload))
        telemetry_.publishSkips.fetch_add(1);
}

void ControllerWorker::run() {
    IControllerBackend* backend = config_.backend;
    if (!backend->initialize()) {
        telemetry_.faults.fetch_add(1);
        attachState_.store(
            static_cast<uint32_t>(ControllerAttachState::BackendUnavailable));
        publishState(SnapshotStatus::Fault, nullptr, false);
        return;
    }
    BackendProbeRequest requests[kMaxControllerProbes];
    const uint32_t probeCount = config_.probeCount < kMaxControllerProbes
        ? config_.probeCount : kMaxControllerProbes;
    for (uint32_t i = 0; i < probeCount; ++i) {
        requests[i].kind = config_.probes[i].kind;
        requests[i].token = config_.probes[i].token;
    }
    uint32_t values[kMaxControllerProbes] = {};

    while (!stop_.load()) {
        const uint32_t startedMs = config_.nowMs();
        if (!open_) {
            const uint32_t nowMs = config_.nowMs();
            const bool due =
                refreshRequested_.exchange(false)
                || static_cast<int32_t>(nowMs - nextDiscoveryMs_) >= 0;
            if (due && !layoutFault_) {
                DeviceCandidate candidates[kMaxIdentityCandidates];
                const uint32_t count = backend->enumerate(
                    candidates, kMaxIdentityCandidates);
                lastCandidateCount_.store(count);
                telemetry_.enumerations.fetch_add(1);
                recordLongestCall(telemetry_, startedMs, config_.nowMs);
                const IdentityResolution resolution =
                    resolveDeviceIdentity(*config_.matcher, candidates,
                                          count);
                ambiguous_ = false;
                if (count != loggedCandidateCount_
                    || resolution.outcome != loggedOutcome_) {
                    loggedCandidateCount_ = count;
                    loggedOutcome_ = resolution.outcome;
                    QLOG("[controller.runtime] discovery candidates=%u "
                         "outcome=%s wanted[serial=%s path=%s guid=%s "
                         "vid=%04X pid=%04X]",
                         count,
                         resolution.outcome == IdentityOutcome::Unique
                             ? "matched"
                             : (resolution.outcome
                                        == IdentityOutcome::Ambiguous
                                    ? "AMBIGUOUS - two or more are "
                                      "indistinguishable"
                                    : "NO MATCH"),
                         config_.matcher->hasSerial ? "yes" : "no",
                         config_.matcher->hasPathFingerprint ? "yes" : "no",
                         config_.matcher->guid, config_.matcher->vendor,
                         config_.matcher->product);
                    for (uint32_t i = 0; i < count; ++i) {
                        QLOG("[controller.runtime]   [%u] name='%s' "
                             "gamepad=%d axes=%u buttons=%u hats=%u "
                             "serial=%d path=%d guid=%s vid=%04X pid=%04X",
                             i, candidates[i].normalizedName,
                             candidates[i].isGamepad ? 1 : 0,
                             static_cast<unsigned>(candidates[i].axisCount),
                             static_cast<unsigned>(candidates[i].buttonCount),
                             static_cast<unsigned>(candidates[i].hatCount),
                             candidates[i].hasSerial ? 1 : 0,
                             candidates[i].hasPathFingerprint ? 1 : 0,
                             candidates[i].guid, candidates[i].vendor,
                             candidates[i].product);
                    }
                }
                if (resolution.outcome == IdentityOutcome::Unique) {
                    telemetry_.openAttempts.fetch_add(1);
                    if (backend->open(
                            candidates[resolution.candidateIndex],
                            config_.mapping, config_.mappingLength)) {
                        open_ = true;
                        ++generation_;
                        discoveryFailures_ = 0;
                        attachState_.store(static_cast<uint32_t>(
                            ControllerAttachState::Attached));
                        QLOG("[controller.runtime] ATTACHED name='%s' "
                             "generation=%u matchedBy=%s - the configured "
                             "controller is open in the game process",
                             candidates[resolution.candidateIndex]
                                 .normalizedName,
                             generation_,
                             resolution.tier
                                     == IdentityMatchTier::VendorProduct
                                 ? "vid/pid only (the profile was captured "
                                   "through a different input backend; the "
                                   "device is the same)"
                                 : resolution.tier
                                           == IdentityMatchTier::Serial
                                       ? "serial"
                                       : resolution.tier
                                                 == IdentityMatchTier::
                                                     PathFingerprint
                                             ? "path"
                                             : "exact class");
                    } else {
                        telemetry_.faults.fetch_add(1);
                        attachState_.store(static_cast<uint32_t>(
                            ControllerAttachState::OpenFailed));
                        QLOG("[controller.runtime] OPEN FAILED name='%s' - "
                             "it was matched but would not open here",
                             candidates[resolution.candidateIndex]
                                 .normalizedName);
                    }
                } else if (resolution.outcome
                           == IdentityOutcome::Ambiguous) {
                    ambiguous_ = true;
                    telemetry_.ambiguityFaults.fetch_add(1);
                    attachState_.store(static_cast<uint32_t>(
                        ControllerAttachState::Ambiguous));
                } else {
                    attachState_.store(static_cast<uint32_t>(
                        ControllerAttachState::NoCandidate));
                }
                const uint32_t backoffIndex =
                    discoveryFailures_ < 3 ? discoveryFailures_ : 3;
                nextDiscoveryMs_ =
                    config_.nowMs()
                    + config_.discoveryBackoffMs[backoffIndex];
                if (!open_) ++discoveryFailures_;
            }
            if (!open_) {
                publishState((ambiguous_ || layoutFault_)
                                 ? SnapshotStatus::Fault
                                 : SnapshotStatus::Disconnected,
                             nullptr, false);
                WaitForSingleObject(static_cast<HANDLE>(wake_), 250);
                continue;
            }
        }
        if (refreshRequested_.exchange(false)) {
            if (!backend->mappingUnchanged()) {
                QLOG("[controller.runtime] LAYOUT FAULT: the open device's "
                     "mapping changed underneath the compiled bindings. The "
                     "controller is released and discovery STOPS until "
                     "relaunch (the backend logs both mappings above).");
                telemetry_.mappingFaults.fetch_add(1);
                backend->close();
                open_ = false;
                layoutFault_ = true;
                attachState_.store(static_cast<uint32_t>(
                    ControllerAttachState::LayoutChanged));
                publishState(SnapshotStatus::Fault, nullptr, false);
                continue;
            }
        }
        const bool read = backend->readProbes(requests, probeCount, values);
        recordLongestCall(telemetry_, startedMs, config_.nowMs);
        telemetry_.updates.fetch_add(1);
        if (read) {
            publishState(SnapshotStatus::Connected, values, true);
        } else {
            telemetry_.faults.fetch_add(1);
            backend->close();
            open_ = false;
            discoveryFailures_ = 0;
            nextDiscoveryMs_ = config_.nowMs();
            attachState_.store(
                static_cast<uint32_t>(ControllerAttachState::NoCandidate));
            publishState(SnapshotStatus::Disconnected, nullptr, false);
        }
        WaitForSingleObject(static_cast<HANDLE>(wake_),
                            config_.updateIntervalMs);
    }
    if (open_) backend->close();
    backend->shutdown();
}

}
