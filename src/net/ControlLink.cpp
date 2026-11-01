#include "ControlLink.h"

#include "AutopunchSession.h"
#include "SecureRandom.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace qnet {
namespace {

bool isLinkType(uint8_t type) {
    return type == MSG_REL_DATA || type == MSG_REL_ACK
        || type == MSG_KEEPALIVE || type == MSG_LINK_HELLO
        || type == MSG_LINK_HELLO_ACK;
}

uint64_t nowMsOf() { return nowUs() / 1000ull; }

}

void UdpControlLink::begin(UdpSock& socket, uint64_t nowMs) {
    _udp = &socket;
    _state = State::Connecting;
    _havePeer = false;
    _peerExpected = false;
    _relStarted = false;
    _inbox.clear();
    _lastErr = 0;
    _failure.clear();
    _stats = Stats{};
    _lastHelloMs = nowMs;
    _lastInboundMs = 0;
    _nonce = 0;
    _helloTargetCount = 0;
}

void UdpControlLink::startClient(UdpSock& socket, const Endpoint& peer,
                                 uint64_t nowMs) {
    begin(socket, nowMs);
    _isHost = false;
    _peer = peer;
    _havePeer = true;
    addHelloTarget(peer);

    _nonceDegraded = false;
    _nonce = secureRandom64(_nonceDegraded);
    if (_nonce == 0) {
        _nonce = nowUs() | 1ull;
        _nonceDegraded = true;
    }

    sendHello(nowMs);
}

bool UdpControlLink::greeted(const Endpoint& source) const {
    for (size_t i = 0; i < _helloTargetCount; ++i)
        if (_helloTargets[i] == source) return true;
    return false;
}

bool UdpControlLink::addHelloTarget(const Endpoint& target) {
    if (target.sa.sin_addr.s_addr == 0 || target.sa.sin_port == 0) return false;
    for (size_t i = 0; i < _helloTargetCount; ++i)
        if (_helloTargets[i] == target) return true;
    if (_helloTargetCount >= kMaxHelloTargets) return false;
    _helloTargets[_helloTargetCount++] = target;
    if (!_havePeer) { _peer = target; _havePeer = true; }
    return true;
}

void UdpControlLink::startHostListen(UdpSock& socket, uint64_t nowMs) {
    begin(socket, nowMs);
    _isHost = true;
}

void UdpControlLink::expectPeer(const Endpoint& peer) {
    _peer = peer;
    _peerExpected = true;
}

void UdpControlLink::initReliable(uint64_t nowMs) {
    UdpSock* socket = _udp;
    Endpoint* peer = &_peer;
    _rel.init(
        [socket, peer](const uint8_t* data, size_t len) {
            socket->sendTo(*peer, data, len);
        },
        [this](uint8_t innerType, const uint8_t* body, size_t len) {
            Received received;
            received.type = innerType;
            received.body.assign(body, body + len);
            _inbox.push_back(std::move(received));
        },
        true, nowMs);
    _relStarted = true;
    _lastInboundMs = nowMs;
}

void UdpControlLink::fail(const std::string& why) {
    if (_state != State::Closed && _failure.empty()) _failure = why;
    _state = State::Closed;
}

void UdpControlLink::close() {
    _state = State::Closed;
    _inbox.clear();
    _relStarted = false;
    _udp = nullptr;
}

void UdpControlLink::closeGraceful(int capMs) {
    if (_state == State::Established && _udp != nullptr && capMs > 0) {
        const uint64_t deadlineUs = nowUs() + static_cast<uint64_t>(capMs) * 1000ull;
        while (nowUs() < deadlineUs && _rel.unackedCount() > 0) {
            service();
            drainSocket(nowMsOf());
            if (_state != State::Established) break;
            _udp->waitReadable(5);
        }
    }
    close();
}

void UdpControlLink::sendRawTo(const Endpoint& to, uint8_t type,
                               const void* body, size_t bodyLen) {
    if (_udp == nullptr) return;
    uint8_t buffer[kMaxMsgSize];
    const size_t n = packMsg(type, body, bodyLen, buffer, sizeof(buffer));
    if (n == 0) return;
    if (!_udp->sendTo(to, buffer, n)) {
        _lastErr = _udp->lastError();
        _stats.sendFailures++;
    }
}

void UdpControlLink::sendRaw(uint8_t type, const void* body, size_t bodyLen) {
    if (!_havePeer) return;
    sendRawTo(_peer, type, body, bodyLen);
}

void UdpControlLink::sendHello(uint64_t nowMs) {
    const LinkHello hello{ kProtocolVersion, _nonce };
    for (size_t i = 0; i < _helloTargetCount; ++i) {
        sendRawTo(_helloTargets[i], MSG_LINK_HELLO, &hello, sizeof(hello));
        _stats.hellosSent++;
    }
    _lastHelloMs = nowMs;
}

bool UdpControlLink::sendMsg(uint8_t type, const void* body, size_t bodyLen) {
    if (_state != State::Established || _udp == nullptr) return false;
    if (_rel.unackedCount() >= kLinkMaxUnacked) {
        fail("the other side stopped acknowledging control messages");
        return false;
    }
    // Refuse before ReliableChannel: it consumes a sequence number before packMsg can fail, and the unsendable entry blocks its window permanently.
    if (sizeof(MsgHeader) + sizeof(RelData) + bodyLen > kLinkMaxDatagram)
        return false;
    _rel.send(type, body, bodyLen, nowMsOf());
    return true;
}

bool UdpControlLink::offerDatagram(const uint8_t* bytes, size_t length,
                                   const Endpoint& from) {
    if (_state == State::Closed || _udp == nullptr) return false;
    return classify(bytes, length, from, nowMsOf());
}

bool UdpControlLink::classify(const uint8_t* bytes, size_t length,
                              const Endpoint& from, uint64_t nowMs) {
    uint8_t type = 0;
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    if (!unpackMsg(bytes, length, type, body, bodyLen)) return false;
    if (!isLinkType(type)) return false;

    _stats.controlDatagrams++;
    _lastInboundMs = nowMs;

    switch (type) {
    case MSG_LINK_HELLO: {
        if (!_isHost) { _stats.rejected++; return true; }
        if (bodyLen != sizeof(LinkHello)) { _stats.rejected++; return true; }
        LinkHello hello{};
        std::memcpy(&hello, body, sizeof(hello));
        _stats.hellosReceived++;

        if (_peerExpected && !(from == _peer)) {
            _stats.rejected++;
            return true;
        }
        if (_state == State::Established) {
            if (from == _peer && hello.nonce == _nonce) {
                LinkHello ack{ kProtocolVersion, _nonce };
                sendRaw(MSG_LINK_HELLO_ACK, &ack, sizeof(ack));
                _stats.helloAcksSent++;
            } else {
                _stats.rejected++;
            }
            return true;
        }
        if (_state != State::Connecting) { _stats.rejected++; return true; }

        _peer = from;
        _havePeer = true;
        _nonce = hello.nonce;
        initReliable(nowMs);
        _state = State::Established;

        LinkHello ack{ kProtocolVersion, _nonce };
        sendRaw(MSG_LINK_HELLO_ACK, &ack, sizeof(ack));
        _stats.helloAcksSent++;
        return true;
    }

    case MSG_LINK_HELLO_ACK: {
        if (_isHost) { _stats.rejected++; return true; }
        if (bodyLen != sizeof(LinkHello)) { _stats.rejected++; return true; }
        LinkHello ack{};
        std::memcpy(&ack, body, sizeof(ack));
        _stats.helloAcksReceived++;

        if (ack.nonce != _nonce || _nonce == 0) { _stats.rejected++; return true; }
        if (!greeted(from)) { _stats.rejected++; return true; }
        if (_state != State::Connecting) return true;

        _peer = from;
        _havePeer = true;
        initReliable(nowMs);
        _state = State::Established;
        return true;
    }

    default:
        break;
    }

    if (_state != State::Established || !_havePeer) { _stats.rejected++; return true; }
    if (!(from == _peer)) { _stats.rejected++; return true; }
    _rel.onDatagram(type, body, bodyLen, nowMs);
    return true;
}

void UdpControlLink::drainSocket(uint64_t nowMs) {
    if (_udp == nullptr) return;
    for (;;) {
        uint8_t buffer[2048];
        Endpoint from{};
        const int received = _udp->recvFrom(buffer, sizeof(buffer), from);
        if (received <= 0) break;
        const size_t length = static_cast<size_t>(received);
        if (classify(buffer, length, from, nowMs)) continue;
        _stats.foreignDatagrams++;
        if (_foreign) _foreign(buffer, length, from);
    }
}

void UdpControlLink::service() {
    if (_udp == nullptr || _state == State::Closed) return;
    const uint64_t nowMs = nowMsOf();

    if (_state == State::Connecting) {
        if (!_isHost && nowMs - _lastHelloMs >= kLinkHelloIntervalMs)
            sendHello(nowMs);
        return;
    }

    if (_relStarted) {
        _rel.update(nowMs);
        if (_lastInboundMs != 0 && nowMs > _lastInboundMs
            && nowMs - _lastInboundMs > _idleTimeoutMs)
            fail("the other side stopped responding");
        else if (_rel.unackedCount() >= kLinkMaxUnacked)
            fail("the other side stopped acknowledging control messages");
    }
}

void UdpControlLink::poll(int waitMs) {
    if (_udp == nullptr || _state == State::Closed) return;
    service();
    if (_state == State::Closed) return;
    if (!_externalPump) drainSocket(nowMsOf());
    if (waitMs <= 0) return;
    if (_externalPump) _externalPump(waitMs);
    else _udp->waitReadable(waitMs);
}

ControlLink::Rs UdpControlLink::recvMsgEx(Received& out, int timeoutMs) {
    out.type = MSG_NONE;
    out.body.clear();
    if (_state == State::Closed || _udp == nullptr) return Rs::Closed;

    if (timeoutMs < 0) timeoutMs = 0;
    const uint64_t deadlineUs = nowUs() + static_cast<uint64_t>(timeoutMs) * 1000ull;

    for (;;) {
        service();
        if (_state == State::Closed) return Rs::Closed;
        if (_externalPump) _externalPump(0);
        else drainSocket(nowMsOf());

        if (!_inbox.empty()) {
            out = std::move(_inbox.front());
            _inbox.pop_front();
            return Rs::Msg;
        }
        if (_state == State::Closed) return Rs::Closed;

        const uint64_t now = nowUs();
        if (now >= deadlineUs) return Rs::Timeout;

        const uint64_t remainMs = (deadlineUs - now) / 1000ull;
        if (remainMs == 0) {
            return Rs::Timeout;
        }
        const int slice = static_cast<int>(remainMs < 5ull ? remainMs : 5ull);
        if (_externalPump) _externalPump(slice);
        else _udp->waitReadable(slice);
    }
}

bool UdpControlLink::connect(int timeoutMs) {
    if (_state == State::Established) return true;
    if (_state != State::Connecting || _udp == nullptr) return false;

    const uint64_t deadlineUs = nowUs() + static_cast<uint64_t>(timeoutMs) * 1000ull;
    for (;;) {
        service();
        if (_state == State::Closed) return false;
        if (!_externalPump) drainSocket(nowMsOf());
        if (_state == State::Established) return true;
        if (nowUs() >= deadlineUs) {
            fail(_isHost
                     ? "no player reached this host over the punched route"
                     : "the host did not answer over the punched route");
            return false;
        }
        if (_externalPump) _externalPump(5);
        else _udp->waitReadable(5);
    }
}

bool dialControlLink(UdpSock& socket, const Endpoint& advertised,
                     const Endpoint* punchRelay, int budgetMs,
                     UdpControlLink& link,
                     const std::function<bool(const ControlDialStatus&)>& onTick,
                     std::string& error) {
    autopunch::AutopunchSession punch;
    if (punchRelay != nullptr) {
        punch.configure(socket.boundPort(), *punchRelay);
        punch.setTarget(advertised.sa.sin_addr.s_addr,
                        ntohs(advertised.sa.sin_port));
    }

    const uint64_t startMs = nowUs() / 1000ull;
    link.startClient(socket, advertised, startMs);
    link.setForeignSink(
        [&punch](const uint8_t* bytes, size_t length, const Endpoint& from) {
            punch.handleInbound(bytes, length, from, nowUs() / 1000ull);
        });
    // Never leave the punch session's address captured in the link after the dial: a delayed relay answer would call through a dangling reference.
    const auto detachPunch = [&link]() { link.setForeignSink({}); };

    const auto sendVia = [](void* context, const Endpoint& destination,
                            const void* bytes, size_t length) -> bool {
        return static_cast<UdpSock*>(context)->sendTo(destination, bytes,
                                                      length);
    };

    Endpoint lastAddedMapping{};
    bool haveAddedMapping = false;
    uint64_t lastTickMs = startMs;
    for (;;) {
        const uint64_t nowMs = nowUs() / 1000ull;
        if (punch.configured()) punch.service(sendVia, &socket, nowMs);

        if (punch.havePeerMapping()
            && (!haveAddedMapping
                || !(lastAddedMapping == punch.peerMapping()))) {
            if (link.addHelloTarget(punch.peerMapping())) {
                lastAddedMapping = punch.peerMapping();
                haveAddedMapping = true;
            }
        }

        link.poll(5);
        if (link.established()) {
            error.clear();
            detachPunch();
            return true;
        }
        if (link.state() == UdpControlLink::State::Closed) {
            error = link.failureText().empty() ? "the control link closed"
                                               : link.failureText();
            detachPunch();
            return false;
        }

        const uint64_t elapsedMs = nowUs() / 1000ull - startMs;
        if (elapsedMs >= static_cast<uint64_t>(budgetMs)) break;

        if (onTick && nowMs - lastTickMs >= 100) {
            lastTickMs = nowMs;
            ControlDialStatus status{};
            status.elapsedMs = elapsedMs;
            status.punchAvailable = punch.configured();
            status.peerMappingKnown = punch.havePeerMapping();
            status.mappingsReceived = punch.stats().mappingsReceived;
            status.mappingsRejected = punch.stats().mappingsRejected;
            status.mappingTargetMismatches =
                punch.stats().mappingTargetMismatches;
            status.mappingExternalPortAliases =
                punch.stats().mappingExternalPortAliases;
            status.mappingSameIpAlternates =
                punch.stats().mappingSameIpAlternates;
            status.lastMappingInternalPort =
                punch.stats().lastMappingInternalPort;
            status.lastMappingNatPort = punch.stats().lastMappingNatPort;
            status.lastMappingIpv4NetworkOrder =
                punch.stats().lastMappingIpv4NetworkOrder;
            if (!onTick(status)) {
                error = "Cancelled";
                link.close();
                detachPunch();
                return false;
            }
        }
    }

    error = punchRelay != nullptr
        ? "The host did not answer. It may not be hosting, the address may be "
          "wrong, or neither network allowed a route between you."
        : "The host did not answer, and hole punching was unavailable.";
    link.close();
    detachPunch();
    return false;
}

void UdpControlListener::open(UdpSock& socket) {
    close();
    _udp = &socket;
}

void UdpControlListener::openInjected(UdpSock& socket, InjectedPump pump) {
    close();
    _udp = &socket;
    _injectedPump = std::move(pump);
}

void UdpControlListener::close() {
    for (Slot& slot : _links)
        if (slot.link) slot.link->close();
    _links.clear();
    _udp = nullptr;
    _injectedPump = {};
    _rejectedForCapacity = 0;
    _datagramsReceived = 0;
    _helloDatagramsReceived = 0;
    _foreignDatagramsReceived = 0;
}

void UdpControlListener::offerDatagram(const uint8_t* bytes, size_t length,
                                       const Endpoint& from) {
    if (_udp == nullptr || bytes == nullptr || length == 0) return;
    ++_datagramsReceived;
    dispatch(bytes, length, from, nowUs() / 1000ull);
}

void UdpControlListener::dispatch(const uint8_t* bytes, size_t length,
                                  const Endpoint& from, uint64_t nowMs) {
    for (Slot& slot : _links) {
        if (!slot.link) continue;
        if (slot.link->havePeer() && slot.link->peer() == from) {
            if (slot.link->offerDatagram(bytes, length, from)) return;
            break;
        }
    }

    uint8_t type = 0;
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    if (!unpackMsg(bytes, length, type, body, bodyLen)
        || type != MSG_LINK_HELLO || bodyLen != sizeof(LinkHello)) {
        ++_foreignDatagramsReceived;
        if (_foreign) _foreign(bytes, length, from);
        return;
    }
    ++_helloDatagramsReceived;

    if (_links.size() >= kLinkMaxPeers) {
        _rejectedForCapacity++;
        return;
    }

    Slot slot;
    slot.link = std::make_unique<UdpControlLink>();
    UdpControlLink* link = slot.link.get();
    link->startHostListen(*_udp, nowMs);
    link->expectPeer(from);
    link->setExternalPump([this](int waitMs) { pump(waitMs); });
    link->setForeignSink(_foreign);
    link->offerDatagram(bytes, length, from);
    _links.push_back(std::move(slot));
}

void UdpControlListener::pump(int waitMs) {
    if (_udp == nullptr) return;
    if (_pumping) return;
    _pumping = true;

    if (_injectedPump) {
        _injectedPump(*this);
    } else {
        for (;;) {
            uint8_t buffer[2048];
            Endpoint from{};
            const int received = _udp->recvFrom(buffer, sizeof(buffer), from);
            if (received <= 0) break;
            offerDatagram(buffer, static_cast<size_t>(received), from);
        }
    }
    for (Slot& slot : _links)
        if (slot.link) slot.link->service();

    for (size_t i = _links.size(); i-- > 0;) {
        Slot& slot = _links[i];
        if (!slot.claimed && slot.link
            && slot.link->state() == UdpControlLink::State::Closed)
            _links.erase(_links.begin() + static_cast<ptrdiff_t>(i));
    }

    if (waitMs > 0) {
        if (_injectedPump) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        } else {
            _udp->waitReadable(waitMs);
        }
    }
    _pumping = false;
}

UdpControlLink* UdpControlListener::acceptOne() {
    for (Slot& slot : _links) {
        if (!slot.claimed && slot.link && slot.link->established()) {
            slot.claimed = true;
            return slot.link.get();
        }
    }
    return nullptr;
}

void UdpControlListener::release(UdpControlLink* link) {
    for (size_t i = 0; i < _links.size(); ++i) {
        if (_links[i].link.get() == link) {
            _links[i].link->close();
            _links.erase(_links.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

}
