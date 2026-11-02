// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_CONTENTS_WEB_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_CONTENTS_WEB_VIEW_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/ui/views/frame/web_contents_close_handler_delegate.h"
#include "chrome/common/buildflags.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/controls/webview/webview.h"

class StatusBubbleViews;
class WebContentsCloseHandler;

namespace ui {
class LayerTreeOwner;
}  // namespace ui

// ContentsWebView is used to present the WebContents of the active tab.
class ContentsWebView : public views::WebView,
                        public WebContentsCloseHandlerDelegate {
  METADATA_HEADER(ContentsWebView, views::WebView)

 public:
  DECLARE_CLASS_ELEMENT_IDENTIFIER_VALUE(kContentsWebViewElementId);

  explicit ContentsWebView(content::BrowserContext* browser_context);
  ContentsWebView(const ContentsWebView&) = delete;
  ContentsWebView& operator=(const ContentsWebView&) = delete;
  ~ContentsWebView() override;

  StatusBubbleViews* GetStatusBubble() const;
  WebContentsCloseHandler* GetWebContentsCloseHandler() const;

  // Toggles whether the background is visible.
  void SetBackgroundVisible(bool background_visible);
  // True only for the committed local New Tab document's desktop material.
  bool HasNoventraGlassBackground() const;
  bool CanShareNoventraGlassBackground(const gfx::Rect& local_bounds) const;

  const gfx::RoundedCornersF& GetBackgroundRadii() const;
  void SetBackgroundRadii(const gfx::RoundedCornersF& radii);

  void set_use_default_deadline_when_animating_bounds(
      bool use_default_deadline) {
    use_default_deadline_when_animating_ = use_default_deadline;
  }

  void SetIsAnimatingBounds(bool is_animating, bool is_origin_changing);

  // Update the blocked state based on the tab's modal dialog status.
  void UpdateIsBlockedByModal();

  // WebView overrides:
  bool GetNeedsNotificationWhenVisibleBoundsChange() const override;
  void OnVisibleBoundsChanged() override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void OnThemeChanged() override;
  void AddedToWidget() override;
  void RemovedFromWidget() override;
  void RenderViewReady() override;
  void OnLetterboxingChanged() override;
  void SetWebContents(content::WebContents* web_contents) override;

  // content::WebContentsObserver overrides:
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void ReadyToCommitNavigation(content::NavigationHandle* navigation) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void PrimaryPageChanged(content::Page& page) override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;

  // Overridden to track physical interactions (mouse/touch) on the WebContents.
  // This allows the browser to force focus synchronization in split view even
  // when native OS focus gets stuck on a different window (like a permission
  // prompt).
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  void DidGetUserInteraction(const blink::WebInputEvent& event) override;
#endif

  // ui::View overrides:
  std::unique_ptr<ui::Layer> RecreateLayer() override;

  // WebContentsCloseHandlerDelegate overrides:
  void CloneWebContentsLayer() override;
  void DestroyClonedLayer() override;

 private:
  void UpdateBackgroundColor(bool allow_glass = true);
  std::unique_ptr<StatusBubbleViews> status_bubble_;
  std::unique_ptr<WebContentsCloseHandler> web_contents_close_handler_;

  bool background_visible_ = true;
  bool noventra_glass_background_ = false;
  bool is_animating_bounds_ = false;
  bool use_default_deadline_when_animating_ = false;

  // Never carry a transparent NTP renderer into an uncommitted document.
  std::optional<int64_t> noventra_committing_navigation_id_;

  std::unique_ptr<ui::LayerTreeOwner> cloned_layer_tree_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_FRAME_CONTENTS_WEB_VIEW_H_
