#include "TestHarness.h"

#include "input/InputSnapshots.h"
#include "input/InputWorkers.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

using namespace qinput;

QNP_TEST(SnapshotTwoSlotHazardNeverTears) {
    auto published =
        std::make_unique<PublishedSnapshot<ControllerSnapshotPayload>>();
    ControllerSnapshotPayload pending{};
    published->initialize(pending);

    std::atomic<bool> stop{false};
    std::atomic<uint32_t> torn{0};
    std::thread writer([&] {
        uint32_t counter = 0;
        while (!stop.load()) {
            ControllerSnapshotPayload payload{};
            payload.status =
                static_cast<uint32_t>(SnapshotStatus::Connected);
            payload.sampleSerial = ++counter;
            for (uint32_t i = 0; i < kMaxControllerProbes; ++i)
                payload.probe[i] = counter;
            (void)published->publish(payload);
        }
    });
    uint32_t reads = 0;
    uint32_t failures = 0;
    for (int i = 0; i < 200000; ++i) {
        ControllerSnapshotPayload copy{};
        if (!published->read(copy)) {
            ++failures;
            continue;
        }
        ++reads;
        for (uint32_t p = 0; p < kMaxControllerProbes; ++p) {
            if (copy.probe[p] != copy.sampleSerial) torn.fetch_add(1);
        }
    }
    stop.store(true);
    writer.join();
    QNP_CHECK(torn.load() == 0);
    QNP_CHECK(reads > 0);
    QNP_CHECK(failures < 200000);
}

QNP_TEST(SnapshotWriterSkipsHazardedSlot) {
    auto published =
        std::make_unique<PublishedSnapshot<KeyboardSnapshotPayload>>();
    KeyboardSnapshotPayload pending{};
    published->initialize(pending);
    published->readerIndex.store(1);
    KeyboardSnapshotPayload payload{};
    payload.sampleSerial = 7;
    QNP_CHECK(!published->publish(payload));
    QNP_CHECK(published->slot[1].sampleSerial == 0);
    published->readerIndex.store(kNoSnapshotReader);
    QNP_CHECK(published->publish(payload));
    QNP_CHECK(published->activeIndex.load() == 1);
    QNP_CHECK(published->slot[1].sampleSerial == 7);
}

QNP_TEST(SnapshotWrapSafety) {
    QNP_CHECK(snapshotAgeMs(5, 0xFFFFFFF0u) == 21u);
    QNP_CHECK(snapshotAgeMs(0, 0) == 0u);
    QNP_CHECK(snapshotAgeMs(0xFFFFFFFFu, 0) == 0xFFFFFFFFu);
    PublishedWindowFocus focus;
    focus.publish(true, 0x7FFFFFFFu);
    bool active = false;
    uint32_t generation = 0;
    focus.readInto(active, generation);
    QNP_CHECK(active);
    QNP_CHECK(generation == 0x7FFFFFFFu);
    focus.publish(false, 0);
    focus.readInto(active, generation);
    QNP_CHECK(!active);
    QNP_CHECK(generation == 0);
}

namespace {

struct FakeKeyboardBackend final : IKeyboardBackend {
    std::atomic<uint32_t> blockMs{0};
    std::atomic<bool> failPoll{false};
    std::atomic<bool> failCreate{false};
    std::atomic<uint32_t> polls{0};
    uint8_t held = 0;

    bool create() override { return !failCreate.load(); }
    void destroy() override {}
    bool poll(uint8_t (&keys)[256]) override {
        const uint32_t block = blockMs.load();
        if (block != 0) Sleep(block);
        polls.fetch_add(1);
        if (failPoll.load()) return false;
        std::memset(keys, 0, sizeof(keys));
        if (held != 0) keys[held] = 0x80;
        return true;
    }
};

struct FakeControllerBackend final : IControllerBackend {
    std::atomic<uint32_t> blockMs{0};
    std::atomic<bool> present{true};
    std::atomic<bool> failRead{false};
    std::atomic<bool> mappingChanged{false};
    std::atomic<bool> duplicate{false};
    std::atomic<uint32_t> reads{0};
    uint32_t buttonValue = 0;

    bool initialize() override { return true; }
    void shutdown() override {}
    uint32_t enumerate(DeviceCandidate* out,
                       uint32_t capacity) override {
        const uint32_t block = blockMs.load();
        if (block != 0) Sleep(block);
        if (!present.load() || capacity == 0) return 0;
        out[0] = DeviceCandidate{};
        std::snprintf(out[0].guid, sizeof(out[0].guid), "fake");
        out[0].vendor = 1;
        out[0].product = 2;
        out[0].version = 3;
        std::snprintf(out[0].normalizedName,
                      sizeof(out[0].normalizedName), "FakePad");
        if (duplicate.load() && capacity > 1) {
            out[1] = out[0];
            out[1].liveHandle = 1;
            return 2;
        }
        return 1;
    }
    bool open(const DeviceCandidate&, const char*, uint32_t) override {
        return present.load();
    }
    void close() override {}
    bool mappingUnchanged() override { return !mappingChanged.load(); }
    bool readProbes(const BackendProbeRequest*, uint32_t count,
                    uint32_t* values) override {
        const uint32_t block = blockMs.load();
        if (block != 0) Sleep(block);
        reads.fetch_add(1);
        if (failRead.load() || !present.load()) return false;
        for (uint32_t i = 0; i < count; ++i) values[i] = buttonValue;
        return true;
    }
};

uint32_t testNowMs() {
    return static_cast<uint32_t>(GetTickCount64());
}

CompiledDeviceMatcher fakeMatcher() {
    CompiledDeviceMatcher matcher{};
    std::snprintf(matcher.guid, sizeof(matcher.guid), "fake");
    matcher.vendor = 1;
    matcher.product = 2;
    matcher.version = 3;
    std::snprintf(matcher.normalizedName, sizeof(matcher.normalizedName),
                  "FakePad");
    return matcher;
}

}

QNP_TEST(WorkerBlockedControllerDoesNotStopKeyboard) {
    FakeKeyboardBackend keyboardBackend;
    keyboardBackend.held = 0x2C;
    KeyboardWorker keyboard;
    KeyboardWorkerConfig keyboardConfig{};
    keyboardConfig.backend = &keyboardBackend;
    keyboardConfig.nowMs = &testNowMs;
    QNP_REQUIRE(keyboard.start(keyboardConfig));

    FakeControllerBackend controllerBackend;
    controllerBackend.blockMs.store(1000);
    const CompiledDeviceMatcher matcher = fakeMatcher();
    CompiledProbe probes[1];
    probes[0].kind = ProbeKind::RawButton;
    probes[0].token = 0;
    ControllerWorker controller;
    ControllerWorkerConfig controllerConfig{};
    controllerConfig.backend = &controllerBackend;
    controllerConfig.nowMs = &testNowMs;
    controllerConfig.matcher = &matcher;
    controllerConfig.probes = probes;
    controllerConfig.probeCount = 1;
    QNP_REQUIRE(controller.start(controllerConfig));

    Sleep(200);
    KeyboardSnapshotPayload snapshot{};
    const uint32_t readStart = testNowMs();
    QNP_REQUIRE(keyboard.snapshot().read(snapshot));
    const uint32_t readElapsed = testNowMs() - readStart;
    QNP_CHECK(readElapsed < 50);
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK((snapshot.keyBits[0x2C >> 5] >> (0x2C & 31)) & 1u);
    QNP_CHECK(keyboardBackend.polls.load() > 5);

    keyboard.requestStop();
    controller.requestStop();
    QNP_CHECK(keyboard.join(3000));
    (void)controller.join(3000);
}

QNP_TEST(WorkerInitialHeldKeyAndIdleHeartbeat) {
    FakeKeyboardBackend backend;
    backend.held = 0x1E;
    KeyboardWorker worker;
    KeyboardWorkerConfig config{};
    config.backend = &backend;
    config.nowMs = &testNowMs;
    QNP_REQUIRE(worker.start(config));
    Sleep(100);
    KeyboardSnapshotPayload first{};
    QNP_REQUIRE(worker.snapshot().read(first));
    QNP_CHECK((first.keyBits[0x1E >> 5] >> (0x1E & 31)) & 1u);
    const uint32_t serialA = first.sampleSerial;
    Sleep(100);
    KeyboardSnapshotPayload second{};
    QNP_REQUIRE(worker.snapshot().read(second));
    QNP_CHECK(second.sampleSerial > serialA);
    QNP_CHECK(snapshotAgeMs(testNowMs(), second.sampleTickMs) < 1000);
    worker.requestStop();
    QNP_CHECK(worker.join(3000));
}

QNP_TEST(WorkerKeyboardFaultAndRecoveryChangesGeneration) {
    FakeKeyboardBackend backend;
    backend.held = 0x1E;
    KeyboardWorker worker;
    KeyboardWorkerConfig config{};
    config.backend = &backend;
    config.nowMs = &testNowMs;
    QNP_REQUIRE(worker.start(config));
    Sleep(100);
    KeyboardSnapshotPayload healthy{};
    QNP_REQUIRE(worker.snapshot().read(healthy));
    QNP_CHECK(healthy.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    backend.failPoll.store(true);
    Sleep(100);
    KeyboardSnapshotPayload faulted{};
    QNP_REQUIRE(worker.snapshot().read(faulted));
    QNP_CHECK(faulted.status
              == static_cast<uint32_t>(SnapshotStatus::Fault));
    backend.failPoll.store(false);
    Sleep(200);
    KeyboardSnapshotPayload recovered{};
    QNP_REQUIRE(worker.snapshot().read(recovered));
    QNP_CHECK(recovered.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK(recovered.deviceGeneration != healthy.deviceGeneration);
    worker.requestStop();
    QNP_CHECK(worker.join(3000));
}

QNP_TEST(WorkerControllerLifecycleAndMappingFault) {
    FakeControllerBackend backend;
    backend.buttonValue = 1;
    const CompiledDeviceMatcher matcher = fakeMatcher();
    CompiledProbe probes[1];
    probes[0].kind = ProbeKind::RawButton;
    probes[0].token = 0;
    ControllerWorker worker;
    ControllerWorkerConfig config{};
    config.backend = &backend;
    config.nowMs = &testNowMs;
    config.matcher = &matcher;
    config.probes = probes;
    config.probeCount = 1;
    QNP_REQUIRE(worker.start(config));
    Sleep(150);
    ControllerSnapshotPayload snapshot{};
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK(snapshot.probe[0] == 1);
    for (int i = 0; i < 1000; ++i) worker.notifyDeviceChange();
    Sleep(100);
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    backend.present.store(false);
    backend.failRead.store(true);
    Sleep(150);
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              != static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK(snapshot.probe[0] == 0);
    const uint32_t oldGeneration = snapshot.deviceGeneration;
    backend.present.store(true);
    backend.failRead.store(false);
    Sleep(400);
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK(snapshot.deviceGeneration != oldGeneration);
    backend.mappingChanged.store(true);
    worker.notifyDeviceChange();
    Sleep(150);
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Fault));
    worker.requestStop();
    QNP_CHECK(worker.join(3000));
}

QNP_TEST(WorkerTransientAmbiguityRecovers) {
    FakeControllerBackend backend;
    backend.buttonValue = 1;
    backend.duplicate.store(true);
    const CompiledDeviceMatcher matcher = fakeMatcher();
    CompiledProbe probes[1];
    probes[0].kind = ProbeKind::RawButton;
    probes[0].token = 0;
    ControllerWorker worker;
    ControllerWorkerConfig config{};
    config.backend = &backend;
    config.nowMs = &testNowMs;
    config.matcher = &matcher;
    config.probes = probes;
    config.probeCount = 1;
    QNP_REQUIRE(worker.start(config));
    Sleep(200);
    ControllerSnapshotPayload snapshot{};
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Fault));
    QNP_CHECK(worker.attachState() == ControllerAttachState::Ambiguous);
    QNP_CHECK(snapshot.probe[0] == 0);
    backend.duplicate.store(false);
    worker.notifyDeviceChange();
    Sleep(300);
    QNP_REQUIRE(worker.snapshot().read(snapshot));
    QNP_CHECK(snapshot.status
              == static_cast<uint32_t>(SnapshotStatus::Connected));
    QNP_CHECK(worker.attachState() == ControllerAttachState::Attached);
    QNP_CHECK(snapshot.probe[0] == 1);
    worker.requestStop();
    QNP_CHECK(worker.join(3000));
}

QNP_TEST(WorkerGameCadenceUnaffectedByBlockedBackend) {
    FakeControllerBackend backend;
    backend.blockMs.store(1000);
    const CompiledDeviceMatcher matcher = fakeMatcher();
    CompiledProbe probes[1];
    probes[0].kind = ProbeKind::RawButton;
    probes[0].token = 0;
    ControllerWorker worker;
    ControllerWorkerConfig config{};
    config.backend = &backend;
    config.nowMs = &testNowMs;
    config.matcher = &matcher;
    config.probes = probes;
    config.probeCount = 1;
    QNP_REQUIRE(worker.start(config));
    uint32_t worstFrameMs = 0;
    for (int frame = 0; frame < 60; ++frame) {
        const uint32_t started = testNowMs();
        ControllerSnapshotPayload snapshot{};
        (void)worker.snapshot().read(snapshot);
        const uint32_t elapsed = testNowMs() - started;
        if (elapsed > worstFrameMs) worstFrameMs = elapsed;
    }
    QNP_CHECK(worstFrameMs < 20);
    worker.requestStop();
    (void)worker.join(2500);
}
