#pragma once

#include <atomic>
#include <cstdint>

#include "input/DeviceIdentity.h"
#include "input/InputLaunchConfig.h"
#include "input/InputSnapshots.h"

namespace qinput {

class IKeyboardBackend {
public:
    virtual ~IKeyboardBackend() = default;
    virtual bool create() = 0;
    virtual void destroy() = 0;
    virtual bool poll(uint8_t (&keys)[256]) = 0;
};

struct BackendProbeRequest {
    ProbeKind kind = ProbeKind::None;
    uint8_t token = 0;
};

class IControllerBackend {
public:
    virtual ~IControllerBackend() = default;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual uint32_t enumerate(DeviceCandidate* out, uint32_t capacity) = 0;
    virtual bool open(const DeviceCandidate& candidate,
                      const char* mapping, uint32_t mappingLength) = 0;
    virtual void close() = 0;
    virtual bool mappingUnchanged() = 0;
    virtual bool readProbes(const BackendProbeRequest* requests,
                            uint32_t count, uint32_t* values) = 0;
    virtual bool semanticGamepad() const { return false; }
    virtual uint32_t currentMapping(char* out, uint32_t capacity) {
        (void)out; (void)capacity; return 0;
    }
    virtual void preferRawJoystick(bool preferRaw) { (void)preferRaw; }
};

struct WorkerTelemetry {
    std::atomic<uint32_t> updates{0};
    std::atomic<uint32_t> publishSkips{0};
    std::atomic<uint32_t> enumerations{0};
    std::atomic<uint32_t> openAttempts{0};
    std::atomic<uint32_t> faults{0};
    std::atomic<uint32_t> ambiguityFaults{0};
    std::atomic<uint32_t> mappingFaults{0};
    std::atomic<uint32_t> longestCallMs{0};
};

typedef uint32_t (*NowMsFn)();

IKeyboardBackend* createDirectInput8KeyboardBackend(void* focusWindow);

IControllerBackend* createDirectInput8ControllerBackend(void* focusWindow);
#ifdef QOH99_WITH_SDL3
IControllerBackend* createSdlControllerBackend();
#endif

struct KeyboardWorkerConfig {
    IKeyboardBackend* backend = nullptr;
    NowMsFn nowMs = nullptr;
    uint32_t updateIntervalMs = 4;
};

struct ControllerWorkerConfig {
    IControllerBackend* backend = nullptr;
    NowMsFn nowMs = nullptr;
    const CompiledDeviceMatcher* matcher = nullptr;
    const CompiledProbe* probes = nullptr;
    uint32_t probeCount = 0;
    const char* mapping = nullptr;
    uint32_t mappingLength = 0;
    uint32_t updateIntervalMs = 4;
    uint32_t discoveryBackoffMs[4] = {0, 250, 500, 1000};
};

class KeyboardWorker {
public:
    ~KeyboardWorker();
    bool start(const KeyboardWorkerConfig& config);
    // Call join() only after simulation ownership has ended; a false return means the thread is stuck and must be leaked, never unloaded.
    void requestStop();
    bool join(uint32_t deadlineMs);
    bool running() const { return running_.load(); }

    PublishedSnapshot<KeyboardSnapshotPayload>& snapshot() {
        return snapshot_;
    }
    WorkerTelemetry& telemetry() { return telemetry_; }

private:
    static unsigned long __stdcall threadEntry(void* self);
    void run();

    KeyboardWorkerConfig config_{};
    PublishedSnapshot<KeyboardSnapshotPayload> snapshot_;
    WorkerTelemetry telemetry_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    void* thread_ = nullptr;
    void* wake_ = nullptr;
    uint32_t serial_ = 0;
    uint32_t generation_ = 0;
};

enum class ControllerAttachState : uint32_t {
    Starting = 0,
    BackendUnavailable = 1,
    NoCandidate = 2,
    Ambiguous = 3,
    OpenFailed = 4,
    LayoutChanged = 5,
    Attached = 6,
};

class ControllerWorker {
public:
    ~ControllerWorker();
    bool start(const ControllerWorkerConfig& config);
    void requestStop();
    bool join(uint32_t deadlineMs);
    bool running() const { return running_.load(); }

    ControllerAttachState attachState() const {
        return static_cast<ControllerAttachState>(attachState_.load());
    }
    uint32_t lastCandidateCount() const {
        return lastCandidateCount_.load();
    }

    void notifyDeviceChange();

    PublishedSnapshot<ControllerSnapshotPayload>& snapshot() {
        return snapshot_;
    }
    WorkerTelemetry& telemetry() { return telemetry_; }

private:
    static unsigned long __stdcall threadEntry(void* self);
    void run();
    void publishState(SnapshotStatus status, const uint32_t* probes,
                      bool heartbeat);

    ControllerWorkerConfig config_{};
    PublishedSnapshot<ControllerSnapshotPayload> snapshot_;
    WorkerTelemetry telemetry_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> refreshRequested_{true};
    std::atomic<uint32_t> attachState_{
        static_cast<uint32_t>(ControllerAttachState::Starting)};
    std::atomic<uint32_t> lastCandidateCount_{0};
    void* thread_ = nullptr;
    void* wake_ = nullptr;
    uint32_t serial_ = 0;
    uint32_t generation_ = 0;
    bool open_ = false;
    bool ambiguous_ = false;
    bool layoutFault_ = false;
    uint32_t discoveryFailures_ = 0;
    uint32_t loggedCandidateCount_ = 0xFFFFFFFFu;
    IdentityOutcome loggedOutcome_ = IdentityOutcome::NoCandidate;
    uint32_t nextDiscoveryMs_ = 0;
};

}
