#include "../../src/Settings/StorageIO.h"
#include "../../src/Streaming/StreamCoordinator.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
namespace {
CommandResult Finish(std::future<CommandResult> result) {
  Require(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "Authorization command must complete within its bound");
  return result.get();
}
}  // namespace
void TestForgetPairing(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  SettingsStore store(root);
  auto saved = store.Load();
  saved.settings.revision = 1;
  ReceiverRecord record;
  record.receiver_id = "AA:BB:CC:DD:EE:FF";
  record.name = record.model = "Virtual pairing receiver";
  record.features =
      (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
  record.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
  saved.receivers = {record};
  store.Save(saved);
  auto credential = root / L"credentials" / L"AA-BB-CC-DD-EE-FF.bin";
  auto marker = credential;
  marker += L".revoked";
  {
    VirtualReceiver receiver;
    receiver.export_credentials = true;
    auto audio = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(receiver, root, false,
                              VirtualAudioServices(audio));
    Require(Finish(service.Connect(record.receiver_id)).status ==
                    CommandStatus::applied &&
                std::filesystem::exists(credential),
            "A successful virtual pairing saves bound credentials");
    Require(service.Shutdown(true).get().status == CommandStatus::applied,
            "The original paired service ends before restart");
  }
  {
    VirtualReceiver receiver;
    auto audio = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(receiver, root, false,
                              VirtualAudioServices(audio));
    Require(Finish(service.Connect(record.receiver_id)).status ==
                    CommandStatus::applied &&
                receiver.imported == 1,
            "A restarted virtual service imports its saved authorization");
    Require(Finish(service.ForgetPairing(record.receiver_id +
                                         std::string("\0invalid", 8)))
                        .status == CommandStatus::failed &&
                receiver.destroyed == 0,
            "Malformed revocation fails without superseding the live session");
    {
      std::lock_guard lock(receiver.mutex);
      receiver.hold_volume = true;
    }
    auto volume = service.SetReceiverVolume(record.receiver_id, 0.4);
    WaitUntil([&] { return service.Snapshot().volume_pending; },
              "Prepare an active receiver command before revocation");
    Require(Finish(service.ForgetPairing("aa:bb:cc:dd:ee:ff")).status ==
                    CommandStatus::applied &&
                Finish(std::move(volume)).status == CommandStatus::canceled &&
                receiver.destroyed > 0,
            "Forgetting cancels pending commands and releases the active "
            "session first");
    auto forgotten = SettingsStore(root).Load();
    Require(forgotten.receivers.size() == 1 &&
                !forgotten.receivers.front().identity_verified &&
                !std::filesystem::exists(credential),
            "Forgetting retains public preferences and removes authorization");
    {
      std::lock_guard lock(receiver.mutex);
      receiver.hold_volume = false;
      receiver.export_credentials = true;
    }
    Require(Finish(service.Connect(record.receiver_id, SecureBuffer("1234")))
                        .status == CommandStatus::applied &&
                receiver.imported == 1 && !std::filesystem::exists(marker),
            "Explicit pairing creates new authorization without importing "
            "revoked material");
    {
      StorageIO::Handle locked(
          CreateFileW(credential.c_str(), GENERIC_READ, FILE_SHARE_READ,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      auto failed = Finish(service.DeleteReceiver(record.receiver_id));
      Require(failed.status == CommandStatus::failed &&
                  !failed.message.empty() && std::filesystem::exists(marker) &&
                  SettingsStore(root).Load().receivers.size() == 1,
              "Failed deletion preserves the public record and a durable "
              "revocation marker");
      Require(Finish(service.ForgetPairing(record.receiver_id)).status ==
                  CommandStatus::failed,
              "Failed forgetting remains a failure while the authorization "
              "file is locked");
    }
    Require(service.Shutdown(true).get().status == CommandStatus::applied,
            "A failed revoke still closes the service");
  }
  {
    VirtualReceiver receiver;
    auto audio = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(receiver, root, false,
                              VirtualAudioServices(audio));
    auto refused = Finish(service.Connect(record.receiver_id));
    Require(refused.status == CommandStatus::failed &&
                refused.sender_code == SENDER_AUTHENTICATION &&
                receiver.imported == 0,
            "A restarted service refuses revoked authorization left by failed "
            "deletion");
    Require(Finish(service.DeleteReceiver(record.receiver_id)).status ==
                    CommandStatus::applied &&
                SettingsStore(root).Load().receivers.empty(),
            "Retry deletion removes the record and old authorization safely");
    Require(Finish(service.ForgetPairing(record.receiver_id)).status ==
                CommandStatus::failed,
            "A stale receiver action reports an asynchronous failure instead "
            "of throwing from the UI entry");
    Require(service.Shutdown(true).get().status == CommandStatus::applied,
            "A revoked service exits normally");
  }
  for (bool delete_receiver : {false, true}) {
    auto failure_root = root / (delete_receiver ? L"delete-restore-failure"
                                                : L"forget-restore-failure");
    auto seed = saved;
    seed.settings.local_playback = false;
    SettingsStore(failure_root).Save(seed);
    {
      VirtualReceiver receiver;
      receiver.export_credentials = true;
      auto audio = std::make_shared<VirtualAudioState>();
      StreamCoordinator service(receiver, failure_root, false,
                                VirtualAudioServices(audio));
      Require(Finish(service.Connect(record.receiver_id)).status ==
                  CommandStatus::applied,
              "Prepare saved authorization and a virtual local mute lease");
      {
        std::lock_guard lock(audio->mutex);
        audio->fail_unmute = true;
      }
      {
        StorageIO::Handle locked(CreateFileW(
            (failure_root / L"credentials" / L"AA-BB-CC-DD-EE-FF.bin").c_str(),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        auto failed =
            Finish(delete_receiver ? service.DeleteReceiver(record.receiver_id)
                                   : service.ForgetPairing(record.receiver_id));
        Require(failed.status == CommandStatus::failed &&
                    failed.message.find("Virtual endpoint restore failed") !=
                        std::string::npos &&
                    failed.message.find("credential revocation:") !=
                        std::string::npos &&
                    receiver.destroyed > 0 &&
                    std::filesystem::exists(failure_root / L"credentials" /
                                            L"AA-BB-CC-DD-EE-FF.bin.revoked") &&
                    SettingsStore(failure_root).Load().receivers.size() == 1,
                "Local restore and file deletion failures retain both errors, "
                "a durable revocation marker, and the public record");
      }
      {
        std::lock_guard lock(audio->mutex);
        audio->fail_unmute = false;
      }
      Require(service.Shutdown(true).get().status == CommandStatus::applied,
              "Local restoration can be retried after the endpoint recovers");
    }
    {
      VirtualReceiver receiver;
      auto audio = std::make_shared<VirtualAudioState>();
      StreamCoordinator service(receiver, failure_root, false,
                                VirtualAudioServices(audio));
      auto refused = Finish(service.Connect(record.receiver_id));
      Require(refused.status == CommandStatus::failed &&
                  refused.sender_code == SENDER_AUTHENTICATION &&
                  receiver.imported == 0,
              "Restart after local restore failure cannot reuse revoked "
              "authorization");
      Require(service.Shutdown(true).get().status == CommandStatus::applied,
              "The restarted revoked service closes normally");
    }
  }
  SecureBuffer secret("temporary");
  secret.Clear();
  secret.Clear();
  Require(secret.Bytes().empty(),
          "Clearing authorization releases its exposed memory view and is "
          "idempotent");
}
