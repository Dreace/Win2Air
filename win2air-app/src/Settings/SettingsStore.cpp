#include "SettingsStore.h"
#include "StorageIO.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace Win2Air {
using namespace winrt::Windows::Data::Json;
namespace {
void Need(bool value, char const* reason) {
  if (!value) throw std::runtime_error(reason);
}
void Fields(JsonObject const& value,
            std::initializer_list<wchar_t const*> fields,
            std::initializer_list<wchar_t const*> optional = {}) {
  auto expected = fields.size();
  for (auto field : optional)
    if (value.HasKey(field)) ++expected;
  Need(value.Size() == expected,
       "Settings object contains missing or unknown fields");
  for (auto field : fields) Need(value.HasKey(field), "Settings field missing");
}
uint64_t Integer(JsonObject const& value, wchar_t const* key,
                 uint64_t maximum) {
  double number = value.GetNamedNumber(key);
  Need(std::isfinite(number) && number >= 0 && std::floor(number) == number &&
           number <= static_cast<double>(maximum),
       "Settings integer outside range");
  return static_cast<uint64_t>(number);
}
int SignedInteger(JsonObject const& value, wchar_t const* key, int minimum,
                  int maximum) {
  double number = value.GetNamedNumber(key);
  Need(std::isfinite(number) && std::floor(number) == number &&
           number >= minimum && number <= maximum,
       "Settings signed integer outside range");
  return static_cast<int>(number);
}
std::string String(JsonObject const& value, wchar_t const* key) {
  auto text = winrt::to_string(value.GetNamedString(key));
  Need(text.size() <= 4096 && text.find('\0') == std::string::npos,
       "Settings string invalid");
  return text;
}
void ValidText(std::string const& text) {
  Need(text.size() <= 4096 && text.find('\0') == std::string::npos,
       "Settings string invalid");
  if (!text.empty())
    Need(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), nullptr, 0) > 0,
         "Settings string must be valid UTF-8");
}
void Put(JsonObject const& object, wchar_t const* key,
         std::string const& value) {
  object.Insert(key, JsonValue::CreateStringValue(winrt::to_hstring(value)));
}
void Put(JsonObject const& object, wchar_t const* key, double value) {
  object.Insert(key, JsonValue::CreateNumberValue(value));
}
void Put(JsonObject const& object, wchar_t const* key, bool value) {
  object.Insert(key, JsonValue::CreateBooleanValue(value));
}
JsonObject ReadJson(std::filesystem::path const& path) {
  auto bytes = StorageIO::Read(path);
  return JsonObject::Parse(
      winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
}
std::string SerializedJson(JsonObject const& value) {
  auto text = winrt::to_string(value.Stringify());
  Need(text.size() <= 1024 * 1024, "Settings document exceeds load size bound");
  return text;
}
JsonObject EndpointJson(ReceiverEndpoint const& endpoint) {
  JsonObject value;
  Put(value, L"host", endpoint.host);
  Put(value, L"resolved_address", endpoint.resolved_address);
  Put(value, L"port", double(endpoint.port));
  Put(value, L"interface_index", double(endpoint.interface_index));
  Put(value, L"scope_id", double(endpoint.scope_id));
  Put(value, L"discovery_epoch", double(endpoint.discovery_epoch));
  Put(value, L"last_seen", double(endpoint.last_seen));
  return value;
}
ReceiverEndpoint ParseEndpoint(JsonObject const& value) {
  Fields(value, {L"host", L"resolved_address", L"port", L"interface_index",
                 L"scope_id", L"discovery_epoch", L"last_seen"});
  ReceiverEndpoint endpoint;
  endpoint.host = String(value, L"host");
  endpoint.resolved_address = String(value, L"resolved_address");
  endpoint.port = unsigned(Integer(value, L"port", 65535));
  endpoint.interface_index =
      unsigned(Integer(value, L"interface_index", UINT32_MAX));
  endpoint.scope_id = unsigned(Integer(value, L"scope_id", UINT32_MAX));
  endpoint.discovery_epoch =
      Integer(value, L"discovery_epoch", 9007199254740991ULL);
  endpoint.last_seen = Integer(value, L"last_seen", 9007199254740991ULL);
  return endpoint;
}
}  // namespace
bool ValidReceiverIdentity(std::string const& value) noexcept {
  if (value.size() != 17) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i % 3 == 2) {
      if (value[i] != ':') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'A' && value[i] <= 'F') ||
                 (value[i] >= 'a' && value[i] <= 'f')))
      return false;
  }
  return true;
}
void ValidateSettings(SettingsSnapshot const& value) {
  auto const& s = value.settings;
  Need(s.schema_version == 1, "Unsupported settings schema version");
  ValidText(s.audio_source.endpoint_id);
  ValidText(s.selected_receiver_id);
  Need(s.revision <= 9007199254740991ULL,
       "Settings revision outside exact JSON range");
  Need(s.audio_source.role == "multimedia", "Unsupported audio source role");
  Need(s.audio_source.follow_system_default
           ? s.audio_source.endpoint_id.empty()
           : !s.audio_source.endpoint_id.empty(),
       "Audio source mode and endpoint disagree");
  Need(s.latency_mode == "ultra" || s.latency_mode == "adaptive" ||
           s.latency_mode == "low" || s.latency_mode == "balanced" ||
           s.latency_mode == "stable" || s.latency_mode == "custom",
       "Unknown latency mode");
  Need(s.custom_ms >= SENDER_MIN_PRESENTATION_MS &&
           s.custom_ms <= SENDER_MAX_PRESENTATION_MS,
       "Custom latency outside range");
  Need(s.silence_timeout == 0 || s.silence_timeout == 10 ||
           s.silence_timeout == 30 || s.silence_timeout == 60,
       "Unknown silence timeout");
  Need(s.standby_disconnect_seconds == 0 ||
           s.standby_disconnect_seconds == 60 ||
           s.standby_disconnect_seconds == 300 ||
           s.standby_disconnect_seconds == 900 ||
           s.standby_disconnect_seconds == 1800,
       "Unknown standby disconnect timeout");
  Need(s.theme == "system" || s.theme == "light" || s.theme == "dark",
       "Unknown theme");
  Need(s.language == "system" || s.language == "zh-CN" || s.language == "en-US",
       "Unknown language");
  Need(s.log_level.empty() || s.log_level == "trace" ||
           s.log_level == "debug" || s.log_level == "info" ||
           s.log_level == "warning" || s.log_level == "error",
       "Unknown log level");
  Need(s.window_placement.width > 0 && s.window_placement.width <= 32768 &&
           s.window_placement.height > 0 && s.window_placement.height <= 32768,
       "Invalid saved window extent");
  for (auto chart : {s.statistics.primary, s.statistics.secondary})
    Need(chart.metric <= 4 && (chart.seconds == 60 || chart.seconds == 600 ||
                               chart.seconds == 3600),
         "Invalid statistics chart preference");
  Need(value.receivers.size() <= 256, "Saved receiver limit exceeded");
  std::set<std::string> identities;
  auto endpoint = [](ReceiverEndpoint const& e) {
    ValidText(e.host);
    ValidText(e.resolved_address);
    Need(e.port >= 1 && e.port <= 65535 && !e.host.empty(),
         "Invalid receiver endpoint");
    Need(e.discovery_epoch <= 9007199254740991ULL &&
             e.last_seen <= 9007199254740991ULL,
         "Endpoint timestamp outside exact JSON range");
  };
  for (auto const& r : value.receivers) {
    ValidText(r.name);
    ValidText(r.model);
    ValidText(r.manufacturer);
    ValidText(r.firmware_version);
    if (r.latency_profile) {
      ValidText(r.latency_profile->firmware);
      ValidText(r.latency_profile->network_context);
      Need(!r.latency_profile->firmware.empty() &&
               !r.latency_profile->network_context.empty() &&
               r.latency_profile->budget_ms >= SENDER_MIN_PRESENTATION_MS &&
               r.latency_profile->budget_ms <= SENDER_MAX_PRESENTATION_MS,
           "Latency profile requires firmware, network context and a bounded "
           "budget");
    }
    auto normalized = r.receiver_id;
    for (auto& c : normalized)
      if (c >= 'a' && c <= 'f') c = char(c - 'a' + 'A');
    Need(ValidReceiverIdentity(r.receiver_id) &&
             identities.insert(normalized).second,
         "Invalid or duplicate receiver identity");
    Need(std::isfinite(r.volume) && r.volume >= 0 && r.volume <= 1 &&
             r.offset_ms >= -1000 && r.offset_ms <= 1000,
         "Receiver preference outside range");
    if (r.manual_endpoint) endpoint(*r.manual_endpoint);
    Need(r.discovered_endpoints.size() <= 256,
         "Receiver endpoint limit exceeded");
    for (auto const& e : r.discovered_endpoints) endpoint(e);
  }
  auto selected = s.selected_receiver_id;
  for (auto& c : selected)
    if (c >= 'a' && c <= 'f') c = char(c - 'a' + 'A');
  Need(selected.empty() || identities.contains(selected),
       "Selected receiver is absent from saved records");
}
SettingsStore::SettingsStore(std::filesystem::path root)
    : root_(std::move(root)) {}
void SettingsStore::Save(SettingsSnapshot const& value) {
  std::scoped_lock lock(writer_, StorageIO::WriterGate);
  ValidateSettings(value);
  auto const& s = value.settings;
  Need(s.revision > 0,
       "A persisted settings transaction requires a positive revision");
  if (std::filesystem::exists(root_ / L"commit.json")) {
    auto previous = ReadJson(root_ / L"commit.json");
    Fields(previous, {L"schema_version", L"revision"});
    Need(Integer(previous, L"schema_version", UINT32_MAX) == 1,
         "Unsupported previous commit schema");
    Need(s.revision > Integer(previous, L"revision", 9007199254740991ULL),
         "Settings revision must increase for each transaction");
  }
  StorageIO::PrivateDirectory(root_);
  JsonObject settings;
  Put(settings, L"schema_version", double(s.schema_version));
  Put(settings, L"revision", double(s.revision));
  JsonObject source;
  Put(source, L"follow_system_default", s.audio_source.follow_system_default);
  Put(source, L"endpoint_id", s.audio_source.endpoint_id);
  Put(source, L"role", s.audio_source.role);
  settings.Insert(L"audio_source", source);
  Put(settings, L"selected_receiver_id", s.selected_receiver_id);
  Put(settings, L"local_playback", s.local_playback);
  Put(settings, L"auto_reconnect", s.auto_reconnect);
  Put(settings, L"latency_mode", s.latency_mode);
  Put(settings, L"custom_ms", double(s.custom_ms));
  Put(settings, L"silence_timeout", double(s.silence_timeout));
  Put(settings, L"standby_disconnect_seconds",
      double(s.standby_disconnect_seconds));
  Put(settings, L"startup", s.startup);
  Put(settings, L"minimized", s.minimized);
  Put(settings, L"restore_previous", s.restore_previous);
  Put(settings, L"close_to_tray", s.close_to_tray);
  Put(settings, L"receiver_media_control", s.receiver_media_control);
  Put(settings, L"log_level", s.log_level);
  Put(settings, L"theme", s.theme);
  Put(settings, L"language", s.language);
  JsonObject statistics;
  auto chart_json = [](AppSettings::ChartPreference const& chart) {
    JsonObject object;
    Put(object, L"metric", double(chart.metric));
    Put(object, L"seconds", double(chart.seconds));
    return object;
  };
  statistics.Insert(L"primary", chart_json(s.statistics.primary));
  statistics.Insert(L"secondary", chart_json(s.statistics.secondary));
  settings.Insert(L"statistics", statistics);
  JsonObject placement;
  Put(placement, L"x", double(s.window_placement.x));
  Put(placement, L"y", double(s.window_placement.y));
  Put(placement, L"width", double(s.window_placement.width));
  Put(placement, L"height", double(s.window_placement.height));
  Put(placement, L"saved", s.window_placement.saved);
  settings.Insert(L"window_placement", placement);
  JsonObject receivers;
  Put(receivers, L"schema_version", 1.0);
  Put(receivers, L"revision", double(s.revision));
  JsonArray records;
  for (auto const& r : value.receivers) {
    JsonObject record;
    Put(record, L"receiver_id", r.receiver_id);
    Put(record, L"identity_verified", r.identity_verified);
    Put(record, L"name", r.name);
    Put(record, L"model", r.model);
    Put(record, L"manufacturer", r.manufacturer);
    Put(record, L"firmware_version", r.firmware_version);
    if (r.latency_profile) {
      JsonObject profile;
      Put(profile, L"firmware", r.latency_profile->firmware);
      Put(profile, L"network_context", r.latency_profile->network_context);
      Put(profile, L"budget_ms", double(r.latency_profile->budget_ms));
      record.Insert(L"latency_profile", profile);
    } else {
      record.Insert(L"latency_profile", JsonValue::CreateNullValue());
    }
    Put(record, L"favorite", r.favorite);
    Put(record, L"hidden", r.hidden);
    Put(record, L"auto_connect", r.auto_connect);
    Put(record, L"connect_established", r.connect_established);
    Put(record, L"volume", r.volume);
    Put(record, L"muted", r.muted);
    Put(record, L"offset_ms", double(r.offset_ms));
    record.Insert(L"features",
                  r.features ? JsonValue::CreateStringValue(winrt::to_hstring(
                                   std::to_string(*r.features)))
                             : JsonValue::CreateNullValue());
    record.Insert(L"manual_endpoint",
                  r.manual_endpoint
                      ? EndpointJson(*r.manual_endpoint).as<IJsonValue>()
                      : JsonValue::CreateNullValue());
    JsonArray endpoints;
    for (auto const& e : r.discovered_endpoints)
      endpoints.Append(EndpointJson(e));
    record.Insert(L"discovered_endpoints", endpoints);
    records.Append(record);
  }
  receivers.Insert(L"records", records);
  JsonObject commit;
  Put(commit, L"schema_version", 1.0);
  Put(commit, L"revision", double(s.revision));
  auto settings_text = SerializedJson(settings),
       receivers_text = SerializedJson(receivers),
       commit_text = SerializedJson(commit);
  StorageIO::AtomicText(root_ / L"settings.json", settings_text);
  StorageIO::AtomicText(root_ / L"receivers.json", receivers_text);
  StorageIO::AtomicText(root_ / L"commit.json", commit_text);
}
SettingsSnapshot SettingsStore::Load() {
  std::scoped_lock lock(writer_, StorageIO::WriterGate);
  bool settings_exists = std::filesystem::exists(root_ / L"settings.json"),
       receivers_exists = std::filesystem::exists(root_ / L"receivers.json"),
       commit_exists = std::filesystem::exists(root_ / L"commit.json");
  if (!settings_exists && !receivers_exists && !commit_exists) {
    SettingsSnapshot initial;
    initial.first_run = true;
    return initial;
  }
  Need(settings_exists && receivers_exists && commit_exists,
       "Incomplete settings transaction");
  auto settings = ReadJson(root_ / L"settings.json"),
       receivers = ReadJson(root_ / L"receivers.json"),
       commit = ReadJson(root_ / L"commit.json");
  Fields(
      settings,
      {L"schema_version", L"revision", L"audio_source", L"selected_receiver_id",
       L"local_playback", L"latency_mode", L"custom_ms", L"silence_timeout",
       L"startup", L"minimized", L"restore_previous", L"close_to_tray",
       L"theme", L"language", L"window_placement", L"auto_reconnect"},
      {L"master_volume", L"statistics", L"receiver_media_control", L"log_level",
       L"standby_disconnect_seconds"});
  Fields(receivers, {L"schema_version", L"revision", L"records"});
  Fields(commit, {L"schema_version", L"revision"});
  SettingsSnapshot result;
  auto& s = result.settings;
  s.schema_version = unsigned(Integer(settings, L"schema_version", UINT32_MAX));
  s.revision = Integer(settings, L"revision", 9007199254740991ULL);
  Need(s.revision > 0, "Persisted settings require a positive revision");
  Need(Integer(receivers, L"schema_version", UINT32_MAX) == 1 &&
           Integer(commit, L"schema_version", UINT32_MAX) == 1,
       "Unsupported transaction schema");
  Need(Integer(receivers, L"revision", 9007199254740991ULL) == s.revision &&
           Integer(commit, L"revision", 9007199254740991ULL) == s.revision,
       "Settings transaction revision mismatch");
  auto source = settings.GetNamedObject(L"audio_source");
  Fields(source, {L"follow_system_default", L"endpoint_id", L"role"});
  s.audio_source = {source.GetNamedBoolean(L"follow_system_default"),
                    String(source, L"endpoint_id"), String(source, L"role")};
  s.selected_receiver_id = String(settings, L"selected_receiver_id");
  s.local_playback = settings.GetNamedBoolean(L"local_playback");
  s.auto_reconnect = settings.GetNamedBoolean(L"auto_reconnect");
  s.latency_mode = String(settings, L"latency_mode");
  s.custom_ms =
      SignedInteger(settings, L"custom_ms", SENDER_MIN_PRESENTATION_MS,
                    SENDER_MAX_PRESENTATION_MS);
  s.silence_timeout = SignedInteger(settings, L"silence_timeout", 0, 60);
  if (settings.HasKey(L"standby_disconnect_seconds"))
    s.standby_disconnect_seconds =
        SignedInteger(settings, L"standby_disconnect_seconds", 0, 1800);
  s.startup = settings.GetNamedBoolean(L"startup");
  s.minimized = settings.GetNamedBoolean(L"minimized");
  s.restore_previous = settings.GetNamedBoolean(L"restore_previous");
  s.close_to_tray = settings.GetNamedBoolean(L"close_to_tray");
  s.receiver_media_control =
      settings.GetNamedBoolean(L"receiver_media_control", true);
  s.theme = String(settings, L"theme");
  s.language = String(settings, L"language");
  if (settings.HasKey(L"log_level"))
    s.log_level = String(settings, L"log_level");
  if (settings.HasKey(L"statistics")) {
    auto statistics = settings.GetNamedObject(L"statistics");
    Fields(statistics, {L"primary", L"secondary"});
    auto read_chart = [&](wchar_t const* name) {
      auto chart = statistics.GetNamedObject(name);
      Fields(chart, {L"metric", L"seconds"});
      return AppSettings::ChartPreference{
          unsigned(Integer(chart, L"metric", 4)),
          unsigned(Integer(chart, L"seconds", 3600))};
    };
    s.statistics = {read_chart(L"primary"), read_chart(L"secondary")};
  }
  auto placement = settings.GetNamedObject(L"window_placement");
  Fields(placement, {L"x", L"y", L"width", L"height", L"saved"});
  s.window_placement = {SignedInteger(placement, L"x", INT_MIN, INT_MAX),
                        SignedInteger(placement, L"y", INT_MIN, INT_MAX),
                        SignedInteger(placement, L"width", 1, 32768),
                        SignedInteger(placement, L"height", 1, 32768),
                        placement.GetNamedBoolean(L"saved")};
  for (auto const& item : receivers.GetNamedArray(L"records")) {
    auto record = item.GetObject();
    Fields(
        record,
        {L"receiver_id", L"identity_verified", L"name", L"model", L"favorite",
         L"hidden", L"auto_connect", L"volume", L"muted", L"offset_ms",
         L"features", L"manual_endpoint", L"discovered_endpoints"},
        {L"firmware_version", L"latency_profile", L"manufacturer",
         L"connect_established"});
    ReceiverRecord r;
    r.receiver_id = String(record, L"receiver_id");
    r.identity_verified = record.GetNamedBoolean(L"identity_verified");
    r.name = String(record, L"name");
    r.model = String(record, L"model");
    if (record.HasKey(L"manufacturer"))
      r.manufacturer = String(record, L"manufacturer");
    if (record.HasKey(L"firmware_version"))
      r.firmware_version = String(record, L"firmware_version");
    if (record.HasKey(L"latency_profile") &&
        record.GetNamedValue(L"latency_profile").ValueType() !=
            JsonValueType::Null) {
      auto profile = record.GetNamedObject(L"latency_profile");
      Fields(profile, {L"firmware", L"network_context", L"budget_ms"});
      r.latency_profile = LatencyProfile{
          String(profile, L"firmware"), String(profile, L"network_context"),
          SignedInteger(profile, L"budget_ms", SENDER_MIN_PRESENTATION_MS,
                        SENDER_MAX_PRESENTATION_MS)};
    }
    r.favorite = record.GetNamedBoolean(L"favorite");
    r.hidden = record.GetNamedBoolean(L"hidden");
    r.auto_connect = record.GetNamedBoolean(L"auto_connect");
    if (record.HasKey(L"connect_established"))
      r.connect_established = record.GetNamedBoolean(L"connect_established");
    r.volume = record.GetNamedNumber(L"volume");
    r.muted = record.GetNamedBoolean(L"muted");
    r.offset_ms = SignedInteger(record, L"offset_ms", -1000, 1000);
    if (record.GetNamedValue(L"features").ValueType() != JsonValueType::Null) {
      auto text = String(record, L"features");
      size_t used{};
      r.features = std::stoull(text, &used);
      Need(used == text.size() && text == std::to_string(*r.features),
           "Noncanonical feature value");
    }
    if (record.GetNamedValue(L"manual_endpoint").ValueType() !=
        JsonValueType::Null)
      r.manual_endpoint =
          ParseEndpoint(record.GetNamedObject(L"manual_endpoint"));
    for (auto const& e : record.GetNamedArray(L"discovered_endpoints"))
      r.discovered_endpoints.push_back(ParseEndpoint(e.GetObject()));
    result.receivers.push_back(std::move(r));
  }
  ValidateSettings(result);
  return result;
}
}  // namespace Win2Air
