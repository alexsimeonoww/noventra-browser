// Copyright 2026 The Noventra Authors. All rights reserved.

#include "chrome/browser/ui/noventra/portable_transfer_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_view_util.h"
#include "base/threading/thread_restrictions.h"
#include "crypto/aead.h"
#include "crypto/kdf.h"
#include "crypto/random.h"
#include "crypto/secure_util.h"
#include "crypto/subtle_passkey.h"

namespace noventra {
namespace {

// The identifiers select one fixed v1 combination, not arbitrary parameters:
// version 1, scrypt preset 1, AES-256-GCM-SIV, reserved flags 0.
constexpr auto kHeader =
    std::to_array<uint8_t>({'N', 'o', 'v', 'e', 'n', 't', 'r', 'a', 'T', 'r',
                            'a', 'n', 's', 'f', 'e', 'r', 1,   1,   1,   0});
constexpr size_t kSaltBytes = 16;
constexpr size_t kNonceBytes = 12;
constexpr size_t kKeyBytes = 32;
constexpr size_t kTagBytes = 16;
constexpr size_t kSaltOffset = kHeader.size();
constexpr size_t kNonceOffset = kSaltOffset + kSaltBytes;
constexpr size_t kCiphertextOffset = kNonceOffset + kNonceBytes;
constexpr auto kAlgorithm = crypto::aead::AES_256_GCM_SIV;

static_assert(kCiphertextOffset + kTagBytes ==
              PortableTransferCodec::kEnvelopeOverheadBytes);

// N=2^17, r=8, p=1 requires 128 MiB plus small implementation overhead.
// The 256 MiB cap leaves room for that overhead without permitting the envelope
// to request a higher cost. These constants are part of the version-1 format.
constexpr crypto::kdf::ScryptParams kScryptParams{
    .cost = 1 << 17,
    .block_size = 8,
    .parallelization = 1,
    .max_memory_bytes = 256 * 1024 * 1024};

constexpr char kPasswordError[] = "Use a password between 12 and 1,024 bytes.";
constexpr char kFormatError[] =
    "This is not a supported Noventra transfer file.";
constexpr char kAuthenticationError[] =
    "The password is incorrect or the transfer file is damaged.";
constexpr char kSizeError[] = "The transfer is larger than the 32 MB limit.";

bool IsPasswordLengthAllowed(std::string_view password) {
  return password.size() >= PortableTransferCodec::kMinimumPasswordBytes &&
         password.size() <= PortableTransferCodec::kMaximumPasswordBytes;
}

}  // namespace

// static
PortableTransferCodec::SealResult PortableTransferCodec::Seal(
    std::string_view plaintext,
    std::string_view password) {
  if (!IsPasswordLengthAllowed(password)) {
    return base::unexpected(kPasswordError);
  }
  if (plaintext.size() > kMaximumPlaintextBytes) {
    return base::unexpected(kSizeError);
  }
  base::AssertLongCPUWorkAllowed();

  std::array<uint8_t, kCiphertextOffset> prefix;
  std::copy(kHeader.begin(), kHeader.end(), prefix.begin());
  crypto::RandBytes(base::span(prefix).subspan(kSaltOffset, kSaltBytes));
  crypto::RandBytes(base::span(prefix).subspan(kNonceOffset, kNonceBytes));
  std::array<uint8_t, kKeyBytes> key;
  DeriveKey(password, base::span(prefix).subspan(kSaltOffset, kSaltBytes), key);
  auto ciphertext = crypto::aead::Seal(
      kAlgorithm, key, base::as_byte_span(plaintext),
      base::span(prefix).subspan(kNonceOffset, kNonceBytes), prefix);
  crypto::SecureZeroBuffer(key);
  if (ciphertext.size() != plaintext.size() + kTagBytes) {
    return base::unexpected("The transfer could not be encrypted.");
  }

  std::vector<uint8_t> envelope;
  envelope.reserve(prefix.size() + ciphertext.size());
  envelope.insert(envelope.end(), prefix.begin(), prefix.end());
  envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
  return envelope;
}

// static
PortableTransferCodec::OpenResult PortableTransferCodec::Open(
    base::span<const uint8_t> envelope,
    std::string_view password) {
  if (!IsPasswordLengthAllowed(password)) {
    return base::unexpected(kPasswordError);
  }
  if (envelope.size() > kMaximumEnvelopeBytes) {
    return base::unexpected(kSizeError);
  }
  if (envelope.size() < kEnvelopeOverheadBytes ||
      !std::ranges::equal(envelope.first(kHeader.size()), kHeader)) {
    return base::unexpected(kFormatError);
  }
  base::AssertLongCPUWorkAllowed();

  std::array<uint8_t, kKeyBytes> key;
  DeriveKey(password, envelope.subspan(kSaltOffset, kSaltBytes), key);
  auto plaintext =
      crypto::aead::Open(kAlgorithm, key, envelope.subspan(kCiphertextOffset),
                         envelope.subspan(kNonceOffset, kNonceBytes),
                         envelope.first(kCiphertextOffset));
  crypto::SecureZeroBuffer(key);
  if (!plaintext) {
    return base::unexpected(kAuthenticationError);
  }
  std::string result(base::as_string_view(*plaintext));
  crypto::SecureZeroBuffer(*plaintext);
  return base::ok(std::move(result));
}

// static
void PortableTransferCodec::DeriveKey(std::string_view password,
                                      base::span<const uint8_t> salt,
                                      base::span<uint8_t> key) {
  crypto::kdf::Scrypt(kScryptParams, base::as_byte_span(password), salt, key,
                      crypto::SubtlePassKey());
}

}  // namespace noventra
