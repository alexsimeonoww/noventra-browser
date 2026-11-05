// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/frame/contents_web_view.h"

#include "base/debug/dump_without_crashing.h"
#include "base/logging.h"
#include "build/build_config.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/views/frame/safe_invoke/safe_invoke.h"
#include "chrome/browser/ui/views/frame/web_contents_close_handler.h"
#include "chrome/browser/ui/views/helium/native_frame_materials.h"
#include "chrome/browser/ui/views/status_bubble_views.h"
#include "components/tabs/public/tab_interface.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer_solid_color.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "ui/webui/buildflags.h"

#if BUILDFLAG(ENABLE_WEBUI_NTP)
#include "chrome/browser/ui/webui/new_tab_page/new_tab_page_ui.h"
#endif

#if defined(USE_AURA)
#include "ui/aura/window.h"
#include "ui/wm/core/window_util.h"
#endif

namespace {

#if BUILDFLAG(ENABLE_WEBUI_NTP)
NewTabPageUI* GetCommittedLocalNewTabPage(content::WebContents* contents) {
  auto* frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!frame || contents->IsCrashed() || !frame->IsRenderFrameLive() ||
      frame->IsErrorDocument() ||
      !NewTabPageUI::IsNewTabPageOrigin(frame->GetLastCommittedURL()) ||
      frame->GetLastCommittedURL().path() != "/") {
    return nullptr;
  }
  auto* web_ui = frame->GetWebUI();
  auto* controller = web_ui ? web_ui->GetController() : nullptr;
  return controller && controller->GetType() ? controller->GetAs<NewTabPageUI>()
                                             : nullptr;
}
#endif

}  // namespace

DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(ContentsWebView,
                                      kContentsWebViewElementId);

ContentsWebView::ContentsWebView(content::BrowserContext* browser_context)
    : views::WebView(browser_context) {
  // Draws the ContentsWebView background.
  SetPaintToLayer(ui::LAYER_SOLID_COLOR);
  SetProperty(views::kElementIdentifierKey, kContentsWebViewElementId);
  status_bubble_ = std::make_unique<StatusBubbleViews>(this);
  status_bubble_->Reposition();
  web_contents_close_handler_ = std::make_unique<WebContentsCloseHandler>(this);
}

ContentsWebView::~ContentsWebView() = default;

StatusBubbleViews* ContentsWebView::GetStatusBubble() const {
  if (status_bubble_) {
    return status_bubble_.get();
  }
  return nullptr;
}

WebContentsCloseHandler* ContentsWebView::GetWebContentsCloseHandler() const {
  return web_contents_close_handler_.get();
}

void ContentsWebView::SetBackgroundVisible(bool background_visible) {
  background_visible_ = background_visible;
  if (GetWidget()) {
    UpdateBackgroundColor();
  }
}

bool ContentsWebView::HasNoventraGlassBackground() const {
#if BUILDFLAG(ENABLE_WEBUI_NTP)
  auto* ntp = GetCommittedLocalNewTabPage(web_contents());
  const bool result = background_visible_ && !is_letterboxing() &&
                      !noventra_committing_navigation_id_.has_value() && ntp &&
                      helium::ShouldUseNativeFrameMaterials(this) &&
                      ntp->HasNoventraGlassBackground();
  if (ntp) {
    VLOG(1) << "NoventraGlass contents.eligible view=" << this << " ntp=" << ntp
            << " result=" << result << " size=" << size().ToString()
            << " background_visible=" << background_visible_
            << " letterboxing=" << is_letterboxing()
            << " committing=" << noventra_committing_navigation_id_.has_value()
            << " native_material="
            << helium::ShouldUseNativeFrameMaterials(this);
  } else {
    VLOG(1) << "NoventraGlass contents.eligible view=" << this << " ntp=0";
  }
  return result;
#else
  return false;
#endif
}

bool ContentsWebView::CanShareNoventraGlassBackground(
    const gfx::Rect& local_bounds) const {
#if BUILDFLAG(ENABLE_WEBUI_NTP)
  if (HasNoventraGlassBackground() && !GetLocalBounds().IsEmpty()) {
    gfx::RectF normalized(local_bounds);
    normalized.Scale(1.f / width(), 1.f / height());
    const bool result =
        GetCommittedLocalNewTabPage(web_contents())
            ->CanShareNoventraGlassBackground(normalized, size());
    VLOG(1) << "NoventraGlass contents.share view=" << this
            << " result=" << result << " size=" << size().ToString()
            << " local=" << local_bounds.ToString()
            << " normalized=" << normalized.ToString();
    return result;
  }
#endif
  return false;
}

const gfx::RoundedCornersF& ContentsWebView::GetBackgroundRadii() const {
  const ui::Layer* background_layer = layer();

  CHECK(background_layer);
  return background_layer->rounded_corner_radii();
}

void ContentsWebView::SetBackgroundRadii(const gfx::RoundedCornersF& radii) {
  ui::Layer* background_layer = layer();

  CHECK(background_layer);
  background_layer->SetRoundedCornerRadius(radii);
  background_layer->SetIsFastRoundedCorner(true);
}

void ContentsWebView::SetIsAnimatingBounds(bool is_animating,
                                           bool is_origin_changing) {
  if (is_animating_bounds_ == is_animating) {
    return;
  }

  is_animating_bounds_ = is_animating;

  if (is_animating_bounds_) {
    if (status_bubble_) {
      status_bubble_->Hide();
    }

    if (use_default_deadline_when_animating_ && is_origin_changing) {
      // Update the render widget host view to set to use default deadline when
      // animating. This is a best effort synchronization between browser and
      // web contents.
      SafeInvoke(web_contents())
          .Then(&content::WebContents::GetRenderWidgetHostView)
          .Then(&content::RenderWidgetHostView::
                    SetShouldUseDefaultDeadlineOnResize,
                /*enable=*/true);
    }
  }
}

void ContentsWebView::UpdateIsBlockedByModal() {
  bool is_blocked = false;
  if (web_contents()) {
    if (auto* tab = tabs::TabInterface::MaybeGetFromContents(web_contents())) {
      is_blocked = tab->IsBlocked();
    }
  }
  holder()->SetProperty(views::kIsBlockedByModalKey, is_blocked);
}

bool ContentsWebView::GetNeedsNotificationWhenVisibleBoundsChange() const {
  return true;
}

void ContentsWebView::OnVisibleBoundsChanged() {
  // If we are animating, the status bubble is hidden and avoid reposition the
  // bubble as an optimization since it's expensive on some platform.
  if (!is_animating_bounds_) {
    SafeInvoke(status_bubble_.get()).Then(&StatusBubbleViews::Reposition);
  }

  // Reset using default deadline once animation is completed after the final
  // bounds changes are made.
  if (!is_animating_bounds_ && use_default_deadline_when_animating_) {
    SafeInvoke(web_contents())
        .Then(&content::WebContents::GetRenderWidgetHostView)
        .Then(
            &content::RenderWidgetHostView::SetShouldUseDefaultDeadlineOnResize,
            /*enable=*/false);
  }
}

void ContentsWebView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  views::WebView::OnBoundsChanged(previous_bounds);
#if BUILDFLAG(ENABLE_WEBUI_NTP)
  if (previous_bounds.size() != size()) {
    if (auto* ntp = GetCommittedLocalNewTabPage(web_contents())) {
      VLOG(1) << "NoventraGlass contents.resize view=" << this
              << " previous_size=" << previous_bounds.size().ToString()
              << " size=" << size().ToString();
      ntp->InvalidateNoventraForegroundBounds();
    }
  }
#endif
}

void ContentsWebView::OnThemeChanged() {
  views::WebView::OnThemeChanged();
  UpdateBackgroundColor();
}

void ContentsWebView::AddedToWidget() {
  views::WebView::AddedToWidget();
  UpdateBackgroundColor();
}

void ContentsWebView::RemovedFromWidget() {
  UpdateBackgroundColor(/*allow_glass=*/false);
  views::WebView::RemovedFromWidget();
}

void ContentsWebView::OnLetterboxingChanged() {
  if (GetWidget()) {
    UpdateBackgroundColor();
  }
}

void ContentsWebView::SetWebContents(content::WebContents* web_contents) {
  if (web_contents != this->web_contents()) {
    UpdateBackgroundColor(/*allow_glass=*/false);
    noventra_committing_navigation_id_.reset();
  }
  views::WebView::SetWebContents(web_contents);
  UpdateBackgroundColor();
  if (web_contents == nullptr) {
    status_bubble_.reset();
    holder()->SetProperty(views::kIsBlockedByModalKey, false);
    // Early exit: Without web contents, views dependent on ContentsWebView's
    // bounds cannot be properly created or positioned. These views will
    // initialize later when valid web contents exist.
    return;
  }

  UpdateIsBlockedByModal();

  if (!status_bubble_) {
    status_bubble_ = std::make_unique<StatusBubbleViews>(this);
    status_bubble_->Reposition();
  }

  // Ensure any dialogs already showing for the webcontents gets
  // re-centered when the active tab changes or a split tab is created.
  SafeInvoke(
      web_modal::WebContentsModalDialogManager::FromWebContents(web_contents))
      .Then(&web_modal::WebContentsModalDialogManager::UpdateDialogHost);
}

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
void ContentsWebView::DidGetUserInteraction(const blink::WebInputEvent& event) {
  // If the user interacts with the web contents, ensure it is activated.
  // This handles cases where the native window does not receive a focus
  // event, such as when a permission prompt is open in another split view.
  if (event.GetType() == blink::WebInputEvent::Type::kMouseDown ||
      event.GetType() == blink::WebInputEvent::Type::kTouchStart) {
    // RequestFocus() ensures the container view receives focus,
    // which is sufficient to update the browser UI.
    if (!HasFocus()) {
      RequestFocus();
    }
  }
}
#endif

void ContentsWebView::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  views::WebView::RenderFrameCreated(render_frame_host);
  if (web_contents() &&
      render_frame_host == web_contents()->GetPrimaryMainFrame()) {
    UpdateBackgroundColor();
  }
}

void ContentsWebView::RenderFrameHostChanged(
    content::RenderFrameHost* old_host,
    content::RenderFrameHost* new_host) {
  views::WebView::RenderFrameHostChanged(old_host, new_host);
  if (web_contents() && new_host == web_contents()->GetPrimaryMainFrame()) {
    UpdateBackgroundColor();
  }
}

void ContentsWebView::ReadyToCommitNavigation(
    content::NavigationHandle* navigation) {
  if (!navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument()) {
    return;
  }
  noventra_committing_navigation_id_ = navigation->GetNavigationId();
  UpdateBackgroundColor(/*allow_glass=*/false);
  // A new or reused target widget must start opaque before its first frame.
  if (GetWidget()) {
    if (auto* view = navigation->GetRenderFrameHost()->GetView()) {
      view->SetBackgroundColor(
          background_visible_
              ? GetColorProvider()->GetColor(kColorWebContentsBackground)
              : SK_ColorTRANSPARENT);
    }
  }
}

void ContentsWebView::DidFinishNavigation(
    content::NavigationHandle* navigation) {
  if (noventra_committing_navigation_id_ == navigation->GetNavigationId()) {
    noventra_committing_navigation_id_.reset();
    // An aborted commit may leave the original trusted NTP displayed.
    UpdateBackgroundColor();
  }
}

void ContentsWebView::PrimaryPageChanged(content::Page& page) {
  views::WebView::PrimaryPageChanged(page);
  noventra_committing_navigation_id_.reset();
  UpdateBackgroundColor();
}

void ContentsWebView::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  noventra_committing_navigation_id_.reset();
  UpdateBackgroundColor(/*allow_glass=*/false);
}

void ContentsWebView::UpdateBackgroundColor(bool allow_glass) {
  if (!GetWidget()) {
    return;
  }
  const SkColor color = GetColorProvider()->GetColor(
      is_letterboxing() ? kColorWebContentsBackgroundLetterboxing
                        : kColorWebContentsBackground);
  // `color` must be opaque, see RenderWidgetHostView::SetBackgroundColor() for
  // details.
  // TODO(crbug.com/456309057): Update this to be a CHECK.
  if (SkColorGetA(color) != SK_AlphaOPAQUE) {
    base::debug::DumpWithoutCrashing();
    if (background_visible_) {
      return;
    }
  }

  bool use_glass = false;
#if BUILDFLAG(ENABLE_WEBUI_NTP)
  if (auto* ntp = GetCommittedLocalNewTabPage(web_contents())) {
    use_glass = allow_glass && background_visible_ && !is_letterboxing() &&
                !noventra_committing_navigation_id_.has_value() &&
                helium::ShouldUseNativeFrameMaterials(this);
    ntp->SetNoventraGlassTint(
        use_glass ? std::make_optional(helium::GetNoventraGlassTint(this))
                  : std::nullopt);
  }
#endif

  auto* background_layer = layer()->AsSolidColor();
  background_layer->SetColor(SkColor4f::FromColor(
      background_visible_ && !use_glass ? color : SK_ColorTRANSPARENT));

  // Renderer backgrounds accept only opaque or fully transparent colors.
  // The trusted NTP paints its tint in the same renderer frame.
  SafeInvoke(web_contents())
      .Then(&content::WebContents::GetRenderWidgetHostView)
      .Then(&content::RenderWidgetHostView::SetBackgroundColor,
            background_visible_ && !use_glass ? color : SK_ColorTRANSPARENT);

  const bool glass_background = HasNoventraGlassBackground();
  if (noventra_glass_background_ != glass_background) {
    noventra_glass_background_ = glass_background;
    // A floating dock may now share this material, or must restore its own
    // backing after navigation. Coalesce the update after this paint stack.
    if (!GetWidget()->IsClosed()) {
      GetWidget()->ResetLastColorProviderKey();
      GetWidget()->ScheduleThemeChanged();
    }
  }
}

std::unique_ptr<ui::Layer> ContentsWebView::RecreateLayer() {
  std::unique_ptr<ui::Layer> old_layer = View::RecreateLayer();

  if (cloned_layer_tree_ && old_layer) {
    // Our layer has been recreated and we have a clone of the WebContents
    // layer. Combined this means we're about to be destroyed and an animation
    // is in effect. The animation cloned our layer, but it won't create another
    // clone of the WebContents layer (|cloned_layer_tree_|). Another clone
    // is not created as the clone has no owner (see CloneChildren()). Because
    // we want the WebContents layer clone to be animated we move it to the
    // old_layer, which is the layer the animation happens on. This animation
    // ends up owning the layer (and all its descendants).
    old_layer->Add(cloned_layer_tree_->release());
    cloned_layer_tree_.reset();
  }

  return old_layer;
}

void ContentsWebView::CloneWebContentsLayer() {
  if (!web_contents()) {
    return;
  }

  views::Widget* widget = GetWidget();
  if (!widget || !widget->GetNativeWindow()) {
    return;
  }

#if defined(USE_AURA)
  // We don't need to clone the layers on non-Aura (Mac), because closing an
  // NSWindow does not animate.
  cloned_layer_tree_ = wm::RecreateLayers(web_contents()->GetNativeView());
#endif
  if (!cloned_layer_tree_ || !cloned_layer_tree_->root()) {
    cloned_layer_tree_.reset();
    return;
  }

  // The cloned layer is in a different coordinate system than our layer (which
  // is now the new parent of the cloned layer). Convert coordinates so that the
  // cloned layer appears at the right location.
  gfx::PointF origin;
  ui::Layer::ConvertPointToLayer(cloned_layer_tree_->root(), layer(),
                                 /*use_target_transform=*/true, &origin);
  cloned_layer_tree_->root()->SetBounds(
      gfx::Rect(gfx::ToFlooredPoint(origin),
                cloned_layer_tree_->root()->bounds().size()));
  layer()->Add(cloned_layer_tree_->root());
}

void ContentsWebView::DestroyClonedLayer() {
  cloned_layer_tree_.reset();
}

void ContentsWebView::RenderViewReady() {
  // Set the background color to be the theme's ntp background on startup.
  if (GetWidget()) {
    UpdateBackgroundColor();
  }
  WebView::RenderViewReady();
}

BEGIN_METADATA(ContentsWebView)
END_METADATA
