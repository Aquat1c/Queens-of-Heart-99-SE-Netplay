#pragma once
#include "Protocol.h"
#include <cstdint>
#include <functional>
#include <list>
#include <vector>

namespace qnet {

constexpr uint32_t kResendIntervalMs   = 50;
constexpr uint32_t kKeepAliveEveryMs   = 1000;
constexpr uint32_t kKeepAliveTimeoutMs = 10000;

class ReliableChannel {
public:
    using RawSend = std::function<void(const uint8_t* data, size_t len)>;
    using OnMessage = std::function<void(uint8_t innerType, const uint8_t* body, size_t len)>;

    void init(RawSend rawSend, OnMessage onMessage, bool keepAlive, uint64_t nowMs);

    void send(uint8_t innerType, const void* body, size_t bodyLen, uint64_t nowMs);

    void onDatagram(uint8_t type, const uint8_t* body, size_t bodyLen, uint64_t nowMs);

    void notifyActivity(uint64_t nowMs) { _lastRecvMs = nowMs; }

    void update(uint64_t nowMs);

    bool isTimedOut(uint64_t nowMs) const {
        return _keepAlive && _started && (nowMs - _lastRecvMs > kKeepAliveTimeoutMs);
    }
    size_t unackedCount() const { return _unacked.size(); }
    uint32_t localSeq()  const { return _sendSeq; }
    uint32_t remoteSeq() const { return _recvSeq; }

private:
    struct Pending { uint32_t seq; std::vector<uint8_t> datagram; };

    void transmit(const Pending& p);
    void sendAck(uint64_t nowMs);

    RawSend   _rawSend;
    OnMessage _onMessage;
    std::list<Pending> _unacked;
    uint32_t _sendSeq = 0;
    uint32_t _recvSeq = 0;
    uint64_t _lastResendMs = 0;
    uint64_t _lastSendMs = 0;
    uint64_t _lastRecvMs = 0;
    bool     _keepAlive = false;
    bool     _started = false;
};

}
