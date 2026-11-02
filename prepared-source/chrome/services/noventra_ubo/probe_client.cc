// Local opt-in qualification client, never a web-page/extension API.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "chrome/services/noventra_ubo/probe_client.h"
#include <string_view>
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/process.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/service_process_host.h"
namespace noventra_ubo {
struct ProbeClient::Inputs {
  base::DictValue configuration;
  std::vector<mojom::AssetPtr> assets;
  std::string error;
};
ProbeClient::ProbeClient() = default;
ProbeClient::~ProbeClient() = default;
void ProbeClient::PostBrowserStart() {
  const auto* command = base::CommandLine::ForCurrentProcess();
  if (!command->HasSwitch("noventra-ubo-probe")) return;
  auto config = command->GetSwitchValuePath("noventra-ubo-probe");
  if (!config.IsAbsolute()) return;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
      base::BindOnce([](base::FilePath path) {
        auto input = std::make_unique<Inputs>();
        std::string text;
        if (!base::ReadFileToStringWithMaxSize(path, &text, 4 * 1024 * 1024)) { input->error = "Configuration read failed"; return input; }
        auto value = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
        if (!value || !value->is_dict()) { input->error = "Invalid configuration"; return input; }
        input->configuration = std::move(*value).TakeDict();
        auto* assets = input->configuration.FindList("assets");
        if (!assets || assets->empty() || assets->size() > 80) { input->error = "Invalid asset descriptors"; return input; }
        size_t total = 0;
        for (const auto& entry : *assets) {
          if (!entry.is_dict()) { input->error = "Invalid asset descriptor"; return input; }
          const auto& d = entry.GetDict();
          auto* name = d.FindString("name"); auto* file = d.FindString("path"); auto* sha = d.FindString("sha256");
          if (!name || !file || !sha) { input->error = "Invalid asset fields"; return input; }
          auto asset = mojom::Asset::New();
          asset->name = *name; asset->sha256 = *sha;
          auto asset_path = base::FilePath::FromUTF8Unsafe(*file);
          std::string bytes;
          if (!asset_path.IsAbsolute() || !base::ReadFileToStringWithMaxSize(asset_path, &bytes, 70 * 1024 * 1024)) { input->error = "Asset read failed"; return input; }
          total += bytes.size();
          if (total > 96 * 1024 * 1024) { input->error = "Asset budget exceeded"; return input; }
          asset->bytes.assign(bytes.begin(), bytes.end());
          input->assets.push_back(std::move(asset));
        }
        return input;
      }, config), base::BindOnce(&ProbeClient::Loaded, weak_factory_.GetWeakPtr()));
}
void ProbeClient::Loaded(std::unique_ptr<Inputs> inputs) {
  const auto* output = inputs->configuration.FindString("output");
  if (!output) return;
  output_ = base::FilePath::FromUTF8Unsafe(*output);
  if (!output_.IsAbsolute() || base::PathExists(output_)) { output_.clear(); return; }
  receipt_.Set("passed", false); receipt_.Set("done", false); receipt_.Set("replies", base::ListValue());
  if (!inputs->error.empty()) { Finish(inputs->error); return; }
  auto* operations = inputs->configuration.FindList("operations");
  if (!operations || operations->empty() || operations->size() > 100) { Finish("Invalid operations"); return; }
  operations_ = std::move(*operations);
  pipeline_ = inputs->configuration.FindBool("pipeline_operations").value_or(false);
  hold_ms_ = inputs->configuration.FindInt("hold_after_ms").value_or(0);
  if (hold_ms_ < 0 || hold_ms_ > 120000) { Finish("Invalid hold duration"); return; }
  content::ServiceProcessHost::Launch(remote_.BindNewPipeAndPassReceiver(),
      content::ServiceProcessHost::Options().WithDisplayName("Noventra uBlock qualification")
          .WithProcessCallback(base::BindOnce([](base::WeakPtr<ProbeClient> self, const base::Process& process) {
            if (self) { self->receipt_.Set("pid", static_cast<int>(process.Pid())); self->Write(); }
          }, weak_factory_.GetWeakPtr())).Pass());
  remote_.set_disconnect_handler(base::BindOnce(&ProbeClient::Finish, weak_factory_.GetWeakPtr(), "Service disconnected"));
  remote_->Initialize(std::move(inputs->assets), base::BindOnce([](base::WeakPtr<ProbeClient> self, const std::string& error) {
    if (!self) return;
    if (!error.empty()) self->Finish(error);
    else self->Next();
  }, weak_factory_.GetWeakPtr()));
}
void ProbeClient::Next() {
  if (receipt_.FindBool("done").value_or(false)) return;
  if (next_ == operations_.size()) {
    if (outstanding_ != 0) return;
    receipt_.Set("passed", !failed_); Finish(failed_ ? "Service rejected operation" : ""); return;
  }
  const auto& item = operations_[next_++];
  if (!item.is_dict()) { Finish("Invalid operation descriptor"); return; }
  const auto& d = item.GetDict(); auto* name = d.FindString("operation"); auto* data = d.Find("data");
  if (!name || !data) { Finish("Invalid operation fields"); return; }
  mojom::Operation op;
  if (*name == "initialize") op = mojom::Operation::kInitialize;
  else if (*name == "literal") op = mojom::Operation::kLiteral;
  else if (*name == "load") op = mojom::Operation::kLoad;
  else if (*name == "replay") op = mojom::Operation::kReplay;
  else if (*name == "benchmark") op = mojom::Operation::kBenchmark;
  else { Finish("Unknown operation"); return; }
  auto json = base::WriteJson(*data);
  if (!json || json->size() > 2 * 1024 * 1024) { Finish("Invalid operation payload"); return; }
  ++outstanding_;
  remote_->Run(op, *json, base::BindOnce(&ProbeClient::Replied, weak_factory_.GetWeakPtr()));
  if (pipeline_) Next();
}
void ProbeClient::Replied(const std::string& reply) {
  if (receipt_.FindBool("done").value_or(false)) return;
  --outstanding_;
  auto value = base::JSONReader::Read(reply, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) { Finish("Invalid service reply"); return; }
  bool ok = value->GetDict().FindBool("ok").value_or(false);
  const auto* result = value->GetDict().FindDict("result");
  if (result && result->FindBool("passed") == false) ok = false;
  receipt_.FindList("replies")->Append(std::move(*value));
  if (!ok) {
    failed_ = true;
    if (!pipeline_) { Finish("Service rejected operation"); return; }
  }
  Write(); Next();
}
void ProbeClient::Finish(const std::string& error) {
  const bool was_done = receipt_.FindBool("done").value_or(false);
  if (was_done && error.empty()) return;
  receipt_.Set("done", true);
  if (!error.empty()) { failed_ = true; receipt_.Set("passed", false); receipt_.Set("error", error); }
  Write();
  if (was_done) { hold_.Stop(); remote_.reset(); return; }
  if (remote_.is_bound()) hold_.Start(FROM_HERE, base::Milliseconds(hold_ms_),
      base::BindOnce([](base::WeakPtr<ProbeClient> self) { if (self) self->remote_.reset(); }, weak_factory_.GetWeakPtr()));
}
void ProbeClient::Write() {
  if (output_.empty()) return;
  auto json = base::WriteJson(receipt_);
  if (!json) return;
  auto temporary = output_.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::WriteFile(temporary, *json) || !base::ReplaceFile(temporary, output_, nullptr)) {
    receipt_.Set("passed", false); receipt_.Set("error", "Receipt write failed");
  }
}
}
