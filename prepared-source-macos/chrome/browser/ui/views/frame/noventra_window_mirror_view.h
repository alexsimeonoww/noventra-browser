// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_WINDOW_MIRROR_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_WINDOW_MIRROR_VIEW_H_

#include <memory>

class BrowserView;
class BrowserWindowInterface;
namespace gfx {
class Point;
}
namespace views {
class View;
}

// Window-local, rendererless presentation of the shared tab catalog. Native
// models continue to own only real pages; projection rows never become tabs.
class NoventraWindowMirrorPresentation {
 public:
  explicit NoventraWindowMirrorPresentation(BrowserView* browser_view);
  ~NoventraWindowMirrorPresentation();
  NoventraWindowMirrorPresentation(const NoventraWindowMirrorPresentation&) =
      delete;
  NoventraWindowMirrorPresentation& operator=(
      const NoventraWindowMirrorPresentation&) = delete;

  void Layout();
  void Refresh();
  views::View* GetDefaultFocusableChild() const;
  void OnActivationChanged(bool active);
  bool ContainsTabStripPoint(const views::View* source,
                             const gfx::Point& point) const;

  // These hooks are called before Chromium's physical-model command guards.
  static bool HandleTabCommand(BrowserWindowInterface* browser, int command);
  static bool CreateMirroredWindow(BrowserWindowInterface* source);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_FRAME_NOVENTRA_WINDOW_MIRROR_VIEW_H_
