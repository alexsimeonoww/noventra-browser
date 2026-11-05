// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_VIEWS_NOVENTRA_LINK_PREVIEW_H_
#define CHROME_BROWSER_UI_VIEWS_NOVENTRA_LINK_PREVIEW_H_

#include <map>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/referrer.h"
#include "ui/gfx/geometry/rect.h"

namespace content {
struct ContextMenuParams;
class NavigationThrottleRegistry;
}  // namespace content
namespace views {
class DialogDelegate;
class Widget;
}  // namespace views
class PreviewTab;

// Tab-scoped input/lifetime state. No preview WebContents until requested.
class NoventraLinkPreview
    : public content::WebContentsObserver,
      public content::WebContentsUserData<NoventraLinkPreview>,
      public content::RenderWidgetHostObserver {
 public:
  ~NoventraLinkPreview() override;
  static void MaybeCreateThrottle(
      content::NavigationThrottleRegistry& registry);
  static void Show(content::WebContents* source,
                   const GURL& url,
                   const content::Referrer& referrer);
  static bool HandleContextMenu(content::RenderFrameHost& frame,
                                const content::ContextMenuParams& params);

 private:
  friend class content::WebContentsUserData<NoventraLinkPreview>;
  explicit NoventraLinkPreview(content::WebContents* source);
  void RenderFrameCreated(content::RenderFrameHost* frame) override;
  void RenderWidgetHostDestroyed(content::RenderWidgetHost* host) override;
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidOpenRequestedURL(content::WebContents* new_contents,
                           content::RenderFrameHost* source_frame,
                           const GURL& url,
                           const content::Referrer& referrer,
                           WindowOpenDisposition disposition,
                           ui::PageTransition transition,
                           bool started_from_context_menu,
                           bool renderer_initiated) override;
  void WebContentsDestroyed() override;
  void Attach(content::RenderFrameHost* frame);
  bool OnMouse(content::RenderWidgetHost* host,
               const blink::WebMouseEvent& event);
  bool CanAutomaticallyPreview() const;
  bool PreviewPinnedUrl(base::WeakPtr<content::WebContents> destination,
                        base::TimeTicks input_start,
                        const GURL& source_url,
                        const GURL& url);
  void ShowPreview(const GURL& url, const content::Referrer& referrer);
  gfx::Rect GetPreviewBounds() const;
  void ClosePreview();
  void OnPreviewClosed();
  std::map<content::RenderWidgetHost*,
           content::RenderWidgetHost::MouseEventCallback>
      hosts_;
  base::TimeTicks pending_hit_test_;
  base::TimeTicks plain_click_input_start_;
  bool swallowing_click_ = false;
  base::WeakPtr<PreviewTab> preview_;
  std::unique_ptr<views::DialogDelegate> preview_delegate_;
  std::unique_ptr<views::Widget> preview_widget_;
  base::WeakPtrFactory<NoventraLinkPreview> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
#endif
