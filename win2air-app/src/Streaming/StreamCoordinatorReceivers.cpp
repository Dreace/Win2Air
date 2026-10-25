#include "StreamCoordinatorInternal.h"

#include <avrt.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "../Settings/StorageIO.h"
#include "FailureDetail.h"
#include "LatencyProfile.h"
#include "ReceiverSession.h"
namespace Win2Air {
void StreamCoordinator::Discover() {
  if (!discovery_enabled_ || shutting_down_ || stopping_) return;
  if (discovery_refresh_.valid()) {
    if (discovery_refresh_.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    discovery_refresh_.get();
    Note(LogLevel::info, LogModule::discovery, LogEvent::discovery_changed,
         "operation=refresh outcome=completed epoch=" +
             std::to_string(discovery_epoch_));
    Publish();
  }
  auto events = browser_->Take();
  for (auto &event : events) {
    if (event.removed) {
      if (event.instance == resolving_instance_) resolving_removed_ = true;
      announcements_.erase(event.instance);
      auto found = discovered_services_.find(event.instance);
      if (found == discovered_services_.end()) continue;
      auto [identity, endpoint] = found->second;
      Note(LogLevel::info, LogModule::discovery, LogEvent::discovery_changed,
           "operation=removed receiver=" + identity);
      discovered_services_.erase(found);
      bool still_announced =
          std::any_of(discovered_services_.begin(), discovered_services_.end(),
                      [&](auto const &service) {
                        return service.second.first == identity &&
                               service.second.second.resolved_address ==
                                   endpoint.resolved_address &&
                               service.second.second.port == endpoint.port;
                      });
      if (!still_announced)
        registry_.RemoveEndpoint(identity, endpoint.resolved_address,
                                 endpoint.port);
      Publish();
    } else {
      if (!EnqueueAnnouncement(announcements_, std::move(event))) {
        std::lock_guard lock(mutex_);
        snapshot_.discovery_error =
            "Receiver discovery queue is full; excess announcements were "
            "rejected";
      }
    }
  }
  if (discovery_resolution_.valid()) {
    if (discovery_resolution_.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    auto receiver = discovery_resolution_.get();
    if (!resolving_removed_ && resolving_epoch_ == discovery_epoch_) {
      registry_.Merge(receiver.record, receiver.endpoint);
      Note(LogLevel::info, LogModule::discovery, LogEvent::discovery_changed,
           "operation=resolved receiver=" + receiver.record.receiver_id +
               " port=" + std::to_string(receiver.endpoint.port));
      discovered_services_.insert_or_assign(
          resolving_instance_,
          std::make_pair(receiver.record.receiver_id, receiver.endpoint));
      AutoConnectSelected();
      Publish();
    }
    resolving_instance_.clear();
  }
  auto next = std::find_if(
      announcements_.begin(), announcements_.end(), [this](auto const &item) {
        return !runtime_ ||
               item.first.find("._airplay._tcp.local") != std::string::npos;
      });
  if (next == announcements_.end()) return;
  auto announcement = std::move(next->second);
  announcements_.erase(next);
  if (announcement.removed) return;
  if (announcement.instance.find("._airplay._tcp.local") != std::string::npos) {
    resolving_instance_ = announcement.instance;
    resolving_epoch_ = announcement.epoch;
    resolving_removed_ = false;
    discovery_resolution_ =
        std::async(std::launch::async, [this, announcement] {
          auto service = ResolveService(announcement.instance, 0, 1500, [this] {
            return shutting_down_.load() || stopping_.load();
          });
          return ReceiverFromService(service, announcement.epoch);
        });
    return;
  }
  auto canceled = [this] { return shutting_down_.load() || stopping_.load(); };
  auto service = ResolveService(announcement.instance, 0, 1500, canceled);
  ResolvedReceiver receiver;
  if (desired_connected_) return;
  if (service.addresses.empty())
    throw std::runtime_error("Receiver service has no numeric address");
  auto address = service.addresses.front().first;
  if (service.addresses.front().second)
    address += "%" + std::to_string(service.addresses.front().second);
  Create({});
  try {
    receiver = ProbeReceiver(library_, runtime_, address, service.port,
                             announcement.epoch, canceled);
    Cleanup();
  } catch (...) {
    Cleanup();
    throw;
  }
  receiver.endpoint.host = service.host;
  receiver.endpoint.interface_index = service.interface_index;
  registry_.Merge(receiver.record, receiver.endpoint);
  discovered_services_.insert_or_assign(
      announcement.instance,
      std::make_pair(receiver.record.receiver_id, receiver.endpoint));
  AutoConnectSelected();
  Publish();
}
std::future<CommandResult> StreamCoordinator::UpdateReceiver(
    std::string identity, std::string name, bool favorite, bool hidden,
    bool auto_connect, int offset_ms) {
  return Queue(false, [this, identity = std::move(identity),
                       name = std::move(name), favorite, hidden, auto_connect,
                       offset_ms](auto result) {
    auto record = registry_.Find(identity);
    record.name = name;
    record.favorite = favorite;
    record.hidden = hidden;
    record.auto_connect = auto_connect;
    record.offset_ms = offset_ms;
    InitialPresentation(settings_.settings, record);
    if (session_.session &&
        session_.session->receiver_id == record.receiver_id &&
        record.offset_ms != registry_.Find(identity).offset_ms)
      return ChangePresentation(result, settings_.settings, std::move(record));
    auto previous = registry_;
    try {
      registry_.Update(std::move(record));
      Save();
    } catch (...) {
      registry_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::EditReceiverEndpoint(
    std::string identity, std::string host, unsigned port) {
  return Queue(false, [this, identity = std::move(identity),
                       host = std::move(host), port](auto result) {
    if (runtime_ || desired_connected_)
      throw std::runtime_error(
          "Disconnect before verifying an edited endpoint");
    auto record = registry_.Find(identity);
    Create({});
    ResolvedReceiver value;
    try {
      value = ProbeReceiver(library_, runtime_, host, port, result.command_id,
                            [this, epoch = result.intent_epoch] {
                              return stopping_ || intent_epoch_ != epoch;
                            });
      Cleanup();
    } catch (...) {
      Cleanup();
      throw;
    }
    ReceiverRegistry checked({record});
    checked.Authenticate(identity, value.record.receiver_id);
    record.manual_endpoint = value.endpoint;
    record.manual_endpoint->host = host;
    auto previous = registry_;
    try {
      registry_.Update(std::move(record));
      Save();
    } catch (...) {
      registry_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
void StreamCoordinator::RevokePairing(std::string const &identity) {
  std::string cleanup_error;
  if (selected_ == identity) {
    desired_connected_ = false;
    suppressed_ = true;
    reconnect_.Reset();
    reconnecting_ = false;
    try {
      Cleanup();
    } catch (...) {
      cleanup_error = DescribeFailure().message;
    }
  }
  for (auto secret_store : {&credentials_, &authentication_inputs_}) {
    try {
      secret_store->Delete(identity);
    } catch (...) {
      if (!cleanup_error.empty()) cleanup_error += "; ";
      cleanup_error += "credential revocation: " + DescribeFailure().message;
    }
  }
  if (!cleanup_error.empty()) throw std::runtime_error(cleanup_error);
}
std::future<CommandResult> StreamCoordinator::DeleteReceiver(
    std::string identity) {
  auto current = Snapshot();
  bool active = ValidReceiverIdentity(identity) && current.session &&
                current.session->session &&
                _stricmp(current.session->session->receiver_id.c_str(),
                         identity.c_str()) == 0;
  return Queue(active, [this, identity = std::move(identity)](auto result) {
    auto record = registry_.Find(identity);
    RevokePairing(record.receiver_id);
    auto previous_registry = registry_;
    auto previous_settings = settings_;
    try {
      registry_.Remove(record.receiver_id);
      if (selected_ == record.receiver_id) {
        selected_.clear();
        settings_.settings.selected_receiver_id.clear();
      }
      Save();
    } catch (...) {
      registry_ = std::move(previous_registry);
      settings_ = std::move(previous_settings);
      selected_ = settings_.settings.selected_receiver_id;
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::ForgetPairing(
    std::string identity) {
  auto current = Snapshot();
  bool active = ValidReceiverIdentity(identity) && current.session &&
                current.session->session &&
                _stricmp(current.session->session->receiver_id.c_str(),
                         identity.c_str()) == 0;
  return Queue(active, [this, identity = std::move(identity)](auto result) {
    auto canonical = registry_.Find(identity).receiver_id;
    RevokePairing(canonical);
    auto previous = registry_;
    registry_.ForgetAuthentication(canonical);
    try {
      Save();
    } catch (...) {
      registry_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::RefreshReceivers() {
  return Queue(false, [this](auto result) {
    if (!discovery_enabled_)
      throw std::runtime_error("Receiver discovery is disabled");
    if (!discovery_refresh_.valid()) {
      auto epoch = ++discovery_epoch_;
      announcements_.clear();
      {
        std::lock_guard lock(mutex_);
        snapshot_.discovery_error.clear();
      }
      // DNS cancellation can wait; keep it off the audio worker.
      discovery_refresh_ = std::async(std::launch::async, [this, epoch] {
        browser_->Stop();
        browser_->Start(epoch);
      });
      Note(LogLevel::info, LogModule::discovery, LogEvent::discovery_changed,
           "operation=refresh outcome=started epoch=" + std::to_string(epoch));
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
void StreamCoordinator::StopDiscovery(BrowseDeadline deadline) {
  std::exception_ptr failure;
  if (discovery_refresh_.valid()) {
    try {
      discovery_refresh_.get();
    } catch (...) {
      failure = std::current_exception();
    }
  }
  browser_->Stop(deadline);
  if (discovery_resolution_.valid()) {
    try {
      discovery_resolution_.get();
    } catch (...) {
      if (!shutting_down_ && !stopping_ && !failure)
        failure = std::current_exception();
    }
  }
  if (failure) std::rethrow_exception(failure);
}
std::future<CommandResult> StreamCoordinator::AddReceiver(std::string host,
                                                          unsigned port,
                                                          int initial_offset) {
  return Queue(
      false, [this, host = std::move(host), port, initial_offset](auto result) {
        if (runtime_ || desired_connected_)
          throw std::runtime_error(
              "End the active session before probing a manual receiver");
        ReceiverRecord budget;
        budget.offset_ms = initial_offset;
        InitialPresentation(settings_.settings, budget);
        auto known = KnownEndpoint(registry_.Records(), host, port);
        auto address = known ? known->resolved_address : host;
        if (known && known->scope_id)
          address += "%" + std::to_string(known->scope_id);
        Create({});
        try {
          auto value = ProbeReceiver(
              library_, runtime_, address, port, result.command_id,
              [this, epoch = result.intent_epoch] {
                return stopping_ || intent_epoch_ != epoch;
              });
          value.endpoint.host = host;
          if (known) value.endpoint.interface_index = known->interface_index;
          auto endpoint = value.endpoint;
          value.record.manual_endpoint = endpoint;
          value.record.offset_ms = initial_offset;
          registry_.Merge(value.record, endpoint);
          Cleanup();
          Save();
          result.status = CommandStatus::applied;
          return result;
        } catch (...) {
          Cleanup();
          throw;
        }
      });
}
}  // namespace Win2Air
