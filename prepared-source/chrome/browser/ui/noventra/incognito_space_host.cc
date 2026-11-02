// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/noventra/incognito_space_host.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_native_widget.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/browser_widget.h"
#include "chrome/common/webui_url_constants.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/service_worker_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/focus/focus_manager_properties.h"
#include "ui/views/widget/widget.h"

#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager_delegate.h"
#include "ui/aura/client/capture_client.h"
#include "ui/views/widget/native_widget_aura.h"
#include "ui/wm/core/transient_window_manager.h"
#include "ui/wm/core/window_modality_controller.h"
#endif

namespace noventra {
namespace {

struct PrivateSpace {
  base::WeakPtr<BrowserWindowInterface> browser;
  std::string id;
  std::u16string name;
};

struct Host {
  base::WeakPtr<BrowserWindowInterface> browser;
  std::vector<PrivateSpace> spaces;
  base::WeakPtr<BrowserWindowInterface> selected;
  bool close_requested = false;
  int next_number = 1;
};

struct Creation {
  base::WeakPtr<BrowserWindowInterface> host;
  std::string id;
  bool consumed = false;
};

// Retain no raw profile/partition pointer across asynchronous worker stop.
// If the last OTR Browser closes, context destruction itself drops every
// memory partition. Otherwise clear exactly this closed Space's partition.
void ClearClosedPrivatePartition(Profile* profile, const std::string& id) {
  CHECK(profile->IsOffTheRecord());
  if (id.empty() || profile->ShutdownStarted()) {
    return;
  }
  bool still_owned = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser) -> bool {
        if (browser->GetProfile() == profile) {
          auto* model = browser->GetTabStripModel();
          for (int i = 0; model && i < model->count(); ++i) {
            if (SpaceId(model->GetWebContentsAt(i)) == id) {
              still_owned = true;
              return false;
            }
          }
        }
        return true;
      });
  // A private tab explicitly moved to another private window still owns its
  // original partition. Never clear data under a live consumer.
  if (still_owned) {
    return;
  }
  const auto config = SpaceConfig(profile, id);
  auto* partition = profile->GetStoragePartition(config, /*can_create=*/false);
  if (!partition) {
    return;
  }
  partition->GetServiceWorkerContext()->StopAllServiceWorkers(base::BindOnce(
      [](base::WeakPtr<content::BrowserContext> context,
         content::StoragePartitionConfig config) {
        if (!context) {
          return;
        }
        if (auto* partition =
                context->GetStoragePartition(config, /*can_create=*/false)) {
          // The opaque StorageKey overload clears all origins in THIS
          // partition, including cookies, workers, cache and site storage.
          partition->ClearData(content::StoragePartition::REMOVE_DATA_MASK_ALL,
                               blink::StorageKey(), base::Time(),
                               base::Time::Max(), base::DoNothing());
        }
      },
      static_cast<content::BrowserContext*>(profile)->GetWeakPtr(), config));
}

class IncognitoHosts {
 public:
  static IncognitoHosts& Get() {
    static base::NoDestructor<IncognitoHosts> instance;
    return *instance;
  }

  Host* Find(BrowserWindowInterface* browser) {
    if (!browser) {
      return nullptr;
    }
    for (auto& [id, host] : hosts_) {
      if (host.browser.get() == browser ||
          std::ranges::any_of(host.spaces, [browser](const auto& space) {
            return space.browser.get() == browser;
          })) {
        return &host;
      }
    }
    return nullptr;
  }

  Host& Enroll(BrowserWindowInterface* browser) {
    auto& host = hosts_[browser->GetSessionID().id()];
    host.browser = browser->GetWeakPtr();
    return host;
  }

  void Select(Host& host,
              BrowserWindowInterface* selected,
              bool cancel_close = true) {
    if (selecting_ || !host.browser || host.browser->IsDeleteScheduled() ||
        (selected && selected->IsDeleteScheduled())) {
      return;
    }
    base::AutoReset<bool> selecting(&selecting_, true);
    const auto regular = host.browser;
    const auto selected_browser = selected ? selected->GetWeakPtr() : nullptr;
    const auto spaces = host.spaces;
    host.selected = selected_browser;
    if (cancel_close) {
      host.close_requested = false;
    }
    // Every private BrowserView stays attached to its own widget/profile.
    // Widget visibility changes never transfer a page between tab models.
    auto* regular_view = BrowserView::GetBrowserViewForBrowser(regular.get());
    if (!regular_view || !regular_view->browser_widget()) {
      return;
    }
    const auto view = regular_view->GetAsWeakPtr();
    regular_view->SetVisible(!selected_browser);
    if (!view || !regular) {
      return;
    }
    for (const auto& space : spaces) {
      if (space.browser) {
        auto* child =
            BrowserView::GetBrowserViewForBrowser(space.browser.get());
        if (child && child->browser_widget()) {
          const bool selected = space.browser.get() == selected_browser.get();
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
          // Only the selected presentation follows the host through a native
          // hide/minimize/restore. Other Spaces must remain hidden.
          if (auto* native = child->browser_widget()->GetNativeView()) {
            wm::TransientWindowManager::GetOrCreate(native)
                ->set_parent_controls_visibility(selected);
          }
#endif
          if (selected) {
            child->Show();
          } else {
            child->browser_widget()->Hide();
          }
        }
      }
    }
    if (!view || !regular || !view->browser_widget()) {
      return;
    }
    LayoutIncognitoSpaces(view.get());
    // Raise the physical OS window, then activate the selected logical widget.
    // Each complete BrowserView owns a separate focus/accelerator tree.
    if (!view->browser_widget()->IsVisible()) {
      view->browser_widget()->Show();
    }
    if (!view || !regular || !view->browser_widget()) {
      return;
    }
    view->browser_widget()->Activate();
    if (!view || !regular) {
      return;
    }
    const auto active = selected_browser ? selected_browser : regular;
    if (active) {
      if (auto* active_view =
              BrowserView::GetBrowserViewForBrowser(active.get());
          active_view && active_view->browser_widget()) {
        active_view->browser_widget()->Activate();
      }
      if (!active) {
        return;
      }
      if (auto* contents = active->GetTabStripModel()->GetActiveWebContents()) {
        contents->Focus();
      }
    }
    changed_.Notify();
  }

  void Destroying(BrowserWindowInterface* browser) {
    auto* host = Find(browser);
    if (!host) {
      return;
    }
    if (host->browser.get() == browser) {
      // Native parent destruction owns the child-widget destruction. Normal
      // close requests first run every private Browser's unload checks below.
      hosts_.erase(browser->GetSessionID().id());
      changed_.Notify();
      return;
    }
    const auto regular = host->browser;
    const bool selected = host->selected.get() == browser;
    const bool finish_close = host->close_requested;
    const auto closed =
        std::ranges::find_if(host->spaces, [browser](const auto& space) {
          return space.browser.get() == browser;
        });
    if (closed != host->spaces.end()) {
      ClearClosedPrivatePartition(browser->GetProfile(), closed->id);
    }
    std::erase_if(host->spaces, [browser](const auto& space) {
      return !space.browser || space.browser.get() == browser;
    });
    if (selected) {
      host->selected.reset();
    }
    // Run after native close has removed the child surface and finished any
    // tab-model notification. A weak host cannot revive a destroying window.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<BrowserWindowInterface> regular, bool selected,
               bool finish_close) {
              if (!regular || regular->IsDeleteScheduled()) {
                return;
              }
              auto& self = Get();
              auto* host = self.Find(regular.get());
              if (!host) {
                return;
              }
              if (finish_close) {
                if (host->spaces.empty()) {
                  self.Select(*host, nullptr, /*cancel_close=*/false);
                }
                if (regular) {
                  regular->GetWindow()->Close();
                }
              } else if (selected) {
                self.Select(*host, nullptr);
                if (regular->GetTabStripModel()->empty()) {
                  chrome::NewTab(regular.get(), NewTabTypes::kNoUserAction);
                }
              }
              self.changed_.Notify();
            },
            regular, selected, finish_close));
    changed_.Notify();
  }

  Creation* creating_ = nullptr;
  bool selecting_ = false;
  base::RepeatingClosureList changed_;

 private:
  std::map<int, Host> hosts_;
};

#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
// BrowserNativeWidgetAura is a DESKTOP widget and creates a new OS host.
// This adapter uses NativeWidgetAura on the existing desktop Aura root, with a
// transient ownership relation to the regular widget. It is a logical top-level
// Widget so that its complete BrowserView owns focus/IME state through
// teardown. No DesktopNativeWidgetAura or additional OS window is created.
class PrivateNativeWidget : public views::NativeWidgetAura,
                            public BrowserNativeWidget {
 public:
  PrivateNativeWidget(BrowserWidget* widget,
                      BrowserView* host,
                      base::WeakPtr<BrowserWindowInterface> owner)
      : views::NativeWidgetAura(widget),
        host_(host->GetAsWeakPtr()),
        owner_(std::move(owner)) {}

  void InitNativeWidget(views::Widget::InitParams params) override {
    const auto widget = GetWidget()->GetWeakPtr();
    views::NativeWidgetAura::InitNativeWidget(std::move(params));
    if (widget) {
      // The native parent embeds an independent Browser/FocusManager, rather
      // than an auxiliary bubble. Keep its accelerator fallback in this root.
      widget->SetNativeWindowProperty(views::kSkipParentAcceleratorFallbackKey,
                                      widget.get());
    }
  }

  void Activate() override {
    if (CanActivatePrivatePresentation()) {
      views::NativeWidgetAura::Activate();
    }
  }
  bool ShouldActivate() const override {
    return CanActivatePrivatePresentation() &&
           views::NativeWidgetAura::ShouldActivate();
  }

  views::Widget::InitParams GetWidgetParams(
      views::Widget::InitParams::Ownership ownership) override {
    CHECK(host_ && host_->GetWidget());
    views::Widget::InitParams params(ownership);
    params.native_widget = this;
    params.type = views::Widget::InitParams::TYPE_WINDOW;
    params.parent = host_->GetWidget()->GetNativeView();
    params.child = false;
    params.remove_standard_frame = true;
    params.bounds = host_->GetBoundsInScreen();
    return params;
  }
  bool UseCustomFrame() const override { return true; }
  bool UsesNativeSystemMenu() const override { return false; }
  bool ShouldSaveWindowPlacement() const override { return false; }
  void GetWindowPlacement(gfx::Rect* bounds,
                          ui::mojom::WindowShowState* state) const override {
    *bounds = GetWidget()->GetWindowBoundsInScreen();
    *state = ui::mojom::WindowShowState::kNormal;
  }
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      const input::NativeWebKeyboardEvent&) override {
    return content::KeyboardEventProcessingResult::NOT_HANDLED;
  }
  bool HandleKeyboardEvent(const input::NativeWebKeyboardEvent&) override {
    return false;
  }
  bool ShouldRestorePreviousBrowserWidgetState() const override {
    return false;
  }
  bool ShouldUseInitialVisibleOnAllWorkspaces() const override { return false; }
  bool IsMoveLoopSupported() const override {
    return host_ && host_->browser_widget() &&
           host_->browser_widget()->IsMoveLoopSupported();
  }
  views::Widget::MoveLoopResult RunMoveLoop(
      const gfx::Vector2d& drag_offset,
      views::Widget::MoveLoopSource source,
      views::Widget::MoveLoopEscapeBehavior escape_behavior) override {
    return host_ && host_->browser_widget()
               ? host_->browser_widget()->RunMoveLoop(drag_offset, source,
                                                      escape_behavior)
               : views::Widget::MoveLoopResult::kCanceled;
  }
  void EndMoveLoop() override {
    if (host_ && host_->browser_widget()) {
      host_->browser_widget()->EndMoveLoop();
    }
  }

 private:
  bool CanActivatePrivatePresentation() const {
    if (!owner_ || !host_ || owner_->IsDeleteScheduled()) {
      return false;
    }
    auto* regular = IncognitoSpaceHost(owner_.get());
    if (!regular || regular->IsDeleteScheduled() ||
        regular != host_->browser() ||
        SelectedIncognitoBrowser(owner_.get()) != owner_.get()) {
      return false;
    }
    auto* physical = host_->browser_widget();
    auto* native = physical ? physical->browser_native_widget() : nullptr;
    // Leave pending FocusManager state for the user's return to the physical
    // OS window and selected Space; never raise that window here.
    return physical && physical->IsVisible() && !physical->IsClosed() &&
           !physical->IsMinimized() && native &&
           native->IsPhysicalWindowActive();
  }

  base::WeakPtr<BrowserView> host_;
  base::WeakPtr<BrowserWindowInterface> owner_;
};
#endif

}  // namespace

BrowserWindowInterface* IncognitoSpaceHost(BrowserWindowInterface* browser) {
  auto* host = IncognitoHosts::Get().Find(browser);
  return host ? host->browser.get() : nullptr;
}

std::string IncognitoSpaceId(BrowserWindowInterface* browser) {
  if (auto* host = IncognitoHosts::Get().Find(browser)) {
    for (const auto& space : host->spaces) {
      if (space.browser.get() == browser) {
        return space.id;
      }
    }
  }
  return {};
}

std::vector<IncognitoSpaceEntry> IncognitoSpaces(
    BrowserWindowInterface* browser) {
  std::vector<IncognitoSpaceEntry> entries;
  if (auto* host = IncognitoHosts::Get().Find(browser)) {
    for (const auto& space : host->spaces) {
      if (space.browser && !space.browser->IsDeleteScheduled()) {
        entries.push_back({space.id, space.name});
      }
    }
  }
  return entries;
}

BrowserWindowInterface* SelectedIncognitoBrowser(
    BrowserWindowInterface* browser) {
  if (auto* host = IncognitoHosts::Get().Find(browser)) {
    return host->selected ? host->selected.get() : host->browser.get();
  }
  return nullptr;
}

bool IncognitoPresentationIsSelected(BrowserWindowInterface* browser) {
  auto* selected = SelectedIncognitoBrowser(browser);
  return !selected || selected == browser;
}

BrowserWindowInterface* IncognitoSpaceTarget(BrowserWindowInterface* browser,
                                             const std::string& id) {
  auto* host = IncognitoHosts::Get().Find(browser);
  if (!host || !host->browser) {
    return nullptr;
  }
  for (const auto& space : host->spaces) {
    if (space.id == id && space.browser) {
      return space.browser.get();
    }
  }
  const auto ids = SpaceIds(host->browser.get());
  return std::ranges::find(ids, id) != ids.end() ? host->browser.get()
                                                 : nullptr;
}

void ShowIncognitoPresentation(BrowserWindowInterface* browser) {
  auto& self = IncognitoHosts::Get();
  if (auto* host = self.Find(browser)) {
    self.Select(*host, host->browser.get() == browser ? nullptr : browser,
                /*cancel_close=*/false);
  }
}

bool DispatchIncognitoHostActivation(BrowserWindowInterface* browser,
                                     bool active) {
  auto* host = IncognitoHosts::Get().Find(browser);
  if (!host || host->browser.get() != browser) {
    return false;
  }
  const auto selected = host->selected;
  // OS activation first activates the desktop content widget. Restore the
  // selected private Aura widget, whose own native activation notifications
  // restore its focus manager and enable its accelerators. Do not translate a
  // regular Aura deactivation into private deactivation: selecting a private
  // root child itself deactivates the regular content window.
  if (active && selected && !selected->IsDeleteScheduled()) {
    if (auto* child = BrowserView::GetBrowserViewForBrowser(selected.get());
        child && child->browser_widget() &&
        !child->browser_widget()->IsClosed()) {
      child->browser_widget()->Activate();
    }
  }
  return !!selected;
}

void RestoreIncognitoFocusAfterModal(content::WebContents* contents) {
  if (!contents) {
    return;
  }
  auto* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(contents);
  auto* host = IncognitoHosts::Get().Find(browser);
  if (!browser || !host || host->selected.get() != browser ||
      IncognitoSpaceId(browser).empty()) {
    return;
  }
  // WindowClosing runs while the modal/widget and event blocker are still
  // unwinding. Restore on the next UI task, retaining no raw view/window.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> regular,
             base::WeakPtr<BrowserWindowInterface> selected) {
            if (!regular || !selected || regular->IsDeleteScheduled() ||
                selected->IsDeleteScheduled()) {
              return;
            }
            auto* host = IncognitoHosts::Get().Find(regular.get());
            if (!host || host->browser.get() != regular.get() ||
                host->selected.get() != selected.get() ||
                host->close_requested ||
                selected->GetTabStripModel()->empty() ||
                javascript_dialogs::AppModalDialogQueue::GetInstance()
                    ->HasActiveDialog()) {
              return;
            }
            auto* regular_view =
                BrowserView::GetBrowserViewForBrowser(regular.get());
            auto* physical =
                regular_view ? regular_view->browser_widget() : nullptr;
            auto* native =
                physical ? physical->browser_native_widget() : nullptr;
            if (!physical || !physical->IsVisible() ||
                physical->IsMinimized() || !native ||
                !native->IsPhysicalWindowActive()) {
              return;
            }
            auto* child = BrowserView::GetBrowserViewForBrowser(selected.get());
            if (!child || !child->browser_widget() ||
                !child->browser_widget()->IsVisible() ||
                child->browser_widget()->IsClosed()) {
              return;
            }
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
            auto* active_contents =
                selected->GetTabStripModel()->GetActiveWebContents();
            if (!active_contents) {
              return;
            }
            // Closing one modal can leave the physical regular root active
            // while the selected private tab already has its next modal.
            // Focus that current dialog directly, without restoring browser
            // or omnibox focus over it. Leave healthy roots and capture alone.
            if (!child->browser_widget()->IsActive() && physical->IsActive() &&
                !aura::client::GetCaptureWindow(physical->GetNativeView())) {
              auto* modal =
                  web_modal::WebContentsModalDialogManager::FromWebContents(
                      active_contents);
              if (modal && modal->IsDialogActive() && modal->delegate() &&
                  modal->delegate()->IsWebContentsVisible(active_contents)) {
                modal->FocusTopmostDialog();
                return;
              }
            }
#endif
            // A child-modal dialog belongs to the active tab's contents,
            // rather than the private widget root. Respect its modal token
            // even when the app-modal queue and root transient check are empty.
            auto* active_tab = selected->GetTabStripModel()->GetActiveTab();
            if (!active_tab || !active_tab->CanShowModalUI()) {
              return;
            }
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
            if (wm::GetModalTransient(active_contents->GetNativeView())) {
              return;
            }
#endif
            const auto view = child->GetAsWeakPtr();
            // NativeWidgetAura::Activate stays inside the already-active OS
            // host. Its activation callback re-enables the private shortcuts.
            child->browser_widget()->Activate();
            if (!view || !regular || !selected ||
                selected->IsDeleteScheduled() ||
                !IncognitoPresentationIsSelected(selected.get())) {
              return;
            }
            regular_view = BrowserView::GetBrowserViewForBrowser(regular.get());
            physical = regular_view ? regular_view->browser_widget() : nullptr;
            native = physical ? physical->browser_native_widget() : nullptr;
            if (!native || !native->IsPhysicalWindowActive()) {
              return;
            }
            // Activation observers may synchronously open another tab modal.
            active_tab = selected->GetTabStripModel()->GetActiveTab();
            if (!active_tab || !active_tab->CanShowModalUI() ||
                javascript_dialogs::AppModalDialogQueue::GetInstance()
                    ->HasActiveDialog()) {
              return;
            }
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
            active_contents =
                selected->GetTabStripModel()->GetActiveWebContents();
            if (!active_contents ||
                wm::GetModalTransient(active_contents->GetNativeView())) {
              return;
            }
#endif
            if (auto* focus = view->GetFocusManager();
                !focus || !focus->RestoreFocusedView()) {
              if (auto* active =
                      selected->GetTabStripModel()->GetActiveWebContents()) {
                active->RestoreFocus();
              }
            }
          },
          host->browser, browser->GetWeakPtr()));
}

bool SelectIncognitoSpace(BrowserWindowInterface* browser,
                          const std::string& id) {
  if (!IncognitoSpaceHost(browser)) {
    return false;
  }
  SwitchSpace(browser, id);
  return true;
}

base::CallbackListSubscription ObserveIncognitoSpaces(
    base::RepeatingClosure callback) {
  return IncognitoHosts::Get().changed_.Add(std::move(callback));
}

BrowserNativeWidget* MaybeCreateIncognitoNativeWidget(BrowserWidget* widget,
                                                      BrowserView* view) {
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  auto& self = IncognitoHosts::Get();
  auto* creation = self.creating_;
  if (!creation || creation->consumed || !creation->host ||
      view->browser()->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !view->GetProfile()->IsIncognitoProfile() ||
      view->GetProfile()->GetOriginalProfile() !=
          creation->host->GetProfile()->GetOriginalProfile()) {
    return nullptr;
  }
  auto* host_view = BrowserView::GetBrowserViewForBrowser(creation->host.get());
  CHECK(host_view && host_view->GetWidget());
  creation->consumed = true;
  auto& host = self.Enroll(creation->host.get());
  const auto name = host.next_number++;
  host.spaces.push_back({view->browser()->GetWeakPtr(), creation->id,
                         name == 1
                             ? u"Incognito"
                             : u"Incognito " + base::NumberToString16(name)});
  return new PrivateNativeWidget(widget, host_view,
                                 view->browser()->GetWeakPtr());
#else
  return nullptr;
#endif
}

BrowserWindowInterface* CreateIncognitoSpace(BrowserWindowInterface* source,
                                             bool add_initial_tab) {
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  if (!source) {
    return nullptr;
  }
  auto* regular = IncognitoSpaceHost(source);
  if (!regular) {
    regular = source;
  }
  if (regular->GetProfile()->IsOffTheRecord() ||
      regular->GetProfile()->IsGuestSession() ||
      regular->GetProfile()->ShutdownStarted() ||
      regular->IsDeleteScheduled() ||
      regular->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      IncognitoModePrefs::GetAvailability(regular->GetProfile()) ==
          policy::IncognitoModeAvailability::kDisabled) {
    return nullptr;
  }
  auto* view = BrowserView::GetBrowserViewForBrowser(regular);
  if (!view || !view->GetWidget() || view->GetWidget()->IsClosed()) {
    return nullptr;
  }
  auto& self = IncognitoHosts::Get();
  Creation creation{regular->GetWeakPtr(),
                    base::Uuid::GenerateRandomV4().AsLowercaseString()};
  base::AutoReset<Creation*> scoped_creation(&self.creating_, &creation);
  auto* profile = regular->GetProfile()->GetPrimaryOTRProfile(true);
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL, profile,
                                   true);
  params.should_trigger_session_restore = false;
  params.omit_from_session_restore = true;
  auto* browser = CreateBrowserWindow(std::move(params));
  CHECK_EQ(IncognitoSpaceHost(browser), regular);
  if (add_initial_tab) {
    const GURL url(chrome::kChromeUINewTabURL);
    NavigateParams navigate(browser, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&navigate);
  }
  self.Select(*self.Find(browser), browser);
  browser->GetWindow()->Show();
  chrome::FocusLocationBar(browser);
  self.changed_.Notify();
  return browser;
#else
  // Unsupported native backends retain genuine Chromium incognito windows;
  // they never put private pages into a regular model as a cosmetic fallback.
  return nullptr;
#endif
}

BrowserWindowInterface* CreateIncognitoSpaceForProfile(Profile* profile,
                                                       bool add_initial_tab) {
  if (!profile) {
    return nullptr;
  }
  BrowserWindowInterface* source = nullptr;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile()->GetOriginalProfile() ==
                profile->GetOriginalProfile() &&
            browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
            (!browser->GetProfile()->IsOffTheRecord() ||
             IncognitoSpaceHost(browser))) {
          source = browser;
          return false;
        }
        return true;
      },
      BrowserCollection::Order::kActivation);
  return CreateIncognitoSpace(source, add_initial_tab);
}

void LayoutIncognitoSpaces(BrowserView* host_view) {
  auto* host = IncognitoHosts::Get().Find(host_view->browser());
  if (!host || host->browser.get() != host_view->browser() ||
      !host_view->GetWidget()) {
    return;
  }
  const gfx::Rect bounds = host_view->GetBoundsInScreen();
  for (const auto& space : host->spaces) {
    if (space.browser) {
      auto* view = BrowserView::GetBrowserViewForBrowser(space.browser.get());
      if (view && view->browser_widget() &&
          !view->browser_widget()->IsClosed()) {
        view->browser_widget()->SetBounds(bounds);
      }
    }
  }
}

bool PrepareIncognitoHostClose(BrowserWindowInterface* browser) {
  auto* host = IncognitoHosts::Get().Find(browser);
  if (!host || host->browser.get() != browser || host->spaces.empty()) {
    return true;
  }
  // Sequential closing preserves each private page's beforeunload/download
  // checks. Show the child so that any cancellation prompt has a visible host.
  const auto child =
      host->selected ? host->selected : host->spaces.back().browser;
  if (child) {
    IncognitoHosts::Get().Select(*host, child.get());
    host->close_requested = true;
    child->GetWindow()->Close();
  }
  return false;
}

bool KeepEmptyIncognitoHost(BrowserWindowInterface* browser) {
  auto* host = IncognitoHosts::Get().Find(browser);
  return host && host->browser.get() == browser && !host->spaces.empty() &&
         !host->close_requested && !browser_shutdown::IsTryingToQuit();
}

void IncognitoWindowCloseCancelled(BrowserWindowInterface* browser) {
  if (auto* host = IncognitoHosts::Get().Find(browser)) {
    host->close_requested = false;
  }
}

void IncognitoBrowserViewDestroying(BrowserWindowInterface* browser) {
  IncognitoHosts::Get().Destroying(browser);
}

}  // namespace noventra
