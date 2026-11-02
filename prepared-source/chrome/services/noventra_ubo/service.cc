// Experimental original-uBlock host. SPDX-License-Identifier: GPL-3.0-or-later
#include "chrome/services/noventra_ubo/service.h"
#include "chrome/services/noventra_ubo/isolate_task_runner.h"
#include <chrono>
#include <cstring>
#include <map>
#include <ratio>
#include <optional>
#include <string_view>
#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/services/noventra_ubo/assets.h"
#include "gin/array_buffer.h"
#include "gin/public/isolate_holder.h"
#include "gin/v8_initializer.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "tools/v8_context_snapshot/buildflags.h"
#include "url/gurl.h"
#include "url/url_util.h"
#include "v8/include/v8.h"
namespace noventra_ubo {
namespace {
using namespace v8;
struct RuntimeState {
  std::map<std::string, std::string> assets;
  std::map<std::string, Global<Module>> modules;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
RuntimeState* State(Isolate* isolate) {
  return static_cast<RuntimeState*>(isolate->GetCurrentContext()->GetAlignedPointerFromEmbedderData(1, v8::kEmbedderDataTypeTagDefault));
}
Local<String> StringValue(Isolate* isolate, std::string_view text) {
  return String::NewFromUtf8(isolate, text.data(), NewStringType::kNormal,
                           static_cast<int>(text.size())).ToLocalChecked();
}
std::string Utf8(Isolate* isolate, Local<Value> value) {
  String::Utf8Value bytes(isolate, value);
  return *bytes ? std::string(*bytes, bytes.length()) : std::string();
}
void Error(Isolate* isolate, const std::string& message) {
  isolate->ThrowException(Exception::Error(StringValue(isolate, message)));
}
std::string Hash(std::string_view bytes) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash);
  std::string result;
  for (unsigned char byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
bool ReadAsset(Isolate* isolate, const std::string& name, std::string* data) {
  auto& assets = State(isolate)->assets;
  auto found = assets.find(name);
  if (found == assets.end()) { Error(isolate, "Asset not allowed or consumed: " + name); return false; }
  *data = std::move(found->second);
  assets.erase(found);  // One-shot immutable fixtures are not retained as engine RAM.
  return true;
}
Local<Uint8Array> Bytes(Isolate* isolate, std::string_view data) {
  auto buffer = ArrayBuffer::New(isolate, data.size());
  if (!data.empty()) std::memcpy(buffer->GetBackingStore()->Data(), data.data(), data.size());
  return Uint8Array::New(buffer, 0, data.size());
}
bool ViewBytes(Isolate* isolate, Local<Value> value, std::string_view* out) {
  if (!value->IsArrayBufferView()) { Error(isolate, "Expected byte view"); return false; }
  auto view = value.As<ArrayBufferView>();
  auto store = view->Buffer()->GetBackingStore();
  *out = std::string_view(static_cast<const char*>(store->Data()) + view->ByteOffset(), view->ByteLength());
  return true;
}
void Read(const FunctionCallbackInfo<Value>& args) {
  std::string data;
  if (ReadAsset(args.GetIsolate(), Utf8(args.GetIsolate(), args[0]), &data))
    args.GetReturnValue().Set(Bytes(args.GetIsolate(), data));
}
void Encode(const FunctionCallbackInfo<Value>& args) {
  auto isolate = args.GetIsolate();
  Local<String> value;
  if (!args[0]->ToString(isolate->GetCurrentContext()).ToLocal(&value)) return;
  std::string data(value->Utf8Length(isolate), '\0');
  value->WriteUtf8(isolate, data.data(), data.size(), String::WriteFlags::kReplaceInvalidUtf8);
  args.GetReturnValue().Set(Bytes(isolate, data));
}
void Decode(const FunctionCallbackInfo<Value>& args) {
  std::string_view data;
  if (!ViewBytes(args.GetIsolate(), args[0], &data)) return;
  // UTF-8 only. The bridge rejects unsupported labels/options explicitly.
  if (data.starts_with("\xEF\xBB\xBF")) data.remove_prefix(3);
  args.GetReturnValue().Set(StringValue(args.GetIsolate(), data));
}
void Sha(const FunctionCallbackInfo<Value>& args) {
  std::string_view data;
  if (ViewBytes(args.GetIsolate(), args[0], &data)) args.GetReturnValue().Set(StringValue(args.GetIsolate(), Hash(data)));
}
void Atob(const FunctionCallbackInfo<Value>& args) {
  std::string decoded;
  auto isolate = args.GetIsolate();
  if (!::base::Base64Decode(Utf8(isolate, args[0]), &decoded, ::base::Base64DecodePolicy::kForgiving)) {
    Error(isolate, "Invalid base64"); return;
  }
  args.GetReturnValue().Set(String::NewFromOneByte(isolate,
      reinterpret_cast<const uint8_t*>(decoded.data()), NewStringType::kNormal,
      static_cast<int>(decoded.size())).ToLocalChecked());
}
void FormDecode(const FunctionCallbackInfo<Value>& args) {
  std::string input = Utf8(args.GetIsolate(), args[0]);
  for (char& c : input) if (c == '+') c = ' ';
  args.GetReturnValue().Set(StringValue(args.GetIsolate(), url::DecodeUrlEscapeSequences(input, url::DecodeUrlMode::kUtf8)));
}
void ParseURL(const FunctionCallbackInfo<Value>& args) {
  auto isolate = args.GetIsolate();
  std::string input = Utf8(isolate, args[0]);
  GURL url = args.Length() > 1 && !args[1]->IsUndefined()
      ? GURL(Utf8(isolate, args[1])).Resolve(input) : GURL(input);
  if (!url.is_valid()) { Error(isolate, "Invalid URL"); return; }
  auto out = Object::New(isolate);
  auto set = [&](const char* key, std::string_view value) {
    out->Set(isolate->GetCurrentContext(), StringValue(isolate, key), StringValue(isolate, value)).Check();
  };
  set("href", url.spec()); set("protocol", std::string(url.scheme()) + ":");
  set("hostname", url.host()); set("search", url.has_query() ? "?" + std::string(url.query()) : "");
  set("hash", url.has_ref() ? "#" + std::string(url.ref()) : "");
  args.GetReturnValue().Set(out);
}
void SetHostname(const FunctionCallbackInfo<Value>& args) {
  auto isolate = args.GetIsolate();
  GURL original(Utf8(isolate, args[0]));
  std::string hostname = Utf8(isolate, args[1]);
  GURL::Replacements replacement;
  replacement.SetHostStr(hostname);
  GURL updated = original.ReplaceComponents(replacement);
  args.GetReturnValue().Set(StringValue(isolate, updated.is_valid() ? updated.spec() : original.spec()));
}
void Now(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(std::chrono::duration<double, std::ratio<1, 1000>>(std::chrono::steady_clock::now() - State(Isolate::GetCurrent())->start).count());
}
void Version(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(StringValue(args.GetIsolate(), V8::GetVersion()));
}
MaybeLocal<Module> CompileModule(Local<Context> context, const std::string& name);
MaybeLocal<Module> Resolve(Local<Context> context, Local<String> specifier,
                          Local<FixedArray> attributes, Local<Module> referrer) {
  auto isolate = Isolate::GetCurrent();
  if (attributes->Length() != 0) { Error(isolate, "Import attributes unsupported"); return {}; }
  std::string parent;
  for (auto& [name, module] : State(Isolate::GetCurrent())->modules) if (module.Get(isolate) == referrer) { parent = name; break; }
  std::string relative = Utf8(isolate, specifier);
  if (parent.empty() || (!relative.starts_with("./") && !relative.starts_with("../"))) {
    Error(isolate, "Only whitelisted relative modules are allowed"); return {};
  }
  GURL resolved = GURL("https://assets.invalid" + parent).Resolve(relative);
  if (!resolved.is_valid() || resolved.host() != "assets.invalid" || resolved.has_query() || resolved.has_ref()) {
    Error(isolate, "Invalid module path"); return {};
  }
  return CompileModule(context, std::string(resolved.path()));
}
MaybeLocal<Module> CompileModule(Local<Context> context, const std::string& name) {
  auto isolate = Isolate::GetCurrent();
  auto found = State(Isolate::GetCurrent())->modules.find(name);
  if (found != State(Isolate::GetCurrent())->modules.end()) return found->second.Get(isolate);
  std::string data;
  if (!ReadAsset(isolate, name, &data)) return {};
  ScriptOrigin origin(StringValue(isolate, name), 0, 0, false, -1, {}, false, false, true);
  ScriptCompiler::Source source(StringValue(isolate, data), origin);
  Local<Module> module;
  if (!ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) return {};
  State(Isolate::GetCurrent())->modules.emplace(name, Global<Module>(isolate, module));
  return module;
}
std::string ExceptionText(Isolate* isolate, TryCatch& catcher) {
  return catcher.HasCaught() ? Utf8(isolate, catcher.Exception()) : "Native runtime validation failed";
}
bool Execute(Local<Context> context, std::string_view code, Local<Value>* result) {
  Local<Script> script;
  return Script::Compile(context, StringValue(Isolate::GetCurrent(), code)).ToLocal(&script) && script->Run(context).ToLocal(result);
}
std::string Failure(const std::string& message) {
  base::DictValue error; error.Set("ok", false); error.Set("error", message);
  return base::WriteJson(error).value_or("{}");
}
}  // namespace

class Runtime {
 public:
  ~Runtime() {
    if (!holder_) return;
    task_runner_->Shutdown();
    v8::Locker lock(holder_->isolate());
    v8::Isolate::Scope scope(holder_->isolate());
    state_.modules.clear();
    response_.Reset();
    context_.Reset();
    holder_->isolate()->ContextDisposedNotification(v8::ContextDependants::kNoDependants);
  }
  std::string Initialize(std::vector<mojom::AssetPtr> assets) {
    if (holder_) return "Already initialized";
    if (assets.empty() || assets.size() > 80) return "Invalid asset count";
    size_t total = 0;
    for (auto& item : assets) {
      total += item->bytes.size();
      if (total > 96 * 1024 * 1024 || item->bytes.size() > 70 * 1024 * 1024) return "Assets too large";
      std::string bytes(item->bytes.begin(), item->bytes.end());
      const auto immutable = kImmutableAssets.find(item->name);
      if (immutable != kImmutableAssets.end()) {
        if (item->sha256 != immutable->second) return "Unpinned asset: " + item->name;
      } else if (item->name != "/lists.json" && item->name != "/requests.json") {
        return "Unknown asset: " + item->name;
      }
      if (Hash(bytes) != item->sha256) return "Asset hash mismatch: " + item->name;
      if (!state_.assets.emplace(item->name, std::move(bytes)).second) return "Duplicate asset: " + item->name;
    }
    if (!state_.assets.contains("/bridge.js") || !state_.assets.contains("/worker.mjs") || !state_.assets.contains("/psl.txt")) return "Missing startup asset";
    if (!gin::IsolateHolder::Initialized()) {
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
#if BUILDFLAG(USE_V8_CONTEXT_SNAPSHOT)
      gin::V8Initializer::LoadV8Snapshot(gin::V8SnapshotFileType::kWithAdditionalContext);
#else
      gin::V8Initializer::LoadV8Snapshot();
#endif
#endif
      gin::IsolateHolder::Initialize(gin::IsolateHolder::kNonStrictMode, gin::ArrayBufferAllocator::SharedInstance());
    }
    task_runner_ = base::MakeRefCounted<IsolateTaskRunner>(base::SingleThreadTaskRunner::GetCurrentDefault());
    holder_ = std::make_unique<gin::IsolateHolder>(task_runner_, gin::IsolateHolder::kUseLocker, gin::IsolateHolder::IsolateType::kUtility);
    auto isolate = holder_->isolate();
    v8::Locker lock(isolate); v8::Isolate::Scope scope(isolate); v8::HandleScope handles(isolate);
    auto global = v8::ObjectTemplate::New(isolate);
    for (auto [name, callback] : std::initializer_list<std::pair<const char*, v8::FunctionCallback>>{
        {"__asset", Read}, {"__encode", Encode}, {"__decode", Decode}, {"__sha", Sha},
        {"__atob", Atob}, {"__formDecode", FormDecode}, {"__parseURL", ParseURL}, {"__setHostname", SetHostname}, {"__now", Now}, {"__nativeVersion", Version}})
      global->Set(StringValue(isolate, name), v8::FunctionTemplate::New(isolate, callback));
    auto context = v8::Context::New(isolate, nullptr, global);
    context->SetAlignedPointerInEmbedderData(1, &state_, v8::kEmbedderDataTypeTagDefault);
    context_.Reset(isolate, context);
    v8::Context::Scope entered(context); v8::TryCatch catcher(isolate);
    std::string bridge; v8::Local<v8::Value> result; v8::Local<v8::Module> module;
    if (!ReadAsset(isolate, "/bridge.js", &bridge) || !Execute(context, bridge, &result) ||
        !CompileModule(context, "/worker.mjs").ToLocal(&module) ||
        !module->InstantiateModule(context, Resolve).FromMaybe(false) || !module->Evaluate(context).ToLocal(&result)) return ExceptionText(isolate, catcher);
    isolate->PerformMicrotaskCheckpoint();
    if (result->IsPromise() && result.As<v8::Promise>()->State() != v8::Promise::kFulfilled) return "Module evaluation failed";
    ready_ = true;
    return "";
  }
  std::string Begin(mojom::Operation op, const std::string& payload) {
    if (!ready_) return "Runtime not initialized";
    if (payload.size() > 2 * 1024 * 1024) return "RPC too large";
    auto isolate = holder_->isolate(); v8::Locker lock(isolate); v8::Isolate::Scope scope(isolate); v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate); v8::Context::Scope entered(context); v8::TryCatch catcher(isolate);
    const char* operation = nullptr;
    switch (op) {
      case mojom::Operation::kInitialize: operation = "initialize"; break;
      case mojom::Operation::kLiteral: operation = "literal"; break;
      case mojom::Operation::kLoad: operation = "load"; break;
      case mojom::Operation::kReplay: operation = "replay"; break;
      case mojom::Operation::kBenchmark: operation = "benchmark"; break;
    }
    v8::Local<v8::Value> data, dispatch, reply;
    if (!v8::JSON::Parse(context, StringValue(isolate, payload)).ToLocal(&data)) return ExceptionText(isolate, catcher);
    auto input = v8::Object::New(isolate);
    input->Set(context, StringValue(isolate, "id"), v8::Integer::New(isolate, ++request_id_)).Check();
    input->Set(context, StringValue(isolate, "operation"), StringValue(isolate, operation)).Check();
    input->Set(context, StringValue(isolate, "data"), data).Check();
    v8::Local<v8::Value> arg = input;
    if (!context->Global()->Get(context, StringValue(isolate, "__dispatch")).ToLocal(&dispatch) ||
        !dispatch.As<v8::Function>()->Call(context, context->Global(), 1, &arg).ToLocal(&reply) || !reply->IsPromise()) return ExceptionText(isolate, catcher);
    response_.Reset(isolate, reply.As<v8::Promise>());
    return "";
  }
  std::optional<std::string> Poll(double* next_ms) {
    auto isolate = holder_->isolate(); v8::Locker lock(isolate); v8::Isolate::Scope scope(isolate); v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate); v8::Context::Scope entered(context); v8::TryCatch catcher(isolate);
    v8::Local<v8::Value> result;
    if (!Execute(context, "__drainTimers()", &result)) return Failure(ExceptionText(isolate, catcher));
    isolate->PerformMicrotaskCheckpoint();
    if (!Execute(context, "__nextTimerDelay()", &result)) return Failure(ExceptionText(isolate, catcher));
    *next_ms = result->NumberValue(context).FromMaybe(-1);
    if (response_.IsEmpty()) return std::nullopt;
    auto promise = response_.Get(isolate);
    if (promise->State() == v8::Promise::kPending) { *next_ms = 5; return std::nullopt; }
    std::string reply;
    if (promise->State() == v8::Promise::kRejected) reply = Failure(Utf8(isolate, promise->Result()));
    else { v8::Local<v8::String> encoded;
      if (!v8::JSON::Stringify(context, promise->Result()).ToLocal(&encoded)) reply = Failure("Invalid response");
      else reply = Utf8(isolate, encoded); }
    response_.Reset();
    return reply;
  }
  bool ready() const { return ready_; }
 private:
  RuntimeState state_;
  scoped_refptr<IsolateTaskRunner> task_runner_;
  std::unique_ptr<gin::IsolateHolder> holder_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Promise> response_;
  int request_id_ = 0;
  bool ready_ = false;
};

UboService::UboService(mojo::PendingReceiver<mojom::UboService> receiver)
    : runtime_(std::make_unique<Runtime>()), receiver_(this, std::move(receiver)) {}
UboService::~UboService() = default;
void UboService::Initialize(std::vector<mojom::AssetPtr> assets, InitializeCallback callback) {
  std::move(callback).Run(runtime_->Initialize(std::move(assets)));
}
void UboService::Run(mojom::Operation operation, const std::string& payload, RunCallback callback) {
  if (pending_) { std::move(callback).Run(Failure("Overlapping operations")); return; }
  auto error = runtime_->Begin(operation, payload);
  if (!error.empty()) { std::move(callback).Run(Failure(error)); return; }
  pending_ = std::move(callback);
  Pump();
}
void UboService::Pump() {
  double delay = -1;
  auto reply = runtime_->Poll(&delay);
  if (reply && pending_) std::move(pending_).Run(*reply);
  if (delay >= 0) timer_.Start(FROM_HERE, base::Milliseconds(static_cast<int64_t>(delay)),
      base::BindOnce(&UboService::Pump, weak_factory_.GetWeakPtr()));
}
}  // namespace noventra_ubo
