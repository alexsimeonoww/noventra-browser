// Copyright 2026 The Noventra Authors. All rights reserved.

#include "chrome/browser/ui/noventra/portable_transfer_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace noventra {
namespace {

constexpr std::string_view kPassword = "fixture-transfer-password";
constexpr std::string_view kDocument =
    R"({"format":"noventra-portable-data","version":1,"title":"Fixture"})";

TEST(PortableTransferCodecTest, RoundTripPreservesExactBinaryPlaintext) {
  const std::string document("a\0b\xff", 4);
  auto sealed = PortableTransferCodec::Seal(document, kPassword);
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->size(),
            document.size() + PortableTransferCodec::kEnvelopeOverheadBytes);
  auto opened = PortableTransferCodec::Open(*sealed, kPassword);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(*opened, document);
}

TEST(PortableTransferCodecTest, EmptyPlaintextStillHasAnAuthenticationTag) {
  auto sealed = PortableTransferCodec::Seal("", kPassword);
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->size(), PortableTransferCodec::kEnvelopeOverheadBytes);
  auto opened = PortableTransferCodec::Open(*sealed, kPassword);
  ASSERT_TRUE(opened.has_value());
  EXPECT_TRUE(opened->empty());
}

TEST(PortableTransferCodecTest, SaltAndNonceChangeForEveryEnvelope) {
  auto first = PortableTransferCodec::Seal(kDocument, kPassword);
  auto second = PortableTransferCodec::Seal(kDocument, kPassword);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(std::ranges::equal(base::span(*first).subspan<20, 16>(),
                                  base::span(*second).subspan<20, 16>()));
  EXPECT_FALSE(std::ranges::equal(base::span(*first).subspan<36, 12>(),
                                  base::span(*second).subspan<36, 12>()));
  EXPECT_NE(*first, *second);
  EXPECT_EQ(std::search(first->begin(), first->end(), kDocument.begin(),
                        kDocument.end()),
            first->end());
}

TEST(PortableTransferCodecTest, WrongPasswordAndCiphertextDamageReleaseNoData) {
  auto sealed = PortableTransferCodec::Seal(kDocument, kPassword);
  ASSERT_TRUE(sealed.has_value());
  auto wrong_password =
      PortableTransferCodec::Open(*sealed, "incorrect-fixture-password");
  ASSERT_FALSE(wrong_password.has_value());

  sealed->back() ^= 1;
  auto damaged = PortableTransferCodec::Open(*sealed, kPassword);
  ASSERT_FALSE(damaged.has_value());
  EXPECT_EQ(wrong_password.error(), damaged.error());
}

TEST(PortableTransferCodecTest, SaltNonceCiphertextAndTagAreAuthenticated) {
  auto sealed = PortableTransferCodec::Seal(kDocument, kPassword);
  ASSERT_TRUE(sealed.has_value());
  for (const size_t offset :
       {size_t{20}, size_t{36}, size_t{48}, sealed->size() - 1}) {
    SCOPED_TRACE(offset);
    auto changed = *sealed;
    changed[offset] ^= 1;
    EXPECT_FALSE(PortableTransferCodec::Open(changed, kPassword).has_value());
  }
}

TEST(PortableTransferCodecTest, RejectsUnknownHeaderVersionKdfAndAlgorithm) {
  auto sealed = PortableTransferCodec::Seal(kDocument, kPassword);
  ASSERT_TRUE(sealed.has_value());
  // Fixed header positions include magic, version, preset, algorithm and flags.
  // Every such change is rejected before selecting or invoking a KDF.
  for (size_t offset = 0; offset < 20; ++offset) {
    SCOPED_TRACE(offset);
    auto changed = *sealed;
    changed[offset] ^= 1;
    EXPECT_FALSE(PortableTransferCodec::Open(changed, kPassword).has_value());
  }
}

TEST(PortableTransferCodecTest, RejectsTruncatedOrAppendedCiphertext) {
  auto sealed = PortableTransferCodec::Seal(kDocument, kPassword);
  ASSERT_TRUE(sealed.has_value());
  for (size_t length = 0;
       length < PortableTransferCodec::kEnvelopeOverheadBytes; ++length) {
    SCOPED_TRACE(length);
    EXPECT_FALSE(PortableTransferCodec::Open(base::span(*sealed).first(length),
                                             kPassword)
                     .has_value());
  }
  auto truncated = *sealed;
  truncated.pop_back();
  EXPECT_FALSE(PortableTransferCodec::Open(truncated, kPassword).has_value());
  auto appended = *sealed;
  appended.push_back(0);
  EXPECT_FALSE(PortableTransferCodec::Open(appended, kPassword).has_value());
}

TEST(PortableTransferCodecTest, RejectsPasswordLengthsOutsideBounds) {
  for (const auto& password :
       {std::string(),
        std::string(PortableTransferCodec::kMinimumPasswordBytes - 1, 'p'),
        std::string(PortableTransferCodec::kMaximumPasswordBytes + 1, 'p')}) {
    EXPECT_FALSE(PortableTransferCodec::Seal(kDocument, password).has_value());
    const std::vector<uint8_t> envelope(
        PortableTransferCodec::kEnvelopeOverheadBytes, 0);
    EXPECT_FALSE(PortableTransferCodec::Open(envelope, password).has_value());
  }
}

TEST(PortableTransferCodecTest, AcceptsExactPasswordLengthBounds) {
  for (const size_t length : {PortableTransferCodec::kMinimumPasswordBytes,
                              PortableTransferCodec::kMaximumPasswordBytes}) {
    SCOPED_TRACE(length);
    const std::string password(length, 'p');
    auto sealed = PortableTransferCodec::Seal(kDocument, password);
    ASSERT_TRUE(sealed.has_value());
    auto opened = PortableTransferCodec::Open(*sealed, password);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(*opened, kDocument);
  }
}

TEST(PortableTransferCodecTest, PasswordBytesAreNotTrimmedOrNullTerminated) {
  const std::string password(" password\0with spaces ", 22);
  auto sealed = PortableTransferCodec::Seal(kDocument, password);
  ASSERT_TRUE(sealed.has_value());
  auto opened = PortableTransferCodec::Open(*sealed, password);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(*opened, kDocument);
  EXPECT_FALSE(PortableTransferCodec::Open(*sealed, " password").has_value());
  EXPECT_FALSE(
      PortableTransferCodec::Open(*sealed, "password with spaces").has_value());
}

TEST(PortableTransferCodecTest, RejectsOversizedInputsBeforeCryptoWork) {
  const std::string document(PortableTransferCodec::kMaximumPlaintextBytes + 1,
                             'x');
  EXPECT_FALSE(PortableTransferCodec::Seal(document, kPassword).has_value());
  const std::vector<uint8_t> envelope(
      PortableTransferCodec::kMaximumEnvelopeBytes + 1, 0);
  EXPECT_FALSE(PortableTransferCodec::Open(envelope, kPassword).has_value());
}

}  // namespace
}  // namespace noventra
