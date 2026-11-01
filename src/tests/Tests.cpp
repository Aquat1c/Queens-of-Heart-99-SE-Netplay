#include "../net/Protocol.h"
#include "../net/RouteCandidates.h"
#include "../net/Sockets.h"
#include "../ipc/IpcConfig.h"
#include "../client/PublicIp.h"
#include "../client/SessionLogArchive.h"
#include "../client/WaveGain.h"
#include "../i18n/Tr.h"
#include "../platform/WidePath.h"
#include "../client/spectator/SpectatorRoute.h"
#include "../client/spectator/SpectatorSidecar.h"
#include "../net/Reliable.h"
#include "../game/SessionPresentation.h"
#include "../qoh99/qoh99_registry.h"
#include "../qoh99/qoh99_state_machine.h"
#include "../qoh99/qoh99_inputs.h"
#include "../qoh99/qoh99_savestate_profile.h"
#include "../qoh99/qoh99_vanilla_net_protocol.h"
#include "../input/InputProfile.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <set>

bool parseHostPort(const std::string& in, std::string& ip, uint16_t& port, uint16_t defPort);
bool parsePortNumber(const std::string& text, uint16_t& port);
uint16_t configuredPortOrDefault(int value, uint16_t fallback);
void initializeIpcPlayerIdentityDefaults(
    qipc::IpcConfig& config, const std::string& localName);
std::string canonicalUtf8Nickname(std::string_view value);
std::string encodeNicknameUtf8Hex(std::string_view value);
bool decodeNicknameUtf8Hex(std::string_view encoded, std::string& out);
bool validLivePresence(const qnet::LivePresence& presence);
std::string livePresenceJson(const qnet::LivePresence& presence);
bool parseSpectatorKey(const std::string& text, uint64_t& low, uint64_t& high);
bool buildSpectateAddress(const std::string& publicIpv4,
                          const uint32_t* localIpv4BE, size_t localCount,
                          uint16_t port, uint32_t sessionId,
                          uint64_t keyLow, uint64_t keyHigh,
                          qnet::SpectateAddress& out,
                          std::string& error);
std::string qoh99ControlsBindingText(
    const std::vector<qinput::ProfileBinding>& bindings,
    bool preferController, bool required);
std::string qoh99ControlsSourceText(const qinput::InputProfile& profile,
                                    const std::string& connectedPadName);
bool qoh99ControlsUsesController(const qinput::InputProfile& profile);
bool qoh99SpectatorPreflightMayLaunch(bool forcedRelay,
                                      bool preflightOk,
                                      bool inconclusive,
                                      uint16_t routeLabel);
int qoh99ClassifyChildExit(bool haveChildExit,
                           DWORD childExit,
                           DWORD lifecycleFailure);
int qoh99PreferNativeChildExit(int childExit, int semanticFailure);
uint16_t qoh99ReplayStartupFlags(uint16_t presentationFlags,
                                 bool pauseAtStart, bool headless);
bool qoh99SpectatorAdmissionReleaseRequired(bool routeOk,
                                             uint64_t grantId,
                                             uint64_t viewerId);
bool qoh99SpectatorAdmissionReleaseGuardSelfTest();
std::string qoh99SpectatorE0SourceEvidenceLine(
    const qclient::spectator::SidecarStatus& status);
std::string qoh99SpectatorE0ViewerEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer);
std::string qoh99SpectatorE0ViewerAttachEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer);
std::string qoh99SpectatorE0ViewerDisconnectEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer,
    const qclient::spectator::ViewerTerminalOutcome* outcome);

namespace qtest {

using namespace qnet;

struct TestCase { const char* name; std::function<bool()> fn; };
static std::vector<TestCase>& cases() { static std::vector<TestCase> v; return v; }
static int  g_checksFailed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("    CHECK FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_checksFailed; \
    } } while (0)

#define TEST(name) \
    static bool test_##name(); \
    static const bool reg_##name = (cases().push_back({#name, &test_##name}), true); \
    static bool test_##name()

static std::vector<uint8_t> mk(uint8_t type, const void* body, size_t len) {
    std::vector<uint8_t> v(kMaxMsgSize);
    size_t n = packMsg(type, body, len, v.data(), v.size());
    v.resize(n);
    return v;
}

static qnetplay::compat::CompatibilityIdentityWire testCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = kProtocolVersion;
    if (!qnetplay::compat::setProfileId(identity, "qoh99-se1126"))
        return {};
    std::memset(identity.runtimeBuild.executableSha256, 0x11,
                sizeof(identity.runtimeBuild.executableSha256));
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    std::memset(identity.simulationContent.simulationRootSha256, 0x22,
                sizeof(identity.simulationContent.simulationRootSha256));
    std::memset(identity.simulationContent.validationRootSha256, 0x33,
                sizeof(identity.simulationContent.validationRootSha256));
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x1001u;
    identity.modSimulationId = 0x2001u;
    identity.timingProfileId = 0x3001u;
    return identity;
}

static RuntimeArtifactIdentityWire testRuntimeArtifactIdentity() {
    RuntimeArtifactIdentityWire identity{};
    identity.schemaVersion = kRuntimeArtifactIdentitySchemaVersion;
    std::memset(identity.casterSha256, 0x44, sizeof(identity.casterSha256));
    std::memset(identity.netplayDllSha256, 0x55,
                sizeof(identity.netplayDllSha256));
    std::memset(identity.ddrawSha256, 0x66, sizeof(identity.ddrawSha256));
    return identity;
}

TEST(UdpSendRecvLoopback) {
    UdpSock a, b;
    CHECK(a.open(0)); CHECK(b.open(0));
    Endpoint toB; Endpoint::parse("127.0.0.1", b.boundPort(), toB);
    const char msg[] = "hello";
    CHECK(a.sendTo(toB, msg, sizeof(msg)));
    CHECK(b.waitReadable(1000));
    char buf[64]; Endpoint from;
    int r = b.recvFrom(buf, sizeof(buf), from);
    CHECK(r == sizeof(msg));
    CHECK(std::memcmp(buf, msg, sizeof(msg)) == 0);
    return true;
}

TEST(UdpBidirectionalExchange) {
    UdpSock a, b;
    CHECK(a.open(0)); CHECK(b.open(0));
    Endpoint toB, toA;
    Endpoint::parse("127.0.0.1", b.boundPort(), toB);
    Endpoint::parse("127.0.0.1", a.boundPort(), toA);
    CHECK(a.sendTo(toB, "ping", 4));
    CHECK(b.waitReadable(1000));
    char buf[16]; Endpoint from;
    CHECK(b.recvFrom(buf, sizeof(buf), from) == 4);
    CHECK(b.sendTo(toA, "pong", 4));
    CHECK(a.waitReadable(1000));
    CHECK(a.recvFrom(buf, sizeof(buf), from) == 4);
    CHECK(std::memcmp(buf, "pong", 4) == 0);
    return true;
}

TEST(UdpNonBlockingEmptyRecv) {
    UdpSock a;
    CHECK(a.open(0));
    char buf[16]; Endpoint from;
    CHECK(a.recvFrom(buf, sizeof(buf), from) == 0);
    return true;
}

TEST(UdpMultipleDatagramsPreserved) {
    UdpSock a, b;
    CHECK(a.open(0)); CHECK(b.open(0));
    Endpoint toB; Endpoint::parse("127.0.0.1", b.boundPort(), toB);
    for (uint32_t i = 0; i < 5; i++)
        CHECK(a.sendTo(toB, &i, sizeof(i)));
    int got = 0;
    uint32_t v; Endpoint from;
    while (b.waitReadable(200) && b.recvFrom(&v, sizeof(v), from) == sizeof(v))
        got++;
    CHECK(got == 5);
    return true;
}

TEST(UdpBindBusyPortFails) {
    UdpSock a, b;
    CHECK(a.open(0));
    CHECK(!b.open(a.boundPort()));
    return true;
}

TEST(UdpProtocolRoundTrip) {
    UdpSock a, b;
    CHECK(a.open(0)); CHECK(b.open(0));
    Endpoint toB; Endpoint::parse("127.0.0.1", b.boundPort(), toB);
    Ping ping{ 7, 123456789ull };
    auto pkt = mk(MSG_PING, &ping, sizeof(ping));
    CHECK(a.sendTo(toB, pkt.data(), pkt.size()));
    CHECK(b.waitReadable(1000));
    uint8_t buf[kMaxMsgSize]; Endpoint from;
    int r = b.recvFrom(buf, sizeof(buf), from);
    uint8_t type; const uint8_t* body; size_t bodyLen;
    CHECK(unpackMsg(buf, static_cast<size_t>(r), type, body, bodyLen));
    CHECK(type == MSG_PING);
    Ping echo; std::memcpy(&echo, body, sizeof(echo));
    CHECK(echo.seq == 7 && echo.sendTimeUs == 123456789ull);
    return true;
}

TEST(UdpCorruptDatagramRejected) {
    Ping ping{ 1, 2 };
    auto pkt = mk(MSG_PING, &ping, sizeof(ping));
    pkt[sizeof(MsgHeader) + 2] ^= 0xFF;
    uint8_t type; const uint8_t* body; size_t bodyLen;
    CHECK(!unpackMsg(pkt.data(), pkt.size(), type, body, bodyLen));
    return true;
}

TEST(UdpPlayerInputsSize) {
    CHECK(sizeof(MsgHeader) + sizeof(PlayerInputs) < 600);
    PlayerInputs pi{};
    pi.indexedFrame = indexedFrame(3, 1234);
    CHECK(ifIndex(pi.indexedFrame) == 3 && ifFrame(pi.indexedFrame) == 1234);
    return true;
}

static bool tcpPair(TcpListener& lis, TcpConn& client, TcpConn& server, uint16_t& port) {
    for (port = 42345; port < 42360; port++)
        if (lis.listen(port)) break;
    if (!lis.isOpen()) return false;
    if (!client.connectTo("127.0.0.1", port, 2000)) return false;
    server = lis.acceptOne(2000);
    return server.isOpen();
}

TEST(TcpListenAcceptConnect) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    return true;
}

TEST(TcpSendRecvFramedMsg) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    VersionConfig vc{ kProtocolVersion };
    CHECK(c.sendMsg(MSG_VERSION_CONFIG, &vc, sizeof(vc)));
    TcpConn::Received rx;
    CHECK(s.recvMsg(rx, 2000));
    CHECK(rx.type == MSG_VERSION_CONFIG);
    CHECK(rx.body.size() == sizeof(VersionConfig));
    return true;
}

TEST(TcpInitialConfigPreservesExplicitMultibyteCodePage) {
    static_assert(sizeof(InitialConfig) == 335,
                  "control identity wire layout changed");
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    InitialConfig identity{};
    identity.name[0] = static_cast<char>(0x81u);
    identity.name[1] = static_cast<char>(0x7Cu);
    identity.nameCodePage = 932u;
    identity.isHost = 1;
    identity.dataPort = 7500u;
    identity.roundsToWin = 2u;
    identity.compatibilityIdentity = testCompatibilityIdentity();
    identity.runtimeArtifacts = testRuntimeArtifactIdentity();
    CHECK(qgame::isCanonicalPlayerNameForCodePage(
        identity.name, identity.nameCodePage));
    CHECK(c.sendMsg(MSG_INITIAL_CONFIG, &identity, sizeof(identity)));
    TcpConn::Received rx;
    CHECK(s.recvMsg(rx, 2000));
    CHECK(rx.type == MSG_INITIAL_CONFIG);
    CHECK(rx.body.size() == sizeof(InitialConfig));
    InitialConfig decoded{};
    if (rx.body.size() == sizeof(decoded))
        std::memcpy(&decoded, rx.body.data(), sizeof(decoded));
    CHECK(decoded.nameCodePage == 932u);
    CHECK(static_cast<uint8_t>(decoded.name[0]) == 0x81u);
    CHECK(static_cast<uint8_t>(decoded.name[1]) == 0x7Cu);
    CHECK(decoded.isHost == 1);
    CHECK(decoded.dataPort == 7500u);
    CHECK(decoded.roundsToWin == 2u);
    CHECK(isValidControlCompatibilityIdentity(
        decoded.compatibilityIdentity));
    CHECK(isValidRuntimeArtifactIdentity(decoded.runtimeArtifacts));
    CHECK(runtimeArtifactMismatch(identity.runtimeArtifacts,
                                  decoded.runtimeArtifacts)
          == RuntimeArtifactMismatch::None);
    CHECK(qnetplay::compat::semanticallyEqual(
        identity.compatibilityIdentity, decoded.compatibilityIdentity));
    return true;
}

TEST(RuntimeArtifactIdentityRejectsStalePackageFile) {
    const RuntimeArtifactIdentityWire local = testRuntimeArtifactIdentity();
    RuntimeArtifactIdentityWire remote = local;
    remote.ddrawSha256[0] ^= 0xFFu;
    CHECK(isValidRuntimeArtifactIdentity(local));
    CHECK(isValidRuntimeArtifactIdentity(remote));
    CHECK(runtimeArtifactMismatch(local, remote)
          == RuntimeArtifactMismatch::Ddraw);
    remote = local;
    remote.netplayDllSha256[0] ^= 0xFFu;
    remote.casterSha256[0] ^= 0xFFu;
    CHECK(runtimeArtifactMismatch(local, remote)
          == RuntimeArtifactMismatch::Multiple);
    remote = {};
    CHECK(runtimeArtifactMismatch(local, remote)
          == RuntimeArtifactMismatch::MissingOrInvalid);
    return true;
}

TEST(IpcCompatibilityIdentityCannotBeReplacedByTheBundleMarker) {
    qipc::IpcConfig config{};
    config.compatibilityId = qipc::kRuntimeCompatibilityId;
    config.compatibilityIdentity = testCompatibilityIdentity();
    CHECK(qipc::validateIpcCompatibilityIdentity(config) == nullptr);

    config.compatibilityIdentity = {};
    const char* reason = qipc::validateIpcCompatibilityIdentity(config);
    CHECK(reason != nullptr);
    CHECK(std::strstr(reason, "identity") != nullptr);
    return true;
}

TEST(IpcIdentityDefaultsKeepOfflineAndSpectatorNamesValid) {
    qipc::IpcConfig config{};
    const std::string mixedName = "\xE3\x81\x82\xD0\x96";
    initializeIpcPlayerIdentityDefaults(config, mixedName);
    CHECK(std::string(config.localName) == mixedName);
    CHECK(std::strcmp(config.remoteName, "Player") == 0);
    CHECK(config.localNameCodePage == CP_UTF8);
    CHECK(config.remoteNameCodePage == CP_UTF8);
    CHECK(qgame::isCanonicalPlayerNameForCodePage(
        config.localName, config.localNameCodePage));
    CHECK(qgame::isCanonicalPlayerNameForCodePage(
        config.remoteName, config.remoteNameCodePage));
    return true;
}

TEST(NicknameUtf8PersistenceEncodingIsExactAndFailClosed) {
    const std::string mixedName = "\xE3\x81\x82\xD0\x96";
    const std::string encoded = encodeNicknameUtf8Hex(mixedName);
    CHECK(encoded == "E38182D096");

    std::string decoded = "unchanged";
    CHECK(decodeNicknameUtf8Hex(encoded, decoded));
    CHECK(decoded == mixedName);

    decoded = "unchanged";
    CHECK(!decodeNicknameUtf8Hex("E38", decoded));
    CHECK(decoded == "unchanged");
    CHECK(!decodeNicknameUtf8Hex("GG", decoded));
    CHECK(decoded == "unchanged");
    CHECK(!decodeNicknameUtf8Hex(std::string(
        qgame::kPlayerNameDisplayBytes * 2u + 2u, '0'), decoded));
    CHECK(decoded == "unchanged");

    const std::string sevenJapaneseGlyphs =
        "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
        "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82";
    const std::string canonical = canonicalUtf8Nickname(sevenJapaneseGlyphs);
    CHECK(canonical.size() == 18u);
    CHECK(qgame::isCanonicalPlayerNameForCodePage(canonical, CP_UTF8));
    return true;
}

TEST(CompletedSessionLogsArchiveToOneVerifiedZip) {
    if (!qclient::sessionLogArchiveAvailable()) return true;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("qoh99_session_archive_"
           + std::to_string(static_cast<unsigned long>(GetCurrentProcessId()))
           + "_" + std::to_string(GetTickCount64()));
    const std::filesystem::path session = root / "session_archive_test";
    const std::filesystem::path nested = session / "nested";
    std::error_code error;
    std::filesystem::create_directories(nested, error);
    CHECK(!error);
    if (error) return false;

    {
        std::ofstream first(session / "client.log", std::ios::binary);
        first << "small repeated log line\nsmall repeated log line\n";
        std::ofstream second(nested / "dll.log", std::ios::binary);
        second << "nested diagnostic\n";
        std::ofstream desync(session / "desync_frame_42_slot_1.bin",
                            std::ios::binary);
        const unsigned char rawState[] = {0x00u, 0xFFu, 0x42u, 0x99u};
        desync.write(reinterpret_cast<const char*>(rawState),
                     sizeof(rawState));
        std::ofstream dump(nested / "crash_host_777.dmp", std::ios::binary);
        const unsigned char minidump[] = {0x4Du, 0x44u, 0x4Du, 0x50u};
        dump.write(reinterpret_cast<const char*>(minidump), sizeof(minidump));
    }

    const std::string sessionUtf8 = qplat::narrow(session.wstring());
    qclient::SessionLogWriterLease liveWriter =
        qclient::acquireSessionLogWriterLease(sessionUtf8);
    CHECK(!liveWriter.markerPath.empty());
    const qclient::SessionLogArchiveResult whileLive =
        qclient::archiveClosedSessionLogDirectory(sessionUtf8);
    CHECK(whileLive.status == qclient::SessionLogArchiveStatus::InUse);
    CHECK(std::filesystem::exists(session));
    qclient::releaseSessionLogWriterLease(liveWriter);

    const qclient::SessionLogArchiveResult archived =
        qclient::archiveClosedSessionLogDirectory(sessionUtf8);
    CHECK(archived.status == qclient::SessionLogArchiveStatus::Archived);
    CHECK(!archived.archivePath.empty());
    CHECK(!std::filesystem::exists(session));
    CHECK(std::filesystem::is_regular_file(
        std::filesystem::path(qplat::widen(archived.archivePath))));

    std::filesystem::remove_all(root, error);
    CHECK(!error);
    return true;
}

TEST(AbandonedMarkedSessionRecoversOnlyAfterWriterHasExited) {
    if (!qclient::sessionLogArchiveAvailable()) return true;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("qoh99_session_recovery_"
           + std::to_string(static_cast<unsigned long>(GetCurrentProcessId()))
           + "_" + std::to_string(GetTickCount64()));
    const std::filesystem::path session = root / "session_recovery_test";
    std::error_code error;
    std::filesystem::create_directories(session, error);
    CHECK(!error);
    if (error) return false;
    {
        std::ofstream artifact(session / "desync_frame_99_slot_0.bin",
                              std::ios::binary);
        artifact << "crash-safe evidence";
    }

    const std::string sessionUtf8 = qplat::narrow(session.wstring());
    qclient::SessionLogWriterLease lease =
        qclient::acquireSessionLogWriterLease(sessionUtf8);
    CHECK(!lease.markerPath.empty());
    if (lease.markerPath.empty()) {
        std::filesystem::remove_all(root, error);
        return false;
    }

    const std::filesystem::path stale = session
        / "._qoh99_writer_429496729_1.lock";
    std::filesystem::rename(
        std::filesystem::path(qplat::widen(lease.markerPath)), stale, error);
    CHECK(!error);
    lease.markerPath.clear();

    const qclient::SessionLogArchiveRecovery recovered =
        qclient::recoverAbandonedSessionLogDirectories(qplat::narrow(root.wstring()));
    CHECK(recovered.inspected == 1u);
    CHECK(recovered.archived == 1u);
    CHECK(recovered.deferred == 0u);
    CHECK(!std::filesystem::exists(session));
    CHECK(std::filesystem::is_regular_file(session.string() + ".zip"));

    std::filesystem::remove_all(root, error);
    CHECK(!error);
    return true;
}

TEST(SessionLogRetentionRemovesOnlyAgedSessionEntries) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("qoh99_session_retention_"
           + std::to_string(static_cast<unsigned long>(GetCurrentProcessId()))
           + "_" + std::to_string(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    CHECK(!error);
    if (error) return false;

    const auto age = [](const std::filesystem::path& path, int days) {
        HANDLE handle = CreateFileW(
            path.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER value{};
        value.LowPart = now.dwLowDateTime;
        value.HighPart = now.dwHighDateTime;
        value.QuadPart -= static_cast<unsigned long long>(days)
            * 24ull * 60ull * 60ull * 10000000ull;
        FILETIME aged{};
        aged.dwLowDateTime = value.LowPart;
        aged.dwHighDateTime = value.HighPart;
        const bool ok = SetFileTime(handle, &aged, &aged, &aged) != FALSE;
        CloseHandle(handle);
        return ok;
    };

    const std::filesystem::path oldDir = root / "session_old";
    std::filesystem::create_directories(oldDir, error);
    { std::ofstream f(oldDir / "client.log"); f << "old"; }
    CHECK(age(oldDir / "client.log", 5));
    CHECK(age(oldDir, 5));

    const std::filesystem::path oldZip = root / "session_old.zip";
    { std::ofstream f(oldZip, std::ios::binary); f << "PK"; }
    CHECK(age(oldZip, 5));

    const std::filesystem::path freshDir = root / "session_fresh";
    std::filesystem::create_directories(freshDir, error);
    { std::ofstream f(freshDir / "client.log"); f << "fresh"; }

    const std::filesystem::path busyDir = root / "session_busy";
    std::filesystem::create_directories(busyDir, error);
    { std::ofstream f(busyDir / "client.log"); f << "still writing"; }
    CHECK(age(busyDir, 5));

    const std::filesystem::path liveDir = root / "session_live";
    std::filesystem::create_directories(liveDir, error);
    { std::ofstream f(liveDir / "client.log"); f << "live"; }
    qclient::SessionLogWriterLease lease =
        qclient::acquireSessionLogWriterLease(
            qplat::narrow(liveDir.wstring()));
    CHECK(!lease.markerPath.empty());
    CHECK(age(liveDir / "client.log", 5));
    CHECK(age(liveDir, 5));

    const std::filesystem::path stranger = root / "notes.txt";
    { std::ofstream f(stranger); f << "keep me"; }
    CHECK(age(stranger, 5));
    const std::filesystem::path strangerDir = root / "replays";
    std::filesystem::create_directories(strangerDir, error);
    CHECK(age(strangerDir, 5));

    const qclient::SessionLogRetentionResult pruned =
        qclient::pruneAgedSessionLogs(qplat::narrow(root.wstring()), 2u);
    CHECK(pruned.removedDirectories == 1u);
    CHECK(pruned.removedArchives == 1u);
    CHECK(pruned.failed == 0u);
    CHECK(pruned.keptInUse == 1u);

    CHECK(!std::filesystem::exists(oldDir));
    CHECK(!std::filesystem::exists(oldZip));
    CHECK(std::filesystem::exists(freshDir));
    CHECK(std::filesystem::exists(busyDir));
    CHECK(std::filesystem::exists(liveDir));
    CHECK(std::filesystem::exists(stranger));
    CHECK(std::filesystem::exists(strangerDir));

    const qclient::SessionLogRetentionResult disabled =
        qclient::pruneAgedSessionLogs(qplat::narrow(root.wstring()), 0u);
    CHECK(disabled.inspected == 0u);
    CHECK(disabled.removedDirectories == 0u);
    CHECK(disabled.removedArchives == 0u);
    CHECK(std::filesystem::exists(freshDir));

    qclient::releaseSessionLogWriterLease(lease);
    std::filesystem::remove_all(root, error);
    return true;
}

TEST(TcpMultipleMessagesInOrder) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    for (uint32_t i = 0; i < 4; i++) {
        Ping p{ i, 0 };
        CHECK(c.sendMsg(MSG_PING, &p, sizeof(p)));
    }
    for (uint32_t i = 0; i < 4; i++) {
        TcpConn::Received rx;
        CHECK(s.recvMsg(rx, 2000));
        CHECK(rx.type == MSG_PING);
        Ping p; std::memcpy(&p, rx.body.data(), sizeof(p));
        CHECK(p.seq == i);
    }
    return true;
}

TEST(TcpBothDirections) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    Ping p{ 11, 0 };
    CHECK(c.sendMsg(MSG_PING, &p, sizeof(p)));
    TcpConn::Received rx;
    CHECK(s.recvMsg(rx, 2000) && rx.type == MSG_PING);
    Ping q{ 22, 0 };
    CHECK(s.sendMsg(MSG_PONG, &q, sizeof(q)));
    CHECK(c.recvMsg(rx, 2000) && rx.type == MSG_PONG);
    return true;
}

TEST(TcpRecvTimeoutNoData) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    TcpConn::Received rx;
    uint64_t t0 = nowUs();
    CHECK(s.recvMsg(rx, 150));
    CHECK(rx.type == MSG_NONE);
    CHECK(nowUs() - t0 >= 100 * 1000);
    return true;
}

TEST(TcpDisconnectDetected) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    c.close();
    TcpConn::Received rx;
    CHECK(!s.recvMsg(rx, 2000));
    CHECK(!s.isOpen());
    return true;
}

TEST(TcpConnectRefusedFails) {
    TcpConn c;
    CHECK(!c.connectTo("127.0.0.1", 42999, 2000));
    CHECK(c.lastError() != 0);
    return true;
}

TEST(TcpRecvMsgExDistinguishesOutcomes) {
    TcpListener lis; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(lis, c, s, port));
    TcpConn::Received rx;
    CHECK(s.recvMsgEx(rx, 120) == TcpConn::Rs::Timeout);
    Ping p{ 1, 0 };
    CHECK(c.sendMsg(MSG_PING, &p, sizeof(p)));
    CHECK(s.recvMsgEx(rx, 2000) == TcpConn::Rs::Msg);
    CHECK(rx.type == MSG_PING);
    c.close();
    CHECK(s.recvMsgEx(rx, 2000) == TcpConn::Rs::Closed);
    CHECK(!s.isOpen());
    return true;
}

TEST(TcpExclusiveListenRejectsSecondBind) {
    TcpListener a, b; TcpConn c, s; uint16_t port;
    CHECK(tcpPair(a, c, s, port));
    CHECK(!b.listen(port));
    CHECK(b.lastError() != 0);
    return true;
}

TEST(ParseHostPortForms) {
    std::string ip; uint16_t port = 0;
    CHECK(::parseHostPort("192.168.1.10:7500", ip, port, 7000) && ip == "192.168.1.10" && port == 7500);
    CHECK(::parseHostPort("192.168.1.10", ip, port, 7000) && port == 7000);
    CHECK(::parseHostPort("  127.0.0.1:1 ", ip, port, 7000) && ip == "127.0.0.1" && port == 1);
    CHECK(!::parseHostPort("192.168.1.10:0", ip, port, 7000));
    CHECK(!::parseHostPort("192.168.1.10:99999", ip, port, 7000));
    CHECK(!::parseHostPort("192.168.1.10:12ab", ip, port, 7000));
    CHECK(!::parseHostPort("not-an-ip:7500", ip, port, 7000));
    CHECK(!::parseHostPort("", ip, port, 7000));
    CHECK(!::parseHostPort("192.168.1.10", ip, port, 0));
    ip = "keep"; port = 1234;
    CHECK(!::parseHostPort("bad:xyz", ip, port, 7000));
    CHECK(ip == "keep" && port == 1234);
    return true;
}

TEST(StrictLegacyAndConfiguredPorts) {
    uint16_t port = 1234;
    CHECK(::parsePortNumber("1", port) && port == 1);
    CHECK(::parsePortNumber("65535", port) && port == 65535);

    port = 1234;
    CHECK(!::parsePortNumber("", port));
    CHECK(!::parsePortNumber("0", port));
    CHECK(!::parsePortNumber("65536", port));
    CHECK(!::parsePortNumber("70000", port));
    CHECK(!::parsePortNumber(
        "184467440737095516161234567890", port));
    CHECK(!::parsePortNumber("12x", port));
    CHECK(!::parsePortNumber("+7500", port));
    CHECK(port == 1234);

    CHECK(::configuredPortOrDefault(1, 7500) == 1);
    CHECK(::configuredPortOrDefault(65535, 7500) == 65535);
    CHECK(::configuredPortOrDefault(0, 7500) == 7500);
    CHECK(::configuredPortOrDefault(-1, 7500) == 7500);
    CHECK(::configuredPortOrDefault(65536, 7500) == 7500);
    return true;
}

TEST(LivePresenceIsBoundedValidatedAndMachineReadable) {
    qnet::LivePresence hosting{};
    hosting.schemaVersion = qnet::kLivePresenceSchemaVersion;
    hosting.state = qnet::LIVE_PRESENCE_HOSTING;
    hosting.roundsToWin = 2u;
    std::memset(hosting.characterIds, 0xFF,
                sizeof(hosting.characterIds));
    std::memcpy(hosting.playerNames[0], "Host", 5u);
    CHECK(::validLivePresence(hosting));
    const std::string hostingJson = ::livePresenceJson(hosting);
    CHECK(hostingJson.find("\"state\":\"hosting\"")
          != std::string::npos);
    CHECK(hostingJson.find("\"session_id\":0")
          != std::string::npos);
    CHECK(hostingJson.find("\"nickname\":\"Host\",")
          != std::string::npos);
    CHECK(hostingJson.find("\"character_id\":null")
          != std::string::npos);

    qnet::LivePresence characterSelect = hosting;
    characterSelect.state = qnet::LIVE_PRESENCE_CHARACTER_SELECT;
    characterSelect.sessionId = 77u;
    std::memcpy(characterSelect.playerNames[1], "Client", 7u);
    CHECK(::validLivePresence(characterSelect));
    const std::string selectJson =
        ::livePresenceJson(characterSelect);
    CHECK(selectJson.find("\"state\":\"character_select\"")
          != std::string::npos);
    CHECK(selectJson.find("\"session_id\":77")
          != std::string::npos);
    CHECK(selectJson.find("\"nickname\":\"Client\",")
          != std::string::npos);
    CHECK(selectJson.find("\"character_id\":null")
          != std::string::npos);

    qnet::LivePresence presence{};
    presence.schemaVersion = qnet::kLivePresenceSchemaVersion;
    presence.state = qnet::LIVE_PRESENCE_MATCH;
    presence.flags = qnet::kLivePresenceRosterValid
                   | qnet::kLivePresenceRoundScoreValid
                   | qnet::kLivePresenceSetScoreValid;
    presence.sessionId = 77u;
    presence.confirmedTimelineFrame = 1234u;
    presence.viewerCount = 2u;
    presence.viewerCapacity = 32u;
    presence.roundsToWin = 2u;
    presence.activeFighterMask = 0x03u;
    presence.characterIds[0] = 0u;
    presence.characterIds[1] = 8u;
    presence.characterIds[2] = 0xFFu;
    presence.characterIds[3] = 0xFFu;
    presence.paletteIndices[0] = 1u;
    presence.paletteIndices[1] = 2u;
    presence.roundWins[0] = 1u;
    presence.matchWins[0] = 3u;
    std::memcpy(presence.playerNames[0], "A\"qua", 6u);
    std::memcpy(presence.playerNames[1], "Player 2", 9u);
    CHECK(::validLivePresence(presence));

    const std::string json = ::livePresenceJson(presence);
    CHECK(json.find("\"state\":\"match\"") != std::string::npos);
    CHECK(json.find("\"nickname\":\"A\\\"qua\"")
          != std::string::npos);
    CHECK(json.find("\"character_id\":0") != std::string::npos);
    CHECK(json.find("\"round_wins\":1") != std::string::npos);
    CHECK(json.find("\"match_wins\":3") != std::string::npos);
    CHECK(json.find("\"current\":2,\"capacity\":32")
          != std::string::npos);
    CHECK(json.find("key") == std::string::npos);
    CHECK(json.find("grant") == std::string::npos);

    qnet::LivePresence malformed = presence;
    malformed.characterIds[0] = 34u;
    CHECK(!::validLivePresence(malformed));
    malformed = presence;
    std::memset(malformed.playerNames[0], 'x',
                sizeof(malformed.playerNames[0]));
    CHECK(!::validLivePresence(malformed));
    return true;
}

TEST(SpectatorSourceCandidateConstruction) {
    uint32_t locals[4]{};
    CHECK(qnet::parseIpv4Text("192.168.50.2", 12, locals[0]));
    CHECK(qnet::parseIpv4Text("10.0.0.4", 8, locals[1]));
    locals[2] = locals[0];
    CHECK(qnet::parseIpv4Text("127.0.0.1", 9, locals[3]));

    qnet::SpectateAddress address{};
    std::string buildError;
    CHECK(::buildSpectateAddress(
        "203.0.113.42", locals, 4, 7501, 3987144498u,
        0x1122334455667788ull, 0x99AABBCCDDEEFF00ull,
        address, buildError));
    CHECK(address.endpointCount == 3);
    CHECK(address.hasKey);
    CHECK(address.sessionId == 3987144498u);
    CHECK(address.keyLow == 0x1122334455667788ull);
    CHECK(address.keyHigh == 0x99AABBCCDDEEFF00ull);
    CHECK(qnet::validateCandidateSet(address.endpoints, address.endpointCount)
          == qnet::CandidateSetError::None);
    bool foundLoopback = false;
    for (size_t i = 0; i < address.endpointCount; ++i) {
        CHECK(address.endpoints[i].kind
              == qnet::CandidateKind::PrivateLan);
        if (qnet::isLoopbackBE(address.endpoints[i].ipv4BE))
            foundLoopback = true;
    }
    CHECK(foundLoopback);

    qnet::SpectateAddress lanOnly{};
    CHECK(::buildSpectateAddress(
        "n/a", locals, 4, 7501, 99,
        0x1111111111111111ull, 0x2222222222222222ull,
        lanOnly, buildError));
    CHECK(lanOnly.endpointCount == 3);
    return true;
}

TEST(SpectatorForcedRelayPreflightIsFailClosed) {
    using qclient::spectator::SpectatorRouteLabel;
    const uint16_t none =
        static_cast<uint16_t>(SpectatorRouteLabel::None);
    const uint16_t direct =
        static_cast<uint16_t>(SpectatorRouteLabel::LanDirect);
    const uint16_t relay =
        static_cast<uint16_t>(SpectatorRouteLabel::Relay);

    CHECK(::qoh99SpectatorPreflightMayLaunch(
        false, false, true, none));

    CHECK(!::qoh99SpectatorPreflightMayLaunch(
        true, false, true, none));
    CHECK(!::qoh99SpectatorPreflightMayLaunch(
        true, false, false, none));
    CHECK(!::qoh99SpectatorPreflightMayLaunch(
        true, true, false, direct));
    CHECK(!::qoh99SpectatorPreflightMayLaunch(
        true, true, true, relay));
    CHECK(::qoh99SpectatorPreflightMayLaunch(
        true, true, false, relay));
    return true;
}

TEST(ReplayStartupPausePreferencePreservesFolderAndPresentationFlags) {
    const uint16_t display = qipc::kReplayShowTransport | qipc::kReplayShowInputs;
    const uint16_t single = display | qipc::kReplayHoldFinal;
    CHECK(::qoh99ReplayStartupFlags(single, false, false)
          == (single | qipc::kReplayAutoStart));
    CHECK(::qoh99ReplayStartupFlags(single, true, false) == single);
    CHECK(::qoh99ReplayStartupFlags(display, false, false)
          == (display | qipc::kReplayAutoStart));
    CHECK(::qoh99ReplayStartupFlags(display, true, false) == display);
    const uint16_t continuation = single | qipc::kReplayAutoStart;
    CHECK(::qoh99ReplayStartupFlags(continuation, true, false) == continuation);
    CHECK(::qoh99ReplayStartupFlags(continuation, false, false) == continuation);
    CHECK(::qoh99ReplayStartupFlags(single, false, true) == 0u);
    CHECK(::qoh99ReplayStartupFlags(continuation, true, true) == 0u);
    return true;
}

TEST(SpectatorCasterLifecyclePolicies) {
    CHECK(::qoh99ClassifyChildExit(true, 0, ERROR_SUCCESS) == 0);
    CHECK(::qoh99ClassifyChildExit(true, 1, ERROR_SUCCESS) == 0);
    CHECK(::qoh99ClassifyChildExit(true, 7, ERROR_SUCCESS) == 7);
    const DWORD accessViolation = 0xC0000005u;
    CHECK(static_cast<DWORD>(::qoh99ClassifyChildExit(
              true, accessViolation, ERROR_SUCCESS))
          == accessViolation);
    CHECK(::qoh99ClassifyChildExit(true, 0, ERROR_TIMEOUT)
          == static_cast<int>(ERROR_TIMEOUT));
    CHECK(::qoh99ClassifyChildExit(true, 1, ERROR_TIMEOUT)
          == static_cast<int>(ERROR_TIMEOUT));
    CHECK(::qoh99ClassifyChildExit(false, STILL_ACTIVE, ERROR_INVALID_HANDLE)
          == static_cast<int>(ERROR_INVALID_HANDLE));
    CHECK(::qoh99ClassifyChildExit(false, STILL_ACTIVE, ERROR_SUCCESS)
          == static_cast<int>(ERROR_GEN_FAILURE));

    CHECK(::qoh99PreferNativeChildExit(0, 1) == 1);
    CHECK(::qoh99PreferNativeChildExit(7, 1) == 7);
    CHECK(::qoh99PreferNativeChildExit(
              ::qoh99ClassifyChildExit(true, 1, ERROR_SUCCESS), 1)
          == 1);

    CHECK(::qoh99SpectatorAdmissionReleaseRequired(true, 11, 13));
    CHECK(!::qoh99SpectatorAdmissionReleaseRequired(false, 11, 13));
    CHECK(!::qoh99SpectatorAdmissionReleaseRequired(true, 0, 13));
    CHECK(!::qoh99SpectatorAdmissionReleaseRequired(true, 11, 0));
    CHECK(::qoh99SpectatorAdmissionReleaseGuardSelfTest());
    return true;
}

TEST(SpectatorE0EvidenceLinesAreRedactedAndCursorAccurate) {
    namespace qs = qclient::spectator;
    qs::SidecarStatus source{};
    source.phase = qs::SidecarPhase::Active;
    source.serving = true;
    source.confirmedRecords = 947;
    source.firstAvailableRecord = 128;
    source.viewerCount = 1;
    source.viewerCapacity = 32;
    const std::string sourceLine =
        ::qoh99SpectatorE0SourceEvidenceLine(source);
    CHECK(sourceLine.find("[spectator-e0] source event=status") == 0);
    CHECK(sourceLine.find("phase=active") != std::string::npos);
    CHECK(sourceLine.find("confirmed_cursor=947") != std::string::npos);
    CHECK(sourceLine.find("first_available_cursor=128") != std::string::npos);

    qs::ViewerStatus viewer{};
    viewer.viewerId = 0x0123456789ABCDEFull;
    viewer.generation = 7;
    CHECK(qnet::parseIpv4Text("203.0.113.7", 11, viewer.endpoint.ipv4BE));
    viewer.endpoint.port = 49246;
    viewer.routeLabel = static_cast<uint16_t>(qs::SpectatorRouteLabel::Punched);
    viewer.state = qs::ViewerServiceState::Streaming;
    viewer.bootstrapRecord = 120;
    viewer.acknowledgedRecord = 321;
    viewer.backlogRecords = 626;
    viewer.bytesSent = 123456;
    viewer.grantTag = "grant-must-not-appear";

    const std::string attachLine =
        ::qoh99SpectatorE0ViewerAttachEvidenceLine(viewer);
    CHECK(attachLine.find("event=hello-accepted") != std::string::npos);
    CHECK(attachLine.find("qsp_proof=accepted") != std::string::npos);
    CHECK(attachLine.find("qsp_capability=accepted") != std::string::npos);
    CHECK(attachLine.find("admission=source-accepted") != std::string::npos);
    CHECK(attachLine.find("compatibility=accepted") != std::string::npos);
    CHECK(attachLine.find("bootstrap_cursor=120") != std::string::npos);
    CHECK(attachLine.find("viewer=0123456789ABCDEF") != std::string::npos);
    CHECK(attachLine.find("generation=7") != std::string::npos);
    CHECK(attachLine.find("route_label=punched") != std::string::npos);
    CHECK(attachLine.find("endpoint=203.0.113.7:49246")
          != std::string::npos);
    CHECK(attachLine.find("endpoint_provenance=source-observed")
          != std::string::npos);
    CHECK(attachLine.find("grant-must-not-appear") == std::string::npos);

    const std::string progressLine =
        ::qoh99SpectatorE0ViewerEvidenceLine(viewer);
    CHECK(progressLine.find("event=status") != std::string::npos);
    CHECK(progressLine.find("ack_cursor=321") != std::string::npos);
    CHECK(progressLine.find("backlog_records=626") != std::string::npos);
    CHECK(progressLine.find("bytes_sent=123456") != std::string::npos);
    CHECK(progressLine.find("bootstrap_cursor=120") != std::string::npos);
    CHECK(progressLine.find("grant-must-not-appear") == std::string::npos);

    qs::ViewerTerminalOutcome detached{};
    detached.viewerId = viewer.viewerId;
    detached.generation = viewer.generation;
    detached.state = qs::ViewerServiceState::Detached;
    detached.acknowledgedRecord = 321;
    const std::string disconnectLine =
        ::qoh99SpectatorE0ViewerDisconnectEvidenceLine(viewer, &detached);
    CHECK(disconnectLine.find("event=viewer-not-active") != std::string::npos);
    CHECK(disconnectLine.find("disconnect_evidence=terminal-outcome")
          != std::string::npos);
    CHECK(disconnectLine.find("disconnect_reason=detached")
          != std::string::npos);
    CHECK(disconnectLine.find("terminal_ack_cursor=321")
          != std::string::npos);
    detached.generation++;
    const std::string unavailableDisconnect =
        ::qoh99SpectatorE0ViewerDisconnectEvidenceLine(viewer, &detached);
    CHECK(unavailableDisconnect.find("disconnect_reason=unavailable")
          != std::string::npos);
    return true;
}

TEST(SpectatorKeyStrictHex) {
    uint64_t lo = 1, hi = 2;
    CHECK(::parseSpectatorKey("99AABBCCDDEEFF001122334455667788", lo, hi));
    CHECK(hi == 0x99AABBCCDDEEFF00ull && lo == 0x1122334455667788ull);
    CHECK(::parseSpectatorKey("0x99AABBCCDDEEFF001122334455667788", lo, hi));
    CHECK(!::parseSpectatorKey("", lo, hi));
    CHECK(!::parseSpectatorKey(std::string(31, 'A'), lo, hi));
    CHECK(!::parseSpectatorKey(std::string(33, 'A'), lo, hi));
    CHECK(!::parseSpectatorKey(std::string(32, 'G'), lo, hi));
    CHECK(!::parseSpectatorKey(std::string(32, '0'), lo, hi));
    return true;
}

TEST(PublicIpv4ResponseNormalization) {
    std::string out = "keep";
    CHECK(qclient::public_ip::normalizeIpv4(
        "  203.0.113.42\r\n", out));
    CHECK(out == "203.0.113.42");
    out = "keep";
    CHECK(!qclient::public_ip::normalizeIpv4("", out));
    CHECK(out == "keep");
    CHECK(!qclient::public_ip::normalizeIpv4(
        "2001:db8::1", out));
    CHECK(!qclient::public_ip::normalizeIpv4(
        "203.0.113.42 extra", out));
    return true;
}

TEST(PublicIpv4HttpResponseValidation) {
    using qclient::public_ip::normalizeHttpIpv4Response;

    std::string out = "keep";
    CHECK(normalizeHttpIpv4Response(
        200, true, "  203.0.113.42\r\n", out));
    CHECK(out == "203.0.113.42");

    out = "keep";
    CHECK(!normalizeHttpIpv4Response(
        200, false, "203.0.113.4", out));
    CHECK(out == "keep");

    CHECK(!normalizeHttpIpv4Response(
        199, true, "203.0.113.42", out));
    CHECK(out == "keep");
    CHECK(normalizeHttpIpv4Response(
        299, true, "203.0.113.42", out));
    CHECK(out == "203.0.113.42");

    out = "keep";
    CHECK(!normalizeHttpIpv4Response(
        300, true, "203.0.113.42", out));
    CHECK(!normalizeHttpIpv4Response(
        0, true, "203.0.113.42", out));
    CHECK(out == "keep");
    return true;
}

struct RelHarness {
    ReliableChannel chA, chB;
    std::vector<std::string> recvAtA, recvAtB;
    uint64_t dropMaskAtoB = 0;
    uint64_t dropMaskBtoA = 0;
    int cntA = 0, cntB = 0;
    uint64_t now = 1000;

    struct Pkt { bool fromA; std::vector<uint8_t> data; };
    std::vector<Pkt> queue;

    void init(bool keepAlive) {
        chA.init(
            [this](const uint8_t* d, size_t n) { queue.push_back({ true,  { d, d + n } }); },
            [this](uint8_t, const uint8_t* d, size_t n) {
                recvAtA.push_back(std::string(reinterpret_cast<const char*>(d), n)); },
            keepAlive, now);
        chB.init(
            [this](const uint8_t* d, size_t n) { queue.push_back({ false, { d, d + n } }); },
            [this](uint8_t, const uint8_t* d, size_t n) {
                recvAtB.push_back(std::string(reinterpret_cast<const char*>(d), n)); },
            keepAlive, now);
    }

    void pump() {
        while (!queue.empty()) {
            Pkt p = queue.front();
            queue.erase(queue.begin());
            bool drop = p.fromA ? ((dropMaskAtoB >> (cntA++)) & 1)
                                : ((dropMaskBtoA >> (cntB++)) & 1);
            if (drop) continue;
            uint8_t t; const uint8_t* b; size_t bl;
            if (unpackMsg(p.data.data(), p.data.size(), t, b, bl)) {
                if (p.fromA) chB.onDatagram(t, b, bl, now);
                else         chA.onDatagram(t, b, bl, now);
            }
        }
    }

    void sendA(const char* s) { chA.send(MSG_MENU_INDEX, s, std::strlen(s), now); pump(); }
    void tick(uint64_t ms) { now += ms; chA.update(now); chB.update(now); pump(); }
};

TEST(RelInOrderDelivery) {
    RelHarness h; h.init(false);
    h.sendA("one");
    h.sendA("two");
    h.sendA("tri");
    CHECK(h.recvAtB.size() == 3);
    CHECK(h.recvAtB[0] == "one" && h.recvAtB[1] == "two" && h.recvAtB[2] == "tri");
    CHECK(h.chA.unackedCount() == 0);
    return true;
}

TEST(RelRetransmitOnLoss) {
    RelHarness h; h.init(false);
    h.dropMaskAtoB = 0b1;
    h.sendA("aa");
    CHECK(h.recvAtB.empty());
    CHECK(h.chA.unackedCount() == 1);
    h.tick(kResendIntervalMs + 1);
    CHECK(h.recvAtB.size() == 1);
    CHECK(h.recvAtB[0] == "aa");
    CHECK(h.chA.unackedCount() == 0);
    return true;
}

TEST(RelGapThenFillInOrder) {
    RelHarness h; h.init(false);
    h.dropMaskAtoB = 0b01;
    h.sendA("m1");
    h.sendA("m2");
    CHECK(h.recvAtB.empty());
    h.tick(kResendIntervalMs + 1);
    CHECK(h.recvAtB.size() == 2);
    CHECK(h.recvAtB[0] == "m1" && h.recvAtB[1] == "m2");
    return true;
}

TEST(RelKeepAliveTimeout) {
    RelHarness h; h.init(true);
    CHECK(!h.chA.isTimedOut(h.now));
    h.dropMaskBtoA = ~0ull;
    h.now += kKeepAliveTimeoutMs + 1;
    CHECK(h.chA.isTimedOut(h.now));
    h.chA.notifyActivity(h.now);
    CHECK(!h.chA.isTimedOut(h.now));
    return true;
}

TEST(TimerAccuracy) {
    uint64_t t0 = nowUs();
    Sleep(100);
    uint64_t dt = nowUs() - t0;
    CHECK(dt > 80 * 1000 && dt < 250 * 1000);
    return true;
}

using namespace qoh99;

TEST(RegistryAddressesInImage) {
    size_t n = 0; const AddressInfo* a = addresses(n);
    CHECK(n > 30);
    for (size_t i = 0; i < n; ++i) {
        CHECK(a[i].va >= kImageBase);
        CHECK(a[i].va < 0x1000000);
        CHECK(a[i].evidence != nullptr && a[i].evidence[0] != '\0');
    }
    return true;
}

TEST(RegistryNoDuplicateGlobals) {
    size_t n = 0; const AddressInfo* a = addresses(n);
    std::set<uintptr_t> seen;
    for (size_t i = 0; i < n; ++i) {
        if (a[i].kind != Kind::Global && a[i].kind != Kind::SaveRegion) continue;
        CHECK(seen.find(a[i].va) == seen.end());
        seen.insert(a[i].va);
    }
    return true;
}

TEST(SaveRegionsNoOverlap) {
    size_t n = 0; const AddressInfo* a = addresses(n);
    uintptr_t b0 = 0, b1 = 0; size_t sz = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!std::strcmp(a[i].name, "matchBlock0")) { b0 = a[i].va; sz = a[i].size; }
        if (!std::strcmp(a[i].name, "matchBlock1")) b1 = a[i].va;
    }
    CHECK(b0 && b1 && sz);
    CHECK(b1 == b0 + sz);
    return true;
}

TEST(IndexedFrameOrdering) {
    IndexedFrame a(0, 5), b(0, 6), c(1, 0);
    CHECK(a < b); CHECK(b < c); CHECK(a < c);
    CHECK(a.index() == 0 && a.frame() == 5);
    CHECK(c.index() == 1 && c.frame() == 0);
    CHECK(IndexedFrame(1, 0).value > IndexedFrame(0, 999).value);
    return true;
}

TEST(InputsContainerSetImmutableGetPredicts) {
    InputsContainer<uint16_t> ic;
    ic.set(0, 0, 0x11);
    ic.set(0, 1, 0x22);
    CHECK(ic.get(0, 0) == 0x11);
    CHECK(ic.get(0, 1) == 0x22);
    ic.set(0, 1, 0x99);
    CHECK(ic.get(0, 1) == 0x22);
    CHECK(ic.get(0, 5) == 0x22);
    ic.assign(0, 1, 0x99);
    CHECK(ic.get(0, 1) == 0x99);
    return true;
}

TEST(InputsContainerLastChangedFrame) {
    InputsContainer<uint16_t> ic;
    ic.clearLastChangedFrame();
    uint16_t predicted[3] = { 0, 0, 0 };
    ic.setRange(0, 0, predicted, 3);
    ic.clearLastChangedFrame();
    uint16_t real[3] = { 0, 0, 0x40 };
    ic.setRange(0, 0, real, 3);
    CHECK(ic.lastChangedFrame().index() == 0);
    CHECK(ic.lastChangedFrame().frame() == 2);
    return true;
}

TEST(PhaseDetectionMainMenu) {
    CHECK(detectPhase({ 1, 0, 0, 2 }) == QohPhase::MainMenu);
    CHECK(detectPhase({ 1, 0, 0, 0 }) == QohPhase::ProcessBoot);
    return true;
}

TEST(PhaseDetectionVerifiedBootSequence) {
    CHECK(detectPhase({ 1, 0, 0, 2 }) == QohPhase::MainMenu);
    CHECK(detectPhase({ 3, 0, 1, 2 }) == QohPhase::ModeSelect);
    CHECK(detectPhase({ 3, 0, 2, 2 }) == QohPhase::CharacterSelect);
    CHECK(detectPhase({ 2, -1, 2, 2 }) == QohPhase::CharaIntro);
    CHECK(detectPhase({ 2, 0, 2, 2 }) == QohPhase::RoundActive);
    CHECK(detectPhase({ 2, 5, 2, 2 }) == QohPhase::RoundKO);
    return true;
}

TEST(PhaseDetectionMatchSubSplit) {
    for (int ms = 1; ms <= 4; ++ms)
        CHECK(detectPhase({ 2, ms, 2, 2 }) == QohPhase::RoundUnknownSub);
    CHECK(policyFor(QohPhase::RoundUnknownSub).sync == InputSyncPolicy::UnknownNoRollback);
    CHECK(policyFor(QohPhase::RoundUnknownSub).rollbackAllowed == false);
    return true;
}

TEST(PhasePolicyRollbackOnlyInGameplay) {
    CHECK(policyFor(QohPhase::RoundActive).sync == InputSyncPolicy::Rollback);
    CHECK(policyFor(QohPhase::RoundActive).rollbackAllowed == true);
    CHECK(policyFor(QohPhase::RoundActive).mask == InputMaskPolicy::FullGameplay);
    CHECK(policyFor(QohPhase::CharacterSelect).sync == InputSyncPolicy::BlockingLockstep);
    CHECK(policyFor(QohPhase::CharacterSelect).rollbackAllowed == false);
    CHECK(policyFor(QohPhase::MainMenu).sync == InputSyncPolicy::LocalOnly);
    CHECK(policyFor(QohPhase::Loading).sync == InputSyncPolicy::NoInput);
    return true;
}

TEST(IntroIsNoControlNotRollback) {
    PhasePolicy in = policyFor(QohPhase::CharaIntro);
    CHECK(in.sync == InputSyncPolicy::NoInput);
    CHECK(in.mask == InputMaskPolicy::None);
    CHECK(in.rollbackAllowed == false);
    CHECK(in.rngMustSync == true);
    PhasePolicy mv = policyFor(QohPhase::PreRoundMovementOnly);
    CHECK(mv.sync == InputSyncPolicy::BlockingLockstep && mv.mask == InputMaskPolicy::DirectionsOnly);
    CHECK(mv.rollbackAllowed == false);
    return true;
}

TEST(NetplayStateMapping) {
    CHECK(toNetplayState(QohPhase::RoundActive) == QohNetplayState::InGame);
    CHECK(toNetplayState(QohPhase::CharacterSelect) == QohNetplayState::CharacterSelect);
    CHECK(toNetplayState(QohPhase::RoundKO) == QohNetplayState::Skippable);
    return true;
}

TEST(InputMaskTableMatchesExecutable) {
    CHECK(QOH_IN_DIR0 == 0x10000 && QOH_IN_DIR1 == 0x20000);
    CHECK(QOH_IN_LEFT == 0x40000 && QOH_IN_RIGHT == 0x80000);
    CHECK(QOH_IN_BTN0 == 1 && QOH_IN_BTN1 == 2 && QOH_IN_BTN2 == 4 && QOH_IN_BTN3 == 8);
    CHECK(QOH_IN_DIRMASK == 0xF0000 && QOH_IN_BTNMASK == 0xF);
    return true;
}

TEST(NetPackEachLogicalBit) {
    const uint32_t bits[] = { QOH_IN_DIR0, QOH_IN_DIR1, QOH_IN_LEFT, QOH_IN_RIGHT,
                              QOH_IN_BTN0, QOH_IN_BTN1, QOH_IN_BTN2, QOH_IN_BTN3 };
    for (uint32_t b : bits) {
        uint32_t packed = qohPackGameplayInputForNet(b, 0);
        CHECK(qohUnpackGameplayInputFromNet(packed) == b);
    }
    return true;
}

TEST(NetPackDirButtonPlacement) {
    uint32_t packed = qohPackGameplayInputForNet(QOH_IN_RIGHT | QOH_IN_BTN2, 0);
    CHECK((packed & 0xF)   == QOH_IN_BTN2);
    CHECK((packed & 0xF00) == ((QOH_IN_RIGHT & 0xF0000) >> 8));
    return true;
}

TEST(NetPackCombosAndSeq) {
    uint32_t logical = QOH_IN_DIR0 | QOH_IN_LEFT | QOH_IN_BTN0 | QOH_IN_BTN3;
    for (uint32_t seq = 0; seq < 5; ++seq) {
        uint32_t packed = qohPackGameplayInputForNet(logical, seq);
        CHECK(qohUnpackGameplayInputFromNet(packed) == logical);
        CHECK(qohUnpackNetSeq(packed) == seq);
    }
    return true;
}

TEST(NetPackSystemBits) {
    uint32_t packed = qohPackGameplayInputForNet(QOH_IN_SYS0 | QOH_IN_SYS1 | QOH_IN_BTN1, 9);
    CHECK(qohUnpackSystemBits(packed) == 0x3);
    CHECK((qohUnpackGameplayInputFromNet(packed) & QOH_IN_SYSMASK) == 0);
    CHECK((qohUnpackGameplayInputFromNet(packed)) == QOH_IN_BTN1);
    return true;
}

TEST(FacingSwapsLeftRightOnly) {
    uint32_t in = QOH_IN_RIGHT | QOH_IN_DIR0 | QOH_IN_BTN2;
    uint32_t out = qohApplyFacing(in, 1);
    CHECK((out & QOH_IN_LEFT) && !(out & QOH_IN_RIGHT));
    CHECK((out & QOH_IN_DIR0) && (out & QOH_IN_BTN2));
    CHECK(qohApplyFacing(in, 0) == in);
    CHECK(qohApplyFacing(QOH_IN_LEFT, 1) == QOH_IN_RIGHT);
    return true;
}

TEST(ChildCommandFieldsMapped) {
    size_t n = 0; const FieldInfo* f = fighterFields(n);
    bool haveDirRing = false, haveBtnHead = false, haveChildPtr = false;
    for (size_t i = 0; i < n; ++i) {
        if (std::strcmp(f[i].owner, "cmd") == 0 && std::strcmp(f[i].name, "dirRing") == 0) {
            haveDirRing = (f[i].offset == 8 && f[i].size == 128);
        }
        if (std::strcmp(f[i].owner, "cmd") == 0 && std::strcmp(f[i].name, "btnHead") == 0)
            haveBtnHead = (f[i].offset == 396);
        if (std::strcmp(f[i].name, "childCmdPtr") == 0)
            haveChildPtr = (f[i].offset == 164 && f[i].confidence == Confidence::VerifiedStatic);
    }
    CHECK(haveDirRing && haveBtnHead && haveChildPtr);
    return true;
}

TEST(SaveProfileNoInternalOverlap) {
    CHECK(qoh99::save::firstOverlap() == -1);
    return true;
}

TEST(SaveProfileRegionsSane) {
    size_t n = 0; const auto* r = qoh99::save::regions(n);
    CHECK(n >= 12);
    bool haveArena = false, haveP1 = false;
    for (size_t i = 0; i < n; ++i) {
        CHECK(r[i].addr >= qoh99::ga::kImageBase && r[i].addr < 0x1000000);
        CHECK(r[i].size > 0);
        CHECK(r[i].evidence && r[i].evidence[0]);
        if (std::strcmp(r[i].name, "actorArena") == 0)
            haveArena = (r[i].size == 2 * qoh99::ga::kArenaBlockSize);
        if (std::strcmp(r[i].name, "fighterP1") == 0)
            haveP1 = (r[i].size == qoh99::fld::kFighterSize);
    }
    CHECK(haveArena && haveP1);
    CHECK(qoh99::save::totalFixedBytes(false) > 0x30000);
    return true;
}

TEST(SaveProfilePointerChildren) {
    size_t n = 0; const auto* p = qoh99::save::pointerRegions(n);
    CHECK(n == 4);
    CHECK(p[0].pointerAddress == qoh99::ga::kFighterP1 + qoh99::fld::kFi_ChildPtr);
    CHECK(p[0].pointeeSize == qoh99::fld::kCmd_SaveSize);
    return true;
}

TEST(VanillaNetInputMsgLayout) {
    using namespace qoh99::vnet;
    CHECK(sizeof(InputMsg) == 12);
    InputMsg m{};
    m.type = MSG_INPUT;
    m.packedInput = buildInputWord(QOH_IN_LEFT | QOH_IN_BTN0, 42);
    CHECK(qohUnpackGameplayInputFromNet(m.packedInput) == (uint32_t)(QOH_IN_LEFT | QOH_IN_BTN0));
    CHECK(qohUnpackNetSeq(m.packedInput) == 42);
    CHECK(kConfigWireSize == 4232 && kConfigBlockOff + kConfigBlockSize == kConfigWireSize);
    return true;
}

static qinput::ProfileBinding padButtonBinding(uint8_t token) {
    qinput::ProfileBinding b{};
    b.type = qinput::CompiledBindingType::ProbeButton;
    b.probeKind = qinput::ProbeKind::GamepadButton;
    b.token = token;
    return b;
}

static qinput::ProfileBinding padAxisBinding(uint8_t token, bool negative) {
    qinput::ProfileBinding b{};
    b.type = qinput::CompiledBindingType::ProbeAxis;
    b.probeKind = qinput::ProbeKind::GamepadAxis;
    b.token = token;
    b.polarity = negative ? qinput::AxisPolarity::Negative
                          : qinput::AxisPolarity::Positive;
    return b;
}

static qinput::ProfileBinding keyBinding(uint8_t dik) {
    qinput::ProfileBinding b{};
    b.type = qinput::CompiledBindingType::KeyboardScan;
    b.dik = dik;
    return b;
}

TEST(ControlsPageShowsPadBindings) {
    qinput::InputProfile pad{};
    pad.id = "default-keyboard";
    pad.sourceMode = qinput::CompiledSourceMode::KeyboardAndController;
    pad.device.guid = "030057564c050000e60c000000016800";
    pad.device.normalizedName = "DualSense Wireless Controller";
    pad.actions[0].push_back(padAxisBinding(1, true));
    pad.actions[1].push_back(padAxisBinding(1, false));
    pad.actions[2].push_back(padAxisBinding(0, true));
    pad.actions[3].push_back(padAxisBinding(0, false));
    pad.actions[4].push_back(padButtonBinding(2));
    pad.actions[7].push_back(padButtonBinding(1));

    const bool padSource = qoh99ControlsUsesController(pad);
    CHECK(padSource);
    CHECK(qoh99ControlsBindingText(pad.actions[0], padSource, true)
          == "Left stick Up");
    CHECK(qoh99ControlsBindingText(pad.actions[3], padSource, true)
          == "Left stick Right");
    CHECK(qoh99ControlsBindingText(pad.actions[4], padSource, true)
          == "X / Square");
    CHECK(qoh99ControlsBindingText(pad.actions[7], padSource, true)
          == "B / Circle");
    CHECK(qoh99ControlsBindingText(pad.actions[5], padSource, true)
          == "not set");
    CHECK(qoh99ControlsBindingText(pad.actions[5], padSource, false) == "-");
    CHECK(qoh99ControlsSourceText(pad, "DualSense Wireless Controller")
          == "controller: DualSense Wireless Controller");
    CHECK(qoh99ControlsSourceText(pad, "")
          == "controller: DualSense Wireless Controller (unplugged)");

    qinput::InputProfile mode1 = pad;
    mode1.sourceMode = qinput::CompiledSourceMode::Controller;
    CHECK(qoh99ControlsUsesController(mode1));
    CHECK(qoh99ControlsBindingText(mode1.actions[4],
                                   qoh99ControlsUsesController(mode1), true)
          == "X / Square");
    return true;
}

static std::vector<uint8_t> testWave(uint16_t format, uint16_t bits,
                                     const std::vector<uint8_t>& data,
                                     bool extraOddChunk) {
    std::vector<uint8_t> w;
    auto u16 = [&](uint16_t v) { w.push_back(uint8_t(v)); w.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(uint8_t(v >> (8 * i))); };
    auto tag = [&](const char* four) { w.insert(w.end(), four, four + 4); };
    tag("RIFF"); u32(0); tag("WAVE");
    tag("fmt "); u32(16); u16(format); u16(1); u32(8000);
    u32(8000u * (bits / 8u)); u16(uint16_t(bits / 8u)); u16(bits);
    if (extraOddChunk) {
        tag("note"); u32(3); w.push_back('a'); w.push_back('b'); w.push_back('c');
        w.push_back(0);
    }
    tag("data"); u32(uint32_t(data.size()));
    w.insert(w.end(), data.begin(), data.end());
    const uint32_t riff = uint32_t(w.size() - 8u);
    for (int i = 0; i < 4; ++i) w[4 + i] = uint8_t(riff >> (8 * i));
    return w;
}

TEST(JoinSoundIsBroughtDownToATargetLoudness) {
    using namespace qclient;
    auto u16At = [](const std::vector<uint8_t>& v, std::size_t at) {
        return uint16_t(v[at] | (v[at + 1] << 8));
    };
    auto u32At = [](const std::vector<uint8_t>& v, std::size_t at) {
        return uint32_t(v[at]) | (uint32_t(v[at + 1]) << 8)
            | (uint32_t(v[at + 2]) << 16) | (uint32_t(v[at + 3]) << 24);
    };
    auto sampleAt = [&](const std::vector<uint8_t>& v, std::size_t index) {
        return int16_t(u16At(v, 44u + index * 2u));
    };

    PcmSamples pcm;
    const std::vector<uint8_t> eight = testWave(1, 8, {0, 128, 255, 192}, true);
    CHECK(decodePcmWave(eight.data(), eight.size(), pcm));
    CHECK(pcm.channels == 1 && pcm.sampleRate == 8000);
    CHECK(pcm.interleaved.size() == 4u);
    if (pcm.interleaved.size() == 4u) {
        CHECK(pcm.interleaved[0] == -1.0);
        CHECK(pcm.interleaved[1] == 0.0);
        CHECK(pcm.interleaved[2] == 127.0 / 128.0);
        CHECK(pcm.interleaved[3] == 0.5);
    }
    const std::vector<uint8_t> pair = testWave(1, 16, {0x00, 0x40, 0x00, 0xC0}, false);
    CHECK(decodePcmWave(pair.data(), pair.size(), pcm));
    CHECK(pcm.interleaved.size() == 2u);
    if (pcm.interleaved.size() == 2u) {
        CHECK(pcm.interleaved[0] == 0.5);
        CHECK(pcm.interleaved[1] == -0.5);
    }

    std::vector<uint8_t> squareData;
    for (int i = 0; i < 8000; ++i) {
        squareData.push_back(0x00);
        squareData.push_back(i % 2 ? 0xC0 : 0x40);
    }
    const std::vector<uint8_t> square = testWave(1, 16, squareData, false);
    CHECK(decodePcmWave(square.data(), square.size(), pcm));
    CHECK(std::fabs(loudestWindowRms(pcm) - 0.5) < 1e-9);

    std::vector<uint8_t> quietThenLoud;
    for (int i = 0; i < 8000; ++i) {
        quietThenLoud.push_back(0x00);
        quietThenLoud.push_back(i % 2 ? 0xF8 : 0x08);
    }
    for (int i = 0; i < 4000; ++i) {
        quietThenLoud.push_back(0x00);
        quietThenLoud.push_back(i % 2 ? 0xC0 : 0x40);
    }
    const std::vector<uint8_t> burst = testWave(1, 16, quietThenLoud, false);
    CHECK(decodePcmWave(burst.data(), burst.size(), pcm));
    CHECK(std::fabs(loudestWindowRms(pcm) - 0.5) < 1e-9);

    double gainDb = 1.0;
    std::vector<uint8_t> out = quietedPcmWave(
        square.data(), square.size(), 20.0 * std::log10(0.25), &gainDb);
    CHECK(std::fabs(gainDb - 20.0 * std::log10(0.5)) < 1e-6);
    CHECK(out.size() == square.size());
    if (out.size() == square.size()) {
        CHECK(std::abs(sampleAt(out, 0) - 8192) <= 1);
        CHECK(std::abs(sampleAt(out, 1) + 8192) <= 1);
    }

    out = quietedPcmWave(square.data(), square.size(), 0.0, &gainDb);
    CHECK(gainDb == 0.0);
    if (out.size() == square.size())
        CHECK(std::abs(sampleAt(out, 0) - 16384) <= 1);

    out = quietedPcmWave(eight.data(), eight.size(), 0.0);
    CHECK(out.size() == 44u + 8u);
    if (out.size() == 44u + 8u) {
        CHECK(std::memcmp(out.data(), "RIFF", 4) == 0);
        CHECK(u32At(out, 4) == out.size() - 8u);
        CHECK(u16At(out, 20) == 1);
        CHECK(u16At(out, 22) == 1);
        CHECK(u32At(out, 24) == 8000);
        CHECK(u32At(out, 28) == 16000);
        CHECK(u16At(out, 32) == 2);
        CHECK(u16At(out, 34) == 16);
        CHECK(u32At(out, 40) == 8);
        CHECK(sampleAt(out, 0) == -32767);
        CHECK(sampleAt(out, 1) == 0);
    }

    const std::vector<uint8_t> silence = testWave(1, 16, {0, 0, 0, 0}, false);
    out = quietedPcmWave(silence.data(), silence.size(), -32.0, &gainDb);
    CHECK(out.size() == 44u + 4u);
    CHECK(gainDb == 0.0);

    const std::vector<uint8_t> adpcm = testWave(0x11, 4, {1, 2, 3, 4}, false);
    CHECK(quietedPcmWave(adpcm.data(), adpcm.size(), -32.0).empty());
    const std::vector<uint8_t> wide = testWave(1, 24, {1, 2, 3}, false);
    CHECK(quietedPcmWave(wide.data(), wide.size(), -32.0).empty());
    std::vector<uint8_t> truncated = eight;
    truncated.resize(truncated.size() - 2u);
    CHECK(quietedPcmWave(truncated.data(), truncated.size(), -32.0).empty());
    const uint8_t notWave[12] = {'R', 'I', 'F', 'X'};
    CHECK(quietedPcmWave(notWave, sizeof(notWave), -32.0).empty());
    CHECK(quietedPcmWave(nullptr, 0, -32.0).empty());
    const std::vector<uint8_t> empty = testWave(1, 16, {}, false);
    CHECK(quietedPcmWave(empty.data(), empty.size(), -32.0).empty());
    CHECK(quietedPcmWave(square.data(), square.size(), 3.0).empty());
    return true;
}

TEST(TranslationKeysAreEnglishAndMissesFallBack) {
    using namespace qi18n;

    CHECK(setLanguage(Language::English));
    CHECK(std::strcmp(tr("Settings"), "Settings") == 0);

    CHECK(setLanguage(Language::Japanese));
    CHECK(std::strcmp(tr("Settings"), "\xE8\xA8\xAD\xE5\xAE\x9A") == 0);

    const char* const absent = "A string that is deliberately untranslated";
    CHECK(tr(absent) == absent);

    CHECK(tr(nullptr) == nullptr);

    CHECK(!setLanguageFromIniValue(7));
    CHECK(language() == Language::Japanese);
    CHECK(setLanguageFromIniValue(0));
    CHECK(language() == Language::English);

    CHECK(setLanguage(Language::Japanese));
    CHECK(translationCount() >= 3u);
    CHECK(std::strcmp(tr("Replays"), "Replays") != 0);

    CHECK(setLanguage(Language::English));
    return true;
}

TEST(ComposedRowsTranslateInSegments) {
    using namespace qi18n;

    CHECK(setLanguage(Language::English));
    CHECK(trText("Who may watch:   anyone with the host address")
          == "Who may watch:   anyone with the host address");

    CHECK(setLanguage(Language::Japanese));

    CHECK(trText("Settings") == std::string(tr("Settings")));

    const std::string label = "Who may watch:   ";
    const std::string value = "anyone with the host address";
    const std::string composed = label + value;
    const std::string wanted =
        std::string(tr(label.c_str())) + tr(value.c_str());
    CHECK(std::strcmp(tr(label.c_str()), label.c_str()) != 0);
    CHECK(std::strcmp(tr(value.c_str()), value.c_str()) != 0);
    CHECK(trText(composed) == wanted);

    const std::string number = label + "32";
    CHECK(trText(number) == std::string(tr(label.c_str())) + "32");

    const std::string notALabel = "Replays and storage";
    CHECK(std::strcmp(tr("Replays"), "Replays") != 0);
    if (std::strcmp(tr(notALabel.c_str()), notALabel.c_str()) == 0) {
        CHECK(trText(notALabel) == notALabel);
    }

    const std::string alien = "qoh99np_record_13228_3981028067";
    CHECK(trText(alien) == alien);
    CHECK(trText("") == "");

    CHECK(setLanguage(Language::English));
    return true;
}

TEST(ControlsPageShowsKeyboardBindings) {
    qinput::InputProfile kb = qinput::makeDefaultKeyboardProfile();
    CHECK(!qoh99ControlsUsesController(kb));
    CHECK(qoh99ControlsSourceText(kb, "") == "keyboard");
    for (int action = 0; action < 8; ++action) {
        const std::string text =
            qoh99ControlsBindingText(kb.actions[action], false, true);
        CHECK(text != "not set");
        CHECK(!text.empty());
    }
    qinput::InputProfile one{};
    one.actions[4].push_back(keyBinding(0x2C));
    CHECK(qoh99ControlsBindingText(one.actions[4], false, true) == "Z");
    CHECK(!qoh99ControlsUsesController(one));

    qinput::InputProfile both{};
    both.sourceMode = qinput::CompiledSourceMode::KeyboardAndController;
    both.actions[4].push_back(keyBinding(0x2C));
    both.actions[4].push_back(padButtonBinding(0));
    CHECK(qoh99ControlsBindingText(both.actions[4], true, true)
          == "A / Cross  +  Z");
    CHECK(qoh99ControlsBindingText(both.actions[4], false, true)
          == "Z  +  A / Cross");

    qinput::InputProfile empty{};
    CHECK(!qoh99ControlsUsesController(empty));
    CHECK(qoh99ControlsSourceText(empty, "") == "keyboard");
    CHECK(qoh99ControlsBindingText(empty.actions[0], false, false) == "-");
    return true;
}

int runAll() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!netInit()) { std::printf("WSAStartup failed\n"); return 1; }
    int failedTests = 0;
    std::printf("Running %zu tests\n\n", cases().size());
    for (const TestCase& tc : cases()) {
        int before = g_checksFailed;
        std::printf("[ RUN  ] %s\n", tc.name);
        bool completed = tc.fn();
        bool pass = completed && g_checksFailed == before;
        std::printf("[ %s ] %s\n", pass ? " OK " : "FAIL", tc.name);
        if (!pass) failedTests++;
    }
    std::printf("\n%d/%zu tests passed, %d checks failed\n",
                static_cast<int>(cases().size()) - failedTests, cases().size(), g_checksFailed);
    netShutdown();
    return failedTests == 0 ? 0 : 1;
}

}

int qoh99_run_tests() { return qtest::runAll(); }
