// Copyright 2026 The Noventra Authors. All rights reserved.
#include "chrome/browser/ui/webui/settings/portable_transfer_handler.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/noventra/layout_model.h"
#include "chrome/browser/ui/noventra/portable_data.h"
#include "chrome/browser/ui/noventra/portable_passwords.h"
#include "chrome/browser/ui/noventra/portable_transfer_codec.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "crypto/secure_util.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

namespace settings {
namespace {
using Result = base::expected<base::DictValue, std::string>;
constexpr size_t kMaximumBase64Bytes =
    ((noventra::PortableTransferCodec::kMaximumEnvelopeBytes + 2) / 3) * 4;

void ClearString(std::string& text) {
  crypto::SecureZeroBuffer(base::as_writable_byte_span(text));
  text.clear();
}

void ClearDocument(base::DictValue& document) {
  if (auto* value = document.Find("passwords_csv");
      value && value->is_string()) {
    ClearString(value->GetString());
  }
  document.clear();
}

Result Seal(base::DictValue document, std::string password) {
  auto summary = noventra::ValidatePortableData(document);
  if (!summary.has_value()) {
    ClearDocument(document);
    ClearString(password);
    return base::unexpected(summary.error());
  }
  if (document.FindDict("categories")->FindBool("passwords").value_or(false) &&
      !document.FindString("passwords_csv")) {
    ClearDocument(document);
    ClearString(password);
    return base::unexpected("The transfer is missing its password data.");
  }
  std::string text;
  const bool serialized = base::JSONWriter::Write(document, &text);
  ClearDocument(document);
  if (!serialized) {
    ClearString(password);
    return base::unexpected("The transfer could not be prepared.");
  }
  auto sealed = noventra::PortableTransferCodec::Seal(text, password);
  ClearString(text);
  ClearString(password);
  if (!sealed.has_value()) {
    return base::unexpected(sealed.error());
  }
  return base::DictValue()
      .Set("data", base::Base64Encode(*sealed))
      .Set("bytes", static_cast<int>(sealed->size()))
      .Set("summary", std::move(*summary))
      .Set("filename", "Noventra-transfer.noventra");
}

Result Open(std::string encoded, std::string password) {
  auto bytes = base::Base64Decode(encoded);
  if (!bytes ||
      bytes->size() > noventra::PortableTransferCodec::kMaximumEnvelopeBytes) {
    ClearString(password);
    return base::unexpected(
        "Choose a supported transfer file smaller than 33 MB.");
  }
  auto text = noventra::PortableTransferCodec::Open(*bytes, password);
  ClearString(password);
  if (!text.has_value()) {
    return base::unexpected(text.error());
  }
  auto document = base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC, 64);
  ClearString(*text);
  if (!document) {
    return base::unexpected("This transfer has an invalid document.");
  }
  return std::move(*document);
}
}  // namespace

PortableTransferHandler::PortableTransferHandler(content::WebUI* web_ui)
    : profile_(Profile::FromWebUI(web_ui)),
      passwords_(std::make_unique<noventra::PortablePasswords>(profile_)) {}

PortableTransferHandler::~PortableTransferHandler() {
  StopPeer();
  Cancel();
}

void PortableTransferHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "noventraTransfer", base::BindRepeating(&PortableTransferHandler::Handle,
                                              base::Unretained(this)));
}

void PortableTransferHandler::ClearPlan() {
  expiry_.Stop();
  passwords_->Cancel();
  ClearDocument(document_);
  review_.clear();
  apply_result_.clear();
  plan_id_.clear();
  layout_revision_ = -1;
}

void PortableTransferHandler::Cancel() {
  ++operation_;
  if (callback_ && IsJavascriptAllowed()) {
    RejectJavascriptCallback(*callback_, base::Value("Transfer canceled."));
  }
  callback_.reset();
  ClearPlan();
}

void PortableTransferHandler::OnJavascriptDisallowed() {
  StopPeer();
  Cancel();
  weak_factory_.InvalidateWeakPtrs();
}

void PortableTransferHandler::StopPeer(bool notify) {
  peer_expiry_.Stop();
  if (!peer_active_) {
    return;
  }
  peer_active_ = false;
  const auto peer_id = std::move(peer_id_);
  peer_id_.clear();
  auto* contents = web_ui()->GetWebContents();
  // Re-read the current policy, including any change made while connected.
  contents->GetMutableRendererPrefs()->webrtc_ip_handling_policy =
      blink::ToWebRTCIPHandlingPolicy(
          profile_->GetPrefs()->GetString(prefs::kWebRTCIPHandlingPolicy));
  contents->SyncRendererPrefs();
  if (notify && IsJavascriptAllowed()) {
    FireWebUIListener("noventra-transfer-peer-stopped", base::Value(peer_id));
  }
}

void PortableTransferHandler::Reply(uint64_t operation, Result result) {
  if (operation != operation_ || !callback_ || !IsJavascriptAllowed()) {
    return;
  }
  auto callback = std::move(*callback_);
  callback_.reset();
  if (result.has_value()) {
    if (plan_id_.empty()) {
      ClearPlan();
    }
    ResolveJavascriptCallback(callback, *result);
  } else {
    ClearPlan();
    RejectJavascriptCallback(callback, base::Value(result.error()));
  }
}

void PortableTransferHandler::Handle(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  AllowJavascript();
  const auto reject = [&](const char* error) {
    RejectJavascriptCallback(args[0], base::Value(error));
  };
  if (args.size() != 3 || !args[1].is_string() || !args[2].is_dict()) {
    reject("Invalid transfer request.");
    return;
  }
  if (profile_->IsOffTheRecord() || profile_->IsGuestSession()) {
    reject("Transfer browser data in a regular profile.");
    return;
  }
  const auto& action = args[1].GetString();
  const auto& payload = args[2].GetDict();
  if (action == "peer-stop") {
    const auto* peer_id = payload.FindString("peer_id");
    if (peer_id && *peer_id == peer_id_) {
      StopPeer();
    }
    ResolveJavascriptCallback(args[0], base::DictValue());
    return;
  }
  if (action == "cancel") {
    Cancel();
    ResolveJavascriptCallback(args[0], base::DictValue());
    return;
  }
  if (callback_) {
    reject("Wait for the current transfer operation.");
    return;
  }
  auto* browser = GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
      web_ui()->GetWebContents());
  if (!browser) {
    reject("The browser window is unavailable.");
    return;
  }
  if (action == "peer-start") {
    const auto* peer_id = payload.FindString("peer_id");
    if (!peer_id || !base::Uuid::ParseLowercase(*peer_id).is_valid()) {
      reject("Create a new direct connection.");
      return;
    }
    auto* contents = web_ui()->GetWebContents();
    if (contents->GetLastCommittedURL().host() !=
            chrome::kChromeUISettingsHost ||
        !contents->GetLastCommittedURL().SchemeIs("chrome")) {
      reject("Start direct transfers from Noventra Settings.");
      return;
    }
    const auto* policy =
        profile_->GetPrefs()->FindPreference(prefs::kWebRTCIPHandlingPolicy);
    // Only the built-in default can receive this temporary, per-WebContents
    // exception. Explicit user/extension/enterprise rules retain their value;
    // per-URL rules also retain precedence in the renderer.
    if (policy && policy->IsDefaultValue()) {
      contents->GetMutableRendererPrefs()->webrtc_ip_handling_policy =
          blink::ToWebRTCIPHandlingPolicy(blink::kWebRTCIPHandlingDefault);
      contents->SyncRendererPrefs();
    }
    peer_active_ = true;
    peer_id_ = *peer_id;
    peer_expiry_.Start(FROM_HERE, base::Minutes(10),
                       base::BindOnce(&PortableTransferHandler::StopPeer,
                                      weak_factory_.GetWeakPtr(), true));
    ResolveJavascriptCallback(args[0], base::DictValue());
    return;
  }
  if (action == "status") {
    ResolveJavascriptCallback(
        args[0],
        base::DictValue()
            .Set("available", true)
            .Set("max_bytes",
                 static_cast<int>(
                     noventra::PortableTransferCodec::kMaximumEnvelopeBytes))
            .Set(
                "layout_revision",
                noventra::ReadOrMigrateLayout(*profile_->GetPrefs()).revision));
    return;
  }
  const auto* categories = payload.FindDict("categories");
  if (action == "apply") {
    const auto* plan = payload.FindString("plan_id");
    if (!plan || *plan != plan_id_ || plan_id_.empty() || !categories) {
      reject("Choose a transfer again to review its contents.");
      return;
    }
    callback_ = args[0].Clone();
    const auto operation = ++operation_;
    expiry_.Stop();
    noventra::ApplyPortableData(
        browser, document_, *categories, layout_revision_,
        base::BindOnce(&PortableTransferHandler::OnDataImported,
                       weak_factory_.GetWeakPtr(), operation,
                       categories->FindBool("passwords").value_or(false)),
        base::BindRepeating(
            [](base::WeakPtr<PortableTransferHandler> self,
               uint64_t operation) {
              return self && self->operation_ == operation && self->callback_ &&
                     self->IsJavascriptAllowed();
            },
            weak_factory_.GetWeakPtr(), operation));
    return;
  }
  if (action != "export" && action != "preview") {
    reject("Unknown transfer action.");
    return;
  }
  const auto* password = payload.FindString("password");
  if (!password ||
      password->size() <
          noventra::PortableTransferCodec::kMinimumPasswordBytes ||
      password->size() >
          noventra::PortableTransferCodec::kMaximumPasswordBytes) {
    reject("Use a transfer password between 12 and 1,024 bytes.");
    return;
  }
  ClearPlan();
  callback_ = args[0].Clone();
  const auto operation = ++operation_;
  if (action == "export") {
    if (!categories) {
      Reply(operation, base::unexpected("Choose data to transfer."));
      return;
    }
    auto document = noventra::CollectPortableData(browser, *categories);
    if (!document.has_value()) {
      Reply(operation, base::unexpected(document.error()));
      return;
    }
    document_ = std::move(*document);
    if (categories->FindBool("passwords").value_or(false)) {
      passwords_->ExportCSV(
          web_ui()->GetWebContents(),
          base::BindOnce(&PortableTransferHandler::ExportCollected,
                         weak_factory_.GetWeakPtr(), operation, *password));
    } else {
      ExportCollected(operation, *password, base::ok(std::string()));
    }
  } else {
    const auto* encoded = payload.FindString("data");
    if (!encoded || encoded->empty() || encoded->size() > kMaximumBase64Bytes) {
      Reply(operation,
            base::unexpected(
                "Choose a supported transfer file smaller than 33 MB."));
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Open, *encoded, *password),
        base::BindOnce(&PortableTransferHandler::OnOpened,
                       weak_factory_.GetWeakPtr(), operation));
  }
}

void PortableTransferHandler::ExportCollected(
    uint64_t operation,
    std::string password,
    base::expected<std::string, std::string> csv) {
  if (operation != operation_ || !callback_) {
    ClearString(password);
    if (csv.has_value()) {
      ClearString(*csv);
    }
    return;
  }
  if (!csv.has_value()) {
    ClearString(password);
    Reply(operation, base::unexpected(csv.error()));
    return;
  }
  if (document_.FindDict("categories")->FindBool("passwords").value_or(false)) {
    document_.Set("passwords_csv", std::move(*csv));
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&Seal, std::move(document_), std::move(password)),
      base::BindOnce(&PortableTransferHandler::Reply,
                     weak_factory_.GetWeakPtr(), operation));
}

void PortableTransferHandler::OnOpened(uint64_t operation, Result document) {
  if (operation != operation_ || !callback_) {
    if (document.has_value()) {
      ClearDocument(*document);
    }
    return;
  }
  if (!document.has_value()) {
    Reply(operation, base::unexpected(document.error()));
    return;
  }
  auto review = noventra::ValidatePortableData(*document);
  if (!review.has_value()) {
    ClearDocument(*document);
    Reply(operation, base::unexpected(review.error()));
    return;
  }
  if (document->FindDict("categories")->FindBool("passwords").value_or(false) &&
      !document->FindString("passwords_csv")) {
    ClearDocument(*document);
    Reply(operation,
          base::unexpected("The transfer is missing its password data."));
    return;
  }
  document_ = std::move(*document);
  review_ = std::move(*review);
  if (const auto* csv = document_.FindString("passwords_csv")) {
    passwords_->PrepareImport(
        *csv, base::BindOnce(&PortableTransferHandler::OnPasswordsPrepared,
                             weak_factory_.GetWeakPtr(), operation));
  } else {
    OnPasswordsPrepared(operation, base::DictValue());
  }
}

void PortableTransferHandler::OnPasswordsPrepared(uint64_t operation,
                                                  Result counts) {
  if (operation != operation_ || !callback_) {
    return;
  }
  if (!counts.has_value()) {
    Reply(operation, base::unexpected(counts.error()));
    return;
  }
  review_.Set("password_counts", std::move(*counts));
  plan_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  layout_revision_ =
      noventra::ReadOrMigrateLayout(*profile_->GetPrefs()).revision;
  review_.Set("plan_id", plan_id_);
  expiry_.Start(FROM_HERE, base::Minutes(10),
                base::BindOnce(&PortableTransferHandler::ClearPlan,
                               weak_factory_.GetWeakPtr()));
  Reply(operation, review_.Clone());
}

void PortableTransferHandler::OnDataImported(uint64_t operation,
                                             bool import_passwords,
                                             Result result) {
  if (operation != operation_ || !callback_ || !IsJavascriptAllowed()) {
    return;
  }
  if (!result.has_value()) {
    Reply(operation, base::unexpected(result.error()));
    return;
  }
  apply_result_ = std::move(*result);
  const auto* ledger = profile_->GetPrefs()
                           ->GetDict("noventra.browser.portable_imports")
                           .FindDict(*document_.FindString("transfer_id"));
  if (import_passwords && document_.FindString("passwords_csv") &&
      (!ledger || !ledger->FindBool("passwords").value_or(false))) {
    passwords_->CommitImport(
        base::BindOnce(&PortableTransferHandler::OnPasswordsImported,
                       weak_factory_.GetWeakPtr(), operation));
  } else {
    auto completed = std::move(apply_result_);
    ClearPlan();
    Reply(operation, std::move(completed));
  }
}

void PortableTransferHandler::OnPasswordsImported(uint64_t operation,
                                                  Result result) {
  if (operation != operation_ || !callback_) {
    return;
  }
  if (result.has_value() && result->FindInt("failed").value_or(0) == 0) {
    auto ledger = profile_->GetPrefs()
                      ->GetDict("noventra.browser.portable_imports")
                      .Clone();
    ledger.EnsureDict(*document_.FindString("transfer_id"))
        ->Set("passwords", true);
    profile_->GetPrefs()->SetDict("noventra.browser.portable_imports",
                                  std::move(ledger));
    apply_result_.Set("password_counts", std::move(*result));
  } else {
    // Other selected categories already succeeded; report partial application.
    apply_result_.Set("password_error",
                      result.has_value()
                          ? "Some passwords could not be imported. Existing "
                            "passwords were kept."
                          : result.error());
  }
  auto completed = std::move(apply_result_);
  ClearPlan();
  Reply(operation, std::move(completed));
}
}  // namespace settings
