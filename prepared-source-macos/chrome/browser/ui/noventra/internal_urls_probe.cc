// Copyright 2026 The Noventra Authors. All rights reserved.

#include <cstdio>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/strings/utf_offset_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "components/url_formatter/url_fixer.h"
#include "components/url_formatter/url_formatter.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    ++failures;
  }
}
}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::FeatureList::InitInstance("", "");
  if (!base::i18n::InitializeICU()) {
    return 2;
  }
  // Match existing content registration, without registering or granting any
  // network privileges to the new display/input alias.
  url::AddStandardScheme("chrome", url::SCHEME_WITH_HOST);
  url::AddStandardScheme("helium", url::SCHEME_WITH_HOST);
  struct FixupCase {
    const char* input;
    const char* expected;
  };
  constexpr FixupCase kCases[] = {
      {"noventra://settings", "chrome://settings/"},
      {"NOVENTRA://settings/system/shortcuts?search=keys#edit",
       "chrome://settings/system/shortcuts?search=keys#edit"},
      {"noventra:settings/privacy", "chrome://settings/privacy"},
      {"noventra://downloads", "chrome://downloads/"},
      {"chrome://settings/system/shortcuts",
       "chrome://settings/system/shortcuts"},
      {"helium://settings/privacy", "helium://settings/privacy"},
      {"https://example.test/noventra://settings",
       "https://example.test/noventra://settings"},
  };
  for (const auto& test : kCases) {
    Check(url_formatter::FixupURL(test.input, "") == GURL(test.expected),
          test.input);
  }
  for (const auto* scheme : {"chrome", "helium", "about"}) {
    Check(url_formatter::IsEquivalentScheme("noventra", scheme),
          "alias equivalence");
    Check(url_formatter::IsEquivalentScheme(scheme, "noventra"),
          "reverse alias equivalence");
  }

  const GURL internal("chrome://settings/system/shortcuts?search=keys#edit");
  url::Parsed parsed;
  size_t prefix_end = 0;
  base::OffsetAdjuster::Adjustments adjustments;
  const auto formatted = url_formatter::FormatUrlWithAdjustments(
      internal, url_formatter::kFormatReplaceChromeProtocol,
      base::UnescapeRule::NONE, &parsed, &prefix_end, &adjustments);
  Check(formatted == u"noventra://settings/system/shortcuts?search=keys#edit",
        "displayed branded URL");
  Check(parsed.scheme.AsViewOn(formatted) == u"noventra",
        "display scheme component");
  Check(parsed.host.AsViewOn(formatted) == u"settings",
        "display host component");
  Check(parsed.path.AsViewOn(formatted) == u"/system/shortcuts",
        "display path component");
  Check(parsed.query.AsViewOn(formatted) == u"search=keys",
        "display query component");
  Check(parsed.ref.AsViewOn(formatted) == u"edit",
        "display fragment component");
  Check(prefix_end == 11, "display host begins after noventra://");
  const auto& original = internal.parsed_for_possibly_invalid_spec();
  std::vector<size_t> offsets = {static_cast<size_t>(original.host.begin),
                                 static_cast<size_t>(original.path.begin),
                                 static_cast<size_t>(original.query.begin),
                                 static_cast<size_t>(original.ref.begin),
                                 internal.spec().size()};
  const auto before = offsets;
  base::OffsetAdjuster::AdjustOffsets(adjustments, &offsets);
  for (size_t i = 0; i < offsets.size(); ++i) {
    Check(offsets[i] == before[i] + 2, "scheme-length selection offset");
  }
  base::OffsetAdjuster::UnadjustOffsets(adjustments, &offsets);
  Check(offsets == before, "selection offsets round-trip");
  Check(url_formatter::FormatUrl(internal, 0, base::UnescapeRule::NONE, nullptr,
                                 nullptr,
                                 nullptr) == base::UTF8ToUTF16(internal.spec()),
        "canonical formatting remains opt-in");
  const GURL web("https://example.test/a?q=x#ref");
  Check(
      url_formatter::FormatUrl(web, url_formatter::kFormatReplaceChromeProtocol,
                               base::UnescapeRule::NONE, nullptr, nullptr,
                               nullptr) == base::UTF8ToUTF16(web.spec()),
      "web URLs stay unchanged");
  Check(
      url_formatter::FormatUrl(GURL("view-source:chrome://settings/"),
                               url_formatter::kFormatReplaceChromeProtocol,
                               base::UnescapeRule::NONE, nullptr, nullptr,
                               nullptr) == u"view-source:noventra://settings/",
      "view-source branded formatting");
  std::printf("Internal URL policy: %s (%d failures)\n",
              failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
