#pragma once

#include "Protocol.h"
#include "Reliable.h"
#include "Sockets.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qnet {

class ControlLink {
public:
    using Received = TcpConn::Received;
    using Rs = TcpConn::Rs;

    virtual ~ControlLink() = default;

    virtual bool sendMsg(uint8_t type, const void* body, size_t bodyLen) = 0;
    virtual Rs recvMsgEx(Received& out, int timeoutMs) = 0;
    virtual bool isOpen() const = 0;
    virtual int lastError() const = 0;
    virtual void close() = 0;
    virtual void closeGraceful(int capMs = 1000) = 0;

    bool recvMsg(Received& out, int timeoutMs) {
        const Rs rs = recvMsgEx(out, timeoutMs);
        if (rs == Rs::Timeout) out.type = MSG_NONE;
        return rs == Rs::Msg || rs == Rs::Timeout;
    }

    // A caller that also reads this socket must offer what it reads (see offerDatagram); an unrecognised datagram may belong to the link.
    virtual bool sharesDataSocket() const { return false; }

    virtual bool offerDatagram(const uint8_t* , size_t ,
                               const Endpoint& ) {
        return false;
    }

    virtual void service() {}

    virtual const char* transportName() const = 0;
};

class TcpControlLink final : public ControlLink {
public:
    explicit TcpControlLink(TcpConn& conn) : _conn(&conn) {}

    bool sendMsg(uint8_t type, const void* body, size_t bodyLen) override {
        return _conn->sendMsg(type, body, bodyLen);
    }
    Rs recvMsgEx(Received& out, int timeoutMs) override {
        return _conn->recvMsgEx(out, timeoutMs);
    }
    bool isOpen() const override { return _conn->isOpen(); }
    int lastError() const override { return _conn->lastError(); }
    void close() override { _conn->close(); }
    void closeGraceful(int capMs = 1000) override {
        _conn->closeGraceful(capMs);
    }
    const char* transportName() const override { return "tcp"; }

    TcpConn& conn() { return *_conn; }

private:
    TcpConn* _conn;
};

// joiner -> host: MSG_LINK_HELLO { protocolVersion, nonce }; host -> joiner: MSG_LINK_HELLO_ACK { protocolVersion, nonce echoed }
#pragma pack(push, 1)
struct LinkHello {
    uint32_t protocolVersion;
    uint64_t nonce;
};
#pragma pack(pop)
static_assert(sizeof(LinkHello) == 12, "link hello wire layout changed");

inline constexpr uint32_t kLinkHelloIntervalMs = 100;

inline constexpr size_t kLinkMaxUnacked = 64;

inline constexpr uint32_t kLinkIdleTimeoutMs = 90000;

inline constexpr size_t kLinkMaxDatagram = 1200;

class UdpControlLink final : public ControlLink {
public:
    using ForeignSink = std::function<void(const uint8_t* bytes, size_t length,
                                           const Endpoint& from)>;

    enum class State : uint8_t { Idle, Connecting, Established, Closed };

    UdpControlLink() = default;
    ~UdpControlLink() override { close(); }

    // The reliable channel's callbacks capture `this` and `&_peer`; copying or moving a link would leave them writing through the original.
    UdpControlLink(const UdpControlLink&) = delete;
    UdpControlLink& operator=(const UdpControlLink&) = delete;
    UdpControlLink(UdpControlLink&&) = delete;
    UdpControlLink& operator=(UdpControlLink&&) = delete;

    void startClient(UdpSock& socket, const Endpoint& peer, uint64_t nowMs);

    void startHostListen(UdpSock& socket, uint64_t nowMs);

    void expectPeer(const Endpoint& peer);

    void setForeignSink(ForeignSink sink) { _foreign = std::move(sink); }

    // When a listener owns the socket the link must not read it: two readers on one socket lose datagrams to each other.
    using PumpFn = std::function<void(int waitMs)>;
    void setExternalPump(PumpFn pump) { _externalPump = std::move(pump); }

    void setIdleTimeoutMs(uint32_t ms) { _idleTimeoutMs = ms; }

    bool addHelloTarget(const Endpoint& target);

    bool greeted(const Endpoint& source) const;

    bool connect(int timeoutMs);

    void poll(int waitMs);

    bool sendMsg(uint8_t type, const void* body, size_t bodyLen) override;
    Rs recvMsgEx(Received& out, int timeoutMs) override;
    bool isOpen() const override { return _state == State::Established
                                       || _state == State::Connecting; }
    int lastError() const override { return _lastErr; }
    void close() override;
    void closeGraceful(int capMs = 1000) override;
    bool sharesDataSocket() const override { return true; }
    bool offerDatagram(const uint8_t* bytes, size_t length,
                       const Endpoint& from) override;
    void service() override;
    const char* transportName() const override { return "udp"; }

    State state() const { return _state; }
    bool established() const { return _state == State::Established; }
    const Endpoint& peer() const { return _peer; }
    bool havePeer() const { return _havePeer; }
    const std::string& failureText() const { return _failure; }
    bool nonceDegraded() const { return _nonceDegraded; }

    struct Stats {
        uint32_t hellosSent = 0;
        uint32_t sendFailures = 0;
        uint32_t hellosReceived = 0;
        uint32_t helloAcksSent = 0;
        uint32_t helloAcksReceived = 0;
        uint32_t controlDatagrams = 0;
        uint32_t foreignDatagrams = 0;
        uint32_t rejected = 0;
    };
    const Stats& stats() const { return _stats; }

private:
    void begin(UdpSock& socket, uint64_t nowMs);
    void initReliable(uint64_t nowMs);
    void sendRaw(uint8_t type, const void* body, size_t bodyLen);
    void sendRawTo(const Endpoint& to, uint8_t type, const void* body,
                   size_t bodyLen);
    void sendHello(uint64_t nowMs);
    void drainSocket(uint64_t nowMs);
    bool classify(const uint8_t* bytes, size_t length, const Endpoint& from,
                  uint64_t nowMs);
    void fail(const std::string& why);

    UdpSock* _udp = nullptr;
    State _state = State::Idle;
    bool _isHost = false;

    Endpoint _peer{};
    bool _havePeer = false;
    bool _peerExpected = false;

    uint64_t _nonce = 0;
    bool _nonceDegraded = false;
    uint64_t _lastHelloMs = 0;

    ReliableChannel _rel;
    bool _relStarted = false;

    static constexpr size_t kMaxHelloTargets = 4;
    Endpoint _helloTargets[kMaxHelloTargets]{};
    size_t _helloTargetCount = 0;

    std::deque<Received> _inbox;
    ForeignSink _foreign;
    PumpFn _externalPump;

    uint32_t _idleTimeoutMs = kLinkIdleTimeoutMs;
    uint64_t _lastInboundMs = 0;

    int _lastErr = 0;
    std::string _failure;
    Stats _stats{};
};

inline constexpr size_t kLinkMaxPeers = 5;

class UdpControlListener {
public:
    using ForeignSink = UdpControlLink::ForeignSink;
    using InjectedPump =
        std::function<void(UdpControlListener& listener)>;

    UdpControlListener() = default;
    ~UdpControlListener() { close(); }
    UdpControlListener(const UdpControlListener&) = delete;
    UdpControlListener& operator=(const UdpControlListener&) = delete;

    void open(UdpSock& socket);
    void openInjected(UdpSock& socket, InjectedPump pump);
    void close();
    bool isOpen() const { return _udp != nullptr; }

    void setForeignSink(ForeignSink sink) { _foreign = std::move(sink); }

    void pump(int waitMs);

    void offerDatagram(const uint8_t* bytes, size_t length,
                       const Endpoint& from);

    UdpControlLink* acceptOne();

    void release(UdpControlLink* link);

    size_t liveLinks() const { return _links.size(); }
    uint32_t rejectedForCapacity() const { return _rejectedForCapacity; }
    uint64_t datagramsReceived() const { return _datagramsReceived; }
    uint64_t helloDatagramsReceived() const {
        return _helloDatagramsReceived;
    }
    uint64_t foreignDatagramsReceived() const {
        return _foreignDatagramsReceived;
    }

private:
    struct Slot {
        std::unique_ptr<UdpControlLink> link;
        bool claimed = false;
    };

    void dispatch(const uint8_t* bytes, size_t length, const Endpoint& from,
                  uint64_t nowMs);

    UdpSock* _udp = nullptr;
    std::vector<Slot> _links;
    ForeignSink _foreign;
    InjectedPump _injectedPump;
    uint32_t _rejectedForCapacity = 0;
    uint64_t _datagramsReceived = 0;
    uint64_t _helloDatagramsReceived = 0;
    uint64_t _foreignDatagramsReceived = 0;
    bool _pumping = false;
};

struct ControlDialStatus {
    uint64_t elapsedMs = 0;
    bool punchAvailable = false;
    bool peerMappingKnown = false;
    uint32_t mappingsReceived = 0;
    uint32_t mappingsRejected = 0;
    uint32_t mappingTargetMismatches = 0;
    uint32_t mappingExternalPortAliases = 0;
    uint32_t mappingSameIpAlternates = 0;
    uint16_t lastMappingInternalPort = 0;
    uint16_t lastMappingNatPort = 0;
    uint32_t lastMappingIpv4NetworkOrder = 0;
};

bool dialControlLink(UdpSock& socket, const Endpoint& advertised,
                     const Endpoint* punchRelay, int budgetMs,
                     UdpControlLink& link,
                     const std::function<bool(const ControlDialStatus&)>& onTick,
                     std::string& error);

static_assert(MSG_LINK_HELLO == 35 && MSG_LINK_HELLO_ACK == 36,
              "control-link establishment types moved");

}
