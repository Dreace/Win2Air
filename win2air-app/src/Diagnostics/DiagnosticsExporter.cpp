#include "DiagnosticsExporter.h"
#include <appmodel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winternl.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include "../Settings/StorageIO.h"
namespace Win2Air {
using namespace winrt::Windows::Data::Json;
namespace {
std::string Key(std::string text) {
  std::string key;
  for (unsigned char c : text)
    if (std::isalnum(c)) key.push_back(static_cast<char>(std::tolower(c)));
  return key;
}
void Text(std::string const& text) {
  if (text.size() > 65536)
    throw std::runtime_error("Diagnostic text field exceeds its size bound");
  static const std::regex sensitive(
      R"((bearer\s+|basic\s+|-----BEGIN|(?:password|pin|token|secret|auth[ _-]?key|pairing[ _-]?code|material)\s*[:=]|[a-f0-9]{64}|[a-z0-9+/]{96}))",
      std::regex::icase);
  if (text.find('\0') != std::string::npos ||
      std::regex_search(text, sensitive))
    throw std::runtime_error("Diagnostic content contains sensitive data");
}
void Inspect(IJsonValue const& value, std::function<bool()> const& canceled,
             unsigned depth = 0) {
  if (canceled()) throw std::runtime_error("Diagnostic export canceled");
  if (depth > 128)
    throw std::runtime_error("Diagnostic nesting exceeds its bound");
  if (value.ValueType() == JsonValueType::Object) {
    static const std::string forbidden[] = {
        "password",   "pin",           "token",      "secret",
        "authkey",    "authorization", "credential", "pairingcode",
        "privatekey", "material",      "dump"};
    for (auto const& field : value.GetObject()) {
      auto key = Key(winrt::to_string(field.Key()));
      for (auto const& blocked : forbidden)
        if (key.find(blocked) != std::string::npos)
          throw std::runtime_error("Diagnostic field is sensitive");
      Inspect(field.Value(), canceled, depth + 1);
    }
  } else if (value.ValueType() == JsonValueType::Array) {
    for (auto const& item : value.GetArray())
      Inspect(item, canceled, depth + 1);
  } else if (value.ValueType() == JsonValueType::String)
    Text(winrt::to_string(value.GetString()));
}
void Put(JsonObject const& value, wchar_t const* key, std::string const& text) {
  value.Insert(key, JsonValue::CreateStringValue(winrt::to_hstring(text)));
}
void Number(JsonObject const& value, wchar_t const* key, double number) {
  value.Insert(key, JsonValue::CreateNumberValue(number));
}
JsonObject Metric(MetricValue const& metric) {
  if ((metric.availability == Availability::unavailable && metric.value) ||
      (metric.availability != Availability::unavailable && !metric.value))
    throw std::invalid_argument(
        "Metric availability and nullable value disagree");
  JsonObject value;
  value.Insert(L"value", metric.value
                             ? JsonValue::CreateNumberValue(*metric.value)
                             : JsonValue::CreateNullValue());
  Put(value, L"availability",
      metric.availability == Availability::measured    ? "measured"
      : metric.availability == Availability::estimated ? "estimated"
                                                       : "unavailable");
  Put(value, L"source", metric.source);
  Put(value, L"reason", metric.reason);
  Number(value, L"captured_at", double(metric.captured_at));
  return value;
}
JsonObject Sample(MetricsSnapshot const& sample) {
  JsonObject value;
  Put(value, L"receiver_id", sample.receiver_id);
  Number(value, L"generation", double(sample.generation));
  Number(value, L"sample_time", double(sample.sample_time));
  Number(value, L"uptime_ns", double(sample.uptime));
  Number(value, L"app_captured_frames", double(sample.app_captured_frames));
  Number(value, L"app_submitted_frames", double(sample.app_submitted_frames));
  JsonObject format;
  Number(format, L"sample_rate", sample.format.rate);
  Number(format, L"channels", sample.format.channels);
  Number(format, L"bits", sample.format.bits);
  value.Insert(L"format", format);
  value.Insert(L"gap", JsonValue::CreateBooleanValue(sample.gap));
  for (auto const& pair :
       {std::pair{L"bitrate", &sample.bitrate},
        {L"sent_packets", &sample.sent_packets},
        {L"sent_bytes", &sample.sent_bytes},
        {L"resend_requests", &sample.resend_requests},
        {L"resend_success", &sample.resend_success},
        {L"resend_expired", &sample.resend_expired},
        {L"pending", &sample.pending},
        {L"queue_fill", &sample.queue_fill},
        {L"underruns", &sample.underruns},
        {L"capture_discontinuities", &sample.capture_discontinuities},
        {L"target_latency", &sample.target_latency},
        {L"output_latency", &sample.output_latency},
        {L"clock_lock", &sample.clock_lock},
        {L"clock_offset", &sample.clock_offset},
        {L"clock_drift", &sample.clock_drift},
        {L"reconnect_count", &sample.reconnect_count},
        {L"captured_frames", &sample.captured_frames},
        {L"submitted_frames", &sample.submitted_frames},
        {L"nonzero_frames", &sample.nonzero_frames}})
    value.Insert(pair.first, Metric(*pair.second));
  if (sample.last_error) {
    JsonObject error;
    Number(error, L"code", sample.last_error->sender_code);
    Number(error, L"platform_code", sample.last_error->platform_code);
    Number(error, L"peer_code", sample.last_error->peer_code);
    Put(error, L"event", "command-failed");
    value.Insert(L"last_error", error);
  }
  return value;
}
uint32_t Crc(std::string const& text, std::function<bool()> const& canceled) {
  uint32_t crc = UINT32_MAX;
  size_t progress{};
  for (unsigned char c : text) {
    if (!(progress++ & 65535) && canceled())
      throw std::runtime_error("Diagnostic export canceled");
    crc ^= c;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
void Word(std::ostream& stream, uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    stream.put(static_cast<char>(value >> (i * 8)));
}
void Canceled(std::function<bool()> const& canceled) {
  if (canceled()) throw std::runtime_error("Diagnostic export canceled");
}
}  // namespace
void CheckDiagnosticText(std::string const& text, bool lines,
                         std::function<bool()> canceled) {
  Canceled(canceled);
  if (!lines)
    Inspect(JsonValue::Parse(winrt::to_hstring(text)), canceled);
  else {
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
      if (!line.empty()) {
        if (line.size() > 65536)
          throw std::runtime_error("Diagnostic log line exceeds its bound");
        Canceled(canceled);
        Inspect(JsonValue::Parse(winrt::to_hstring(line)), canceled);
      }
  }
}
DiagnosticFiles BuildDiagnostics(PlaybackSnapshot const& state,
                                 std::string logs,
                                 std::function<bool()> canceled) {
  Canceled(canceled);
  DiagnosticFiles files;
  files["app.log"] = std::move(logs);
  JsonObject system;
  Put(system, L"architecture", "x64");
  Put(system, L"os", "Windows");
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  using Query = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
  auto query = reinterpret_cast<Query>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (!query || query(&version) != 0)
    throw std::runtime_error("Read system version for diagnostics failed");
  Number(system, L"major", version.dwMajorVersion);
  Number(system, L"minor", version.dwMinorVersion);
  Number(system, L"build", version.dwBuildNumber);
  files["system.json"] = winrt::to_string(system.Stringify());
  JsonArray receivers;
  for (auto const& record : state.receivers) {
    Canceled(canceled);
    JsonObject value;
    Put(value, L"receiver_id", record.receiver_id);
    Put(value, L"name", record.name);
    Put(value, L"model", record.model);
    Number(value, L"volume", record.volume);
    Number(value, L"offset_ms", record.offset_ms);
    value.Insert(L"muted", JsonValue::CreateBooleanValue(record.muted));
    receivers.Append(value);
  }
  files["receivers.json"] = winrt::to_string(receivers.Stringify());
  JsonObject configuration;
  auto const& s = state.preferences;
  Put(configuration, L"selected_receiver_id", s.selected_receiver_id);
  Put(configuration, L"latency_mode", s.latency_mode);
  Put(configuration, L"language", s.language);
  Put(configuration, L"theme", s.theme);
  Put(configuration, L"source_endpoint", s.audio_source.endpoint_id);
  Number(configuration, L"custom_ms", s.custom_ms);
  Number(configuration, L"silence_timeout", s.silence_timeout);
  for (auto pair :
       {std::pair{L"local_playback", s.local_playback},
        {L"auto_reconnect", s.auto_reconnect},
        {L"follow_default_source", s.audio_source.follow_system_default},
        {L"restore_previous", s.restore_previous},
        {L"close_to_tray", s.close_to_tray},
        {L"startup", s.startup},
        {L"minimized", s.minimized}})
    configuration.Insert(pair.first,
                         JsonValue::CreateBooleanValue(pair.second));
  files["configuration.json"] = winrt::to_string(configuration.Stringify());
  JsonObject metrics;
  JsonArray samples, gaps;
  if (state.metrics_history) {
    for (auto const& sample : state.metrics_history->snapshots) {
      Canceled(canceled);
      if (!sample)
        throw std::invalid_argument("Diagnostic history sample is missing");
      samples.Append(Sample(*sample));
    }
    for (auto gap : state.metrics_history->gaps)
      gaps.Append(JsonValue::CreateNumberValue(double(gap)));
  }
  metrics.Insert(L"samples", samples);
  metrics.Insert(L"gaps", gaps);
  JsonObject source;
  Put(source, L"endpoint_id", state.source.endpoint_id);
  Put(source, L"availability", state.source.availability);
  Number(source, L"sample_rate", state.source.mix_format.rate);
  Number(source, L"channels", state.source.mix_format.channels);
  Number(source, L"bits", state.source.mix_format.bits);
  metrics.Insert(L"capture_source", source);
  Put(metrics, L"health", state.health.level);
  Number(metrics, L"health_policy_version", state.health.policy_version);
  Number(metrics, L"health_window_seconds", state.health.window);
  metrics.Insert(
      L"safe_logs_unavailable",
      JsonValue::CreateBooleanValue(!state.diagnostics_error.empty()));
  metrics.Insert(L"protocol_snapshot_unavailable",
                 JsonValue::CreateBooleanValue(!state.metrics_error.empty()));
  JsonArray reasons;
  for (auto const& reason : state.health.reason_codes)
    reasons.Append(JsonValue::CreateStringValue(winrt::to_hstring(reason)));
  metrics.Insert(L"health_reasons", reasons);
  files["metrics.json"] = winrt::to_string(metrics.Stringify());
  JsonObject manifest;
  Put(manifest, L"format", "Win2Air diagnostics");
  Put(manifest, L"source", "typed-public-state-and-safe-events");
  Number(manifest, L"schema_version", 1);
  Number(manifest, L"created_at_epoch_ms",
         double(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()));
  UINT32 length{};
  auto package_status = GetCurrentPackageId(&length, nullptr);
  if (package_status == APPMODEL_ERROR_NO_PACKAGE) {
    manifest.Insert(L"source_version", JsonValue::CreateNullValue());
    Put(manifest, L"source_version_reason", "unpackaged-executable");
  } else {
    if (package_status != ERROR_INSUFFICIENT_BUFFER)
      throw std::runtime_error("Read diagnostic package identity failed");
    std::vector<uint8_t> bytes(length);
    if (GetCurrentPackageId(&length, bytes.data()) != ERROR_SUCCESS)
      throw std::runtime_error("Read diagnostic package version failed");
    auto package_version =
        reinterpret_cast<PACKAGE_ID const*>(bytes.data())->version;
    Put(manifest, L"source_version",
        std::to_string(package_version.Major) + "." +
            std::to_string(package_version.Minor) + "." +
            std::to_string(package_version.Build) + "." +
            std::to_string(package_version.Revision));
  }
  JsonArray names;
  for (auto const& file : files)
    names.Append(JsonValue::CreateStringValue(winrt::to_hstring(file.first)));
  names.Append(JsonValue::CreateStringValue(L"manifest.json"));
  manifest.Insert(L"files", names);
  files["manifest.json"] = winrt::to_string(manifest.Stringify());
  return files;
}
std::string DiagnosticsExporter::ReadLogs(std::filesystem::path const& root,
                                          std::function<bool()> canceled) {
  std::string text;
  for (unsigned index = 0; index < 5; ++index) {
    Canceled(canceled);
    auto path =
        root / (index ? "app." + std::to_string(index) + ".log" : "app.log");
    if (!std::filesystem::exists(path)) continue;
    if (std::filesystem::is_symlink(path))
      throw std::runtime_error("Log link cannot be exported");
    auto bytes = StorageIO::Read(path, 10 * 1024 * 1024);
    text.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
  }
  CheckDiagnosticText(text, true, canceled);
  return text;
}
void DiagnosticsExporter::Export(std::filesystem::path const& path,
                                 DiagnosticFiles const& files,
                                 std::function<bool()> canceled) {
  static const std::set<std::string> allowed = {
      "app.log",      "system.json",        "receivers.json",
      "metrics.json", "configuration.json", "manifest.json"};
  if (!path.is_absolute() || path.extension() != L".zip" ||
      files.size() != allowed.size() || std::filesystem::exists(path))
    throw std::invalid_argument(
        "Export requires a new absolute ZIP path and the complete whitelist");
  uint64_t total = 22;
  for (auto const& [name, text] : files) {
    if (!allowed.contains(name))
      throw std::invalid_argument("Diagnostic file is outside the whitelist");
    if (text.size() > kLimit || total > kLimit - text.size())
      throw std::runtime_error("Diagnostic archive exceeds its size limit");
    total += text.size() + 76 + name.size() * 2;
    if (total > kLimit)
      throw std::runtime_error("Diagnostic archive exceeds its size limit");
    Canceled(canceled);
    CheckDiagnosticText(text, name == "app.log", canceled);
  }
  GUID id{};
  winrt::check_hresult(CoCreateGuid(&id));
  wchar_t uuid[40];
  StringFromGUID2(id, uuid, 40);
  auto temporary = path.parent_path() / (std::wstring(uuid) + L".partial");
  struct Entry {
    std::string name;
    uint32_t crc, size, offset;
  };
  std::vector<Entry> entries;
  try {
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(temporary, std::ios::binary | std::ios::trunc);
    for (auto const& [name, text] : files) {
      Canceled(canceled);
      Entry entry{name, Crc(text, canceled), static_cast<uint32_t>(text.size()),
                  static_cast<uint32_t>(output.tellp())};
      Word(output, 0x04034b50, 4);
      Word(output, 20, 2);
      Word(output, 0x800, 2);
      Word(output, 0, 2);
      Word(output, 0, 2);
      Word(output, 0x21, 2);
      Word(output, entry.crc, 4);
      Word(output, entry.size, 4);
      Word(output, entry.size, 4);
      Word(output, static_cast<uint32_t>(name.size()), 2);
      Word(output, 0, 2);
      output << name;
      for (size_t offset = 0; offset < text.size(); offset += 65536) {
        Canceled(canceled);
        output.write(text.data() + offset,
                     std::min<size_t>(65536, text.size() - offset));
      }
      entries.push_back(std::move(entry));
    }
    auto directory = static_cast<uint32_t>(output.tellp());
    for (auto const& entry : entries) {
      Word(output, 0x02014b50, 4);
      Word(output, 20, 2);
      Word(output, 20, 2);
      Word(output, 0x800, 2);
      Word(output, 0, 2);
      Word(output, 0, 2);
      Word(output, 0x21, 2);
      Word(output, entry.crc, 4);
      Word(output, entry.size, 4);
      Word(output, entry.size, 4);
      Word(output, static_cast<uint32_t>(entry.name.size()), 2);
      Word(output, 0, 2);
      Word(output, 0, 2);
      Word(output, 0, 2);
      Word(output, 0, 2);
      Word(output, 0, 4);
      Word(output, entry.offset, 4);
      output << entry.name;
    }
    auto size = static_cast<uint32_t>(output.tellp()) - directory;
    Word(output, 0x06054b50, 4);
    Word(output, 0, 2);
    Word(output, 0, 2);
    Word(output, static_cast<uint32_t>(entries.size()), 2);
    Word(output, static_cast<uint32_t>(entries.size()), 2);
    Word(output, size, 4);
    Word(output, directory, 4);
    Word(output, 0, 2);
    output.flush();
    output.close();
    Canceled(canceled);
    if (std::filesystem::file_size(temporary) != total)
      throw std::runtime_error("Diagnostic ZIP did not complete");
    StorageIO::Check(
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH),
        "Commit diagnostic ZIP");
  } catch (...) {
    std::error_code cleanup;
    std::filesystem::remove(temporary, cleanup);
    throw;
  }
}
}  // namespace Win2Air
