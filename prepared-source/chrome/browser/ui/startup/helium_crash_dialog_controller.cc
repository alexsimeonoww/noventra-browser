// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/startup/helium_crash_dialog_controller.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/i18n/message_formatter.h"
#include "base/no_destructor.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/crash_upload_list/helium_crash_reporting.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/dialogs/browser_dialogs.h"
#include "chrome/grit/generated_resources.h"
#include "components/crash/core/app/crashpad.h"
#include "components/strings/grit/components_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/models/dialog_model_host.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"

namespace {

constexpr int kCrashDialogIconSize = 32;

std::set<std::string>& ReservedReportIds() {
  static base::NoDestructor<std::set<std::string>> reports;
  return *reports;
}

class HeliumCrashDialogDelegate : public ui::DialogModelDelegate {
 public:
  explicit HeliumCrashDialogDelegate(std::vector<std::string> report_ids)
      : report_ids_(std::move(report_ids)) {}

  HeliumCrashDialogDelegate(const HeliumCrashDialogDelegate&) = delete;
  HeliumCrashDialogDelegate& operator=(const HeliumCrashDialogDelegate&) =
      delete;
  ~HeliumCrashDialogDelegate() override = default;

  void OnDontAskAgain(const ui::Event&) {
    Resolve(/*upload=*/false);
    helium::SetCrashReportingMode(helium::CrashReportingMode::kDisabled);
    dialog_model()->host()->Close();
  }

  void OnAccepted() { Resolve(/*upload=*/true); }

  void OnDismissed() { Resolve(/*upload=*/false); }

 private:
  void Resolve(bool upload) {
    if (resolved_) {
      return;
    }
    resolved_ = true;

    const bool can_upload = upload && helium::IsCrashUploadConfigured() &&
                            !helium::UploadsDisabledByPolicy();
    for (const std::string& report_id : report_ids_) {
      ReservedReportIds().erase(report_id);
      if (can_upload) {
        crash_reporter::RequestSingleCrashUpload(report_id);
      } else {
        crash_reporter::DeleteCrashReport(report_id);
      }
    }
  }

  const std::vector<std::string> report_ids_;
  bool resolved_ = false;
};

std::u16string FormatPluralString(int message_id, size_t report_count) {
  return base::i18n::MessageFormatter::FormatWithNamedArgs(
      l10n_util::GetStringUTF16(message_id), "COUNT",
      static_cast<int>(report_count));
}

std::unique_ptr<ui::DialogModel> BuildCrashDialog(
    std::vector<std::string> report_ids) {
  const size_t report_count = report_ids.size();
  auto delegate =
      std::make_unique<HeliumCrashDialogDelegate>(std::move(report_ids));
  HeliumCrashDialogDelegate* delegate_ptr = delegate.get();

  return ui::DialogModel::Builder(std::move(delegate))
      .SetTitle(FormatPluralString(IDS_HELIUM_CRASH_REPORTING_DIALOG_TITLE,
                                   report_count))
      .SetIcon(ui::ImageModel::FromVectorIcon(
          kSadTabIcon, ui::kColorIcon, kCrashDialogIconSize))
      .AddParagraph(ui::DialogModelLabel(
          IDS_HELIUM_CRASH_REPORTING_DIALOG_DESCRIPTION))
      .AddParagraph(ui::DialogModelLabel(
          IDS_HELIUM_CRASH_REPORTING_DIALOG_PRIVACY))
      .AddParagraph(ui::DialogModelLabel(
          IDS_HELIUM_CRASH_REPORTING_DIALOG_OPTIONAL))
      .AddExtraButton(
          base::BindRepeating(&HeliumCrashDialogDelegate::OnDontAskAgain,
                              base::Unretained(delegate_ptr)),
          ui::DialogModel::Button::Params().SetLabel(
              l10n_util::GetStringUTF16(
                  IDS_TAB_GROUP_DELETION_DIALOG_DONT_ASK)))
      .AddCancelButton(
          base::BindOnce(&HeliumCrashDialogDelegate::OnDismissed,
                         base::Unretained(delegate_ptr)),
          ui::DialogModel::Button::Params().SetLabel(
              l10n_util::GetStringUTF16(IDS_NO_THANKS)))
      .AddOkButton(base::BindOnce(&HeliumCrashDialogDelegate::OnAccepted,
                                  base::Unretained(delegate_ptr)),
                   ui::DialogModel::Button::Params().SetLabel(
                       FormatPluralString(
                           IDS_HELIUM_CRASH_REPORTING_DIALOG_SEND_REPORT,
                           report_count)))
      .OverrideDefaultButton(ui::mojom::DialogButton::kNone)
      .OverrideShowCloseButton(/*show_close_button=*/true)
      .SetCloseActionCallback(
          base::BindOnce(&HeliumCrashDialogDelegate::OnDismissed,
                         base::Unretained(delegate_ptr)))
      .SetDialogDestroyingCallback(
          base::BindOnce(&HeliumCrashDialogDelegate::OnDismissed,
                         base::Unretained(delegate_ptr)))
      .Build();
}

}  // namespace

void MaybeShowHeliumCrashDialog(BrowserWindowInterface* browser) {
  if (!browser || browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !browser->GetProfile()->IsRegularProfile()) {
    return;
  }

  if (!helium::IsCrashUploadConfigured() ||
      helium::UploadsDisabledByPolicy() ||
      helium::GetCrashReportingMode() != helium::CrashReportingMode::kAsk) {
    return;
  }

  std::vector<crash_reporter::Report> reports;
  crash_reporter::GetReports(&reports);
  std::vector<std::string> report_ids;
  for (const auto& report : reports) {
    if (report.state != crash_reporter::ReportUploadState::Pending ||
        !ReservedReportIds().insert(report.local_id).second) {
      continue;
    }
    report_ids.emplace_back(report.local_id);
  }
  if (report_ids.empty()) {
    return;
  }

  chrome::ShowBrowserModal(browser, BuildCrashDialog(std::move(report_ids)));
}
