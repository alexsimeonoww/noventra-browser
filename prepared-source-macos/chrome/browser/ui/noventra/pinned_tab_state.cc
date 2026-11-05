// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/noventra/pinned_tab_state.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/url_constants.h"
#include "ui/base/page_transition_types.h"

namespace noventra {
namespace {

constexpr size_t kMaxPinnedMetadataBytes = 40 * 1024;
constexpr size_t kPinnedNameAndStructureBudget = 2 * 1024;

bool IsPinned(const content::WebContents* contents) {
  // Restore/clone/discard callbacks can precede insertion into a TabModel.
  const auto* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  return tab && tab->IsPinned();
}

std::optional<std::u16string> NormalizeName(std::u16string_view name) {
  std::u16string trimmed(base::TrimWhitespace(name, base::TRIM_ALL));
  if (trimmed.size() > 256) {
    return std::nullopt;
  }
  for (char16_t character : trimmed) {
    if (character < 0x20 || (character >= 0x7f && character <= 0x9f) ||
        character == 0x2028 || character == 0x2029 ||
        (character >= 0x202a && character <= 0x202e) ||
        (character >= 0x2066 && character <= 0x2069)) {
      return std::nullopt;
    }
  }
  std::string utf8;
  if (!base::UTF16ToUTF8(trimmed.data(), trimmed.size(), &utf8)) {
    return std::nullopt;
  }
  return trimmed;
}

std::string Serialize(const PinnedTabData* data) {
  if (!data || !IsValidPinnedHomeUrl(data->home_url)) {
    return {};
  }
  base::DictValue value;
  value.Set("version", 1);
  value.Set("home_url", data->home_url.spec());
  value.Set("custom_name", base::UTF16ToUTF8(data->custom_name));
  auto encoded = base::WriteJson(value);
  return encoded && encoded->size() <= kMaxPinnedMetadataBytes
             ? std::move(*encoded)
             : std::string();
}

std::optional<PinnedTabData> Deserialize(std::string_view serialized) {
  if (serialized.size() > kMaxPinnedMetadataBytes) {
    return std::nullopt;
  }
  auto value = base::JSONReader::ReadDict(serialized, base::JSON_PARSE_RFC, 4);
  if (!value || value->FindInt("version") != 1) {
    return std::nullopt;
  }
  const std::string* home = value->FindString("home_url");
  const std::string* name = value->FindString("custom_name");
  if (!home || !name) {
    return std::nullopt;
  }
  GURL url(*home);
  std::u16string decoded;
  if (!IsValidPinnedHomeUrl(url) ||
      !base::UTF8ToUTF16(name->data(), name->size(), &decoded)) {
    return std::nullopt;
  }
  auto normalized = NormalizeName(decoded);
  if (!normalized) {
    return std::nullopt;
  }
  return PinnedTabData{std::move(url), std::move(*normalized)};
}

class PinnedTabState : public content::WebContentsUserData<PinnedTabState>,
                       public content::WebContentsObserver {
 public:
  const PinnedTabData* data() const { return data_ ? &*data_ : nullptr; }

  void Ensure() {
    if (data_ && IsValidPinnedHomeUrl(data_->home_url)) {
      return;
    }
    if (!data_) {
      data_.emplace();
    }
    const GURL url = web_contents()->GetVisibleURL();
    awaiting_home_ = !IsValidPinnedHomeUrl(url);
    if (!awaiting_home_) {
      data_->home_url = url;
      Persist();
    }
  }

  void Set(PinnedTabData data) {
    data_ = std::move(data);
    awaiting_home_ = false;
    Persist();
    web_contents()->NotifyNavigationStateChanged(
        content::INVALIDATE_TYPE_TITLE);
  }

  void Clear() {
    if (!data_ && !awaiting_home_) {
      return;
    }
    data_.reset();
    awaiting_home_ = false;
    Persist();
  }

  void Restore(PinnedTabData data) {
    data_ = std::move(data);
    awaiting_home_ = false;
  }

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (awaiting_home_ && navigation->HasCommitted() &&
        navigation->IsInPrimaryMainFrame() && IsPinned(web_contents())) {
      Ensure();
    }
  }

  void DidCloneToNewWebContents(content::WebContents* old_contents,
                                content::WebContents* new_contents) override {
    CopyTo(new_contents);
  }

  void AboutToBeDiscarded(content::WebContents* new_contents) override {
    CopyTo(new_contents);
  }

 private:
  friend class content::WebContentsUserData<PinnedTabState>;
  explicit PinnedTabState(content::WebContents* contents)
      : content::WebContentsUserData<PinnedTabState>(*contents),
        content::WebContentsObserver(contents) {}

  void CopyTo(content::WebContents* contents) const {
    if (!data_ && !awaiting_home_) {
      return;
    }
    auto* copy = GetOrCreateForWebContents(contents);
    copy->data_ = data_;
    copy->awaiting_home_ = awaiting_home_;
  }

  void Persist() const {
    auto* helper = sessions::SessionTabHelper::FromWebContents(web_contents());
    if (!helper || !helper->window_id().is_valid()) {
      return;
    }
    auto* profile =
        Profile::FromBrowserContext(web_contents()->GetBrowserContext());
    if (auto* service =
            SessionServiceFactory::GetForProfileIfExisting(profile)) {
      service->AddTabExtraData(helper->window_id(), helper->session_id(),
                               kPinnedTabExtraData, Serialize(data()));
    }
  }

  std::optional<PinnedTabData> data_;
  bool awaiting_home_ = false;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PinnedTabState);

}  // namespace

bool IsValidPinnedHomeUrl(const GURL& url) {
  if (!url.is_valid() || url.has_username() || url.has_password() ||
      url.spec().size() > kMaxPinnedHomeUrlBytes) {
    return false;
  }
  if (!(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
        url.SchemeIs(content::kChromeUIScheme) || url.IsAboutBlank() ||
        url.SchemeIs("chrome-extension"))) {
    return false;
  }
  // Account for actual JSON escaping, not just URL length. The remaining
  // budget covers the bounded name and field names even in escaped form.
  auto encoded = base::WriteJson(base::Value(url.spec()));
  return encoded && encoded->size() <=
                        kMaxPinnedMetadataBytes - kPinnedNameAndStructureBudget;
}

const PinnedTabData* GetPinnedTabData(const content::WebContents* contents) {
  if (!IsPinned(contents)) {
    return nullptr;
  }
  const auto* state = PinnedTabState::FromWebContents(contents);
  return state ? state->data() : nullptr;
}

void EnsurePinnedTabData(content::WebContents* contents) {
  if (IsPinned(contents)) {
    PinnedTabState::GetOrCreateForWebContents(contents)->Ensure();
  }
}

bool SetPinnedTabData(content::WebContents* contents,
                      const GURL& home_url,
                      std::u16string_view custom_name) {
  auto normalized = NormalizeName(custom_name);
  if (!IsPinned(contents) || !IsValidPinnedHomeUrl(home_url) || !normalized) {
    return false;
  }
  PinnedTabState::GetOrCreateForWebContents(contents)->Set(
      PinnedTabData{home_url, std::move(*normalized)});
  return true;
}

void ClearPinnedTabData(content::WebContents* contents) {
  if (contents) {
    if (auto* state = PinnedTabState::FromWebContents(contents)) {
      state->Clear();
    }
  }
}

bool ResetPinnedTabToHome(content::WebContents* contents) {
  EnsurePinnedTabData(contents);
  const auto* data = GetPinnedTabData(contents);
  if (!data || !IsValidPinnedHomeUrl(data->home_url)) {
    return false;
  }
  // Navigating the existing controller preserves the fixed Space partition,
  // back/forward history and normal beforeunload behavior.
  content::NavigationController::LoadURLParams params(data->home_url);
  params.transition_type = ui::PAGE_TRANSITION_AUTO_BOOKMARK;
  contents->GetController().LoadURLWithParams(params);
  return true;
}

std::u16string PinnedTabDisplayTitle(const content::WebContents* contents,
                                     std::u16string_view fallback) {
  const auto* data = GetPinnedTabData(contents);
  return data && !data->custom_name.empty() ? data->custom_name
                                            : std::u16string(fallback);
}

void OnPinnedStateChanged(content::WebContents* contents, bool pinned) {
  if (pinned) {
    EnsurePinnedTabData(contents);
  } else {
    ClearPinnedTabData(contents);
  }
}

void PopulatePinnedTabExtraData(
    const content::WebContents* contents,
    std::map<std::string, std::string>* extra_data) {
  // Retain state while a pin is temporarily detached or being reconstructed.
  // An explicit unpin clears this stored state in OnPinnedStateChanged().
  const auto* state =
      contents ? PinnedTabState::FromWebContents(contents) : nullptr;
  (*extra_data)[kPinnedTabExtraData] =
      Serialize(state ? state->data() : nullptr);
}

void RestorePinnedTabExtraData(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  const auto found = extra_data.find(kPinnedTabExtraData);
  if (found == extra_data.end()) {
    return;
  }
  if (auto data = Deserialize(found->second)) {
    PinnedTabState::GetOrCreateForWebContents(contents)->Restore(
        std::move(*data));
  }
}

}  // namespace noventra
