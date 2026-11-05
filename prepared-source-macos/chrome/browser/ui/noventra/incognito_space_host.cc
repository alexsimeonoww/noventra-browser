// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/noventra/incognito_space_host.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/noventra_space_profile_registry.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
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
#include "chrome/browser/ui/views/tabs/dragging/tab_drag_controller.h"
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

enum class PresentationKind { kRegular, kPrivate };

struct Presentation {
  base::WeakPtr<BrowserWindowInterface> browser;
  std::string id;
  std::u16string name;
  PresentationKind kind;
  base::WeakPtr<BrowserWindowInterface> regular_parent;
};

struct Host {
  base::WeakPtr<BrowserWindowInterface> browser;
  std::vector<Presentation> spaces;
  base::WeakPtr<BrowserWindowInterface> selected;
  bool close_requested = false;
  int next_number = 1;
  uint64_t selection_generation = 0;
  std::string group;
  std::set<std::string> open_regular_spaces;
  std::string selected_regular_space;
};

struct Creation {
  base::WeakPtr<BrowserWindowInterface> host;
  std::string id;
  raw_ptr<Profile> profile;
  PresentationKind kind;
  base::WeakPtr<BrowserWindowInterface> regular_parent;
  bool consumed = false;
};

std::string RestoredHostGroup(const std::map<std::string, std::string>& data,
                              int32_t restore_id) {
  const auto found = data.find(kProfileSpaceHostKey);
  if (found != data.end()) {
    int legacy_id = 0;
    if (base::Uuid::ParseLowercase(found->second).is_valid() ||
        (base::StartsWith(found->second, "legacy:") &&
         base::StringToInt(found->second.substr(7), &legacy_id) &&
         legacy_id > 0)) {
      return found->second;
    }
  }
  // A legacy mixed-Profile window has the same saved native SessionID in all
  // UUID-filtered seeds. The owning root Profile scopes this temporary key.
  return "legacy:" + base::NumberToString(restore_id);
}

void PersistHostMetadata(Host& host) {
  if (!ProfileSpacesEnabled() || !host.browser || host.close_requested ||
      host.browser->IsDeleteScheduled()) {
    return;
  }
  std::map<std::string, std::string> data;
  PopulateProfileSpaceWindowExtraData(host.browser.get(), &data);
  auto save = [&data](BrowserWindowInterface* browser) {
    if (!browser || browser->GetProfile()->IsOffTheRecord()) {
      return;
    }
    if (auto* session =
            SessionServiceFactory::GetForProfile(browser->GetProfile())) {
      for (const auto& [key, value] : data) {
        session->AddWindowExtraData(browser->GetSessionID(), key.c_str(),
                                    value);
      }
    }
  };
  save(host.browser.get());
  for (const auto& space : host.spaces) {
    if (space.kind == PresentationKind::kRegular && space.browser) {
      save(space.browser.get());
    }
  }
}

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

class SpaceWindowHosts {
 public:
  static SpaceWindowHosts& Get() {
    static base::NoDestructor<SpaceWindowHosts> instance;
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
    if (host.group.empty()) {
      host.group = base::Uuid::GenerateRandomV4().AsLowercaseString();
    }
    return host;
  }

  Host* FindGroup(Profile* root, const std::string& group) {
    for (auto& [id, host] : hosts_) {
      if (host.browser && !host.browser->IsDeleteScheduled() &&
          host.browser->GetProfile() == root && host.group == group) {
        return &host;
      }
    }
    return nullptr;
  }

  void Select(Host& host,
              BrowserWindowInterface* selected,
              bool cancel_close = true,
              bool activate = true) {
    if (selecting_ || !host.browser || host.browser->IsDeleteScheduled() ||
        (selected && selected->IsDeleteScheduled())) {
      return;
    }
    base::AutoReset<bool> selecting(&selecting_, true);
    ++host.selection_generation;
    const auto regular = host.browser;
    const auto selected_browser = selected ? selected->GetWeakPtr() : nullptr;
    const auto spaces = host.spaces;
    host.selected = selected_browser;
    if (ProfileSpacesEnabled() && !host.close_requested &&
        (!selected || !selected->GetProfile()->IsOffTheRecord())) {
      host.selected_regular_space =
          selected ? ProfileSpaceId(selected->GetProfile()) : std::string();
      PersistHostMetadata(host);
    }
    if (cancel_close) {
      host.close_requested = false;
    }
    // Every hosted BrowserView stays attached to its own widget/profile.
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
          const bool is_selected =
              space.browser.get() == selected_browser.get();
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
          // Only the selected presentation follows the host through a native
          // hide/minimize/restore. Other Spaces must remain hidden.
          if (auto* native = child->browser_widget()->GetNativeView()) {
            wm::TransientWindowManager::GetOrCreate(native)
                ->set_parent_controls_visibility(is_selected);
          }
#endif
          if (is_selected) {
            if (activate) {
              child->Show();
            } else {
              child->ShowInactive();
            }
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
      if (activate) {
        view->browser_widget()->Show();
      } else {
        view->browser_widget()->ShowInactive();
      }
    }
    if (!activate) {
      changed_.Notify();
      return;
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
      // close requests first run every hosted Browser's unload checks below.
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
    const auto parent = closed != host->spaces.end()
                            ? closed->regular_parent
                            : base::WeakPtr<BrowserWindowInterface>();
    if (closed != host->spaces.end() &&
        closed->kind == PresentationKind::kPrivate) {
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
               bool finish_close,
               base::WeakPtr<BrowserWindowInterface> parent) {
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
                const auto target = parent && !parent->IsDeleteScheduled() &&
                                            self.Find(parent.get()) == host
                                        ? parent
                                        : regular;
                self.Select(*host, target.get() == regular.get()
                                       ? nullptr
                                       : target.get());
                if (target && !target->IsDeleteScheduled() &&
                    target->GetTabStripModel()->empty()) {
                  chrome::NewTab(target.get(), NewTabTypes::kNoUserAction);
                }
              }
              self.changed_.Notify();
            },
            regular, selected, finish_close, parent));
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
class HostedNativeWidget : public views::NativeWidgetAura,
                           public BrowserNativeWidget {
 public:
  HostedNativeWidget(BrowserWidget* widget,
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
    if (CanActivatePresentation()) {
      views::NativeWidgetAura::Activate();
    }
  }
  bool ShouldActivate() const override {
    return CanActivatePresentation() &&
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
  bool CanActivatePresentation() const {
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

void PopulateProfileSpaceWindowExtraData(
    BrowserWindowInterface* browser,
    std::map<std::string, std::string>* extra_data) {
  if (!ProfileSpacesEnabled() || !browser ||
      browser->GetProfile()->IsOffTheRecord()) {
    return;
  }
  auto* host = SpaceWindowHosts::Get().Find(browser);
  if (!host) {
    return;
  }
  (*extra_data)[kProfileSpaceHostKey] = host->group;
  const std::vector<std::string> ids(host->open_regular_spaces.begin(),
                                     host->open_regular_spaces.end());
  (*extra_data)[kProfileSpaceOpenKey] = base::JoinString(ids, ",");
  (*extra_data)[kProfileSpaceSelectedKey] = host->selected_regular_space;
}

void RestoreProfileSpaceWindowIdentity(
    BrowserWindowInterface* browser,
    const std::map<std::string, std::string>& extra_data,
    int32_t restore_id) {
  if (!ProfileSpacesEnabled() || !browser ||
      browser->GetProfile()->IsOffTheRecord() ||
      IsProfileSpace(browser->GetProfile())) {
    return;
  }
  auto& self = SpaceWindowHosts::Get();
  auto& host = self.Enroll(browser);
  host.group = RestoredHostGroup(extra_data, restore_id);
  auto selected = extra_data.find(kProfileSpaceSelectedKey);
  host.selected_regular_space =
      selected != extra_data.end() &&
              base::Uuid::ParseLowercase(selected->second).is_valid()
          ? selected->second
          : std::string();
  if (auto requested = SessionRestore::TakeProfileSpaceStartupSelection(
          browser->GetProfile())) {
    host.selected_regular_space = *requested;
  }
  const auto visible = SpaceIds(browser);
  if (!host.selected_regular_space.empty() &&
      std::ranges::find(visible, host.selected_regular_space) ==
          visible.end()) {
    host.selected_regular_space.clear();
  }
  if (auto open = extra_data.find(kProfileSpaceOpenKey);
      open != extra_data.end()) {
    for (auto id : base::SplitString(open->second, ",", base::TRIM_WHITESPACE,
                                     base::SPLIT_WANT_NONEMPTY)) {
      if (base::Uuid::ParseLowercase(id).is_valid()) {
        host.open_regular_spaces.insert(std::move(id));
      }
    }
  }
  if (!host.selected_regular_space.empty()) {
    host.open_regular_spaces.insert(host.selected_regular_space);
  }
  const std::vector<std::string> open(host.open_regular_spaces.begin(),
                                      host.open_regular_spaces.end());
  const auto selected_id = host.selected_regular_space;
  const auto generation = host.selection_generation;
  PersistHostMetadata(host);
  // The root's native restore finishes its window/tab transaction first.
  // Children can then restore without competing with that transaction's Show.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> physical,
             std::vector<std::string> open, std::string selected_id,
             uint64_t generation) {
            if (!physical || physical->IsDeleteScheduled()) {
              return;
            }
            for (const auto& id : open) {
              const auto visible = SpaceIds(physical.get());
              if (std::ranges::find(visible, id) == visible.end()) {
                continue;
              }
              AcquireProfileSpaceBrowser(
                  physical.get(), id,
                  base::BindOnce(
                      [](base::WeakPtr<BrowserWindowInterface> physical,
                         std::string id, std::string selected_id,
                         uint64_t generation, BrowserWindowInterface* child) {
                        if (!physical || physical->IsDeleteScheduled() ||
                            !child || browser_shutdown::IsTryingToQuit()) {
                          return;
                        }
                        const auto retained = child->GetWeakPtr();
                        if (child->GetTabStripModel()->empty()) {
                          chrome::NewTab(child, NewTabTypes::kNoUserAction);
                        }
                        auto& self = SpaceWindowHosts::Get();
                        auto* host = self.Find(physical.get());
                        if (host && !host->close_requested && retained &&
                            !retained->IsDeleteScheduled() &&
                            id == selected_id &&
                            host->selection_generation == generation) {
                          self.Select(*host, retained.get(),
                                      /*cancel_close=*/true,
                                      physical->GetWindow()->IsActive());
                        }
                      },
                      physical, id, selected_id, generation));
            }
          },
          browser->GetWeakPtr(), open, selected_id, generation));
}

BrowserWindowInterface* CreateRestoredProfileSpaceBrowser(
    BrowserWindowCreateParams params,
    const std::map<std::string, std::string>& extra_data,
    int32_t restore_id,
    BrowserWindowInterface* preferred_host) {
  auto* profile = &params.profile.get();
  auto* root = SpaceCatalogProfile(profile);
  CHECK(ProfileSpacesEnabled() && IsProfileSpace(profile) && root);
  auto& self = SpaceWindowHosts::Get();
  auto effective_data = extra_data;
  if (restore_id <= 0 && !effective_data.contains(kProfileSpaceHostKey)) {
    auto* preferred = self.Find(preferred_host);
    effective_data[kProfileSpaceHostKey] =
        preferred && preferred->browser &&
                preferred->browser->GetProfile() == root
            ? preferred->group
            : base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  const auto group = RestoredHostGroup(effective_data, restore_id);
  auto* host = self.FindGroup(root, group);
  if (!host) {
    BrowserWindowCreateParams physical(BrowserWindowInterface::TYPE_NORMAL,
                                       root, false);
    physical.should_trigger_session_restore = false;
    physical.initial_bounds = params.initial_bounds;
    physical.initial_workspace = params.initial_workspace;
    physical.initial_visible_on_all_workspaces_state =
        params.initial_visible_on_all_workspaces_state;
    physical.initial_show_state = params.initial_show_state;
    auto* browser = CreateBrowserWindow(std::move(physical));
    CHECK(browser);
    RestoreProfileSpaceWindowIdentity(browser, effective_data, restore_id);
    host = self.Find(browser);
    CHECK(host);
    if (browser->GetTabStripModel()->empty()) {
      chrome::NewTab(browser, NewTabTypes::kNoUserAction);
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<BrowserWindowInterface> physical) {
                         if (physical && !physical->IsDeleteScheduled() &&
                             !physical->GetWindow()->IsVisible()) {
                           physical->GetWindow()->ShowInactive();
                         }
                       },
                       browser->GetWeakPtr()));
  }
  CHECK(host->browser && !host->close_requested);
  return CreateProfileSpaceBrowser(host->browser.get(), std::move(params));
}

BrowserWindowInterface* IncognitoSpaceHost(BrowserWindowInterface* browser) {
  auto* host = SpaceWindowHosts::Get().Find(browser);
  return host ? host->browser.get() : nullptr;
}

std::string IncognitoSpaceId(BrowserWindowInterface* browser) {
  if (auto* host = SpaceWindowHosts::Get().Find(browser)) {
    for (const auto& space : host->spaces) {
      if (space.browser.get() == browser &&
          space.kind == PresentationKind::kPrivate) {
        return space.id;
      }
    }
  }
  return {};
}

std::vector<IncognitoSpaceEntry> IncognitoSpaces(
    BrowserWindowInterface* browser) {
  std::vector<IncognitoSpaceEntry> entries;
  if (auto* host = SpaceWindowHosts::Get().Find(browser)) {
    for (const auto& space : host->spaces) {
      if (space.kind == PresentationKind::kPrivate && space.browser &&
          !space.browser->IsDeleteScheduled()) {
        entries.push_back({space.id, space.name});
      }
    }
  }
  return entries;
}

bool IsIncognitoSpaceHostClosing(BrowserWindowInterface* browser) {
  auto* host = SpaceWindowHosts::Get().Find(browser);
  return host && host->close_requested;
}

BrowserWindowInterface* SelectedIncognitoBrowser(
    BrowserWindowInterface* browser) {
  if (auto* host = SpaceWindowHosts::Get().Find(browser)) {
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
  auto* host = SpaceWindowHosts::Get().Find(browser);
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
  auto& self = SpaceWindowHosts::Get();
  if (ProfileSpacesEnabled() && browser && !self.Find(browser) &&
      !browser->GetProfile()->IsOffTheRecord() &&
      !IsProfileSpace(browser->GetProfile())) {
    if (auto requested = SessionRestore::TakeProfileSpaceStartupSelection(
            browser->GetProfile())) {
      std::map<std::string, std::string> data;
      data[kProfileSpaceHostKey] =
          base::Uuid::GenerateRandomV4().AsLowercaseString();
      data[kProfileSpaceSelectedKey] = *requested;
      data[kProfileSpaceOpenKey] = *requested;
      RestoreProfileSpaceWindowIdentity(browser, data,
                                        browser->GetSessionID().id());
      return;
    }
  }
  if (ProfileSpacesEnabled() && browser &&
      SessionRestore::IsRestoring(browser->GetProfile())) {
    return;
  }
  if (auto* host = self.Find(browser)) {
    self.Select(*host, host->browser.get() == browser ? nullptr : browser,
                /*cancel_close=*/false);
  }
}

bool DispatchIncognitoHostActivation(BrowserWindowInterface* browser,
                                     bool active) {
  auto* host = SpaceWindowHosts::Get().Find(browser);
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
  auto* host = SpaceWindowHosts::Get().Find(browser);
  if (!browser || !host || host->selected.get() != browser) {
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
            auto* host = SpaceWindowHosts::Get().Find(regular.get());
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

BrowserWindowInterface* CreateProfileSpaceBrowser(
    BrowserWindowInterface* physical_host,
    BrowserWindowCreateParams params) {
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  auto* profile = &params.profile.get();
  if (!ProfileSpacesEnabled() || !physical_host ||
      physical_host->IsDeleteScheduled() ||
      physical_host->GetProfile()->IsOffTheRecord() ||
      physical_host->GetProfile()->ShutdownStarted() ||
      physical_host->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      params.type != BrowserWindowInterface::TYPE_NORMAL ||
      !IsProfileSpace(profile) || profile->IsOffTheRecord() ||
      profile->ShutdownStarted() ||
      SpaceCatalogProfile(profile) != physical_host->GetProfile()) {
    return nullptr;
  }
  auto& self = SpaceWindowHosts::Get();
  if (auto* host = self.Find(physical_host)) {
    if (host->browser.get() != physical_host || host->close_requested) {
      return nullptr;
    }
    for (const auto& space : host->spaces) {
      if (space.kind == PresentationKind::kRegular && space.browser &&
          !space.browser->IsDeleteScheduled() &&
          space.browser->GetProfile() == profile) {
        return space.browser.get();
      }
    }
  }
  auto* view = BrowserView::GetBrowserViewForBrowser(physical_host);
  if (!view || !view->GetWidget() || view->GetWidget()->IsClosed()) {
    return nullptr;
  }
  Creation creation{physical_host->GetWeakPtr(),
                    ProfileSpaceId(profile),
                    profile,
                    PresentationKind::kRegular,
                    {}};
  const bool session_restore_creation =
      params.creation_source ==
      BrowserWindowCreateParams::CreationSource::kSessionRestore;
  base::AutoReset<Creation*> scoped_creation(&self.creating_, &creation);
  auto* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    return nullptr;
  }
  CHECK(creation.consumed);
  CHECK_EQ(IncognitoSpaceHost(browser), physical_host);
  auto* host = self.Find(physical_host);
  CHECK(host);
  host->open_regular_spaces.insert(ProfileSpaceId(profile));
  PersistHostMetadata(*host);
  const auto child = browser->GetWeakPtr();
  if (!session_restore_creation && !SessionRestore::IsRestoring(profile) &&
      SessionRestore::HasPendingProfileSpaceSession(profile) &&
      SessionRestore::RestoreProfileSpaceSession(profile, browser) ==
          SessionRestore::ProfileSpaceSessionResult::kUnavailable) {
    return nullptr;
  }
  return child && !child->IsDeleteScheduled() ? child.get() : nullptr;
#else
  return nullptr;
#endif
}

void AcquireProfileSpaceBrowser(
    BrowserWindowInterface* source,
    const std::string& id,
    base::OnceCallback<void(BrowserWindowInterface*)> callback,
    bool select) {
  if (!ProfileSpacesEnabled() || !source || source->IsDeleteScheduled() ||
      browser_shutdown::IsTryingToQuit()) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto& self = SpaceWindowHosts::Get();
  auto* physical = IncognitoSpaceHost(source);
  if (!physical) {
    physical = source;
  }
  if (physical->GetProfile()->IsOffTheRecord() ||
      physical->GetProfile()->ShutdownStarted() ||
      SpaceCatalogProfile(physical->GetProfile()) != physical->GetProfile() ||
      physical->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto& host = self.Enroll(physical);
  const auto visible = SpaceIds(physical, /*include_archived=*/!select);
  if (host.close_requested || std::ranges::find(visible, id) == visible.end()) {
    std::move(callback).Run(nullptr);
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<BrowserWindowInterface> source,
         base::WeakPtr<BrowserWindowInterface> physical, uint64_t generation,
         bool select,
         base::OnceCallback<void(BrowserWindowInterface*)> callback,
         BrowserWindowInterface* browser) {
        auto& self = SpaceWindowHosts::Get();
        auto* host = self.Find(physical.get());
        if (!source || source->IsDeleteScheduled() || !physical || !host ||
            host->close_requested || !browser || browser->IsDeleteScheduled()) {
          std::move(callback).Run(nullptr);
          return;
        }
        const auto target = browser->GetWeakPtr();
        std::move(callback).Run(browser);
        host = self.Find(physical.get());
        if (select && source && !source->IsDeleteScheduled() && target &&
            !target->IsDeleteScheduled() && host && !host->close_requested &&
            host->selection_generation == generation &&
            !IsSpaceArchived(physical->GetProfile(),
                             ProfileSpaceId(target->GetProfile())) &&
            !target->GetTabStripModel()->empty()) {
          self.Select(*host,
                      target.get() == physical.get() ? nullptr : target.get());
        }
      },
      source->GetWeakPtr(), physical->GetWeakPtr(), host.selection_generation,
      select, std::move(callback));
  if (id.empty()) {
    std::move(callback).Run(physical);
    return;
  }
  for (const auto& space : host.spaces) {
    if (space.kind == PresentationKind::kRegular && space.id == id &&
        space.browser && !space.browser->IsDeleteScheduled()) {
      std::move(callback).Run(space.browser.get());
      return;
    }
  }
  AcquireSpaceProfile(
      physical->GetProfile(), id,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> source,
             base::WeakPtr<BrowserWindowInterface> physical, std::string id,
             bool select,
             base::OnceCallback<void(BrowserWindowInterface*)> callback,
             Profile* profile) {
            auto& self = SpaceWindowHosts::Get();
            auto* host = self.Find(physical.get());
            if (!source || source->IsDeleteScheduled() || !physical ||
                !profile || physical->IsDeleteScheduled() || !host ||
                host->close_requested || browser_shutdown::IsTryingToQuit() ||
                ProfileSpaceId(profile) != id ||
                SpaceCatalogProfile(profile) != physical->GetProfile()) {
              std::move(callback).Run(nullptr);
              return;
            }
            const auto visible =
                SpaceIds(physical.get(), /*include_archived=*/!select);
            if (std::ranges::find(visible, id) == visible.end()) {
              std::move(callback).Run(nullptr);
              return;
            }
            BrowserWindowCreateParams params(
                BrowserWindowInterface::TYPE_NORMAL, profile, false);
            params.should_trigger_session_restore = false;
            auto* browser =
                CreateProfileSpaceBrowser(physical.get(), std::move(params));
            if (!browser) {
              std::move(callback).Run(nullptr);
              return;
            }
            const auto child = browser->GetWeakPtr();
            const auto restored =
                SessionRestore::RestoreProfileSpaceSession(profile, browser);
            host = self.Find(physical.get());
            std::move(callback).Run(
                restored != SessionRestore::ProfileSpaceSessionResult::
                                kUnavailable &&
                        child && !child->IsDeleteScheduled() && host &&
                        !host->close_requested && source &&
                        !source->IsDeleteScheduled()
                    ? child.get()
                    : nullptr);
          },
          source->GetWeakPtr(), physical->GetWeakPtr(), id, select,
          std::move(callback)));
}

bool SelectProfileSpace(BrowserWindowInterface* source, const std::string& id) {
  if (!ProfileSpacesEnabled()) {
    return false;
  }
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  if (!source || source->IsDeleteScheduled() ||
      browser_shutdown::IsTryingToQuit()) {
    return true;
  }
  auto& self = SpaceWindowHosts::Get();
  auto* physical = IncognitoSpaceHost(source);
  if (!physical) {
    physical = source;
  }
  if (physical->GetProfile()->IsOffTheRecord() ||
      physical->GetProfile()->IsGuestSession() ||
      physical->GetProfile()->ShutdownStarted() ||
      SpaceCatalogProfile(physical->GetProfile()) != physical->GetProfile() ||
      physical->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return true;
  }
  auto& host = self.Enroll(physical);
  if (host.close_requested) {
    return true;
  }
  const auto ids = SpaceIds(physical);
  if (std::ranges::find(ids, id) == ids.end()) {
    return true;
  }
  for (const auto& space : host.spaces) {
    if (space.id == id && space.browser &&
        !space.browser->IsDeleteScheduled()) {
      self.Select(host, space.browser.get());
      return true;
    }
  }
  if (id.empty()) {
    const auto personal = physical->GetWeakPtr();
    self.Select(host, nullptr);
    if (personal && !personal->IsDeleteScheduled() &&
        personal->GetTabStripModel()->empty()) {
      chrome::NewTab(personal.get(), NewTabTypes::kNoUserAction);
    }
    return true;
  }
  const auto generation = ++host.selection_generation;
  AcquireProfileSpaceBrowser(
      source, id,
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> physical,
             uint64_t generation, BrowserWindowInterface* browser) {
            if (!physical || physical->IsDeleteScheduled() || !browser ||
                browser_shutdown::IsTryingToQuit()) {
              return;
            }
            auto& self = SpaceWindowHosts::Get();
            auto* host = self.Find(physical.get());
            const auto child = browser->GetWeakPtr();
            if (!host || host->close_requested ||
                host->selection_generation != generation ||
                child->IsDeleteScheduled()) {
              return;
            }
            if (child->GetTabStripModel()->empty()) {
              chrome::NewTab(child.get(), NewTabTypes::kNoUserAction);
            }
            host = self.Find(physical.get());
            if (!host || host->close_requested || !child ||
                host->selection_generation != generation ||
                child->IsDeleteScheduled()) {
              return;
            }
            self.Select(*host, child.get());
            if (child && !child->IsDeleteScheduled()) {
              chrome::FocusLocationBar(child.get());
            }
          },
          physical->GetWeakPtr(), generation));
#endif
  return true;
}

base::CallbackListSubscription ObserveIncognitoSpaces(
    base::RepeatingClosure callback) {
  return SpaceWindowHosts::Get().changed_.Add(std::move(callback));
}

BrowserNativeWidget* MaybeCreateIncognitoNativeWidget(BrowserWidget* widget,
                                                      BrowserView* view) {
#if defined(USE_AURA) && !BUILDFLAG(IS_CHROMEOS)
  auto& self = SpaceWindowHosts::Get();
  auto* creation = self.creating_;
  if (!creation || creation->consumed || !creation->host ||
      view->browser()->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      view->GetProfile() != creation->profile ||
      creation->host->IsDeleteScheduled()) {
    return nullptr;
  }
  auto* host_view = BrowserView::GetBrowserViewForBrowser(creation->host.get());
  CHECK(host_view && host_view->GetWidget());
  creation->consumed = true;
  auto& host = self.Enroll(creation->host.get());
  std::u16string name;
  if (creation->kind == PresentationKind::kPrivate) {
    const auto number = host.next_number++;
    name = number == 1 ? u"Incognito"
                       : u"Incognito " + base::NumberToString16(number);
  }
  host.spaces.push_back({view->browser()->GetWeakPtr(), creation->id,
                         std::move(name), creation->kind,
                         creation->regular_parent});
  return new HostedNativeWidget(widget, host_view,
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
  auto& self = SpaceWindowHosts::Get();
  auto* physical = IncognitoSpaceHost(source);
  if (!physical) {
    physical = source;
  }
  auto* regular = source;
  if (source == physical) {
    if (auto* selected = SelectedIncognitoBrowser(source)) {
      regular = selected;
    }
  }
  if (regular->GetProfile()->IsOffTheRecord()) {
    if (auto* host = self.Find(regular)) {
      for (const auto& space : host->spaces) {
        if (space.browser.get() == regular && space.regular_parent) {
          regular = space.regular_parent.get();
          break;
        }
      }
    }
  }
  if (regular->GetProfile()->IsOffTheRecord() ||
      regular->GetProfile()->IsGuestSession() ||
      regular->GetProfile()->ShutdownStarted() ||
      regular->IsDeleteScheduled() || physical->IsDeleteScheduled() ||
      regular->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      physical->GetProfile()->IsOffTheRecord() ||
      IncognitoModePrefs::GetAvailability(regular->GetProfile()) ==
          policy::IncognitoModeAvailability::kDisabled) {
    return nullptr;
  }
  auto* view = BrowserView::GetBrowserViewForBrowser(physical);
  if (!view || !view->GetWidget() || view->GetWidget()->IsClosed()) {
    return nullptr;
  }
  auto* profile = regular->GetProfile()->GetPrimaryOTRProfile(true);
  Creation creation{physical->GetWeakPtr(),
                    base::Uuid::GenerateRandomV4().AsLowercaseString(), profile,
                    PresentationKind::kPrivate, regular->GetWeakPtr()};
  base::AutoReset<Creation*> scoped_creation(&self.creating_, &creation);
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL, profile,
                                   true);
  params.should_trigger_session_restore = false;
  params.omit_from_session_restore = true;
  auto* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    return nullptr;
  }
  CHECK_EQ(IncognitoSpaceHost(browser), physical);
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
  auto* host = SpaceWindowHosts::Get().Find(host_view->browser());
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
  auto* host = SpaceWindowHosts::Get().Find(browser);
  if (!host) {
    return true;
  }
  if (host->browser.get() != browser) {
    if (!browser->GetProfile()->IsOffTheRecord() && !host->close_requested &&
        !browser_shutdown::IsTryingToQuit()) {
      const auto physical = host->browser;
      host->close_requested = true;
      ++host->selection_generation;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(
                         [](base::WeakPtr<BrowserWindowInterface> physical) {
                           if (physical && !physical->IsDeleteScheduled()) {
                             physical->GetWindow()->Close();
                           }
                         },
                         physical));
      return false;
    }
    return true;
  }
  if (host->spaces.empty()) {
    return true;
  }
  // Sequential closing preserves each hosted page's beforeunload/download
  // checks. Show the child so that any cancellation prompt has a visible host.
  const auto child =
      host->selected ? host->selected : host->spaces.back().browser;
  if (child) {
    host->close_requested = true;
    SpaceWindowHosts::Get().Select(*host, child.get(), /*cancel_close=*/false);
    child->GetWindow()->Close();
  }
  return false;
}

bool KeepEmptyIncognitoHost(BrowserWindowInterface* browser) {
  auto* host = SpaceWindowHosts::Get().Find(browser);
  if (!host || host->close_requested || browser_shutdown::IsTryingToQuit()) {
    return false;
  }
  if (host->browser.get() == browser) {
    return !host->spaces.empty();
  }
  if (browser->GetProfile()->IsOffTheRecord()) {
    return false;
  }
  if (host->browser && TabDragController::IsNewBrowserForDrag(browser)) {
    auto* physical_model = host->browser->GetTabStripModel();
    const bool placeholder_only =
        physical_model->empty() ||
        (physical_model->count() == 1 && !physical_model->IsTabPinned(0) &&
         !physical_model->GetTabAtIndex(0)->GetGroup() &&
         (physical_model->GetWebContentsAt(0)->GetVisibleURL().is_empty() ||
          physical_model->GetWebContentsAt(0)->GetVisibleURL() ==
              GURL(chrome::kChromeUINewTabURL)));
    const bool other_space_has_tabs =
        std::ranges::any_of(host->spaces, [browser](const auto& space) {
          return space.browser && space.browser.get() != browser &&
                 !space.browser->GetTabStripModel()->empty();
        });
    if (placeholder_only && !other_space_has_tabs) {
      // The native drop consumed this temporary presentation. Its normal
      // close path also closes the otherwise empty physical host.
      return false;
    }
  }
  // Tab-strip observers are still unwinding. Keep a regular Space usable
  // without creating its replacement tab inside those notifications.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<BrowserWindowInterface> browser) {
                       if (!browser || browser->IsDeleteScheduled() ||
                           browser_shutdown::IsTryingToQuit()) {
                         return;
                       }
                       auto* host = SpaceWindowHosts::Get().Find(browser.get());
                       if (host && !host->close_requested &&
                           browser->GetTabStripModel()->empty()) {
                         chrome::NewTab(browser.get(),
                                        NewTabTypes::kNoUserAction);
                       }
                     },
                     browser->GetWeakPtr()));
  return true;
}

void IncognitoWindowCloseCancelled(BrowserWindowInterface* browser) {
  if (auto* host = SpaceWindowHosts::Get().Find(browser)) {
    const bool was_closing = host->close_requested;
    host->close_requested = false;
    ++host->selection_generation;
    if (was_closing && ProfileSpacesEnabled()) {
      const auto selected = host->selected ? host->selected : host->browser;
      if (selected && !selected->GetProfile()->IsOffTheRecord()) {
        host->selected_regular_space = ProfileSpaceId(selected->GetProfile());
        PersistHostMetadata(*host);
      }
    }
  }
}

void IncognitoBrowserViewDestroying(BrowserWindowInterface* browser) {
  SpaceWindowHosts::Get().Destroying(browser);
}

}  // namespace noventra
