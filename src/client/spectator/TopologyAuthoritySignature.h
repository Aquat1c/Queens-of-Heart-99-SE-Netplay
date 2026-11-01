#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace qclient::spectator::topology {

// CNG's BCRYPT_ECCPUBLIC_BLOB for an ECDSA P-256 public key consists of the
// eight-byte BCRYPT_ECCKEY_BLOB header followed by 32-byte X and Y values.
constexpr std::size_t kTopologyAuthorityPublicBlobBytes = 72u;
constexpr std::size_t kTopologyAuthoritySignatureBytes = 64u;
constexpr std::size_t kTopologyAuthoritySignatureMaxInputBytes = 4096u;

using TopologyAuthorityPublicBlob =
    std::array<std::uint8_t, kTopologyAuthorityPublicBlobBytes>;
using TopologyAuthoritySignature =
    std::array<std::uint8_t, kTopologyAuthoritySignatureBytes>;

class TopologyAuthoritySigner final {
public:
    TopologyAuthoritySigner() noexcept;
    ~TopologyAuthoritySigner();

    TopologyAuthoritySigner(const TopologyAuthoritySigner&) = delete;
    TopologyAuthoritySigner& operator=(const TopologyAuthoritySigner&) = delete;
    TopologyAuthoritySigner(TopologyAuthoritySigner&&) = delete;
    TopologyAuthoritySigner& operator=(TopologyAuthoritySigner&&) = delete;

    bool generate() noexcept;
    void reset() noexcept;
    bool ready() const noexcept;

    bool publicBlob(TopologyAuthorityPublicBlob& out) const noexcept;

    bool sign(const std::uint8_t* input, std::size_t inputBytes,
              TopologyAuthoritySignature& out) const noexcept;

private:
    struct State;
    std::unique_ptr<State> _state;
};

class TopologyAuthoritySignatureVerifier final {
public:
    TopologyAuthoritySignatureVerifier() noexcept;
    ~TopologyAuthoritySignatureVerifier();

    TopologyAuthoritySignatureVerifier(
        const TopologyAuthoritySignatureVerifier&) = delete;
    TopologyAuthoritySignatureVerifier& operator=(
        const TopologyAuthoritySignatureVerifier&) = delete;
    TopologyAuthoritySignatureVerifier(
        TopologyAuthoritySignatureVerifier&&) = delete;
    TopologyAuthoritySignatureVerifier& operator=(
        TopologyAuthoritySignatureVerifier&&) = delete;

    bool initialize(const std::uint8_t* blob, std::size_t blobBytes) noexcept;
    bool initialize(const TopologyAuthorityPublicBlob& blob) noexcept {
        return initialize(blob.data(), blob.size());
    }
    void reset() noexcept;
    bool ready() const noexcept;

    bool verify(const std::uint8_t* input, std::size_t inputBytes,
                const std::uint8_t* signature,
                std::size_t signatureBytes) const noexcept;
    bool verify(const std::uint8_t* input, std::size_t inputBytes,
                const TopologyAuthoritySignature& signature) const noexcept {
        return verify(input, inputBytes, signature.data(), signature.size());
    }

private:
    struct State;
    std::unique_ptr<State> _state;
};

}
