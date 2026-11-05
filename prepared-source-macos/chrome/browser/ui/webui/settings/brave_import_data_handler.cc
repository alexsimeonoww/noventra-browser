/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "chrome/browser/ui/webui/settings/brave_import_data_handler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/importer/brave_external_process_importer_host.h"
#include "chrome/browser/importer/importer_list.h"
#include "chrome/browser/importer/profile_writer.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/user_data_importer/common/imported_bookmark_entry.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/foundation_util.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tab_modal_confirm_dialog.h"
#include "chrome/browser/ui/webui/settings/brave_full_disk_access_confirm_dialog_delegate.h"
#endif  // BUILDFLAG(IS_MAC)

namespace {
// Item completion also occurs for empty or invalid HTML files. Observe the
// normal synchronous write so an imported URL or explicit source folder counts
// as success, rather than a generated import wrapper alone.
class BookmarkFileProfileWriter : public ProfileWriter,
                                  public bookmarks::BaseBookmarkModelObserver {
 public:
  BookmarkFileProfileWriter(Profile* profile,
                            base::RepeatingClosure on_entry_added)
      : ProfileWriter(profile),
        profile_(profile),
        on_entry_added_(std::move(on_entry_added)) {}

  void AddBookmarks(
      const std::vector<user_data_importer::ImportedBookmarkEntry>& bookmarks,
      const std::u16string& top_level_folder_name) override {
    auto* model = BookmarkModelFactory::GetForBrowserContext(profile_);
    DCHECK(model->loaded());
    source_folder_titles_.clear();
    for (const auto& entry : bookmarks) {
      if (entry.is_folder) {
        source_folder_titles_.push_back(entry.title);
      }
    }
    base::ScopedObservation<bookmarks::BookmarkModel,
                            bookmarks::BookmarkModelObserver>
        observation(this);
    observation.Observe(model);
    added_entry_ = false;
    ProfileWriter::AddBookmarks(bookmarks, top_level_folder_name);
    observation.Reset();
    if (added_entry_) {
      on_entry_added_.Run();
    }
  }

  void BookmarkModelChanged() override {}

  void BookmarkNodeAdded(const bookmarks::BookmarkNode* parent,
                         size_t index,
                         bool added_by_user) override {
    if (added_entry_) {
      return;
    }
    const auto* node = parent->children()[index].get();
    added_entry_ |= node->is_url() || std::ranges::find(source_folder_titles_,
                                                        node->GetTitle()) !=
                                          source_folder_titles_.end();
  }

 protected:
  ~BookmarkFileProfileWriter() override = default;

 private:
  const raw_ptr<Profile> profile_;
  const base::RepeatingClosure on_entry_added_;
  std::vector<std::u16string> source_folder_titles_;
  bool added_entry_ = false;
};

#if BUILDFLAG(IS_MAC)
bool HasProperDiskAccessPermission(uint16_t imported_items) {
  DCHECK(imported_items);

  const base::FilePath& library_dir = base::apple::GetUserLibraryPath();
  const base::FilePath safari_dir = library_dir.Append("Safari");

  if (imported_items & user_data_importer::FAVORITES) {
    const base::FilePath bookmarks_path = safari_dir.Append("Bookmarks.plist");
    if (!PathIsWritable(bookmarks_path)) {
      LOG(ERROR) << __func__ << " " << bookmarks_path << " is not accessible."
                 << " Please check full disk access permission.";
      return false;
    }
  }

  if (imported_items & user_data_importer::HISTORY) {
    const base::FilePath history_path = safari_dir.Append("History.plist");
    if (!PathIsWritable(history_path)) {
      LOG(ERROR) << __func__ << " " << history_path << " is not accessible."
                 << " Please check full disk access permission.";
      return false;
    }
  }

  return true;
}
#endif  // BUILDFLAG(IS_MAC)
constexpr char kImportStatusSucceeded[] = "succeeded";
constexpr char kImportStatusFailed[] = "failed";
}  // namespace

namespace settings {

BraveImportDataHandler::BraveImportDataHandler() = default;
BraveImportDataHandler::~BraveImportDataHandler() = default;

void BraveImportDataHandler::StartImport(
    const user_data_importer::SourceProfile& source_profile,
    uint16_t imported_items) {
  if (!imported_items) {
    return;
  }
  if (pending_password_writer_) {
    return;
  }
  Profile* profile = Profile::FromWebUI(web_ui());
  cookie_partition_.reset();
  if (imported_items & user_data_importer::COOKIES) {
    if (profile->IsOffTheRecord()) {
      FireWebUIListener("import-data-status-changed",
                        base::Value(kImportStatusFailed));
      return;
    }
    cookie_partition_ = noventra::SpaceConfig(
        profile, noventra::SpaceId(web_ui()->GetWebContents()));
  }
#if BUILDFLAG(IS_MAC)
  CheckDiskAccess(imported_items, source_profile.source_path,
                  source_profile.importer_type,
                  base::BindOnce(&BraveImportDataHandler::StartImportImpl,
                                 weak_factory_.GetWeakPtr(), source_profile,
                                 imported_items, profile));
#else
  StartImportImpl(source_profile, imported_items, profile);
#endif
}

void BraveImportDataHandler::StartImportImpl(
    const user_data_importer::SourceProfile& source_profile,
    uint16_t imported_items,
    Profile* profile) {
  if (pending_password_writer_) {
    return;
  }
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
  if ((source_profile.importer_type == user_data_importer::TYPE_CHROME ||
       source_profile.importer_type == user_data_importer::TYPE_BRAVE) &&
      (imported_items & user_data_importer::PASSWORDS)) {
    // Inspect the destination before opening the source. Imported passwords
    // must not replace credentials that are already saved in Noventra.
    import_did_succeed_ = false;
    import_failed_ = false;
    FireWebUIListener("import-data-status-changed", base::Value("inProgress"));
    pending_password_writer_ = noventra::PreparePasswordProfileWriter(
        profile,
        base::BindOnce(&BraveImportDataHandler::OnPasswordWriterPrepared,
                       weak_factory_.GetWeakPtr(), source_profile,
                       imported_items, profile));
    return;
  }
#endif
  StartImportWithWriter(source_profile, imported_items, profile, nullptr);
}

void BraveImportDataHandler::OnPasswordWriterPrepared(
    const user_data_importer::SourceProfile& source_profile,
    uint16_t imported_items,
    Profile* profile,
    noventra::PreparedPasswordProfileWriter result) {
  pending_password_writer_.reset();
  if (!result.has_value()) {
    import_failed_ = true;
    FireWebUIListener("import-data-status-changed", base::Value("failed"));
    return;
  }
  StartImportWithWriter(source_profile, imported_items, profile,
                        std::move(result.value()));
}

void BraveImportDataHandler::StartImportWithWriter(
    const user_data_importer::SourceProfile& source_profile,
    uint16_t imported_items,
    Profile* profile,
    scoped_refptr<ProfileWriter> prepared_writer) {
  // If another import is already ongoing, let it finish silently.
  if (import_observers_.count(source_profile.source_path)) {
    import_observers_.erase(source_profile.source_path);
  }

  // This handler can import more than one profile during optional setup.
  // A previous successful import must not hide a later failure.
  import_did_succeed_ = false;
  import_failed_ = false;
  FireWebUIListener("import-data-status-changed", base::Value("inProgress"));

  // Using weak pointers because it destroys itself when finshed.
  auto* importer_host = new BraveExternalProcessImporterHost();
  import_observers_[source_profile.source_path] =
      std::make_unique<BraveImporterObserver>(
          importer_host, source_profile, imported_items,
          base::BindRepeating(&BraveImportDataHandler::NotifyImportProgress,
                              weak_factory_.GetWeakPtr()));

  importer_host->set_parent_window(
      web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  importer_host->set_parent_view(web_ui()->GetWebContents()->GetNativeView());

  scoped_refptr<ProfileWriter> profile_writer = std::move(prepared_writer);
  if (source_profile.importer_type == user_data_importer::TYPE_BOOKMARKS_FILE) {
    profile_writer = new BookmarkFileProfileWriter(
        profile, base::BindRepeating(
                     [](base::WeakPtr<BraveImportDataHandler> handler) {
                       if (handler) {
                         handler->import_did_succeed_ = true;
                       }
                     },
                     weak_factory_.GetWeakPtr()));
  } else if (!profile_writer) {
    profile_writer = new ProfileWriter(profile);
  }
  if (imported_items & user_data_importer::COOKIES) {
    if (!cookie_partition_) {
      import_observers_.erase(source_profile.source_path);
      FireWebUIListener("import-data-status-changed",
                        base::Value(kImportStatusFailed));
      return;
    }
    profile_writer->ConfigureCookieImport(
        *cookie_partition_,
        base::BindRepeating(&ImportDataHandler::OnCookieImportResult,
                            cookie_weak_factory_.GetWeakPtr()));
  }
  importer_host->StartImportSettings(source_profile, profile, imported_items,
                                     profile_writer.get());
}

void BraveImportDataHandler::NotifyImportProgress(
    const user_data_importer::SourceProfile& source_profile,
    const base::DictValue& info) {
  const std::string* event = info.FindString("event");
  if (!event) {
    return;
  }
  if (*event == "ImportFailed") {
    import_failed_ = true;
    import_did_succeed_ = false;
  } else if (*event == "ImportItemEnded") {
    if (source_profile.importer_type !=
            user_data_importer::TYPE_BOOKMARKS_FILE &&
        !import_failed_) {
      import_did_succeed_ = true;
    }
  } else if (*event == "ImportEnded") {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&BraveImportDataHandler::OnImportEnded,
                                  weak_factory_.GetWeakPtr(), source_profile));
  }
}

void BraveImportDataHandler::HandleImportData(const base::ListValue& args) {
  ImportDataHandler::HandleImportData(args);
}

void BraveImportDataHandler::OnImportEnded(
    const user_data_importer::SourceProfile& source_profile) {
  import_observers_.erase(source_profile.source_path);
  FireWebUIListener("import-data-status-changed",
                    base::Value(import_did_succeed_ && !import_failed_
                                    ? kImportStatusSucceeded
                                    : kImportStatusFailed));
}

void BraveImportDataHandler::OnJavascriptDisallowed() {
  pending_password_writer_.reset();
  ImportDataHandler::OnJavascriptDisallowed();

  // When the WebUI is unloading, we ignore all further updates from the host as
  // ImportDataHandler does.
  import_observers_.clear();
}

const user_data_importer::SourceProfile&
BraveImportDataHandler::GetSourceProfileAt(int browser_index) {
  return importer_list_->GetSourceProfileAt(browser_index);
}

#if BUILDFLAG(IS_MAC)
void BraveImportDataHandler::CheckDiskAccess(
    uint16_t imported_items,
    base::FilePath source_path,
    user_data_importer::ImporterType importer_type,
    ContinueImportCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  guide_dialog_is_requested_ = false;

  if (importer_type == user_data_importer::TYPE_SAFARI) {
    // Start import if Brave has full disk access permission.
    // If not, show dialog that has infos about that permission.
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&HasProperDiskAccessPermission, imported_items),
        base::BindOnce(&BraveImportDataHandler::OnGetDiskAccessPermission,
                       weak_factory_.GetWeakPtr(), std::move(callback),
                       source_path));
    return;
  }
  std::move(callback).Run();
}

void BraveImportDataHandler::OnGetDiskAccessPermission(
    ContinueImportCallback callback,
    base::FilePath source_path,
    bool allowed) {
  if (!allowed) {
    // Notify to webui to finish import process and launch tab modal dialog
    // to guide full disk access information to users.
    // Guide dialog will be opened after import dialog is closed.
    FireWebUIListener("import-data-status-changed", base::Value("failed"));
    if (import_observers_.count(source_path)) {
      import_observers_[source_path]->ImportEnded();
    }
    // Observing web_contents is started here to know the closing timing of
    // import dialog.
    Observe(web_ui()->GetWebContents());

    guide_dialog_is_requested_ = true;
    return;
  }

  std::move(callback).Run();
}

void BraveImportDataHandler::DidStopLoading() {
  Observe(nullptr);

  if (!guide_dialog_is_requested_) {
    return;
  }

  guide_dialog_is_requested_ = false;

  auto* web_contents = web_ui()->GetWebContents();
  TabModalConfirmDialog::Create(
      std::make_unique<FullDiskAccessConfirmDialogDelegate>(
          web_contents,
          GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
              web_contents)),
      web_contents);
}
#endif
}  // namespace settings
