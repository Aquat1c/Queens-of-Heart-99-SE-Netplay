#include "client/spectator/SpectatorRoute.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "net/Protocol.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Log.h"
#include "net/Autopunch.h"
#include "net/ControlLink.h"
#include "net/SecureRandom.h"
#include "net/RelayClient.h"
#include "net/RelayProtocol.h"
#include "net/Sockets.h"
#endif

namespace qclient::spectator {
namespace {

using namespace qnetplay::spectator;

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr size_t kRecvBuffer =
    kMaxSpectatorDatagramSize + qnet::relay::kSpectatorFrameHeaderSize;
constexpr uint8_t kPunchByte = 0;

}

bool parseSpectateKeyHex(const std::string& text, uint64_t& high, uint64_t& low) {
    const size_t length = text.size();
    size_t begin = 0;
    if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        begin = 2;
    if (length - begin != 32) return false;

    uint64_t parsedHigh = 0;
    uint64_t parsedLow = 0;
    for (size_t i = 0; i < 32; ++i) {
        const char c = text[begin + i];
        uint64_t nibble;
        if (c >= '0' && c <= '9') nibble = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<uint64_t>(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') nibble = static_cast<uint64_t>(c - 'A') + 10u;
        else return false;
        if (i < 16) parsedHigh = (parsedHigh << 4) | nibble;
        else parsedLow = (parsedLow << 4) | nibble;
    }
    if (parsedHigh == 0 && parsedLow == 0) return false;

    high = parsedHigh;
    low = parsedLow;
    return true;
}

const char* routeLabelName(SpectatorRouteLabel label) {
    switch (label) {
    case SpectatorRouteLabel::None: return "none";
    case SpectatorRouteLabel::LanDirect: return "lan-direct";
    case SpectatorRouteLabel::PublicDirect: return "public-direct";
    case SpectatorRouteLabel::PublicDirectHairpin:
        return "public-direct (possible NAT hairpin)";
    case SpectatorRouteLabel::Punched: return "punched";
    case SpectatorRouteLabel::Relay: return "relay";
    }
    return "none";
}

const char* preflightFailureText(PreflightFailure failure) {
    switch (failure) {
    case PreflightFailure::None:
        return "";
    case PreflightFailure::Cancelled:
        return "Spectating was cancelled before the game was started.";
    case PreflightFailure::InvalidTarget:
        return "That spectator address is not usable: it needs a host address "
               "and a live session from the host.";
    case PreflightFailure::WrongKey:
        return "The host was reachable but refused this spectator pass. Try "
               "the host address again.";
    case PreflightFailure::GrantInvalid:
        return "This spectator pass is not valid for that match. Ask the host "
               "for a new one.";
    case PreflightFailure::GrantExpired:
        return "This spectator pass has expired. Ask the host for a new one.";
    case PreflightFailure::StaleSession:
        return "That spectator session has already ended. Try the host address "
               "again.";
    case PreflightFailure::SourceUnavailable:
        return "The host answered but is not running a match to watch right "
               "now.";
    case PreflightFailure::ArchiveUnavailable:
        return "The host is no longer keeping that recording; it has expired.";
    case PreflightFailure::CapacityFull:
        return "The host has reached its spectator limit. Try again shortly.";
    case PreflightFailure::RateLimited:
        return "Too many spectate attempts from this address. Wait a moment "
               "and try again.";
    case PreflightFailure::Unreachable:
        return "No spectator route to the host could be established (direct, "
               "punched, and relay all failed).";
    case PreflightFailure::RelayUnavailable:
        return "The direct route failed and no spectator relay was available "
               "to fall back to.";
    case PreflightFailure::ManifestInvalid:
        return "The host is running a different build or match configuration; "
               "this spectator cannot follow it.";
    case PreflightFailure::HostDeclined:
        return "The host declined this spectate request.";
    case PreflightFailure::Timeout:
        return "The host did not finish the spectator handshake in time.";
    case PreflightFailure::InternalError:
        return "The spectator connection could not be prepared on this "
               "machine.";
    case PreflightFailure::LiveRouteUnavailable:
        return "The host confirmed that this match is live, but no spectator "
               "data route could be reached. The game was not started. "
               "Players and spectators use different sockets, so a match "
               "that plays fine can still have a blocked spectator route; "
               "both casters' logs now record every address that was tried.";
    }
    return "The spectator connection could not be prepared.";
}

bool isDefinitivePreflightFailure(PreflightFailure failure) {
    switch (failure) {
    case PreflightFailure::InvalidTarget:
    case PreflightFailure::WrongKey:
    case PreflightFailure::GrantInvalid:
    case PreflightFailure::GrantExpired:
    case PreflightFailure::StaleSession:
    case PreflightFailure::ArchiveUnavailable:
    case PreflightFailure::CapacityFull:
    case PreflightFailure::RateLimited:
    case PreflightFailure::ManifestInvalid:
    case PreflightFailure::HostDeclined:
    case PreflightFailure::LiveRouteUnavailable:
        return true;

    case PreflightFailure::SourceUnavailable:
    case PreflightFailure::Unreachable:
    case PreflightFailure::RelayUnavailable:
    case PreflightFailure::Timeout:
    case PreflightFailure::InternalError:
        return false;

    case PreflightFailure::Cancelled:
    case PreflightFailure::None:
        return false;
    }
    return false;
}

SpectatorRouteLabel labelForCandidate(const SpectateCandidate& candidate,
                                      uint32_t ownObservedPublicIpv4BE,
                                      bool confirmedByPunch) {
    switch (candidate.kind) {
    case SpectateCandidateKind::Relay:
        return SpectatorRouteLabel::Relay;
    case SpectateCandidateKind::Lan:
        return SpectatorRouteLabel::LanDirect;
    case SpectateCandidateKind::Host:
    case SpectateCandidateKind::Reflexive:
        if (qnet::isAdvertisablePrivateBE(candidate.endpoint.ipv4BE))
            return SpectatorRouteLabel::LanDirect;
        if (ownObservedPublicIpv4BE != 0
            && ownObservedPublicIpv4BE == candidate.endpoint.ipv4BE)
            return SpectatorRouteLabel::PublicDirectHairpin;
        return confirmedByPunch ? SpectatorRouteLabel::Punched
                                : SpectatorRouteLabel::PublicDirect;
    case SpectateCandidateKind::None:
        break;
    }
    return SpectatorRouteLabel::None;
}

std::string redactedTag(uint64_t secret) {
    if (secret == 0) return "#none";
    const uint64_t mixed = splitmix64(secret ^ 0xA5A5A5A5DEADBEEFull);
    char text[8];
    std::snprintf(text, sizeof(text), "#%04x",
                  static_cast<unsigned>(mixed & 0xFFFFu));
    return std::string(text);
}

SessionIdentity SpectateTarget::identity() const {
    SessionIdentity id{};
    id.sessionId = sessionId;
    id.epoch = epoch;
    id.cookie.low = keyLow;
    id.cookie.high = keyHigh;
    return id;
}

bool punchRelayEndpointForTarget(const SpectateTarget& target,
                                 RouteEndpoint& out) {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = target.punchRelayIpv4BE;
    endpoint.port = target.punchRelayPort;
    if (!endpoint.valid()) {
        out = {};
        return false;
    }
    out = endpoint;
    return true;
}

SpectateTarget migratedTarget(const SpectateTarget& previous,
                              uint32_t receivedNextRecord,
                              uint64_t receivedDigest) {
    SpectateTarget next = previous;
    next.generation = previous.generation + 1u;
    if (next.generation == 0) next.generation = 1;
    next.resumeRecord = receivedNextRecord;
    next.resumeDigest = receivedDigest;
    return next;
}

bool decodeSpectateRouteCandidates(
    const qnet::RouteCandidates& wire,
    uint32_t expectedSessionId,
    qnet::CandidateEndpoint* out,
    size_t outCap,
    uint8_t& outCount,
    std::string& error) {
    outCount = 0;
    error.clear();
    if (wire.sessionId != expectedSessionId) {
        error = "the host returned spectator routes for a foreign session";
        return false;
    }
    if (wire.reserved[0] != 0 || wire.reserved[1] != 0
        || wire.reserved[2] != 0) {
        error = "the host returned malformed spectator route metadata";
        return false;
    }
    if (wire.count > qnet::kMaxRouteCandidates || wire.count > outCap
        || (wire.count != 0 && out == nullptr)) {
        error = "the host returned too many spectator routes";
        return false;
    }

    qnet::CandidateEndpoint decoded[qnet::kMaxRouteCandidates]{};
    for (size_t i = 0; i < wire.count; ++i) {
        const qnet::RouteCandidateWire& source = wire.entries[i];
        if (source.routeId == 0) {
            error = "the host returned a spectator route with no id";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (wire.entries[j].routeId == source.routeId) {
                error = "the host returned duplicate spectator route ids";
                return false;
            }
        }
        decoded[i].kind = static_cast<qnet::CandidateKind>(source.kind);
        decoded[i].ipv4BE = source.ipv4BE;
        decoded[i].portBE = source.portBE;
    }
    if (wire.count != 0
        && qnet::validateCandidateSet(decoded, wire.count)
               != qnet::CandidateSetError::None) {
        error = "the host's spectator routes failed validation";
        return false;
    }
    for (size_t i = 0; i < wire.count; ++i) out[i] = decoded[i];
    outCount = wire.count;
    return true;
}

namespace {

struct CandidateProbe {
    SpectateCandidate candidate{};
    uint64_t lastHelloMs = 0;
    uint64_t lastPunchMs = 0;
    bool helloSent = false;
    bool punchEchoed = false;
    uint32_t hellosSent = 0;
    uint32_t punchesSent = 0;
    uint32_t answers = 0;
    uint64_t firstAnswerMs = 0;
};

inline const char* candidateKindName(const SpectateCandidateKind kind) {
    switch (kind) {
        case SpectateCandidateKind::Host:      return "host";
        case SpectateCandidateKind::Lan:       return "lan";
        case SpectateCandidateKind::Reflexive: return "reflexive";
        case SpectateCandidateKind::Relay:     return "relay";
        case SpectateCandidateKind::None:      break;
    }
    return "none";
}

inline void logCandidateProbes(const char* const phase,
                               const std::vector<CandidateProbe>& probes,
                               const uint16_t localPort,
                               const uint64_t elapsedMs,
                               const uint64_t nowMs) {
    QLOG("[spectator-route] stage=%s elapsed=%llums candidates=%zu "
         "viewerPort=%u",
         phase, static_cast<unsigned long long>(elapsedMs), probes.size(),
         static_cast<unsigned>(localPort));
    for (size_t i = 0; i < probes.size(); ++i) {
        const CandidateProbe& probe = probes[i];
        const long long firstAnswerAgo = probe.firstAnswerMs != 0
            ? static_cast<long long>(nowMs - probe.firstAnswerMs)
            : -1;
        QLOG("[spectator-route] stage=%s candidate=%zu kind=%s target=%s:%u "
             "routeId=%u hellos=%u punches=%u answers=%u punchEchoed=%d "
             "firstAnswer=%lldms-ago",
             phase, i, candidateKindName(probe.candidate.kind),
             qnet::formatIpv4BE(probe.candidate.endpoint.ipv4BE).c_str(),
             static_cast<unsigned>(probe.candidate.endpoint.port),
             static_cast<unsigned>(probe.candidate.routeId),
             probe.hellosSent, probe.punchesSent, probe.answers,
             probe.punchEchoed ? 1 : 0, firstAnswerAgo);
    }
}

inline std::string describeCandidateProbes(
    const std::vector<CandidateProbe>& probes) {
    std::string out;
    for (const CandidateProbe& probe : probes) {
        char text[128];
        std::snprintf(text, sizeof(text),
                      "%s%s %s:%u hellos=%u punches=%u answers=%u%s",
                      out.empty() ? "" : "; ",
                      candidateKindName(probe.candidate.kind),
                      qnet::formatIpv4BE(probe.candidate.endpoint.ipv4BE)
                          .c_str(),
                      static_cast<unsigned>(probe.candidate.endpoint.port),
                      probe.hellosSent, probe.punchesSent, probe.answers,
                      probe.punchEchoed ? " (punch echoed)" : "");
        out += text;
    }
    return out;
}

constexpr uint64_t kAutopunchAssistMs = 10000;
constexpr uint64_t kAutopunchLookupRetryMs = 1000;
constexpr uint64_t kRelayLaneReserveMs = 5000;
constexpr unsigned kControlExchangeAttempts = 3;
constexpr uint64_t kControlExchangeRetryReserveMs = 12000;
constexpr uint64_t kControlExchangeRetryPauseMs = 250;
struct PendingPunchLookup {
    uint64_t lookupId = 0;
    uint64_t targetNodeId = 0;
    RouteEndpoint requestedEndpoint{};
    uint64_t routeGeneration = 0;
    uint64_t firstSentMs = 0;
    uint64_t lastSentMs = 0;
    uint64_t expiresMs = 0;
    uint32_t attempts = 0;
};

bool spectatorKindFor(qnet::CandidateKind kind,
                      SpectateCandidateKind& out) {
    switch (kind) {
    case qnet::CandidateKind::PrivateLan:
        out = SpectateCandidateKind::Lan;
        return true;
    case qnet::CandidateKind::PublicObserved:
        out = SpectateCandidateKind::Reflexive;
        return true;
    case qnet::CandidateKind::Relay:
        out = SpectateCandidateKind::None;
        return false;
    case qnet::CandidateKind::None:
        break;
    }
    out = SpectateCandidateKind::None;
    return false;
}

RouteEndpoint endpointFor(const qnet::CandidateEndpoint& candidate) {
    RouteEndpoint endpoint{};
    endpoint.ipv4BE = candidate.ipv4BE;
    endpoint.port = qnet::swapPort16(candidate.portBE);
    return endpoint;
}

bool validateCandidateArray(const qnet::CandidateEndpoint* candidates,
                            size_t count,
                            size_t cap) {
    if (count > cap || count > qnet::kMaxRouteCandidates) return false;
    if (count == 0) return true;
    return qnet::validateCandidateSet(candidates, count)
        == qnet::CandidateSetError::None;
}

bool decodeGrantWire(
    const qnet::SpectatorGrantWire& wire,
    qnetplay::spectator::SpectatorGrant& grant) {
    for (const uint8_t byte : wire.reserved)
        if (byte != 0) return false;
    if (wire.grantId == 0 || wire.viewerId == 0
        || wire.sessionId == 0 || wire.expiryUnix == 0
        || wire.sourceGeneration == 0
        || (wire.macLow == 0 && wire.macHigh == 0)
        || (wire.route != qnet::SPECTATE_ROUTE_DIRECT
            && wire.route != qnet::SPECTATE_ROUTE_PUNCHED
            && wire.route != qnet::SPECTATE_ROUTE_RELAY)) {
        return false;
    }
    grant = {};
    grant.grantId = wire.grantId;
    grant.viewerId = wire.viewerId;
    grant.sessionId = wire.sessionId;
    grant.expiryUnix = wire.expiryUnix;
    grant.sourceGeneration = wire.sourceGeneration;
    grant.resumeRecord = wire.resumeRecord;
    grant.resumeDigest = wire.resumeDigest;
    grant.route = wire.route;
    grant.macLow = wire.macLow;
    grant.macHigh = wire.macHigh;
    return true;
}

void appendCandidate(std::vector<CandidateProbe>& probes,
                     SpectateCandidateKind kind,
                     const RouteEndpoint& endpoint,
                     uint8_t routeId = 0) {
    if (!endpoint.valid()) return;
    if (probes.size() >= kMaxSpectateCandidates) return;
    for (const CandidateProbe& existing : probes)
        if (existing.candidate.endpoint == endpoint) return;
    CandidateProbe probe{};
    probe.candidate.kind = kind;
    probe.candidate.endpoint = endpoint;
    probe.candidate.routeId = routeId != 0
        ? routeId : static_cast<uint8_t>(probes.size() + 1);
    probes.push_back(probe);
}

void appendCandidateSet(std::vector<CandidateProbe>& probes,
                        const qnet::CandidateEndpoint* candidates,
                        size_t count) {
    for (size_t i = 0; i < count; ++i) {
        SpectateCandidateKind kind = SpectateCandidateKind::None;
        if (!spectatorKindFor(candidates[i].kind, kind)) continue;
        appendCandidate(probes, kind, endpointFor(candidates[i]));
    }
}

void appendPunchLookupTarget(std::vector<RouteEndpoint>& targets,
                             const RouteEndpoint& endpoint) {
    if (!endpoint.valid() || targets.size() >= kMaxSpectateCandidates) return;
    for (const RouteEndpoint& existing : targets)
        if (existing == endpoint) return;
    targets.push_back(endpoint);
}

void appendPunchLookupCandidateSet(
    std::vector<RouteEndpoint>& targets,
    const qnet::CandidateEndpoint* candidates,
    size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].kind == qnet::CandidateKind::Relay) continue;
        appendPunchLookupTarget(targets, endpointFor(candidates[i]));
    }
}

PreflightFailure failureForControl(ControlOutcome outcome) {
    switch (outcome) {
    case ControlOutcome::Ok: return PreflightFailure::None;
    case ControlOutcome::NoMatch: return PreflightFailure::SourceUnavailable;
    case ControlOutcome::Refused: return PreflightFailure::HostDeclined;
    case ControlOutcome::CapacityFull: return PreflightFailure::CapacityFull;
    case ControlOutcome::Unreachable: return PreflightFailure::Unreachable;
    case ControlOutcome::Stale: return PreflightFailure::StaleSession;
    case ControlOutcome::Expired: return PreflightFailure::GrantExpired;
    case ControlOutcome::RateLimited: return PreflightFailure::RateLimited;
    case ControlOutcome::Error: break;
    }
    return PreflightFailure::InternalError;
}

PreflightFailure failureForReject(RejectReason reason) {
    switch (reason) {
    case RejectReason::GameUnavailable: return PreflightFailure::SourceUnavailable;
    case RejectReason::ResumeMismatch: return PreflightFailure::StaleSession;
    case RejectReason::ContractMismatch: return PreflightFailure::ManifestInvalid;
    case RejectReason::CapacityReached: return PreflightFailure::CapacityFull;
    case RejectReason::InvalidRequest: return PreflightFailure::InvalidTarget;
    case RejectReason::ArchiveUnavailable: return PreflightFailure::ArchiveUnavailable;
    case RejectReason::GrantInvalid: return PreflightFailure::GrantInvalid;
    case RejectReason::GrantExpired: return PreflightFailure::GrantExpired;
    case RejectReason::MigrationRejected: return PreflightFailure::StaleSession;
    case RejectReason::RateLimited: return PreflightFailure::RateLimited;
    }
    return PreflightFailure::InternalError;
}

const char* waitingTextForPhase(SourcePhase phase) {
    switch (phase) {
    case SourcePhase::Waiting: return "waiting for match start";
    case SourcePhase::Active: return "streaming";
    case SourcePhase::Paused: return "the match is paused";
    case SourcePhase::Checkpointing: return "preparing a mid-match join point";
    case SourcePhase::Sealed: return "the match ended; downloading the tail";
    case SourcePhase::Draining: return "the match ended; downloading the tail";
    case SourcePhase::Retained: return "watching a completed match";
    case SourcePhase::Expired: return "that recording has expired";
    case SourcePhase::Failed: return "the host's recording failed";
    }
    return "waiting for match start";
}

struct Preflight {
    const SpectateTarget& target;
    RouteIo& io;
    const std::function<bool()>& cancelled;
    SpectatePreflightResult result{};
    std::vector<CandidateProbe> probes;
    std::vector<RouteEndpoint> helloedEndpoints;
    SessionIdentity identity{};
    uint32_t sequence = 1;
    bool socketOpen = false;
    bool relayReserved = false;
    bool controlAdmissionHeld = false;
    bool uncontrolledDirectHelloSent = false;
    RouteEndpoint punchMapping{};
    bool havePunchMapping = false;
    std::vector<RouteEndpoint> punchMappings;
    std::vector<PendingPunchLookup> pendingPunchLookups;
    uint64_t punchRouteGeneration = 1;
    uint64_t nextPunchLookupId = 1;
    uint16_t localPort = 0;
    bool anyHelloSent = false;
    bool sawForeignSourceTraffic = false;
    uint32_t ownObservedPublicIpv4BE = 0;
    uint64_t startMs = 0;
    uint32_t punchRegistersSent = 0;
    uint32_t punchLookupsSent = 0;
    uint32_t punchMappingsSeen = 0;
    uint32_t punchMappingsAccepted = 0;

    uint64_t elapsedMs() const { return io.nowMs() - startMs; }

    std::string raceSummaryShort() const {
        uint32_t answered = 0;
        for (const CandidateProbe& probe : probes)
            if (probe.answers != 0) ++answered;
        char text[256];
        const unsigned long long ms = elapsedMs();
        if (probes.empty()) {
            std::snprintf(text, sizeof(text),
                          "no spectator address to try after %llu.%llus",
                          ms / 1000ull, (ms % 1000ull) / 100ull);
        } else if (answered != 0) {
            std::snprintf(text, sizeof(text),
                          "%u of %zu spectator addresses answered but none "
                          "accepted this viewer in %llu.%llus",
                          answered, probes.size(), ms / 1000ull,
                          (ms % 1000ull) / 100ull);
        } else {
            std::snprintf(text, sizeof(text),
                          "none of %zu spectator addresses answered in "
                          "%llu.%llus",
                          probes.size(), ms / 1000ull,
                          (ms % 1000ull) / 100ull);
        }
        std::string out = text;
        if (punchRegistersSent != 0 || punchLookupsSent != 0) {
            std::snprintf(text, sizeof(text),
                          "; punch relay: %u lookups, %u mappings%s",
                          punchLookupsSent, punchMappingsAccepted,
                          punchMappingsAccepted == 0
                              ? " - the relay never reported the host's "
                                "spectator socket, so the host's caster "
                                "may not have registered it"
                              : " - the host's public address was learned "
                                "but did not answer either");
            out += text;
        } else if (target.allowPunch
                   && target.requestedRoute != qnet::SPECTATE_ROUTE_DIRECT) {
            out += "; the hole-punch relay could not be resolved on this "
                   "side, so only the advertised addresses were tried";
        }
        return out;
    }

    std::string raceSummaryLong() const {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "data race: %llums, viewerPort=%u, punch relay "
                      "registers=%u lookups=%u mappings=%u/%u, foreign=%d",
                      static_cast<unsigned long long>(elapsedMs()),
                      static_cast<unsigned>(localPort), punchRegistersSent,
                      punchLookupsSent, punchMappingsAccepted,
                      punchMappingsSeen, sawForeignSourceTraffic ? 1 : 0);
        std::string out = text;
        const std::string candidates = describeCandidateProbes(probes);
        if (!candidates.empty()) out += "; candidates: " + candidates;
        return out;
    }

    Preflight(const SpectateTarget& t, RouteIo& i,
              const std::function<bool()>& c)
        : target(t), io(i), cancelled(c),
          ownObservedPublicIpv4BE(t.observedPublicIpv4BE) {}

    bool isCancelled() const { return cancelled && cancelled(); }

    void recordPunchLookup(const RouteEndpoint& endpoint, uint64_t nowMs) {
        if (!endpoint.valid()) return;
        for (PendingPunchLookup& lookup : pendingPunchLookups) {
            if (!(lookup.requestedEndpoint == endpoint)) continue;
            lookup.routeGeneration = punchRouteGeneration;
            lookup.lastSentMs = nowMs;
            lookup.expiresMs = nowMs + kAutopunchAssistMs;
            ++lookup.attempts;
            return;
        }
        if (pendingPunchLookups.size() >= kMaxSpectateCandidates) return;
        PendingPunchLookup lookup{};
        lookup.lookupId = nextPunchLookupId++;
        if (lookup.lookupId == 0) lookup.lookupId = nextPunchLookupId++;
        lookup.requestedEndpoint = endpoint;
        lookup.routeGeneration = punchRouteGeneration;
        lookup.firstSentMs = nowMs;
        lookup.lastSentMs = nowMs;
        lookup.expiresMs = nowMs + kAutopunchAssistMs;
        lookup.attempts = 1;
        pendingPunchLookups.push_back(lookup);
    }

    bool acceptPunchMapping(const qnet::autopunch::Mapping& mapping,
                            uint64_t nowMs, RouteEndpoint& mapped) {
        mapped = {};
        if (target.punchRelayIpv4BE == 0 || target.punchRelayPort == 0)
            return false;
        if (mapping.ipv4NetworkOrder == target.punchRelayIpv4BE
            && mapping.natPort == target.punchRelayPort) {
            return false;
        }
        if (ownObservedPublicIpv4BE != 0
            && mapping.ipv4NetworkOrder == ownObservedPublicIpv4BE
            && mapping.natPort == localPort) {
            return false;
        }
        for (const PendingPunchLookup& lookup : pendingPunchLookups) {
            if (lookup.routeGeneration != punchRouteGeneration
                || nowMs >= lookup.expiresMs) {
                continue;
            }
            if (lookup.requestedEndpoint.port != mapping.internalPort
                || lookup.requestedEndpoint.ipv4BE
                       != mapping.ipv4NetworkOrder) {
                continue;
            }
            mapped.ipv4BE = mapping.ipv4NetworkOrder;
            mapped.port = mapping.natPort;
            return mapped.valid();
        }
        return false;
    }

    void addPunchMapping(const RouteEndpoint& mapped) {
        if (!mapped.valid()) return;
        for (const RouteEndpoint& existing : punchMappings)
            if (existing == mapped) return;
        if (punchMappings.size() >= kMaxSpectateCandidates) return;
        punchMappings.push_back(mapped);
        punchMapping = mapped;
        havePunchMapping = true;
    }

    void sendHelloTo(const CandidateProbe& probe) {
        HelloPayload hello{};
        hello.simulationContract = target.simulationContract;
        hello.compatibilityIdentity = target.compatibilityIdentity;
        hello.requestedGameId = kLatestGameId;
        hello.viewerId = result.viewerId;
        hello.generation = result.generation;
        hello.viewerNonce = viewerGenerationNonce(result.viewerId,
                                                  result.generation);
        hello.resumeRecord = target.resumeRecord;
        hello.resumeDigest = target.resumeDigest;
        std::vector<uint8_t> payload;
        if (!encodePayload(hello, payload)) return;
        std::vector<uint8_t> datagram;
        if (!encodeDatagram(MessageType::Hello, identity, sequence++, payload,
                            datagram))
            return;
        io.sendTo(probe.candidate.endpoint, datagram.data(), datagram.size());
        anyHelloSent = true;
        if (!controlAdmissionHeld
            && probe.candidate.kind != SpectateCandidateKind::Relay) {
            uncontrolledDirectHelloSent = true;
        }
        bool known = false;
        for (const RouteEndpoint& endpoint : helloedEndpoints)
            if (endpoint == probe.candidate.endpoint) known = true;
        if (!known) helloedEndpoints.push_back(probe.candidate.endpoint);
    }

    void sendBestEffortDetach() {
        if (anyHelloSent && socketOpen) {
            DetachPayload detach{};
            detach.viewerId = result.viewerId;
            detach.generation = result.generation;
            detach.nextRecord = target.resumeRecord;
            detach.digest = target.resumeDigest;
            std::vector<uint8_t> payload;
            std::vector<uint8_t> datagram;
            if (encodePayload(detach, payload)
                && encodeDatagram(MessageType::Detach, identity, sequence++,
                                  payload, datagram)) {
                for (const RouteEndpoint& endpoint : helloedEndpoints) {
                    io.sendTo(endpoint, datagram.data(), datagram.size());
                    result.detachSent = true;
                }
            }
        }
    }

    bool releaseControlAdmission(std::string& error) {
        if (!controlAdmissionHeld) {
            error.clear();
            return true;
        }
        if (!io.controlRelease(target, result.viewerId, error)) return false;
        controlAdmissionHeld = false;
        return true;
    }

    void releaseEverything() {
        sendBestEffortDetach();
        bool released = true;
        std::string releaseError;
        if (!releaseControlAdmission(releaseError)) released = false;
        if (uncontrolledDirectHelloSent) released = false;
        if (relayReserved) {
            io.relayRelease();
            relayReserved = false;
        }
        result.reservationsReleased = released;
        if (socketOpen) {
            io.closeViewerSocket();
            socketOpen = false;
        }
    }

    SpectatePreflightResult finishFailure(PreflightFailure failure,
                                          const std::string& extra = {}) {
        releaseEverything();
        result.ok = false;
        result.failure = failure;
        result.inconclusive = !isDefinitivePreflightFailure(failure)
                           && failure != PreflightFailure::Cancelled;
        result.failureReason = preflightFailureText(failure);
        if (!extra.empty()) result.failureReason += " (" + extra + ")";
        result.processCreated = false;
        result.elapsedMs =
            static_cast<unsigned>(io.nowMs() - startMs);
        return result;
    }

    SpectatePreflightResult finishSuccess() {
        result.ok = true;
        result.failure = PreflightFailure::None;
        result.inconclusive = false;
        result.failureReason.clear();
        result.processCreated = false;
        result.reservationsReleased = false;
        result.elapsedMs = static_cast<unsigned>(io.nowMs() - startMs);
        result.nextSequence = sequence;
        return result;
    }
};

int pumpRace(Preflight& state,
             uint64_t deadlineMs,
             PreflightFailure& failure,
             std::string& detail) {
    uint8_t buffer[kRecvBuffer];
    for (int drained = 0; drained < 64; ++drained) {
        RouteEndpoint from{};
        const int received = state.io.recvFrom(buffer, sizeof(buffer), from);
        if (received == 0) break;
        if (received < 0) {
            failure = PreflightFailure::InternalError;
            detail = "spectator socket read failed";
            return -1;
        }
        if (received == static_cast<int>(qnet::autopunch::kMappingBytes)
            && state.target.punchRelayIpv4BE != 0
            && from.ipv4BE == state.target.punchRelayIpv4BE
            && from.port == state.target.punchRelayPort) {
            qnet::autopunch::Mapping mapping{};
            RouteEndpoint mapped{};
            ++state.punchMappingsSeen;
            if (qnet::autopunch::decodeMapping(
                    buffer, static_cast<size_t>(received), mapping)
                && state.acceptPunchMapping(mapping, state.io.nowMs(),
                                            mapped)) {
                const bool fresh = std::none_of(
                    state.punchMappings.begin(), state.punchMappings.end(),
                    [&](const RouteEndpoint& e) { return e == mapped; });
                state.addPunchMapping(mapped);
                if (fresh) {
                    ++state.punchMappingsAccepted;
                    QLOG("[spectator-route] stage=assist-mapping elapsed=%llums "
                         "target=%s:%u internalPort=%u",
                         static_cast<unsigned long long>(state.elapsedMs()),
                         qnet::formatIpv4BE(mapped.ipv4BE).c_str(),
                         static_cast<unsigned>(mapped.port),
                         static_cast<unsigned>(mapping.internalPort));
                }
                appendCandidate(state.probes, SpectateCandidateKind::Reflexive,
                                mapped);
            } else {
                qnet::autopunch::Mapping decoded{};
                const bool readable = qnet::autopunch::decodeMapping(
                    buffer, static_cast<size_t>(received), decoded);
                std::string asked;
                for (const PendingPunchLookup& lookup :
                     state.pendingPunchLookups) {
                    if (!asked.empty()) asked += ",";
                    asked += qnet::formatIpv4BE(
                                 lookup.requestedEndpoint.ipv4BE)
                        + ":" + std::to_string(lookup.requestedEndpoint.port);
                }
                QLOG("[spectator-route] stage=assist-mapping elapsed=%llums "
                     "result=rejected decoded=%d mapping=%s:%u "
                     "internalPort=%u asked=[%s]: the relay answered about a "
                     "registration this viewer never looked up",
                     static_cast<unsigned long long>(state.elapsedMs()),
                     readable ? 1 : 0,
                     readable
                         ? qnet::formatIpv4BE(decoded.ipv4NetworkOrder).c_str()
                         : "?",
                     static_cast<unsigned>(decoded.natPort),
                     static_cast<unsigned>(decoded.internalPort),
                     asked.c_str());
            }
            continue;
        }

        CandidateProbe* probe = nullptr;
        for (CandidateProbe& candidate : state.probes) {
            if (candidate.candidate.endpoint == from) {
                probe = &candidate;
                break;
            }
        }
        if (!probe) continue;
        ++probe->answers;
        if (probe->firstAnswerMs == 0) {
            probe->firstAnswerMs = state.io.nowMs();
            QLOG("[spectator-route] stage=first-answer elapsed=%llums kind=%s "
                 "from=%s:%u bytes=%d",
                 static_cast<unsigned long long>(state.elapsedMs()),
                 candidateKindName(probe->candidate.kind),
                 qnet::formatIpv4BE(from.ipv4BE).c_str(),
                 static_cast<unsigned>(from.port), received);
        }

        if (received == 1) {
            probe->punchEchoed = true;
            continue;
        }

        Datagram datagram{};
        const DecodeError error =
            decodeDatagram(buffer, static_cast<size_t>(received),
                           state.identity, datagram);
        if (error != DecodeError::None) {
            if (error == DecodeError::CookieMismatch
                || error == DecodeError::SessionMismatch
                || error == DecodeError::EpochMismatch)
                state.sawForeignSourceTraffic = true;
            continue;
        }

        switch (datagram.header.type) {
        case MessageType::GameStart: {
            GameStartPayload start{};
            if (!decodePayload(datagram.payload, start)) break;
            if (start.viewerId != state.result.viewerId
                || start.generation != state.result.generation)
                break;
            if (state.target.simulationContract != 0
                && start.simulationContract != state.target.simulationContract) {
                failure = PreflightFailure::ManifestInvalid;
                detail = "simulation contract mismatch";
                return -1;
            }
            const qnetplay::compat::IdentityMismatch mismatch =
                qnetplay::compat::compatibilityMismatch(
                    state.target.compatibilityIdentity,
                    start.compatibilityIdentity);
            if (mismatch != qnetplay::compat::IdentityMismatch::None) {
                failure = PreflightFailure::ManifestInvalid;
                detail =
                    std::string("cannot watch this match: ")
                    + qnetplay::compat::identityMismatchPlayerText(mismatch)
                    + ". ["
                    + qnetplay::compat::identityMismatchReason(mismatch)
                    + "]";
                return -1;
            }
            state.result.hasManifest = true;
            state.result.manifest = start;
            state.result.gameId = start.gameId;
            state.result.manifestDigest = gameStartFingerprint(start);
            state.result.firstAvailableRecord = start.resumeRecord;
            state.result.waitingForStart = false;
            state.result.statusText = "streaming";
            state.result.confirmedEndpoint = from;
            state.result.confirmedKind = probe->candidate.kind;
            state.result.routeLabel = static_cast<uint16_t>(
                labelForCandidate(probe->candidate,
                                  state.ownObservedPublicIpv4BE,
                                  probe->punchEchoed));
            return 1;
        }
        case MessageType::Heartbeat: {
            HeartbeatPayload beat{};
            if (!decodePayload(datagram.payload, beat)) break;
            if (!isKnownSourcePhase(beat.sourcePhase)) break;
            if (beat.sourcePhase == SourcePhase::Expired) {
                failure = PreflightFailure::ArchiveUnavailable;
                return -1;
            }
            if (beat.sourcePhase == SourcePhase::Failed) {
                failure = PreflightFailure::SourceUnavailable;
                detail = "the host's recording failed";
                return -1;
            }
            state.result.waitingForStart =
                beat.sourcePhase == SourcePhase::Waiting
                || beat.sourcePhase == SourcePhase::Paused;
            state.result.statusText = waitingTextForPhase(beat.sourcePhase);
            state.result.firstAvailableRecord = beat.checkpointRecord;
            state.result.confirmedEndpoint = from;
            state.result.confirmedKind = probe->candidate.kind;
            state.result.routeLabel = static_cast<uint16_t>(
                labelForCandidate(probe->candidate,
                                  state.ownObservedPublicIpv4BE,
                                  probe->punchEchoed));
            return 1;
        }
        case MessageType::Reject: {
            RejectPayload reject{};
            if (!decodePayload(datagram.payload, reject)) break;
            if (reject.viewerId != 0
                && reject.viewerId != state.result.viewerId)
                break;
            failure = failureForReject(reject.reason);
            if (reject.reason == RejectReason::ResumeMismatch
                && state.target.resumeRecord == 0
                && state.target.resumeDigest == 0) {
                failure = PreflightFailure::ArchiveUnavailable;
                detail = "the host no longer holds the beginning of this set: "
                         "its recording keeps roughly the last three hours, "
                         "and a set that long can only be joined at its "
                         "latest checkpoint (spectate --latest)";
            }
            return -1;
        }
        default:
            break;
        }
    }
    if (state.io.nowMs() >= deadlineMs) return 0;
    return 0;
}

void sendProbeBursts(Preflight& state, const uint64_t now) {
    const uint8_t punch = kPunchByte;
    for (CandidateProbe& probe : state.probes) {
        if (state.target.allowPunch
            && (!probe.helloSent
                || now - probe.lastPunchMs >= state.target.punchIntervalMs)) {
            state.io.sendTo(probe.candidate.endpoint, &punch, 1);
            probe.lastPunchMs = now;
            ++probe.punchesSent;
        }
        if (!probe.helloSent
            || now - probe.lastHelloMs >= state.target.helloIntervalMs) {
            state.sendHelloTo(probe);
            probe.helloSent = true;
            probe.lastHelloMs = now;
            ++probe.hellosSent;
        }
    }
}

int raceCandidates(Preflight& state,
                   uint64_t phaseDeadlineMs,
                   PreflightFailure& failure,
                   std::string& detail) {
    if (state.io.nowMs() >= phaseDeadlineMs) return 0;
    while (true) {
        if (state.isCancelled()) {
            failure = PreflightFailure::Cancelled;
            return -1;
        }
        sendProbeBursts(state, state.io.nowMs());
        const int outcome = pumpRace(state, phaseDeadlineMs, failure, detail);
        if (outcome != 0) return outcome;
        if (state.io.nowMs() >= phaseDeadlineMs) return 0;
        state.io.waitMs(10);
    }
}

}

SpectatePreflightResult preflightSpectateWith(
    const SpectateTarget& target,
    unsigned timeoutMs,
    const std::function<bool()>& cancelled,
    RouteIo& io) {
    Preflight state(target, io, cancelled);
    state.startMs = io.nowMs();
    const uint64_t deadlineMs = state.startMs + timeoutMs;

    const bool structurallyBoundRelay =
        !target.relayHostText.empty() && target.relayPort != 0
        && qnet::relay::isValidId(target.relayRoom)
        && qnet::relay::isValidId(target.relayViewerAccessToken);
    const bool validRequestedRoute =
        target.requestedRoute == 0
        || target.requestedRoute == qnet::SPECTATE_ROUTE_DIRECT
        || target.requestedRoute == qnet::SPECTATE_ROUTE_PUNCHED
        || target.requestedRoute == qnet::SPECTATE_ROUTE_RELAY;
    if (target.hostText.empty()
        || (target.hostPort == 0 && target.directUdpPort == 0
            && !structurallyBoundRelay)
        || target.sessionId == 0
        || (target.keyLow == 0 && target.keyHigh == 0)
        || !qnetplay::compat::isValidCompatibilityIdentity(
            target.compatibilityIdentity)
        || !validRequestedRoute) {
        return state.finishFailure(PreflightFailure::InvalidTarget);
    }
    if (!validateCandidateArray(target.initialCandidates,
                                target.initialCandidateCount,
                                kMaxInitialSpectateCandidates)) {
        return state.finishFailure(
            PreflightFailure::InvalidTarget,
            "the spectate URI candidate set failed validation");
    }

    state.identity = target.identity();
    if (!isValidIdentity(state.identity))
        return state.finishFailure(PreflightFailure::InvalidTarget);

    state.result.viewerId = target.viewerId;
    if (state.result.viewerId == 0) {
        bool degraded = false;
        state.result.viewerId = qnet::secureRandom64(degraded);
        if (state.result.viewerId == 0) state.result.viewerId = 1;
    }
    state.result.generation = target.generation == 0 ? 1u : target.generation;

    if (state.isCancelled())
        return state.finishFailure(PreflightFailure::Cancelled);

    uint16_t boundPort = 0;
    std::string socketError;
    if (!io.openViewerSocket(boundPort, socketError))
        return state.finishFailure(PreflightFailure::InternalError, socketError);
    state.socketOpen = true;
    state.localPort = boundPort;

    SpectateTarget admissionTarget = target;
    admissionTarget.viewerDataPort = boundPort;
    if (admissionTarget.requestedRoute == 0)
        admissionTarget.requestedRoute = qnet::SPECTATE_ROUTE_DIRECT;
    ControlAnswer answer{};
    for (unsigned attempt = 1;; ++attempt) {
        io.controlExchange(admissionTarget, answer);
        const bool transient =
            answer.outcome == ControlOutcome::Unreachable
            || answer.outcome == ControlOutcome::Error;
        if (!transient || attempt >= kControlExchangeAttempts) break;
        if (state.isCancelled())
            return state.finishFailure(PreflightFailure::Cancelled);
        if (io.nowMs() + kControlExchangeRetryReserveMs >= deadlineMs) break;
        QLOG("[spectator-route] stage=control-retry attempt=%u/%u "
             "elapsed=%llums outcome=%u reason='%s'",
             attempt + 1u, kControlExchangeAttempts,
             static_cast<unsigned long long>(state.elapsedMs()),
             static_cast<unsigned>(answer.outcome), answer.error.c_str());
        io.waitMs(static_cast<unsigned>(kControlExchangeRetryPauseMs));
    }
    if (answer.outcome != ControlOutcome::Ok)
        return state.finishFailure(failureForControl(answer.outcome),
                                   answer.error);
    state.result.sourceConfirmedLive = answer.sourceConfirmedLive;
    if (answer.identity.sessionId != 0
        && answer.identity.sessionId != target.sessionId)
        return state.finishFailure(PreflightFailure::StaleSession);
    if (target.simulationContract != 0 && answer.simulationContract != 0
        && answer.simulationContract != target.simulationContract)
        return state.finishFailure(PreflightFailure::ManifestInvalid,
                                   "simulation contract mismatch");
    if (!answer.sourceConfirmedLive && answer.hasGrant) {
        return state.finishFailure(
            PreflightFailure::ManifestInvalid,
            "an unconfirmed spectator control answer cannot carry a grant");
    }
    if (answer.sourceConfirmedLive) {
        const qnetplay::compat::IdentityMismatch controlMismatch =
            qnetplay::compat::compatibilityMismatch(
                target.compatibilityIdentity, answer.compatibilityIdentity);
        if (controlMismatch != qnetplay::compat::IdentityMismatch::None) {
            return state.finishFailure(
                PreflightFailure::ManifestInvalid,
                std::string("cannot watch this match: ")
                    + qnetplay::compat::identityMismatchPlayerText(
                        controlMismatch)
                    + ". ["
                    + qnetplay::compat::identityMismatchReason(controlMismatch)
                    + "]");
        }
    }
    if (answer.viewerCapacity != 0
        && answer.viewerCount >= answer.viewerCapacity && !answer.hasGrant)
        return state.finishFailure(PreflightFailure::CapacityFull);
    if (!validateCandidateArray(answer.candidates, answer.candidateCount,
                                kMaxControlSpectateCandidates)) {
        return state.finishFailure(
            PreflightFailure::ManifestInvalid,
            "the host's spectator candidate set failed validation");
    }
    if (answer.observedPublicIpv4BE != 0)
        state.ownObservedPublicIpv4BE = answer.observedPublicIpv4BE;
    state.punchRouteGeneration = answer.sourceGeneration != 0
        ? answer.sourceGeneration
        : state.result.generation;

    if (answer.hasGrant) {
        state.controlAdmissionHeld = true;
        if (answer.grant.route != admissionTarget.requestedRoute) {
            return state.finishFailure(
                PreflightFailure::GrantInvalid,
                "the host returned a spectator pass for a different route");
        }
        state.result.grant = answer.grant;
        if (answer.grant.sourceGeneration != 0)
            state.punchRouteGeneration = answer.grant.sourceGeneration;
        if (answer.grant.viewerId != 0)
            state.result.viewerId = answer.grant.viewerId;
    } else if (answer.viewerId != 0) {
        state.result.viewerId = answer.viewerId;
    }
    if (!answer.statusText.empty()) state.result.statusText = answer.statusText;

    SpectateTarget relayTarget = target;
    if (qnet::relay::isValidId(answer.relayRoom)) {
        relayTarget.relayRoom = answer.relayRoom;
        relayTarget.relayViewerAccessToken =
            answer.relayViewerAccessToken;
        for (size_t i = 0; i < answer.candidateCount; ++i) {
            if (answer.candidates[i].kind
                != qnet::CandidateKind::Relay) {
                continue;
            }
            relayTarget.relayHostText =
                qnet::formatIpv4BE(answer.candidates[i].ipv4BE);
            relayTarget.relayPort =
                qnet::swapPort16(answer.candidates[i].portBE);
            break;
        }
    }

    appendCandidateSet(state.probes, target.initialCandidates,
                       target.initialCandidateCount);
    std::vector<RouteEndpoint> punchLookupTargets;
    appendPunchLookupCandidateSet(
        punchLookupTargets, target.initialCandidates,
        target.initialCandidateCount);

    if (answer.sourceConfirmedLive && answer.sourceUdpPort != 0) {
        RouteEndpoint loopback{};
        loopback.ipv4BE = qnet::kLoopbackIpv4BE;
        loopback.port = answer.sourceUdpPort;
        appendCandidate(state.probes, SpectateCandidateKind::Lan, loopback);
        appendPunchLookupTarget(punchLookupTargets, loopback);
    }

    appendCandidateSet(state.probes, answer.candidates,
                       answer.candidateCount);
    appendPunchLookupCandidateSet(
        punchLookupTargets, answer.candidates, answer.candidateCount);
    appendCandidate(state.probes, SpectateCandidateKind::Reflexive,
                    answer.sourceDirect);
    appendPunchLookupTarget(punchLookupTargets, answer.sourceDirect);
    if (target.directUdpPort != 0) {
        RouteEndpoint typed{};
        std::string resolveError;
        if (io.resolveHost(target.hostText, target.directUdpPort, typed,
                           resolveError)) {
            appendCandidate(state.probes, SpectateCandidateKind::Host, typed);
            appendPunchLookupTarget(punchLookupTargets, typed);
        }
    }
    const bool relayCandidateAvailable =
        (target.requestedRoute == 0
            || target.requestedRoute == qnet::SPECTATE_ROUTE_RELAY)
        &&
        relayTarget.allowRelayFallback
        && !relayTarget.relayHostText.empty() && relayTarget.relayPort != 0
        && qnet::relay::isValidId(relayTarget.relayRoom)
        && (qnet::relay::isValidId(relayTarget.relayViewerAccessToken)
            || state.result.grant.grantId != 0);
    if (state.probes.empty() && !relayCandidateAvailable) {
        return state.finishFailure(
            answer.sourceConfirmedLive
                ? PreflightFailure::LiveRouteUnavailable
                : PreflightFailure::Unreachable,
            "the host advertised no spectator route");
    }

    logCandidateProbes("race-start", state.probes, state.localPort,
                       state.elapsedMs(), io.nowMs());
    const bool relayLanePossible =
        relayTarget.allowRelayFallback
        && (target.requestedRoute == 0
            || target.requestedRoute == qnet::SPECTATE_ROUTE_RELAY)
        && !relayTarget.relayHostText.empty()
        && relayTarget.relayPort != 0;
    const uint64_t relayReserveMs = relayLanePossible
        ? (std::min<uint64_t>)(kRelayLaneReserveMs, timeoutMs / 3u)
        : 0u;
    const uint64_t laneDeadlineMs =
        deadlineMs > state.startMs + relayReserveMs
            ? deadlineMs - relayReserveMs
            : state.startMs;
    PreflightFailure failure = PreflightFailure::Timeout;
    std::string detail;
    int outcome = 0;
    const auto finishRaceSuccess = [&state]() {
        if (state.result.confirmedKind == SpectateCandidateKind::Reflexive) {
            state.result.routeLabel =
                static_cast<uint16_t>(SpectatorRouteLabel::Punched);
        }
        return state.finishSuccess();
    };
    if (target.requestedRoute != qnet::SPECTATE_ROUTE_RELAY) {
        uint64_t raceDeadline = state.startMs + target.directRaceMs;
        if (raceDeadline > deadlineMs) raceDeadline = deadlineMs;
        outcome = raceCandidates(state, raceDeadline, failure, detail);
        if (outcome < 0) return state.finishFailure(failure, detail);
        if (outcome > 0) return finishRaceSuccess();
        if (target.directRaceMs != 0) {
            QLOG("[spectator-route] stage=direct-race-exhausted elapsed=%llums "
                 "candidates=%zu foreignTraffic=%d",
                 static_cast<unsigned long long>(state.elapsedMs()),
                 state.probes.size(), state.sawForeignSourceTraffic ? 1 : 0);
        }
    }

    const bool forcedPunch =
        target.requestedRoute == qnet::SPECTATE_ROUTE_PUNCHED;
    bool autopunchAttempted = false;
    if (target.allowPunch
        && target.requestedRoute != qnet::SPECTATE_ROUTE_DIRECT
        && target.requestedRoute != qnet::SPECTATE_ROUTE_RELAY
        && target.punchRelayIpv4BE != 0
        && target.punchRelayPort != 0
        && !punchLookupTargets.empty()
        && io.nowMs() < deadlineMs) {
        RouteEndpoint relayEp{};
        relayEp.ipv4BE = target.punchRelayIpv4BE;
        relayEp.port = target.punchRelayPort;

        if (forcedPunch) state.probes.clear();

        uint8_t reg[qnet::autopunch::kRegisterBytes]{};
        const bool haveReg =
            qnet::autopunch::encodeRegister(state.localPort, reg);

        uint64_t punchDeadline = io.nowMs() + kAutopunchAssistMs;
        if (punchDeadline > laneDeadlineMs) punchDeadline = laneDeadlineMs;
        uint64_t nextRequestMs = io.nowMs();
        QLOG("[spectator-route] stage=assist-start elapsed=%llums "
             "window=%llums relay=%s:%u lookupTargets=%zu forced=%d",
             static_cast<unsigned long long>(state.elapsedMs()),
             static_cast<unsigned long long>(
                 punchDeadline > io.nowMs() ? punchDeadline - io.nowMs() : 0),
             qnet::formatIpv4BE(target.punchRelayIpv4BE).c_str(),
             static_cast<unsigned>(target.punchRelayPort),
             punchLookupTargets.size(), forcedPunch ? 1 : 0);
        while (haveReg && io.nowMs() < punchDeadline) {
            if (state.isCancelled())
                return state.finishFailure(PreflightFailure::Cancelled);
            const uint64_t now = io.nowMs();
            sendProbeBursts(state, now);
            if (!state.havePunchMapping && now >= nextRequestMs) {
                const bool registered =
                    io.sendTo(relayEp, reg, sizeof(reg));
                bool lookupSent = false;
                if (registered) {
                    ++state.punchRegistersSent;
                    for (const RouteEndpoint& lookupTarget :
                         punchLookupTargets) {
                        uint8_t lookup[qnet::autopunch::kLookupBytes]{};
                        if (qnet::autopunch::encodeLookup(
                                state.localPort, lookupTarget.ipv4BE,
                                lookupTarget.port, lookup)
                            && io.sendTo(relayEp, lookup, sizeof(lookup))) {
                            state.recordPunchLookup(lookupTarget, now);
                            ++state.punchLookupsSent;
                            lookupSent = true;
                        }
                    }
                }
                autopunchAttempted = autopunchAttempted
                    || (registered && lookupSent);
                nextRequestMs = now + kAutopunchLookupRetryMs;
            }
            PreflightFailure punchFailure = PreflightFailure::Timeout;
            std::string punchDetail;
            const int punchOutcome =
                pumpRace(state, punchDeadline, punchFailure, punchDetail);
            if (punchOutcome < 0)
                return state.finishFailure(punchFailure, punchDetail);
            if (punchOutcome > 0) return finishRaceSuccess();
            const uint64_t afterPump = io.nowMs();
            if (afterPump >= punchDeadline) break;
            const uint64_t nextWake = state.havePunchMapping
                ? (std::min)(afterPump + 10u, punchDeadline)
                : (std::min)(nextRequestMs, punchDeadline);
            const uint64_t untilWake =
                nextWake > afterPump ? nextWake - afterPump : 1u;
            io.waitMs(static_cast<unsigned>(
                (std::min<uint64_t>)(untilWake, 25u)));
        }
        QLOG("[spectator-route] stage=assist-exhausted elapsed=%llums "
             "registers=%u lookups=%u mappings=%u/%u candidates=%zu",
             static_cast<unsigned long long>(state.elapsedMs()),
             state.punchRegistersSent, state.punchLookupsSent,
             state.punchMappingsAccepted, state.punchMappingsSeen,
             state.probes.size());
    }

    if (!state.probes.empty()
        && target.requestedRoute != qnet::SPECTATE_ROUTE_RELAY
        && (target.directRaceMs != 0 || !state.punchMappings.empty())
        && io.nowMs() < laneDeadlineMs) {
        const uint64_t retryDeadline = laneDeadlineMs;
        {
            QLOG("[spectator-route] stage=retry-race elapsed=%llums "
                 "budget=%llums candidates=%zu",
                 static_cast<unsigned long long>(state.elapsedMs()),
                 static_cast<unsigned long long>(retryDeadline - io.nowMs()),
                 state.probes.size());
            outcome = raceCandidates(state, retryDeadline, failure, detail);
            if (outcome < 0) return state.finishFailure(failure, detail);
            if (outcome > 0) return finishRaceSuccess();
        }
    }

    const bool punchProven = std::any_of(
        state.probes.begin(), state.probes.end(),
        [](const CandidateProbe& probe) { return probe.punchEchoed; });

    bool relayRefused = false;
    std::string relayError;
    if (relayTarget.allowRelayFallback
        && (target.requestedRoute == 0
            || target.requestedRoute == qnet::SPECTATE_ROUTE_RELAY)
        && !relayTarget.relayHostText.empty()
        && relayTarget.relayPort != 0 && io.nowMs() < deadlineMs) {
        if (target.requestedRoute == 0) {
            state.sendBestEffortDetach();
            state.helloedEndpoints.clear();
            state.anyHelloSent = false;
            if (state.controlAdmissionHeld) {
                std::string releaseError;
                if (!state.releaseControlAdmission(releaseError)) {
                    return state.finishFailure(
                        PreflightFailure::RelayUnavailable,
                        releaseError.empty()
                            ? "the host did not release direct spectator admission"
                            : releaseError);
                }
                state.uncontrolledDirectHelloSent = false;
                state.result.grant = {};

                SpectateTarget relayAdmissionTarget = relayTarget;
                relayAdmissionTarget.requestedRoute =
                    qnet::SPECTATE_ROUTE_RELAY;
                ControlAnswer relayAnswer{};
                io.controlExchange(relayAdmissionTarget, relayAnswer);
                if (relayAnswer.outcome != ControlOutcome::Ok) {
                    return state.finishFailure(
                        failureForControl(relayAnswer.outcome),
                        relayAnswer.error);
                }
                if (relayAnswer.identity.sessionId != 0
                    && relayAnswer.identity.sessionId != target.sessionId) {
                    return state.finishFailure(PreflightFailure::StaleSession);
                }
                if (!relayAnswer.sourceConfirmedLive && relayAnswer.hasGrant) {
                    return state.finishFailure(
                        PreflightFailure::ManifestInvalid,
                        "an unconfirmed relay control answer cannot carry a grant");
                }
            if (target.simulationContract != 0
                && relayAnswer.simulationContract != 0
                && relayAnswer.simulationContract
                       != target.simulationContract) {
                return state.finishFailure(
                    PreflightFailure::ManifestInvalid,
                    "simulation contract mismatch on relay admission");
            }
            if (relayAnswer.sourceConfirmedLive) {
                const qnetplay::compat::IdentityMismatch relayMismatch =
                    qnetplay::compat::compatibilityMismatch(
                        target.compatibilityIdentity,
                        relayAnswer.compatibilityIdentity);
                if (relayMismatch
                    != qnetplay::compat::IdentityMismatch::None) {
                    return state.finishFailure(
                        PreflightFailure::ManifestInvalid,
                        std::string("cannot watch this match: ")
                            + qnetplay::compat::identityMismatchPlayerText(
                                relayMismatch)
                            + ". [relay admission; "
                            + qnetplay::compat::identityMismatchReason(
                                relayMismatch)
                            + "]");
                }
            }
            if (!validateCandidateArray(
                    relayAnswer.candidates, relayAnswer.candidateCount,
                    kMaxControlSpectateCandidates)) {
                return state.finishFailure(
                    PreflightFailure::ManifestInvalid,
                    "the relay admission candidate set failed validation");
            }
            if (!relayAnswer.hasGrant
                || relayAnswer.grant.viewerId != state.result.viewerId
                || relayAnswer.grant.sessionId != target.sessionId
                || relayAnswer.grant.resumeRecord != target.resumeRecord
                || relayAnswer.grant.resumeDigest != target.resumeDigest
                || relayAnswer.grant.route
                       != qnet::SPECTATE_ROUTE_RELAY) {
                return state.finishFailure(
                    PreflightFailure::GrantInvalid,
                    "the host did not return the exact Relay spectator pass");
            }
            state.result.grant = relayAnswer.grant;
            state.controlAdmissionHeld = true;
            relayTarget.requestedRoute = qnet::SPECTATE_ROUTE_RELAY;
            if (qnet::relay::isValidId(relayAnswer.relayRoom)) {
                relayTarget.relayRoom = relayAnswer.relayRoom;
                relayTarget.relayViewerAccessToken =
                    relayAnswer.relayViewerAccessToken;
                for (size_t i = 0;
                     i < relayAnswer.candidateCount; ++i) {
                    if (relayAnswer.candidates[i].kind
                        != qnet::CandidateKind::Relay) {
                        continue;
                    }
                    relayTarget.relayHostText = qnet::formatIpv4BE(
                        relayAnswer.candidates[i].ipv4BE);
                    relayTarget.relayPort = qnet::swapPort16(
                        relayAnswer.candidates[i].portBE);
                    break;
                }
            }
            }
        } else if (target.requestedRoute
                   == qnet::SPECTATE_ROUTE_RELAY) {
            relayTarget.requestedRoute = qnet::SPECTATE_ROUTE_RELAY;
        }
        RelayReservation reservation{};
        do {
            io.relayReserve(
                relayTarget, state.result.grant, reservation);
            if (reservation.ok || !reservation.retryable
                || io.nowMs() >= deadlineMs) {
                break;
            }
            if (state.isCancelled())
                return state.finishFailure(
                    PreflightFailure::Cancelled);
            const uint64_t remaining = deadlineMs - io.nowMs();
            const uint64_t requestedWait =
                reservation.retryAfterMs == 0
                    ? 500u : reservation.retryAfterMs;
            const uint64_t retryAt = io.nowMs()
                + (std::min)(remaining, requestedWait);
            while (io.nowMs() < retryAt) {
                if (state.isCancelled())
                    return state.finishFailure(
                        PreflightFailure::Cancelled);
                const uint64_t sliceRemaining = retryAt - io.nowMs();
                io.waitMs(static_cast<unsigned>(
                    (std::min<uint64_t>)(sliceRemaining, 25u)));
            }
            reservation = {};
        } while (io.nowMs() < deadlineMs);
        if (!reservation.ok) {
            if (state.isCancelled())
                return state.finishFailure(PreflightFailure::Cancelled);
            relayRefused = true;
            relayError = reservation.error;
        } else {
            state.relayReserved = true;
            state.result.reservationsReleased = false;
            if (reservation.viewerId != 0)
                state.result.viewerId = reservation.viewerId;
            state.probes.clear();
            appendCandidate(state.probes, SpectateCandidateKind::Relay,
                            reservation.allocated);
            if (state.probes.empty())
                return state.finishFailure(PreflightFailure::RelayUnavailable,
                                           "the relay returned no binding");
            outcome = raceCandidates(state, deadlineMs, failure, detail);
            if (outcome < 0) return state.finishFailure(failure, detail);
            if (outcome > 0) {
                state.result.routeLabel =
                    static_cast<uint16_t>(SpectatorRouteLabel::Relay);
                if (target.requestedRoute == 0) {
                    state.result.fallbackReason = autopunchAttempted
                        ? "direct-and-punch-unproved"
                        : "direct-unproved";
                }
                return state.finishSuccess();
            }
        }
    }

    logCandidateProbes("race-exhausted", state.probes, state.localPort,
                       state.elapsedMs(), io.nowMs());
    QLOG("[spectator-route] stage=race-exhausted elapsed=%llums "
         "foreignTraffic=%d punchProven=%d relayRefused=%d sourceLive=%d "
         "punch registers=%u lookups=%u mappings=%u/%u",
         static_cast<unsigned long long>(state.elapsedMs()),
         state.sawForeignSourceTraffic ? 1 : 0, punchProven ? 1 : 0,
         relayRefused ? 1 : 0, answer.sourceConfirmedLive ? 1 : 0,
         state.punchRegistersSent, state.punchLookupsSent,
         state.punchMappingsAccepted, state.punchMappingsSeen);
    state.result.routeSummary = state.raceSummaryLong();
    if (state.isCancelled())
        return state.finishFailure(PreflightFailure::Cancelled);
    if (state.sawForeignSourceTraffic || punchProven) {
        return state.finishFailure(PreflightFailure::WrongKey);
    }
    if (answer.sourceConfirmedLive) {
        std::string extra = state.raceSummaryShort();
        if (relayRefused) extra += "; relay: " + relayError;
        return state.finishFailure(PreflightFailure::LiveRouteUnavailable,
                                   extra);
    }
    if (relayRefused)
        return state.finishFailure(PreflightFailure::RelayUnavailable,
                                   relayError);
    return state.finishFailure(PreflightFailure::Unreachable,
                               state.raceSummaryShort());
}

#ifdef _WIN32
namespace {

RouteEndpoint fromQnet(const qnet::Endpoint& endpoint) {
    RouteEndpoint out{};
    out.ipv4BE = endpoint.sa.sin_addr.s_addr;
    out.port = ntohs(endpoint.sa.sin_port);
    return out;
}

qnet::Endpoint toQnet(const RouteEndpoint& endpoint) {
    qnet::Endpoint out;
    out.sa.sin_family = AF_INET;
    out.sa.sin_addr.s_addr = endpoint.ipv4BE;
    out.sa.sin_port = htons(endpoint.port);
    return out;
}

class SystemRouteIo final : public RouteIo {
public:
    SystemRouteIo() { qnet::netInit(); }
    ~SystemRouteIo() override { relayRelease(); }

    uint64_t nowMs() override { return qnet::nowUs() / 1000ull; }
    void waitMs(unsigned ms) override {
        Sleep(ms > 25 ? 25 : ms);
        refreshRelayRegistration();
    }

    bool resolveHost(const std::string& text,
                     uint16_t port,
                     RouteEndpoint& out,
                     std::string& error) override {
        qnet::Endpoint endpoint;
        if (!qnet::Endpoint::parse(text, port, endpoint)) {
            error = "could not resolve " + text;
            return false;
        }
        out = fromQnet(endpoint);
        return out.valid();
    }

    size_t localIpv4BE(uint32_t* out, size_t cap) override {
        return qnet::collectLocalIpv4BE(out, cap);
    }

    bool openViewerSocket(uint16_t& boundPort, std::string& error) override {
        if (_udp.isOpen()) {
            boundPort = _udp.boundPort();
            return true;
        }
        if (!_udp.open(0)) {
            error = "cannot open the spectator UDP socket (WSA "
                  + std::to_string(_udp.lastError()) + ")";
            return false;
        }
        _udp.drain();
        boundPort = _udp.boundPort();
        return true;
    }

    void closeViewerSocket() override { _udp.close(); }

    uintptr_t detachViewerSocket() override {
        const SOCKET raw = _udp.raw();
        if (raw == INVALID_SOCKET) return ~uintptr_t(0);
        _udp.release();
        return static_cast<uintptr_t>(raw);
    }

    bool relayViewerBinding(
        qnetplay::spectator::RelayViewerBinding& out) const override {
        if (!_relayHeld || !_relayAdapter.enabled()) {
            out = {};
            return false;
        }
        out = _relayAdapter.binding();
        return true;
    }

    bool sendTo(const RouteEndpoint& to,
                const uint8_t* bytes,
                size_t length) override {
        if (!_udp.isOpen() || !to.valid()) return false;
        const qnet::Endpoint endpoint = toQnet(to);
        if (_relayHeld && endpoint == _relayEndpoint) {
            refreshRelayRegistration();
            if (length == 1) return true;
            std::vector<uint8_t> framed;
            if (!_relayAdapter.encodeOutbound(bytes, length, framed))
                return false;
            return _udp.sendTo(endpoint, framed.data(), framed.size());
        }
        return _udp.sendTo(endpoint, bytes, length);
    }

    int recvFrom(uint8_t* buffer, size_t capacity, RouteEndpoint& from) override {
        if (!_udp.isOpen()) return -1;
        refreshRelayRegistration();
        uint8_t wire[kRecvBuffer]{};
        for (unsigned drained = 0; drained < 64; ++drained) {
            qnet::Endpoint source;
            const int received = _udp.recvFrom(
                wire, sizeof(wire), source);
            if (received <= 0) return received;
            if (!_relayHeld || !(source == _relayEndpoint)) {
                if (static_cast<size_t>(received) > capacity) continue;
                std::memcpy(buffer, wire, static_cast<size_t>(received));
                from = fromQnet(source);
                return received;
            }

            std::vector<uint8_t> inner;
            const auto ingress = _relayAdapter.decodeInbound(
                true, wire, static_cast<size_t>(received), inner);
            if (ingress
                != qnetplay::spectator::RelayViewerIngress::Data) {
                continue;
            }
            if (inner.size() > capacity) continue;
            std::memcpy(buffer, inner.data(), inner.size());
            from = fromQnet(source);
            return static_cast<int>(inner.size());
        }
        return 0;
    }

    void controlExchange(const SpectateTarget& target,
                         ControlAnswer& out) override;
    bool controlRelease(const SpectateTarget& target,
                        uint64_t viewerId,
                        std::string& error) override;
    void relayReserve(const SpectateTarget& target,
                      const qnetplay::spectator::SpectatorGrant& grant,
                      RelayReservation& out) override;
    void relayRelease() override;

private:
    void refreshRelayRegistration(bool force = false) {
        if (!_relayHeld || !_udp.isOpen()) return;
        std::vector<uint8_t> registration;
        if (_relayAdapter.encodeRegistration(
                nowMs(), registration, force)) {
            (void)_udp.sendTo(
                _relayEndpoint, registration.data(), registration.size());
        }
    }

    qnet::UdpSock _udp;
    qnet::TcpConn _relayControl;
    bool _relayHeld = false;
    qnet::relay::RoomId _relayRoom{};
    qnet::relay::RegistrationToken _relayToken{};
    qnet::Endpoint _relayEndpoint{};
    uint64_t _relayViewerId = 0;
    qnetplay::spectator::RelayViewerAdapter _relayAdapter;
};

void SystemRouteIo::controlExchange(const SpectateTarget& target,
                                    ControlAnswer& out) {
    out = ControlAnswer{};
    QLOG("[spectator-route] stage=control-start host=%s:%u dataPort=%u "
         "initialCandidates=%u requestedRoute=%u punch=%d",
         target.hostText.c_str(), static_cast<unsigned>(target.hostPort),
         static_cast<unsigned>(target.directUdpPort),
         static_cast<unsigned>(target.initialCandidateCount),
         static_cast<unsigned>(target.requestedRoute),
         target.punchRelayIpv4BE != 0 && target.punchRelayPort != 0 ? 1 : 0);
    const auto acceptCopiedAddressFallback = [&](const char* stage) -> bool {
        if (target.initialCandidateCount == 0) return false;
        out = ControlAnswer{};
        out.outcome = ControlOutcome::Ok;
        out.identity.sessionId = target.sessionId;
        QLOG("[spectator-route] stage=%s result=candidate-fallback "
             "initialCandidates=%u sourceLive=0",
             stage, static_cast<unsigned>(target.initialCandidateCount));
        return true;
    };
    if (target.hostPort == 0) {
        RouteEndpoint direct{};
        std::string error;
        if (target.directUdpPort != 0
            && resolveHost(target.hostText, target.directUdpPort, direct,
                           error)) {
            out.outcome = ControlOutcome::Ok;
            out.sourceDirect = direct;
            out.sourceUdpPort = target.directUdpPort;
            out.identity.sessionId = target.sessionId;
            QLOG("[spectator-route] stage=no-control result=direct-candidate "
                 "endpoint=%s:%u",
                 target.hostText.c_str(),
                 static_cast<unsigned>(target.directUdpPort));
            return;
        }
        if (!target.relayHostText.empty() && target.relayPort != 0
            && qnet::relay::isValidId(target.relayRoom)
            && qnet::relay::isValidId(
                   target.relayViewerAccessToken)) {
            out.outcome = ControlOutcome::Ok;
            out.identity.sessionId = target.sessionId;
            QLOG("[spectator-route] stage=no-control result=relay-capability");
            return;
        }
        out.outcome = ControlOutcome::Unreachable;
        out.error = error.empty() ? "no host control port" : error;
        QLOG("[spectator-route] stage=no-control result=failed reason='%s'",
             out.error.c_str());
        return;
    }

    qnet::UdpSock controlSocket;
    if (!controlSocket.open(0)) {
        out.outcome = ControlOutcome::Error;
        out.error = "could not open a socket to reach the host";
        QLOG("[spectator-route] stage=control-socket result=failed error=%d",
             controlSocket.lastError());
        return;
    }
    qnet::Endpoint advertised{};
    {
        RouteEndpoint resolved{};
        std::string resolveError;
        if (!resolveHost(target.hostText, target.hostPort, resolved,
                         resolveError)) {
            out.outcome = ControlOutcome::Unreachable;
            out.error = resolveError.empty()
                ? ("could not resolve " + target.hostText) : resolveError;
            QLOG("[spectator-route] stage=control-resolve result=failed "
                 "reason='%s'", out.error.c_str());
            return;
        }
        advertised.sa.sin_family = AF_INET;
        advertised.sa.sin_addr.s_addr = resolved.ipv4BE;
        advertised.sa.sin_port = htons(resolved.port);
    }

    RouteEndpoint punchRoute{};
    const bool punchReady =
        punchRelayEndpointForTarget(target, punchRoute);
    const qnet::Endpoint punchRelay = toQnet(punchRoute);

    qnet::UdpControlLink control;
    std::string dialError;
    qnet::ControlDialStatus lastDialStatus{};
    const auto dialTick = [&](const qnet::ControlDialStatus& status) {
        lastDialStatus = status;
        return true;
    };
    if (!qnet::dialControlLink(controlSocket, advertised,
                               punchReady ? &punchRelay : nullptr, 8000,
                               control, dialTick, dialError)) {
        qnet::Endpoint lastMapping{};
        lastMapping.sa.sin_addr.s_addr =
            lastDialStatus.lastMappingIpv4NetworkOrder;
        lastMapping.sa.sin_port = htons(lastDialStatus.lastMappingNatPort);
        const std::string lastMappingText =
            lastDialStatus.lastMappingIpv4NetworkOrder != 0
                && lastDialStatus.lastMappingNatPort != 0
            ? lastMapping.str() : std::string("none");
        QLOG("[spectator-route] stage=control-dial result=failed target=%s "
             "punch=%d peerMapping=%d mappingRx=%u mappingReject=%u "
             "targetMismatch=%u externalAlias=%u sameIpAlternate=%u "
             "lastMapping=%s "
             "lastInternal=%u hellos=%u acks=%u foreign=%u rejected=%u "
             "reason='%s'",
             advertised.str().c_str(), punchReady ? 1 : 0,
             lastDialStatus.peerMappingKnown ? 1 : 0,
             lastDialStatus.mappingsReceived,
             lastDialStatus.mappingsRejected,
             lastDialStatus.mappingTargetMismatches,
             lastDialStatus.mappingExternalPortAliases,
             lastDialStatus.mappingSameIpAlternates,
             lastMappingText.c_str(),
             static_cast<unsigned>(
                 lastDialStatus.lastMappingInternalPort),
             control.stats().hellosSent, control.stats().helloAcksReceived,
             control.stats().foreignDatagrams, control.stats().rejected,
             dialError.c_str());
        if (acceptCopiedAddressFallback("control-dial")) return;
        out.outcome = ControlOutcome::Unreachable;
        out.error = "could not reach " + target.hostText + ":"
                  + std::to_string(target.hostPort)
                  + (dialError.empty() ? "" : " - " + dialError);
        return;
    }
    QLOG("[spectator-route] stage=control-dial result=ok peer=%s "
         "peerMapping=%d hellos=%u acks=%u foreign=%u rejected=%u",
         control.peer().str().c_str(),
         lastDialStatus.peerMappingKnown ? 1 : 0,
         control.stats().hellosSent, control.stats().helloAcksReceived,
         control.stats().foreignDatagrams, control.stats().rejected);

    qnet::ControlLink::Received rx;
    const auto receive = [&](uint8_t expected) -> bool {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const qnet::ControlLink::Rs state = control.recvMsgEx(rx, 5000);
            if (state != qnet::ControlLink::Rs::Msg) return false;
            if (rx.type == expected) return true;
            if (rx.type == qnet::MSG_ERROR_TEXT) return false;
        }
        return false;
    };

    if (!receive(qnet::MSG_VERSION_CONFIG)
        || rx.body.size() != sizeof(qnet::VersionConfig)) {
        if (acceptCopiedAddressFallback("version")) {
            control.closeGraceful();
            return;
        }
        out.outcome = ControlOutcome::Unreachable;
        out.error = "no answer from the host";
        QLOG("[spectator-route] stage=version result=failed rxType=%u "
             "bodyBytes=%zu socketError=%d",
             static_cast<unsigned>(rx.type), rx.body.size(),
             control.lastError());
        control.closeGraceful();
        return;
    }
    qnet::VersionConfig hostVersion{};
    std::memcpy(&hostVersion, rx.body.data(), sizeof(hostVersion));
    if (hostVersion.protocolVersion != qnet::kProtocolVersion) {
        out.outcome = ControlOutcome::Stale;
        out.error = "the host runs a different caster version";
        QLOG("[spectator-route] stage=version result=mismatch host=%u local=%u",
             hostVersion.protocolVersion, qnet::kProtocolVersion);
        control.closeGraceful();
        return;
    }
    qnet::VersionConfig mine{ qnet::kProtocolVersion };
    control.sendMsg(qnet::MSG_VERSION_CONFIG, &mine, sizeof(mine));
    qnet::ClientIntent intent{};
    intent.intent = qnet::INTENT_SPECTATE;
    intent.spectatorRoute = target.requestedRoute != 0
        ? target.requestedRoute
        : qnet::SPECTATE_ROUTE_DIRECT;
    intent.sessionId = target.sessionId;
    intent.keyLow = target.keyLow;
    intent.keyHigh = target.keyHigh;
    intent.viewerId = target.viewerId;
    intent.spectatorDataPort = target.viewerDataPort;
    intent.resumeRecord = target.resumeRecord;
    intent.resumeDigest = target.resumeDigest;
    control.sendMsg(qnet::MSG_CLIENT_INTENT, &intent, sizeof(intent));

    if (!receive(qnet::MSG_SPECTATE_INFO)
        || rx.body.size() != sizeof(qnet::SpectateInfo)) {
        out.outcome = ControlOutcome::Error;
        out.error = "the host did not answer the spectate request";
        QLOG("[spectator-route] stage=admission result=no-answer rxType=%u "
             "bodyBytes=%zu socketError=%d",
             static_cast<unsigned>(rx.type), rx.body.size(),
             control.lastError());
        control.closeGraceful();
        return;
    }
    qnet::SpectateInfo info{};
    std::memcpy(&info, rx.body.data(), sizeof(info));
    control.closeGraceful();

    if (info.state != qnet::SPECTATE_LIVE) {
        out.outcome = ControlOutcome::NoMatch;
        QLOG("[spectator-route] stage=admission result=no-match state=%u",
             static_cast<unsigned>(info.state));
        return;
    }
    if (info.reserved != 0
        || info.admissionPolicy < 1 || info.admissionPolicy > 4) {
        out.outcome = ControlOutcome::Error;
        out.error = "the host returned malformed spectator admission";
        return;
    }
    if (!qnetplay::compat::isValidCompatibilityIdentity(
            info.compatibilityIdentity)) {
        out.outcome = ControlOutcome::Error;
        out.error = "the host returned no complete simulation compatibility "
                    "identity";
        return;
    }
    switch (info.admissionState) {
    case qnet::SPECTATE_ADMISSION_GRANTED:
        break;
    case qnet::SPECTATE_ADMISSION_REFUSED:
        out.outcome = ControlOutcome::Refused;
        out.error = "the host declined this spectator";
        return;
    case qnet::SPECTATE_ADMISSION_PENDING:
        out.outcome = ControlOutcome::Refused;
        out.error = "the host has not approved this spectator yet";
        return;
    case qnet::SPECTATE_ADMISSION_CAPACITY:
        out.outcome = ControlOutcome::CapacityFull;
        return;
    case qnet::SPECTATE_ADMISSION_REQUIRED:
        out.outcome = ControlOutcome::Refused;
        out.error =
            "this host is not accepting address-based spectators";
        return;
    case qnet::SPECTATE_ADMISSION_UNAVAILABLE:
        out.outcome = ControlOutcome::Refused;
        out.error = "spectator admission is unavailable for this match";
        return;
    case qnet::SPECTATE_ADMISSION_NONE:
    default:
        out.outcome = ControlOutcome::Error;
        out.error = "the host did not reserve spectator admission";
        return;
    }
    if (info.sessionId != target.sessionId
        || info.keyLow != target.keyLow || info.keyHigh != target.keyHigh) {
        out.outcome = ControlOutcome::Stale;
        return;
    }

    std::string routeError;
    if (!decodeSpectateRouteCandidates(
            info.sourceRoutes, info.sessionId, out.candidates,
            kMaxControlSpectateCandidates, out.candidateCount, routeError)) {
        out.outcome = ControlOutcome::Error;
        out.error = routeError;
        return;
    }

    RouteEndpoint direct{};
    std::string error;
    const uint16_t udpPort =
        info.spectatorPort != 0 ? info.spectatorPort : target.directUdpPort;
    if (udpPort != 0)
        resolveHost(target.hostText, udpPort, direct, error);

    out.outcome = ControlOutcome::Ok;
    out.sourceDirect = direct;
    out.sourceUdpPort = udpPort;
    out.sourceConfirmedLive = true;
    out.identity.sessionId = info.sessionId;
    out.identity.cookie.low = info.keyLow;
    out.identity.cookie.high = info.keyHigh;
    out.simulationContract = info.simulationContract;
    out.compatibilityIdentity = info.compatibilityIdentity;
    out.observedPublicIpv4BE = info.sourceRoutes.observedPublicIpv4BE;
    out.viewerCount = info.viewerCount;
    out.viewerCapacity = info.viewerCapacity;
    out.relayRoom = {
        info.relayRoomLow,
        info.relayRoomHigh,
    };
    out.relayViewerAccessToken = {
        info.relayViewerAccessLow,
        info.relayViewerAccessHigh,
    };
    qnetplay::spectator::SpectatorGrant admitted{};
    if (!decodeGrantWire(info.grant, admitted)
        || admitted.sessionId != target.sessionId
        || admitted.viewerId != target.viewerId
        || admitted.resumeRecord != target.resumeRecord
        || admitted.resumeDigest != target.resumeDigest
        || admitted.route != intent.spectatorRoute) {
        out.outcome = ControlOutcome::Error;
        out.error =
            "the host returned a spectator pass for a different request";
        return;
    }
    out.hasGrant = true;
    out.grant = admitted;
    out.viewerId = admitted.viewerId;
    out.sourceGeneration = admitted.sourceGeneration;
    QLOG("[spectator-route] stage=admission result=granted sourcePort=%u "
         "candidates=%u viewers=%u/%u generation=%u route=%u",
         static_cast<unsigned>(out.sourceUdpPort),
         static_cast<unsigned>(out.candidateCount),
         static_cast<unsigned>(out.viewerCount),
         static_cast<unsigned>(out.viewerCapacity), out.sourceGeneration,
         static_cast<unsigned>(admitted.route));
}

bool SystemRouteIo::controlRelease(const SpectateTarget& target,
                                   uint64_t viewerId,
                                   std::string& error) {
    error.clear();
    if (target.hostText.empty() || target.hostPort == 0
        || target.sessionId == 0
        || (target.keyLow == 0 && target.keyHigh == 0)
        || viewerId == 0) {
        error = "the spectator admission cannot be released without its "
                "exact host, session, key, and viewer identity";
        return false;
    }

    qnet::UdpSock controlSocket;
    if (!controlSocket.open(0)) {
        error = "could not open a socket to release spectator admission";
        return false;
    }
    RouteEndpoint resolved{};
    if (!resolveHost(target.hostText, target.hostPort, resolved, error)) {
        if (error.empty()) error = "could not resolve the spectator host";
        return false;
    }
    qnet::Endpoint advertised = toQnet(resolved);
    RouteEndpoint punchRoute{};
    const bool punchReady =
        punchRelayEndpointForTarget(target, punchRoute);
    const qnet::Endpoint punchRelay = toQnet(punchRoute);

    qnet::UdpControlLink control;
    std::string dialError;
    if (!qnet::dialControlLink(controlSocket, advertised,
                               punchReady ? &punchRelay : nullptr, 8000,
                               control, nullptr, dialError)) {
        error = "could not reach the host to release spectator admission";
        if (!dialError.empty()) error += " - " + dialError;
        return false;
    }

    qnet::ControlLink::Received rx;
    const auto receive = [&](uint8_t expected) -> bool {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const qnet::ControlLink::Rs state = control.recvMsgEx(rx, 5000);
            if (state != qnet::ControlLink::Rs::Msg) return false;
            if (rx.type == expected) return true;
            if (rx.type == qnet::MSG_ERROR_TEXT) return false;
        }
        return false;
    };
    if (!receive(qnet::MSG_VERSION_CONFIG)
        || rx.body.size() != sizeof(qnet::VersionConfig)) {
        error = "the host did not answer the admission release";
        control.closeGraceful();
        return false;
    }
    qnet::VersionConfig hostVersion{};
    std::memcpy(&hostVersion, rx.body.data(), sizeof(hostVersion));
    if (hostVersion.protocolVersion != qnet::kProtocolVersion) {
        error = "the host runs a different caster version";
        control.closeGraceful();
        return false;
    }
    const qnet::VersionConfig mine{qnet::kProtocolVersion};
    if (!control.sendMsg(qnet::MSG_VERSION_CONFIG, &mine, sizeof(mine))) {
        error = "could not answer the host version exchange";
        control.closeGraceful();
        return false;
    }
    qnet::ClientIntent intent{};
    intent.intent = qnet::INTENT_SPECTATE_RELEASE;
    intent.spectatorRoute = qnet::SPECTATE_ROUTE_NONE;
    intent.sessionId = target.sessionId;
    intent.keyLow = target.keyLow;
    intent.keyHigh = target.keyHigh;
    intent.viewerId = viewerId;
    if (!control.sendMsg(qnet::MSG_CLIENT_INTENT, &intent, sizeof(intent))) {
        error = "could not send the admission release";
        control.closeGraceful();
        return false;
    }
    if (!receive(qnet::MSG_SPECTATE_INFO)
        || rx.body.size() != sizeof(qnet::SpectateInfo)) {
        error = "the host did not acknowledge the admission release";
        control.closeGraceful();
        return false;
    }
    qnet::SpectateInfo info{};
    std::memcpy(&info, rx.body.data(), sizeof(info));
    control.closeGraceful();
    if (info.state != qnet::SPECTATE_LIVE
        || info.sessionId != target.sessionId
        || info.keyLow != target.keyLow
        || info.keyHigh != target.keyHigh
        || info.admissionState != qnet::SPECTATE_ADMISSION_RELEASED) {
        error = "the host returned an invalid admission-release "
                "acknowledgement";
        return false;
    }
    return true;
}

void SystemRouteIo::relayReserve(const SpectateTarget& target,
                                 const qnetplay::spectator::SpectatorGrant& grant,
                                 RelayReservation& out) {
    out = RelayReservation{};
    if (target.relayHostText.empty() || target.relayPort == 0) {
        out.error = "no relay configured";
        return;
    }
    qnet::Endpoint relayEndpoint;
    if (!qnet::Endpoint::parse(target.relayHostText, target.relayPort,
                               relayEndpoint)) {
        out.error = "the configured relay address is not usable";
        return;
    }
    const bool grantAdmission =
        grant.grantId != 0 && grant.viewerId != 0
        && grant.sessionId == target.sessionId
        && grant.expiryUnix != 0
        && grant.sourceGeneration != 0
        && (grant.macLow != 0 || grant.macHigh != 0)
        && grant.route == static_cast<uint8_t>(
                              qnet::relay::SpectatorRouteKind::Relay);
    const bool publicAdmission =
        qnet::relay::isValidId(target.relayViewerAccessToken);
    if (!grantAdmission && !publicAdmission) {
        out.error =
            "the host did not provide relay spectator admission";
        return;
    }
    if (!qnet::relay::isValidId(target.relayRoom)) {
        out.error =
            "the host response did not carry the exact spectator relay room";
        return;
    }
    _relayRoom = target.relayRoom;

    qnet::TcpConn control;
    if (!control.connectTo(target.relayHostText, target.relayPort, 5000)) {
        out.error = "could not reach the relay";
        out.retryable = true;
        out.retryAfterMs = 2000;
        return;
    }
    qnet::relay::SpectatorHello hello{};
    hello.room = _relayRoom;
    hello.party = qnet::relay::SpectatorParty::Viewer;
    hello.route = qnet::relay::SpectatorRouteKind::Relay;
    hello.sessionId = target.sessionId;
    hello.resumeRecord = target.resumeRecord;
    hello.resumeDigest = target.resumeDigest;
    if (grantAdmission) {
        hello.admission = qnet::relay::SpectatorAdmission::Grant;
        hello.grantId = grant.grantId;
        hello.viewerId = grant.viewerId;
        hello.expiryUnix = grant.expiryUnix;
        hello.sourceGeneration = grant.sourceGeneration;
        hello.macLow = grant.macLow;
        hello.macHigh = grant.macHigh;
    } else {
        hello.admission =
            qnet::relay::SpectatorAdmission::PublicToken;
        hello.sourceToken = target.relayViewerAccessToken;
    }
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorHello(hello, encoded)) {
        out.error = "the spectator relay request could not be encoded";
        control.closeGraceful();
        return;
    }
    if (!control.sendMsg(qnet::relay::kTcpEnvelopeType, encoded.data(),
                         encoded.size())) {
        out.error = "the relay closed the spectator request";
        out.retryable = true;
        out.retryAfterMs = 2000;
        control.closeGraceful();
        return;
    }
    qnet::TcpConn::Received rx;
    if (control.recvMsgEx(rx, 5000) != qnet::TcpConn::Rs::Msg) {
        out.error = "the relay did not answer the spectator request";
        out.retryable = true;
        out.retryAfterMs = 2000;
        control.closeGraceful();
        return;
    }
    qnet::relay::SpectatorWelcome welcome{};
    if (rx.type != qnet::relay::kTcpEnvelopeType
        || qnet::relay::decodeSpectatorWelcome(rx.body.data(), rx.body.size(),
                                               welcome)
               != qnet::relay::DecodeError::None) {
        out.error = "the relay answered with a malformed spectator welcome";
        control.closeGraceful();
        return;
    }
    switch (welcome.status) {
    case qnet::relay::SpectatorStatus::Accepted:
    case qnet::relay::SpectatorStatus::Waiting:
        break;
    case qnet::relay::SpectatorStatus::CapacityReached:
        out.error = "the relay spectator lane is full";
        out.retryAfterMs = welcome.retryAfterMs;
        return;
    case qnet::relay::SpectatorStatus::Unsupported:
        out.error = "this relay does not serve spectators";
        return;
    case qnet::relay::SpectatorStatus::SourceUnavailable:
        out.retryable = true;
        out.retryAfterMs =
            welcome.retryAfterMs == 0 ? 2000u : welcome.retryAfterMs;
        out.error = "the spectator relay source is still starting";
        return;
    case qnet::relay::SpectatorStatus::RoomNotFound:
        out.error = "the spectator relay room has expired";
        return;
    case qnet::relay::SpectatorStatus::GrantRejected:
        out.error = "the relay rejected the spectator access code";
        return;
    default:
        out.error = "the relay refused spectator admission (status "
            + std::to_string(
                static_cast<unsigned>(welcome.status))
            + ")";
        return;
    }

    const uint64_t viewerId = welcome.viewerId;
    if (!(welcome.room.low == _relayRoom.low
          && welcome.room.high == _relayRoom.high)
        || welcome.party != qnet::relay::SpectatorParty::Viewer
        || !qnet::relay::isValidId(welcome.registrationToken)
        || welcome.sessionId != target.sessionId
        || welcome.sourceGeneration == 0
        || (grantAdmission
            && welcome.sourceGeneration != grant.sourceGeneration)
        || viewerId == 0
        || (grantAdmission && viewerId != hello.viewerId)) {
        out.error = "the relay welcome did not match this spectator pass";
        control.closeGraceful();
        return;
    }

    _relayToken = welcome.registrationToken;
    _relayEndpoint = relayEndpoint;
    _relayViewerId = viewerId;
    qnetplay::spectator::RelayViewerBinding binding{};
    binding.enabled = true;
    binding.room = _relayRoom;
    binding.token = _relayToken;
    binding.viewerId = _relayViewerId;
    binding.sourceGeneration = welcome.sourceGeneration;
    binding.identity = target.identity();
    if (!_relayAdapter.configure(binding)) {
        out.error = "the relay returned an unusable spectator binding";
        control.closeGraceful();
        _relayToken = {};
        _relayEndpoint = {};
        _relayViewerId = 0;
        return;
    }
    // The TCP lease owns the relay capacity reservation: closing it before the viewer exits makes the relay detach the viewer.
    _relayControl = std::move(control);
    _relayHeld = true;

    refreshRelayRegistration(true);

    out.ok = true;
    out.allocated = fromQnet(relayEndpoint);
    out.viewerId = _relayViewerId;
}

void SystemRouteIo::relayRelease() {
    if (!_relayHeld) return;
    std::vector<uint8_t> encoded;
    if (_relayAdapter.encodeDetach(
            qnet::relay::SpectatorDetachReason::ViewerRequested, encoded)
        && _udp.isOpen()) {
        _udp.sendTo(_relayEndpoint, encoded.data(), encoded.size());
    }
    _relayControl.closeGraceful(250);
    _relayHeld = false;
    _relayAdapter.reset();
    _relayRoom = {};
    _relayToken = {};
    _relayEndpoint = {};
    _relayViewerId = 0;
}

}

std::unique_ptr<RouteIo> makeSystemRouteIo() {
    return std::unique_ptr<RouteIo>(new SystemRouteIo());
}

SpectatePreflightResult preflightSpectate(
    const SpectateTarget& target,
    unsigned timeoutMs,
    const std::function<bool()>& cancelled) {
    std::unique_ptr<RouteIo> io = makeSystemRouteIo();
    return preflightSpectateWith(target, timeoutMs, cancelled, *io);
}

#else

std::unique_ptr<RouteIo> makeSystemRouteIo() { return nullptr; }

SpectatePreflightResult preflightSpectate(
    const SpectateTarget& target,
    unsigned timeoutMs,
    const std::function<bool()>& cancelled) {
    (void)target;
    (void)timeoutMs;
    (void)cancelled;
    SpectatePreflightResult result{};
    result.failure = PreflightFailure::InternalError;
    result.failureReason = preflightFailureText(result.failure);
    return result;
}

#endif

}
