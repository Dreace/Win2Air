#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cstdio>
#include <fstream>
#include <iterator>

#include "../../src/Settings/CredentialStore.h"
#include "../../src/Settings/SettingsStore.h"
#include "test_support.h"
using namespace Win2Air;
void TestSettingsCredentials(wchar_t const* path) {
  auto root = std::filesystem::path(path);
  SettingsStore store(root / "settings");
  auto first = store.Load();
  Require(first.first_run && first.settings.local_playback &&
              !first.settings.startup,
          "First-run defaults");
  first.settings.revision = 1;
  ReceiverRecord receiver;
  receiver.receiver_id = "AA:BB:CC:DD:EE:FF";
  receiver.identity_verified = true;
  first.receivers.push_back(receiver);
  first.settings.selected_receiver_id = receiver.receiver_id;
  store.Save(first);
  Require(!store.Load().first_run && store.Load().settings.revision == 1,
          "Persistent settings revision");
  {
    auto settings_path = root / "settings" / "settings.json";
    std::ifstream input(settings_path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    input.close();
    auto saved =
        winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(text));
    Require(!saved.HasKey(L"master_volume"),
            "Persisted settings contain one volume per receiver");
    saved.Insert(L"master_volume",
                 winrt::Windows::Data::Json::JsonValue::CreateNumberValue(0.2));
    std::ofstream(settings_path, std::ios::trunc)
        << winrt::to_string(saved.Stringify());
    auto loaded = store.Load();
    Require(
        loaded.settings.revision == 1 && loaded.receivers.front().volume == 0.5,
        "An obsolete master volume does not change the receiver volume");
    std::ofstream(settings_path, std::ios::trunc) << text;
  }
  auto oversized = first;
  oversized.settings.revision = 2;
  oversized.receivers.front().name = std::string(4097, 'x');
  Rejects([&] { store.Save(oversized); },
          "Save rejects strings that Load would refuse");
  auto inaccurate = first;
  inaccurate.settings.revision = 2;
  ReceiverEndpoint large_epoch;
  large_epoch.host = "127.0.0.1";
  large_epoch.discovery_epoch = 9007199254740992ULL;
  inaccurate.receivers.front().manual_endpoint = large_epoch;
  Rejects([&] { store.Save(inaccurate); },
          "Save rejects inexact JSON endpoint epochs");
  auto invalid_text = first;
  invalid_text.settings.revision = 2;
  invalid_text.receivers.front().name = std::string("bad\0name", 8);
  Rejects([&] { store.Save(invalid_text); },
          "Embedded null rejected before persistence");
  invalid_text.receivers.front().name = std::string("\xFF", 1);
  Rejects([&] { store.Save(invalid_text); },
          "Invalid UTF-8 rejected before persistence");
  auto large_document = first;
  large_document.settings.revision = 2;
  large_document.receivers.clear();
  for (unsigned i = 0; i < 256; ++i) {
    auto record = receiver;
    char identity[18]{};
    std::snprintf(identity, sizeof(identity), "AA:BB:CC:DD:EE:%02X", i);
    record.receiver_id = identity;
    record.name = std::string(4096, 'n');
    record.model = std::string(4096, 'm');
    large_document.receivers.push_back(std::move(record));
  }
  large_document.settings.selected_receiver_id =
      large_document.receivers.front().receiver_id;
  Rejects([&] { store.Save(large_document); },
          "Serialized document exceeding load bound rejected before replace");
  Require(store.Load().settings.revision == 1,
          "Rejected persistence leaves previous transaction readable");
  Rejects([&] { store.Save(first); },
          "Reused transaction revision cannot conceal partial replacement");
  auto invalid = first;
  invalid.settings.schema_version = 2;
  Rejects([&] { store.Save(invalid); }, "Unknown schema must fail");
  invalid = first;
  invalid.receivers.front().volume = 2;
  Rejects([&] { store.Save(invalid); }, "Volume outside range");
  invalid = first;
  invalid.settings.audio_source.follow_system_default = false;
  Rejects([&] { store.Save(invalid); }, "Explicit source needs endpoint");
  invalid = first;
  invalid.receivers.push_back(receiver);
  Rejects([&] { store.Save(invalid); }, "Duplicate receiver identity");
  invalid = first;
  invalid.receivers.front().offset_ms = 1001;
  Rejects([&] { store.Save(invalid); }, "Receiver offset outside range");
  invalid = first;
  invalid.settings.theme = "unknown";
  Rejects([&] { store.Save(invalid); }, "Unknown theme rejected");
  invalid = first;
  invalid.settings.selected_receiver_id = "11:22:33:44:55:66";
  Rejects([&] { store.Save(invalid); }, "Unknown selected identity rejected");
  auto blocker = root / "blocked";
  std::ofstream(blocker) << "existing file";
  Rejects(
      [&] {
        SettingsStore bad(blocker);
        bad.Save(first);
      },
      "Failed save must surface");
  Require(store.Load().settings.revision == 1,
          "Failed validation preserves old file");
  auto locked = CreateFileW((root / "settings" / "settings.json").c_str(),
                            GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(locked != INVALID_HANDLE_VALUE,
          "Lock existing settings for replace failure");
  auto updated = first;
  updated.settings.revision = 2;
  Rejects([&] { store.Save(updated); }, "Atomic replace failure visible");
  CloseHandle(locked);
  Require(store.Load().settings.revision == 1,
          "Failed replacement leaves valid old transaction");
  auto settings_path = root / "settings" / "settings.json";
  auto mutate = [&](auto action) {
    std::ifstream input(settings_path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    input.close();
    auto json =
        winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(text));
    action(json);
    std::ofstream(settings_path, std::ios::trunc)
        << winrt::to_string(json.Stringify());
    Rejects([&] { store.Load(); }, "Invalid persisted schema or type rejected");
    std::ifstream preserved(settings_path);
    std::string after((std::istreambuf_iterator<char>(preserved)), {});
    preserved.close();
    Require(after == winrt::to_string(json.Stringify()),
            "Failed load leaves damaged file unchanged");
    std::ofstream(settings_path, std::ios::trunc) << text;
  };
  mutate([](auto json) {
    json.Insert(L"schema_version",
                winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
  });
  mutate([](auto json) {
    json.Insert(
        L"extra",
        winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"unknown"));
  });
  mutate([](auto json) {
    json.Insert(L"revision",
                winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
  });
  auto receiver_lock = CreateFileW(
      (root / "settings" / "receivers.json").c_str(), GENERIC_READ,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(receiver_lock != INVALID_HANDLE_VALUE,
          "Lock related file for partial commit check");
  updated.settings.revision = 3;
  Rejects([&] { store.Save(updated); }, "Related file replace failure visible");
  CloseHandle(receiver_lock);
  Rejects([&] { store.Load(); },
          "Partial related-file transaction refuses automatic recovery");
  store.Save(updated);
  Require(store.Load().settings.revision == 3,
          "Explicit consistent transaction repairs partial write");
  std::ofstream(root / "settings" / "settings.json", std::ios::trunc)
      << "{broken";
  Rejects([&] { store.Load(); }, "Corrupt settings fail instead of defaults");
  CredentialStore credentials(root / "credentials");
  std::string secret = "private-authorization-test-material";
  SecureBuffer borrowed(secret);
  credentials.Save(receiver.receiver_id, 37, borrowed.Bytes());
  borrowed.Clear();
  auto loaded = credentials.Load(receiver.receiver_id, 37);
  Require(std::string(loaded.Bytes().begin(), loaded.Bytes().end()) == secret,
          "DPAPI credential round trip");
  auto lower = credentials.Load("aa:bb:cc:dd:ee:ff", 37);
  Require(lower.Bytes().size() == loaded.Bytes().size(),
          "Identity casing preserves stable credential binding");
  Rejects([&] { credentials.Load(receiver.receiver_id, 38); },
          "Wrong sender binding");
  Rejects([&] { credentials.Load("11:22:33:44:55:66", 37); },
          "Wrong receiver binding");
  std::filesystem::copy_file(root / "credentials" / "AA-BB-CC-DD-EE-FF.bin",
                             root / "credentials" / "11-22-33-44-55-66.bin");
  Rejects([&] { credentials.Load("11:22:33:44:55:66", 37); },
          "Copied blob refuses different receiver identity");
  Require(ImpersonateAnonymousToken(GetCurrentThread()) != FALSE,
          "Anonymous user impersonation for DPAPI boundary");
  bool denied = false;
  try {
    credentials.Load(receiver.receiver_id, 37);
  } catch (...) {
    denied = true;
  }
  Require(RevertToSelf() != FALSE, "Restore current user after DPAPI boundary");
  Require(denied, "Anonymous principal cannot load current-user credential");
  for (auto const& file : std::filesystem::recursive_directory_iterator(root))
    if (file.is_regular_file()) {
      std::ifstream input(file.path(), std::ios::binary);
      std::string contents((std::istreambuf_iterator<char>(input)), {});
      Require(contents.find(secret) == std::string::npos,
              "Secret must not appear on disk");
    }
  auto credential_file = root / "credentials" / "AA-BB-CC-DD-EE-FF.bin";
  auto credential_lock =
      CreateFileW(credential_file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(credential_lock != INVALID_HANDLE_VALUE,
          "Lock credential for deletion failure");
  Rejects([&] { credentials.Delete(receiver.receiver_id); },
          "Delete failure must be visible");
  Rejects([&] { credentials.Load(receiver.receiver_id, 37); },
          "Delete failure blocks old authorization");
  CredentialStore restarted(root / "credentials");
  Rejects([&] { restarted.Load(receiver.receiver_id, 37); },
          "Revocation persists across store restart");
  CloseHandle(credential_lock);
  credentials.Delete(receiver.receiver_id);
  Rejects([&] { credentials.Load(receiver.receiver_id, 37); },
          "Deleted credential unavailable");
  SettingsStore low_store(root / "low-budget");
  auto low = low_store.Load();
  low.settings.latency_mode = "custom";
  auto profiled = receiver;
  profiled.firmware_version = "virtual-firmware";
  profiled.latency_profile =
      LatencyProfile{"virtual-firmware", "virtual-route", 20};
  low.receivers = {profiled};
  for (int budget : {20, 30, 40, 60, 80, 100, 120, 200, 249}) {
    ++low.settings.revision;
    low.settings.custom_ms = budget;
    low_store.Save(low);
    auto restored = low_store.Load();
    Require(restored.settings.custom_ms == budget &&
                restored.receivers.front().latency_profile->budget_ms == 20,
            "Low sender budgets and learned profiles survive persistence and "
            "restart without a stricter reader bound");
  }
  ++low.settings.revision;
  low.settings.custom_ms = 19;
  Rejects([&] { low_store.Save(low); },
          "An unsupported sender budget is rejected before persistence");
}
