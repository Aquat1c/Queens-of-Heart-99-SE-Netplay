#include "Reliable.h"
#include <cstring>

namespace qnet {

void ReliableChannel::init(RawSend rawSend, OnMessage onMessage, bool keepAlive, uint64_t nowMs) {
    _rawSend = std::move(rawSend);
    _onMessage = std::move(onMessage);
    _keepAlive = keepAlive;
    _unacked.clear();
    _sendSeq = _recvSeq = 0;
    _lastResendMs = _lastSendMs = _lastRecvMs = nowMs;
    _started = true;
}

void ReliableChannel::send(uint8_t innerType, const void* body, size_t bodyLen, uint64_t nowMs) {
    std::vector<uint8_t> inner(sizeof(RelData) + bodyLen);
    RelData rd{};
    rd.seq = ++_sendSeq;
    rd.innerType = innerType;
    std::memcpy(inner.data(), &rd, sizeof(rd));
    if (bodyLen) std::memcpy(inner.data() + sizeof(rd), body, bodyLen);

    Pending p;
    p.seq = rd.seq;
    p.datagram.resize(kMaxMsgSize);
    size_t n = packMsg(MSG_REL_DATA, inner.data(), inner.size(), p.datagram.data(), p.datagram.size());
    p.datagram.resize(n);

    // Enqueue before transmitting: transmit() can deliver the ACK re-entrantly, and it must find this message in the unacked window.
    _unacked.push_back(std::move(p));
    transmit(_unacked.back());
    _lastSendMs = nowMs;
}

void ReliableChannel::transmit(const Pending& p) {
    if (_rawSend && !p.datagram.empty())
        _rawSend(p.datagram.data(), p.datagram.size());
}

void ReliableChannel::sendAck(uint64_t nowMs) {
    RelAck ack{ _recvSeq };
    uint8_t buf[64];
    size_t n = packMsg(MSG_REL_ACK, &ack, sizeof(ack), buf, sizeof(buf));
    if (_rawSend && n) _rawSend(buf, n);
    _lastSendMs = nowMs;
}

void ReliableChannel::onDatagram(uint8_t type, const uint8_t* body, size_t bodyLen, uint64_t nowMs) {
    _lastRecvMs = nowMs;

    switch (type) {
        case MSG_REL_DATA: {
            if (bodyLen < sizeof(RelData)) return;
            RelData rd;
            std::memcpy(&rd, body, sizeof(rd));
            if (rd.seq == _recvSeq + 1) {
                _recvSeq = rd.seq;
                if (_onMessage)
                    _onMessage(rd.innerType, body + sizeof(RelData), bodyLen - sizeof(RelData));
                sendAck(nowMs);
            } else {
                sendAck(nowMs);
            }
            break;
        }
        case MSG_REL_ACK: {
            if (bodyLen < sizeof(RelAck)) return;
            RelAck ack;
            std::memcpy(&ack, body, sizeof(ack));
            while (!_unacked.empty() && _unacked.front().seq <= ack.ackSeq)
                _unacked.pop_front();
            break;
        }
        case MSG_KEEPALIVE:
            break;
        default:
            break;
    }
}

void ReliableChannel::update(uint64_t nowMs) {
    if (!_started) return;

    // Snapshot first: transmit() can deliver an ACK re-entrantly and mutate _unacked during iteration.
    if (!_unacked.empty() && nowMs - _lastResendMs >= kResendIntervalMs) {
        std::vector<std::vector<uint8_t>> snapshot;
        snapshot.reserve(_unacked.size());
        for (const Pending& p : _unacked) snapshot.push_back(p.datagram);
        for (const auto& d : snapshot)
            if (_rawSend && !d.empty()) _rawSend(d.data(), d.size());
        _lastResendMs = nowMs;
        _lastSendMs = nowMs;
    }

    if (_keepAlive && nowMs - _lastSendMs >= kKeepAliveEveryMs) {
        uint8_t buf[32];
        size_t n = packMsg(MSG_KEEPALIVE, nullptr, 0, buf, sizeof(buf));
        if (_rawSend && n) _rawSend(buf, n);
        _lastSendMs = nowMs;
    }
}

}
