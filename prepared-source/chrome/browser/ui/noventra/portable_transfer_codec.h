// Copyright 2026 The Noventra Authors. All rights reserved.

#ifndef CHROME_BROWSER_UI_NOVENTRA_PORTABLE_TRANSFER_CODEC_H_
#define CHROME_BROWSER_UI_NOVENTRA_PORTABLE_TRANSFER_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"

namespace noventra {

// An authenticated, password-protected transfer envelope. This module performs
// no file I/O and does not parse or apply the decrypted browser document.
//
// Call on a long-CPU-work-allowed worker sequence, with at most one operation
// per transfer controller. The fixed scrypt preset uses about 128 MiB of
// working memory. Never call synchronously from a browser/UI or network
// sequence. Passwords are byte strings: the UI may enforce a stronger character
// policy, but must preserve the exact UTF-8 bytes (including whitespace) on
// both sides. The caller owns and must clear its password and plaintext buffers
// when done.
class PortableTransferCodec final {
 public:
  PortableTransferCodec() = delete;

  static constexpr size_t kMinimumPasswordBytes = 12;
  static constexpr size_t kMaximumPasswordBytes = 1024;
  static constexpr size_t kMaximumPlaintextBytes = 32 * 1024 * 1024;
  static constexpr size_t kEnvelopeOverheadBytes = 64;
  static constexpr size_t kMaximumEnvelopeBytes =
      kMaximumPlaintextBytes + kEnvelopeOverheadBytes;

  using SealResult = base::expected<std::vector<uint8_t>, std::string>;
  using OpenResult = base::expected<std::string, std::string>;

  // Version 1 consists of a 20-byte fixed format/version/KDF/algorithm header,
  // a 16-byte salt, a 12-byte nonce, ciphertext and a 16-byte authentication
  // tag. The whole 48-byte prefix is authenticated. Unsupported or oversized
  // inputs are rejected before deriving a key; input bytes cannot choose KDF
  // costs.
  static SealResult Seal(std::string_view plaintext, std::string_view password);
  static OpenResult Open(base::span<const uint8_t> envelope,
                         std::string_view password);

 private:
  static void DeriveKey(std::string_view password,
                        base::span<const uint8_t> salt,
                        base::span<uint8_t> key);
};

}  // namespace noventra

#endif  // CHROME_BROWSER_UI_NOVENTRA_PORTABLE_TRANSFER_CODEC_H_
