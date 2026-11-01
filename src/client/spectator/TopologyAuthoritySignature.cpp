#include "TopologyAuthoritySignature.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace qclient::spectator::topology {
namespace {

constexpr ULONG kP256KeyBits = 256u;
constexpr ULONG kP256CoordinateBytes = 32u;
constexpr std::size_t kMaxHashObjectBytes = 64u * 1024u;

static_assert(sizeof(BCRYPT_ECCKEY_BLOB) + 2u * kP256CoordinateBytes
                  == kTopologyAuthorityPublicBlobBytes,
              "P-256 public blob size must stay canonical");
static_assert(2u * kP256CoordinateBytes == kTopologyAuthoritySignatureBytes,
              "P-256 CNG signature size must stay canonical");

bool cngOk(const NTSTATUS status) noexcept {
    return status >= 0;
}

void zero(void* bytes, const std::size_t count) noexcept {
    if (bytes != nullptr && count != 0u) SecureZeroMemory(bytes, count);
}

bool validInput(const std::uint8_t* input,
                const std::size_t inputBytes) noexcept {
    return input != nullptr && inputBytes != 0u
        && inputBytes <= kTopologyAuthoritySignatureMaxInputBytes;
}

bool validPublicBlob(const std::uint8_t* blob,
                     const std::size_t blobBytes) noexcept {
    if (blob == nullptr || blobBytes != kTopologyAuthorityPublicBlobBytes)
        return false;

    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, blob, sizeof(header));
    if (header.dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC
        || header.cbKey != kP256CoordinateBytes) {
        return false;
    }

    std::uint8_t anyCoordinateByte = 0u;
    for (std::size_t index = sizeof(header); index < blobBytes; ++index)
        anyCoordinateByte |= blob[index];
    return anyCoordinateByte != 0u;
}

bool hashInput(const std::uint8_t* input, const std::size_t inputBytes,
               std::array<std::uint8_t, 32u>& digest) noexcept {
    zero(digest.data(), digest.size());
    if (!validInput(input, inputBytes)) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    bool ok = false;

    if (!cngOk(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        goto done;
    }

    ULONG objectBytes = 0u;
    ULONG hashBytes = 0u;
    ULONG returnedBytes = 0u;
    if (!cngOk(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
            &returnedBytes, 0))
        || objectBytes == 0u || objectBytes > kMaxHashObjectBytes
        || !cngOk(BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
            &returnedBytes, 0))
        || hashBytes != digest.size()) {
        goto done;
    }

    try {
        object.resize(objectBytes);
    } catch (const std::bad_alloc&) {
        goto done;
    } catch (...) {
        goto done;
    }

    if (!cngOk(BCryptCreateHash(
            algorithm, &hash, object.data(), objectBytes, nullptr, 0u, 0))
        || !cngOk(BCryptHashData(
            hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(input)),
            static_cast<ULONG>(inputBytes), 0))
        || !cngOk(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        goto done;
    }
    ok = true;

done:
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    zero(object.data(), object.size());
    if (!ok) zero(digest.data(), digest.size());
    return ok;
}

}

struct TopologyAuthoritySigner::State final {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE privateKey = nullptr;
    TopologyAuthorityPublicBlob publicKey{};

    ~State() {
        if (privateKey != nullptr) BCryptDestroyKey(privateKey);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        zero(publicKey.data(), publicKey.size());
    }
};

struct TopologyAuthoritySignatureVerifier::State final {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE publicKey = nullptr;

    ~State() {
        if (publicKey != nullptr) BCryptDestroyKey(publicKey);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

TopologyAuthoritySigner::TopologyAuthoritySigner() noexcept = default;
TopologyAuthoritySigner::~TopologyAuthoritySigner() = default;

bool TopologyAuthoritySigner::generate() noexcept {
    reset();
    try {
        auto candidate = std::make_unique<State>();
        if (!cngOk(BCryptOpenAlgorithmProvider(
                &candidate->algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                nullptr, 0))
            || !cngOk(BCryptGenerateKeyPair(
                candidate->algorithm, &candidate->privateKey, kP256KeyBits, 0))
            || !cngOk(BCryptFinalizeKeyPair(candidate->privateKey, 0))) {
            return false;
        }

        ULONG publicBytes = 0u;
        if (!cngOk(BCryptExportKey(
                candidate->privateKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                nullptr, 0u, &publicBytes, 0))
            || publicBytes != candidate->publicKey.size()) {
            return false;
        }
        ULONG writtenBytes = 0u;
        if (!cngOk(BCryptExportKey(
                candidate->privateKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                candidate->publicKey.data(), publicBytes, &writtenBytes, 0))
            || writtenBytes != candidate->publicKey.size()
            || !validPublicBlob(candidate->publicKey.data(),
                                candidate->publicKey.size())) {
            return false;
        }

        _state = std::move(candidate);
        return true;
    } catch (...) {
        reset();
        return false;
    }
}

void TopologyAuthoritySigner::reset() noexcept {
    _state.reset();
}

bool TopologyAuthoritySigner::ready() const noexcept {
    return _state != nullptr && _state->algorithm != nullptr
        && _state->privateKey != nullptr;
}

bool TopologyAuthoritySigner::publicBlob(
    TopologyAuthorityPublicBlob& out) const noexcept {
    zero(out.data(), out.size());
    if (!ready()) return false;
    std::memcpy(out.data(), _state->publicKey.data(), out.size());
    return true;
}

bool TopologyAuthoritySigner::sign(
    const std::uint8_t* input, const std::size_t inputBytes,
    TopologyAuthoritySignature& out) const noexcept {
    zero(out.data(), out.size());
    if (!ready()) return false;

    std::array<std::uint8_t, 32u> digest{};
    if (!hashInput(input, inputBytes, digest)) return false;

    ULONG writtenBytes = 0u;
    const bool ok = cngOk(BCryptSignHash(
        _state->privateKey, nullptr, digest.data(),
        static_cast<ULONG>(digest.size()), out.data(),
        static_cast<ULONG>(out.size()), &writtenBytes, 0))
        && writtenBytes == out.size();
    zero(digest.data(), digest.size());
    if (!ok) zero(out.data(), out.size());
    return ok;
}

TopologyAuthoritySignatureVerifier::TopologyAuthoritySignatureVerifier() noexcept = default;
TopologyAuthoritySignatureVerifier::~TopologyAuthoritySignatureVerifier() = default;

bool TopologyAuthoritySignatureVerifier::initialize(
    const std::uint8_t* blob, const std::size_t blobBytes) noexcept {
    reset();
    if (!validPublicBlob(blob, blobBytes)) return false;

    try {
        auto candidate = std::make_unique<State>();
        if (!cngOk(BCryptOpenAlgorithmProvider(
                &candidate->algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                nullptr, 0))
            || !cngOk(BCryptImportKeyPair(
                candidate->algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                &candidate->publicKey,
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(blob)),
                static_cast<ULONG>(blobBytes), 0))) {
            return false;
        }
        _state = std::move(candidate);
        return true;
    } catch (...) {
        reset();
        return false;
    }
}

void TopologyAuthoritySignatureVerifier::reset() noexcept {
    _state.reset();
}

bool TopologyAuthoritySignatureVerifier::ready() const noexcept {
    return _state != nullptr && _state->algorithm != nullptr
        && _state->publicKey != nullptr;
}

bool TopologyAuthoritySignatureVerifier::verify(
    const std::uint8_t* input, const std::size_t inputBytes,
    const std::uint8_t* signature, const std::size_t signatureBytes) const noexcept {
    if (!ready() || signature == nullptr
        || signatureBytes != kTopologyAuthoritySignatureBytes) {
        return false;
    }

    std::array<std::uint8_t, 32u> digest{};
    if (!hashInput(input, inputBytes, digest)) return false;
    const bool ok = cngOk(BCryptVerifySignature(
        _state->publicKey, nullptr, digest.data(),
        static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(signature)),
        static_cast<ULONG>(signatureBytes), 0));
    zero(digest.data(), digest.size());
    return ok;
}

}
