#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"
#include "VanillaNetProtocol.hpp"

namespace qoh99 {

class VanillaDirectPlayDispatcher {
public:
    explicit VanillaDirectPlayDispatcher(MemoryView& m) : m_(m) {}

    std::uint32_t frameSeq() const   { return m_.get<std::uint32_t>(addr::kNetFrameSeq); }
    std::uint32_t ringCount() const  { return m_.get<std::uint32_t>(addr::kNetInputRingCount); }
    std::uint32_t ringSlot(int i) const {
        return m_.get<std::uint32_t>(addr::kNetInputRing + 4u * (static_cast<GameAddr>(i) % 3u));
    }
    std::uint32_t remoteInput() const{ return m_.get<std::uint32_t>(addr::kNetRemoteInput); }
    int localSide() const            { return m_.get<int>(addr::kNetLocalSide); }
    bool ready() const               { return m_.get<int>(addr::kNetReadyFlag) != 0; }
    bool clientConnected() const     { return m_.get<int>(addr::kNetClientGotFlag) != 0; }
    bool isNetworkSession() const    { return m_.get<int>(addr::kNetIsNetworkSession) != 0; }

    void applyScalar(const vnet::ScalarMsg& msg);
    bool applyInput(const vnet::InputMsg& msg);

    static constexpr int kRingDepth = 3;

private:
    MemoryView& m_;
};

enum class NetSide : int { None = 0, Host = 1, Client = 2 };

class DirectPlaySystem {
public:
    explicit DirectPlaySystem(MemoryView& m) : m_(m), disp_(m) {}

    VanillaDirectPlayDispatcher& dispatcher() { return disp_; }

    NetSide side() const { return static_cast<NetSide>(m_.get<int>(addr::kNetLocalSide)); }
    bool hostShouldSendBoot() const {
        return disp_.isNetworkSession() && disp_.clientConnected() && !disp_.ready();
    }

private:
    MemoryView& m_;
    VanillaDirectPlayDispatcher disp_;
};

}
