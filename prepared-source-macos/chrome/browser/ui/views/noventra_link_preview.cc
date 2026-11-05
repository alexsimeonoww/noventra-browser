// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/views/noventra_link_preview.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "build/build_config.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/tab_helpers.h"
#include "chrome/browser/ui/tabs/public/tab_dialog_manager.h"
#include "chrome/browser/ui/tabs/public/tab_features.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/window/frame_view.h"

#if BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/ozone_platform.h"
#endif

namespace {
const char kPreviewContentsKey = 0;
const char kAutomaticPreviewTargetKey = 0;

// The page and its floating action capsule paint their own rounded surfaces.
// A normal dialog frame would add a full-width background and title insets.
class PreviewFrameView : public views::FrameView {
 public:
  gfx::Rect GetBoundsForClientView() const override { return GetLocalBounds(); }
  int NonClientHitTest(const gfx::Point& point) override {
    return GetLocalBounds().Contains(point) ? HTCLIENT : HTNOWHERE;
  }
};

// DialogDelegate's default custom-frame path bypasses WidgetDelegate's frame
// factory. Override it while retaining translucent child-dialog init params.
class PreviewDialogDelegate : public views::DialogDelegate {
 public:
  std::unique_ptr<views::FrameView> CreateFrameView(views::Widget*) override {
    return std::make_unique<PreviewFrameView>();
  }
};

// Only this exact newly created, uncommitted destination may be closed when
// converting a target=_blank anchor navigation into a preview. Any subsequent
// navigation or commit invalidates it, including another navigation to the
// same URL. Ordinary blank tabs and script-created popups are never inferred.
struct AutomaticPreviewTarget : public base::SupportsUserData::Data,
                                public content::WebContentsObserver {
  AutomaticPreviewTarget(content::WebContents* destination,
                         content::WebContents* source_contents,
                         const GURL& target_url,
                         base::TimeTicks input)
      : content::WebContentsObserver(destination),
        source(source_contents->GetWeakPtr()),
        url(target_url),
        input_start(input),
        requires_link_click(destination->HasOpener()) {}

  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if (!navigation->IsInPrimaryMainFrame()) {
      return;
    }
    if (navigation_id == 0 && navigation->GetURL() == url &&
        navigation->NavigationInputStart() == input_start &&
        navigation->IsNavigatingFromInitialEmptyDocument()) {
      navigation_id = navigation->GetNavigationId();
    } else if (navigation->GetNavigationId() != navigation_id) {
      valid = false;
    }
  }
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted()) {
      // A normal committed tab no longer needs preview-only bookkeeping.
      // Removing this user data also detaches this observer; do not access
      // members after the call.
      web_contents()->RemoveUserData(&kAutomaticPreviewTargetKey);
    }
  }
  base::WeakPtr<content::WebContents> source;
  GURL url;
  base::TimeTicks input_start;
  const bool requires_link_click;
  int64_t navigation_id = 0;
  bool valid = true;
};

class AutomaticPreviewNavigationThrottle : public content::NavigationThrottle {
 public:
  AutomaticPreviewNavigationThrottle(
      content::NavigationThrottleRegistry& registry,
      base::RepeatingCallback<bool(const GURL&)> preview)
      : content::NavigationThrottle(registry), preview_(std::move(preview)) {}
  ThrottleCheckResult WillStartRequest() override { return CheckURL(); }
  ThrottleCheckResult WillRedirectRequest() override { return CheckURL(); }
  const char* GetNameForLogging() override {
    return "NoventraPinnedPreviewNavigationThrottle";
  }

 private:
  ThrottleCheckResult CheckURL() {
    return preview_.Run(navigation_handle()->GetURL()) ? CANCEL_AND_IGNORE
                                                       : PROCEED;
  }
  base::RepeatingCallback<bool(const GURL&)> preview_;
};

class PreviewNavigationThrottle : public content::NavigationThrottle {
 public:
  explicit PreviewNavigationThrottle(
      content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}
  ThrottleCheckResult WillStartRequest() override { return CheckURL(); }
  ThrottleCheckResult WillRedirectRequest() override { return CheckURL(); }
  ThrottleCheckResult WillCommitWithoutUrlLoader() override {
    return CheckURL();
  }
  const char* GetNameForLogging() override {
    return "NoventraPreviewNavigationThrottle";
  }

 private:
  ThrottleCheckResult CheckURL() {
    auto* handle = navigation_handle();
    if (!handle->GetWebContents()->GetUserData(&kPreviewContentsKey)) {
      return PROCEED;
    }
    const GURL& url = handle->GetURL();
    if (url.SchemeIsHTTPOrHTTPS() || url.IsAboutBlank() ||
        url.IsAboutSrcdoc()) {
      return PROCEED;
    }
    if (!handle->IsInMainFrame() &&
        (url.SchemeIs("data") || url.SchemeIs("blob"))) {
      return PROCEED;
    }
    return CANCEL_AND_IGNORE;
  }
};
}  // namespace

// TabHelpers already grants PreviewTab access for pages promoted into real
// tabs.
class PreviewTab : public views::View, public content::WebContentsDelegate {
  METADATA_HEADER(PreviewTab, views::View)
 public:
  PreviewTab(content::WebContents* source,
             const GURL& url,
             const content::Referrer& referrer)
      : source_(source->GetWeakPtr()) {
    page_ = AddChildView(std::make_unique<views::View>());
    page_->GetViewAccessibility().SetRole(ax::mojom::Role::kPane);
    page_->GetViewAccessibility().SetName(u"Preview page");
    page_->SetLayoutManager(std::make_unique<views::FillLayout>());
    page_->SetBackground(
        views::CreateRoundedRectBackground(ui::kColorDialogBackground, 12));
    page_->SetBorder(
        views::CreateRoundedRectBorder(1, 12, ui::kColorSeparator));
    page_->SetPaintToLayer();
    page_->layer()->SetFillsBoundsOpaquely(false);
    page_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(12));
    page_->layer()->SetIsFastRoundedCorner(true);
    controls_ = AddChildView(std::make_unique<views::View>());
    controls_->GetViewAccessibility().SetRole(ax::mojom::Role::kToolbar);
    controls_->GetViewAccessibility().SetName(u"Preview controls");
    controls_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(3, 3), 4));
    controls_->SetBackground(
        views::CreateRoundedRectBackground(ui::kColorDialogBackground, 12));
    controls_->SetBorder(
        views::CreateRoundedRectBorder(1, 12, ui::kColorSeparator));
    auto add_icon = [this](views::Button::PressedCallback callback,
                           const gfx::VectorIcon& icon,
                           const std::u16string& tooltip) {
      auto* button =
          controls_->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
              std::move(callback), icon, 18));
      button->SetPreferredSize(gfx::Size(32, 32));
      button->SetTooltipText(tooltip);
      return button;
    };
    back_ = add_icon(base::BindRepeating(&PreviewTab::NavigateHistory,
                                       base::Unretained(this), -1),
                     kBackArrowTouchOldIcon, u"Back in preview");
    forward_ = add_icon(base::BindRepeating(&PreviewTab::NavigateHistory,
                                          base::Unretained(this), 1),
                        kForwardArrowTouchOldIcon, u"Forward in preview");
    back_->SetEnabled(false);
    forward_->SetEnabled(false);
    open_ = add_icon(base::BindRepeating(&PreviewTab::PromoteAndClose,
                                         base::Unretained(this)),
                     kOpenInNewIcon, u"Open in tab without reloading");
    open_->GetViewAccessibility().SetName(u"Open in tab without reloading");
    UpdateIdentity(u"Link preview", base::UTF8ToUTF16(url.spec()));
    split_ = add_icon(
        base::BindRepeating(&PreviewTab::SplitAndClose, base::Unretained(this)),
        kSplitSceneIcon, u"Open in split without reloading");
    add_icon(base::BindRepeating(&PreviewTab::Close, base::Unretained(this)),
             kCloseIcon, u"Close preview (Esc)");
    auto* profile = Profile::FromBrowserContext(source->GetBrowserContext());
    content::WebContents::CreateParams create_params(profile);
    const std::string space = noventra::SpaceId(source);
    if (!space.empty()) create_params.site_instance = noventra::SpaceSiteInstance(profile, url, space);
    contents_ = content::WebContents::Create(create_params);
    contents_->SetUserData(&kPreviewContentsKey,
                           std::make_unique<base::SupportsUserData::Data>());
    TabHelpers::AttachTabHelpers(contents_.get());
    view_ = page_->AddChildView(std::make_unique<views::WebView>(profile));
    view_->SetWebContents(contents_.get());
    contents_->SetDelegate(this);
    contents_->GetController().LoadURL(url, referrer, ui::PAGE_TRANSITION_LINK,
                                       std::string());
    UpdateSplitButton();
  }
  ~PreviewTab() override {
    view_->holder()->SetNativeViewCornerRadii(gfx::RoundedCornersF());
    view_->SetWebContents(nullptr);
    if (contents_) {
      contents_->SetDelegate(nullptr);
    }
  }
  views::View* initial_focus() const { return view_; }
  void Layout(PassKey) override {
    constexpr int kControlsWidth = 40;
    constexpr int kControlsGap = 8;
    const int controls_width = std::min(kControlsWidth, width());
    page_->SetBounds(0, 0, std::max(0, width() - controls_width - kControlsGap),
                     height());
    controls_->SetBounds(
        width() - controls_width, 0, controls_width,
        std::min(height(), controls_->GetPreferredSize().height()));
    page_->DeprecatedLayoutImmediately();
    controls_->DeprecatedLayoutImmediately();
    view_->holder()->SetNativeViewCornerRadii(gfx::RoundedCornersF(11));
  }
  base::WeakPtr<PreviewTab> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  void Close() {
    if (GetWidget()) {
      GetWidget()->Close();
    }
  }
  void NavigationStateChanged(content::WebContents* source,
                              content::InvalidateTypes) override {
    const auto address = base::UTF8ToUTF16(source->GetVisibleURL().spec());
    const auto title = source->GetTitle();
    UpdateIdentity(title.empty() ? u"Link preview" : title, address);
    back_->SetEnabled(source->GetController().CanGoBack());
    forward_->SetEnabled(source->GetController().CanGoForward());
  }
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents*,
      const input::NativeWebKeyboardEvent& event) override {
    if (event.GetType() == blink::WebInputEvent::Type::kRawKeyDown &&
        event.windows_key_code == ui::VKEY_ESCAPE) {
      Close();
      return content::KeyboardEventProcessingResult::HANDLED;
    }
    return content::KeyboardEventProcessingResult::NOT_HANDLED;
  }
  void CloseContents(content::WebContents*) override { Close(); }
  void CanDownload(const GURL&,
                   const std::string&,
                   base::OnceCallback<void(bool)> callback) override {
    // Downloads and other full browser interactions remain available after
    // promotion.
    std::move(callback).Run(false);
  }
  content::WebContents* OpenURLFromTab(
      content::WebContents*,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)> callback) override {
    if (!params.url.SchemeIsHTTPOrHTTPS()) {
      return nullptr;
    }
    content::NavigationController::LoadURLParams load(params);
    contents_->GetController().LoadURLWithParams(load);
    return contents_.get();
  }

  void NavigateHistory(int offset) {
    if (contents_ && contents_->GetController().CanGoToOffset(offset)) {
      contents_->GetController().GoToOffset(offset);
    }
  }
  void PromoteAndClose() {
    // Navigate() can synchronously close this tab-scoped dialog when it
    // activates the destination tab. Never access this view afterwards.
    auto weak = GetWeakPtr();
    const bool promoted = PromoteContents(false);
    if (weak && promoted) {
      weak->Close();
    }
  }
  void SplitAndClose() {
    if (!CanSplit()) {
      UpdateSplitButton();
      return;
    }
    auto weak = GetWeakPtr();
    const bool promoted = PromoteContents(true);
    if (weak && promoted) {
      weak->Close();
    }
  }
  void Promote() { PromoteContents(false); }

 private:
  void UpdateIdentity(const std::u16string& title,
                      const std::u16string& address) {
    // Destination identity stays available in trusted browser UI without a
    // title bar taking space from the page or clipping the address text.
    open_->SetTooltipText(u"Open in tab without reloading\n" + title + u"\n" +
                          address);
    open_->GetViewAccessibility().SetName(u"Open in tab without reloading");
    open_->GetViewAccessibility().SetDescription(title + u"\n" + address);
  }
  bool CanSplit() const {
    if (!source_ || !contents_) {
      return false;
    }
    auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
        source_.get());
    if (!browser) {
      return false;
    }
    auto* model = browser->GetTabStripModel();
    const int index = model->GetIndexOfWebContents(source_.get());
    if (!model->ContainsIndex(index)) {
      return false;
    }
    auto* tab = model->GetTabAtIndex(index);
    return !tab->IsSplit() ||
           model->GetSplitData(tab->GetSplit().value())->ListTabs().size() < 4;
  }
  void UpdateSplitButton() {
    const bool can_split = CanSplit();
    split_->SetEnabled(can_split);
    split_->SetTooltipText(can_split ? u"Open in split without reloading"
                                     : u"A split can contain up to four tabs");
  }
  bool PromoteContents(bool split) {
    if (!source_ || !contents_) {
      return false;
    }
    auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
        source_.get());
    if (!browser) {
      return false;
    }
    auto* model = browser->GetTabStripModel();
    const int source_index = model->GetIndexOfWebContents(source_.get());
    if (!model->ContainsIndex(source_index) || (split && !CanSplit())) {
      return false;
    }
    // Insertion and split activation can synchronously destroy this dialog.
    // Retain only weak contents identities and local values across Navigate().
    auto source = source_;
    auto preview = contents_->GetWeakPtr();
    const bool pinned = model->IsTabPinned(source_index);
    view_->holder()->SetNativeViewCornerRadii(gfx::RoundedCornersF());
    view_->SetWebContents(nullptr);
    contents_->SetDelegate(nullptr);
    contents_->RemoveUserData(&kPreviewContentsKey);
    NavigateParams params(browser, std::move(contents_));
    params.source_contents = source.get();
    params.disposition = split ? WindowOpenDisposition::NEW_BACKGROUND_TAB
                               : WindowOpenDisposition::NEW_FOREGROUND_TAB;
    if (split) {
      params.tabstrip_add_types = pinned ? AddTabTypes::ADD_PINNED : 0;
    }
    Navigate(&params);
    if (!split || !preview) {
      return true;
    }
    auto* destination =
        GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
            preview.get());
    if (!destination) {
      return false;
    }
    model = destination->GetTabStripModel();
    const int preview_index = model->GetIndexOfWebContents(preview.get());
    const int target_index = source ? model->GetIndexOfWebContents(source.get())
                                    : TabStripModel::kNoTab;
    if (model->ContainsIndex(preview_index) &&
        (!model->ContainsIndex(target_index) ||
         !model->JoinNoventraSplit(preview_index, target_index))) {
      // A concurrent model change must not discard an already promoted page.
      model->ActivateTabAt(model->GetIndexOfWebContents(preview.get()));
    }
    return true;
  }

  base::WeakPtr<content::WebContents> source_;
  std::unique_ptr<content::WebContents> contents_;
  raw_ptr<views::WebView> view_ = nullptr;
  raw_ptr<views::View> page_ = nullptr;
  raw_ptr<views::View> controls_ = nullptr;
  raw_ptr<views::ImageButton> open_ = nullptr;
  raw_ptr<views::ImageButton> back_ = nullptr;
  raw_ptr<views::ImageButton> forward_ = nullptr;
  raw_ptr<views::ImageButton> split_ = nullptr;
  base::WeakPtrFactory<PreviewTab> weak_factory_{this};
};
BEGIN_METADATA(PreviewTab)
END_METADATA

WEB_CONTENTS_USER_DATA_KEY_IMPL(NoventraLinkPreview);
NoventraLinkPreview::NoventraLinkPreview(content::WebContents* source)
    : content::WebContentsObserver(source),
      content::WebContentsUserData<NoventraLinkPreview>(*source) {
  source->ForEachRenderFrameHost(
      [this](content::RenderFrameHost* frame) { Attach(frame); });
}
NoventraLinkPreview::~NoventraLinkPreview() {
  ClosePreview();
  for (auto& [host, callback] : hosts_) {
    host->RemoveMouseEventCallback(callback);
    host->RemoveObserver(this);
  }
}
void NoventraLinkPreview::Attach(content::RenderFrameHost* frame) {
  auto* host = frame->GetRenderWidgetHost();
  if (hosts_.contains(host)) {
    return;
  }
  auto callback = base::BindRepeating(&NoventraLinkPreview::OnMouse,
                                      base::Unretained(this), host);
  hosts_.emplace(host, callback);
  host->AddObserver(this);
  host->AddMouseEventCallback(callback);
}
void NoventraLinkPreview::RenderFrameCreated(content::RenderFrameHost* frame) {
  Attach(frame);
}
void NoventraLinkPreview::RenderWidgetHostDestroyed(
    content::RenderWidgetHost* host) {
  auto it = hosts_.find(host);
  if (it != hosts_.end()) {
    host->RemoveMouseEventCallback(it->second);
    host->RemoveObserver(this);
    hosts_.erase(it);
  }
}
bool NoventraLinkPreview::OnMouse(content::RenderWidgetHost* host,
                                  const blink::WebMouseEvent& event) {
  if (event.button != blink::WebMouseEvent::Button::kLeft) {
    return false;
  }
  constexpr int kKeys =
      blink::WebInputEvent::kAltKey | blink::WebInputEvent::kControlKey |
      blink::WebInputEvent::kShiftKey | blink::WebInputEvent::kMetaKey;
  const int modifiers = event.GetModifiers() & kKeys;
  if (event.GetType() == blink::WebInputEvent::Type::kMouseUp) {
    if (swallowing_click_) {
      swallowing_click_ = false;
      return true;
    }
    // Observe the real anchor click, allowing page handlers and ordinary
    // controls to run. The navigation must later carry this exact timestamp.
    plain_click_input_start_ = modifiers == 0 && CanAutomaticallyPreview()
                                   ? event.TimeStamp()
                                   : base::TimeTicks();
    return false;
  }
  if (event.GetType() != blink::WebInputEvent::Type::kMouseDown) {
    return false;
  }
  pending_hit_test_ = base::TimeTicks();
  plain_click_input_start_ = base::TimeTicks();
  const auto* prefs =
      Profile::FromBrowserContext(web_contents()->GetBrowserContext())
          ->GetPrefs();
  const int choice = prefs->GetInteger(kNoventraLinkPreviewTrigger);
  const int trigger = choice == 1   ? blink::WebInputEvent::kControlKey
                      : choice == 2 ? blink::WebInputEvent::kShiftKey
                                    : blink::WebInputEvent::kAltKey;
  if (modifiers != trigger || !prefs->GetBoolean(kNoventraLinkPreview) ||
      !GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
          web_contents())) {
    return false;
  }
  swallowing_click_ = true;
  pending_hit_test_ = base::TimeTicks::Now();
  // kNone selects Noventra's located hit-test path in WebFrameWidgetImpl.
  // It bypasses page contextmenu handlers and preserves ordinary clicks.
  host->ShowContextMenuAtPoint(gfx::ToRoundedPoint(event.PositionInWidget()),
                               ui::mojom::MenuSourceType::kNone);
  return true;
}
bool NoventraLinkPreview::CanAutomaticallyPreview() const {
  if (!web_contents() ||
      !web_contents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  auto* tab = tabs::TabInterface::MaybeGetFromContents(web_contents());
  if (!tab || !tab->CanShowModalUI()) {
    return false;
  }
  auto* prefs = Profile::FromBrowserContext(web_contents()->GetBrowserContext())
                    ->GetPrefs();
  if (!prefs->GetBoolean(kNoventraLinkPreview) ||
      !prefs->GetBoolean(kNoventraLinkPreviewPinned)) {
    return false;
  }
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
      web_contents());
  if (!browser) {
    return false;
  }
  auto* model = browser->GetTabStripModel();
  const int index = model->GetIndexOfWebContents(web_contents());
  return model->ContainsIndex(index) && model->IsTabPinned(index);
}

void NoventraLinkPreview::DidOpenRequestedURL(
    content::WebContents* new_contents,
    content::RenderFrameHost* source_frame,
    const GURL& url,
    const content::Referrer& referrer,
    WindowOpenDisposition disposition,
    ui::PageTransition transition,
    bool started_from_context_menu,
    bool renderer_initiated) {
  if (!new_contents || new_contents == web_contents() || !renderer_initiated ||
      started_from_context_menu ||
      !ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_LINK) ||
      !url.SchemeIsHTTPOrHTTPS() || !CanAutomaticallyPreview() ||
      plain_click_input_start_.is_null() ||
      base::TimeTicks::Now() - plain_click_input_start_ > base::Seconds(2) ||
      new_contents->GetBrowserContext() !=
          web_contents()->GetBrowserContext() ||
      (disposition != WindowOpenDisposition::NEW_FOREGROUND_TAB &&
       disposition != WindowOpenDisposition::NEW_BACKGROUND_TAB &&
       disposition != WindowOpenDisposition::NEW_WINDOW &&
       disposition != WindowOpenDisposition::NEW_POPUP)) {
    return;
  }
  new_contents->SetUserData(
      &kAutomaticPreviewTargetKey,
      std::make_unique<AutomaticPreviewTarget>(new_contents, web_contents(),
                                               url, plain_click_input_start_));
}

bool NoventraLinkPreview::PreviewPinnedUrl(
    base::WeakPtr<content::WebContents> destination,
    base::TimeTicks input_start,
    const GURL& source_url,
    const GURL& url) {
  if (!destination || !CanAutomaticallyPreview() ||
      plain_click_input_start_ != input_start ||
      web_contents()->GetLastCommittedURL() != source_url ||
      !url.SchemeIsHTTPOrHTTPS() ||
      net::registry_controlled_domains::SameDomainOrHost(
          source_url, url,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return false;
  }
  plain_click_input_start_ = base::TimeTicks();
  auto source = web_contents()->GetWeakPtr();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NoventraLinkPreview> helper,
             base::WeakPtr<content::WebContents> source,
             base::WeakPtr<content::WebContents> destination,
             base::TimeTicks input_start, GURL url) {
            bool activate_source = false;
            if (destination && destination.get() != source.get()) {
              auto* tag = static_cast<AutomaticPreviewTarget*>(
                  destination->GetUserData(&kAutomaticPreviewTargetKey));
              if (!tag || !tag->valid || tag->source.get() != source.get() ||
                  tag->input_start != input_start) {
                return;
              }
              auto* browser =
                  GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
                      destination.get());
              activate_source =
                  browser &&
                  browser->GetTabStripModel()->GetActiveWebContents() ==
                      destination.get();
              destination->Close();
            }
            if (!helper || !source || !helper->CanAutomaticallyPreview()) {
              return;
            }
            auto* browser =
                GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
                    source.get());
            if (!browser) {
              return;
            }
            auto* model = browser->GetTabStripModel();
            if (activate_source) {
              const int index = model->GetIndexOfWebContents(source.get());
              if (!model->ContainsIndex(index)) {
                return;
              }
              model->ActivateTabAt(index);
            }
            if (helper && model->GetActiveWebContents() == source.get()) {
              helper->ShowPreview(url, content::Referrer());
            }
          },
          weak_factory_.GetWeakPtr(), source, destination, input_start, url));
  return true;
}
bool NoventraLinkPreview::HandleContextMenu(
    content::RenderFrameHost& frame,
    const content::ContextMenuParams& params) {
  auto* source = content::WebContents::FromRenderFrameHost(&frame);
  auto* helper = FromWebContents(source);
  if (!helper || params.source_type != ui::mojom::MenuSourceType::kNone) {
    return false;
  }
  const bool recent =
      !helper->pending_hit_test_.is_null() &&
      base::TimeTicks::Now() - helper->pending_hit_test_ < base::Seconds(2);
  helper->pending_hit_test_ = base::TimeTicks();
  if (!recent) {
    return true;
  }
  if (params.link_url.SchemeIsHTTPOrHTTPS()) {
    // No referrer is safer than reconstructing a link-specific policy
    // incorrectly.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&NoventraLinkPreview::ShowPreview,
                                  helper->weak_factory_.GetWeakPtr(),
                                  params.link_url, content::Referrer()));
  }
  return true;
}
void NoventraLinkPreview::Show(content::WebContents* source,
                               const GURL& url,
                               const content::Referrer& referrer) {
  if (auto* helper = FromWebContents(source)) {
    helper->ShowPreview(url, referrer);
  }
}
void NoventraLinkPreview::ShowPreview(const GURL& url,
                                      const content::Referrer& referrer) {
  if (!url.SchemeIsHTTPOrHTTPS() || !web_contents() ||
      !GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
          web_contents())) {
    return;
  }
  if (preview_) {
    preview_->Close();
    return;
  }
  auto* tab = tabs::TabInterface::MaybeGetFromContents(web_contents());
  if (!tab || !tab->CanShowModalUI()) {
    return;
  }
  ClosePreview();
  auto delegate = std::make_unique<PreviewDialogDelegate>();
  delegate->SetModalType(ui::mojom::ModalType::kChild);
  delegate->SetOwnershipOfNewWidget(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  delegate->SetTitle(u"Link preview");
  delegate->SetShowTitle(false);
  delegate->SetShowCloseButton(false);
  delegate->SetButtons(0);
  delegate->set_margins(gfx::Insets());
  auto preview = std::make_unique<PreviewTab>(web_contents(), url, referrer);
  preview_ = preview->GetWeakPtr();
  delegate->SetInitiallyFocusedView(preview->initial_focus());
  delegate->SetAcceptCallback(base::BindOnce(&PreviewTab::Promote, preview_));
  delegate->SetContentsView(std::move(preview));
  delegate->RegisterWindowClosingCallback(base::BindOnce(
      [](base::WeakPtr<NoventraLinkPreview> helper) {
        if (helper) {
          base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE,
              base::BindOnce(&NoventraLinkPreview::OnPreviewClosed, helper));
        }
      },
      weak_factory_.GetWeakPtr()));
  preview_delegate_ = std::move(delegate);
  auto* manager = tab->GetTabFeatures()->tab_dialog_manager();
  preview_widget_ = manager->CreateTabScopedDialog(preview_delegate_.get());
  auto params = std::make_unique<tabs::TabDialogManager::Params>();
  params->get_dialog_bounds = base::BindRepeating(
      &NoventraLinkPreview::GetPreviewBounds, base::Unretained(this));
  manager->ShowDialog(preview_widget_.get(), std::move(params));
}
gfx::Rect NoventraLinkPreview::GetPreviewBounds() const {
  CHECK(web_contents());
  CHECK(preview_widget_);
  auto* tab = tabs::TabInterface::MaybeGetFromContents(web_contents());
  auto* host = tab->GetTabFeatures()->tab_dialog_manager()->GetHostWidget();
  gfx::Rect bounds = web_contents()->GetViewBounds();
  // Zen's Glance uses 80% of the page width and its full available height.
  // Keep a small outside-click gutter; on narrow windows use the available
  // width rather than squeezing the page and its controls into an 80% column.
  const int margin =
      std::min(12, std::min(bounds.width(), bounds.height()) / 20);
  bounds.Inset(margin);
  const int width =
      std::min(bounds.width(), std::max(640, bounds.width() * 4 / 5));
  bounds.set_x(bounds.x() + (bounds.width() - width) / 2);
  bounds.set_width(width);
  // Child widgets use parent-client coordinates. Desktop modal widgets use
  // global coordinates only on platforms that support them, matching
  // TabDialogManager's default positioning path.
  bool screen_coordinates = preview_widget_->is_top_level();
#if BUILDFLAG(IS_OZONE)
  screen_coordinates &= ui::OzonePlatform::GetInstance()
                            ->GetPlatformProperties()
                            .supports_global_screen_coordinates;
#endif
  if (host && !screen_coordinates) {
    bounds.Offset(-host->GetClientAreaBoundsInScreen().OffsetFromOrigin());
  }
  return bounds;
}
void NoventraLinkPreview::ClosePreview() {
  preview_.reset();
  preview_widget_.reset();
  preview_delegate_.reset();
}
void NoventraLinkPreview::OnPreviewClosed() {
  if (preview_widget_ && preview_widget_->IsClosed()) {
    ClosePreview();
  }
}
void NoventraLinkPreview::MaybeCreateThrottle(
    content::NavigationThrottleRegistry& registry) {
  auto& navigation = registry.GetNavigationHandle();
  if (navigation.GetWebContents()->GetUserData(&kPreviewContentsKey)) {
    registry.AddThrottle(std::make_unique<PreviewNavigationThrottle>(registry));
    return;
  }
  if (!navigation.IsInPrimaryMainFrame() || !navigation.IsRendererInitiated() ||
      navigation.IsSameDocument() || navigation.IsFormSubmission() ||
      navigation.IsPost() || navigation.WasStartedFromContextMenu() ||
      !ui::PageTransitionCoreTypeIs(navigation.GetPageTransition(),
                                    ui::PAGE_TRANSITION_LINK) ||
      navigation.NavigationInputStart().is_null() ||
      !navigation.GetInitiatorFrameToken()) {
    return;
  }
  auto* frame = content::RenderFrameHost::FromFrameToken(
      {navigation.GetInitiatorProcessId(),
       *navigation.GetInitiatorFrameToken()});
  auto* source =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  auto* helper = source ? FromWebContents(source) : nullptr;
  if (!helper || !helper->CanAutomaticallyPreview() ||
      helper->plain_click_input_start_ != navigation.NavigationInputStart() ||
      base::TimeTicks::Now() - helper->plain_click_input_start_ >
          base::Seconds(2)) {
    return;
  }
  auto* destination = navigation.GetWebContents();
  if (destination == source) {
    // Cross-process frame-proxy navigations deliberately filter HasUserGesture
    // and drop the link flag upstream. Their input time is forwarded only for
    // actual anchor gestures; it must still match our observed native click.
    // Require the initiator to belong to this primary frame tree and a distinct
    // process, so ordinary iframe script navigation cannot use this exception.
    auto* primary = source->GetPrimaryMainFrame();
    const bool remote_anchor = frame->GetParent() &&
                               frame->GetMainFrame() == primary &&
                               frame->GetProcess() != primary->GetProcess() &&
                               navigation.StartedWithTransientActivation();
    if (!(navigation.HasUserGesture() &&
          navigation.WasInitiatedByLinkClick()) &&
        !remote_anchor) {
      return;
    }
  } else {
    if (!navigation.HasUserGesture()) {
      return;
    }
    auto* tag = static_cast<AutomaticPreviewTarget*>(
        destination->GetUserData(&kAutomaticPreviewTargetKey));
    if (!tag || !tag->valid || tag->source.get() != source ||
        tag->navigation_id != navigation.GetNavigationId() ||
        tag->input_start != navigation.NavigationInputStart() ||
        // Opener windows navigate in the renderer and retain the anchor flag.
        // Noopener loads use the browser's anchor-only forwarded input time.
        (tag->requires_link_click && !navigation.WasInitiatedByLinkClick()) ||
        !navigation.IsNavigatingFromInitialEmptyDocument()) {
      return;
    }
  }
  registry.AddThrottle(std::make_unique<AutomaticPreviewNavigationThrottle>(
      registry,
      base::BindRepeating(
          [](base::WeakPtr<NoventraLinkPreview> helper,
             base::WeakPtr<content::WebContents> destination,
             base::TimeTicks input_start, const GURL& source_url,
             const GURL& url) {
            return helper && helper->PreviewPinnedUrl(destination, input_start,
                                                      source_url, url);
          },
          helper->weak_factory_.GetWeakPtr(), destination->GetWeakPtr(),
          navigation.NavigationInputStart(), source->GetLastCommittedURL())));
}
void NoventraLinkPreview::DidStartNavigation(
    content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && !navigation->IsSameDocument()) {
    pending_hit_test_ = base::TimeTicks();
    weak_factory_.InvalidateWeakPtrs();
    ClosePreview();
  }
}
void NoventraLinkPreview::WebContentsDestroyed() {
  weak_factory_.InvalidateWeakPtrs();
  ClosePreview();
}
