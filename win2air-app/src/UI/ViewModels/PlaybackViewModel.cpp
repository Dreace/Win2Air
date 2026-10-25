#include "PlaybackViewModel.h"
#include <algorithm>
namespace Win2Air {
void PlaybackViewModel::Present(PlaybackSnapshot snapshot) {
  snapshot_ = std::move(snapshot);
}
void PlaybackViewModel::Refresh() {
  for (auto& [id, input] : volumes_)
    if (auto result = input.Complete())
      feedback_.Complete(id, volume_revisions_.at(id), *result);
  feedback_.Refresh();
  snapshot_ = service_.Snapshot();
  for (auto& receiver : snapshot_.receivers) {
    auto found = volumes_.find(receiver.receiver_id);
    if (found == volumes_.end()) continue;
    found->second.Update(receiver.volume);
    receiver.volume = found->second.Value();
    found->second.Send([this, id = receiver.receiver_id](double value) {
      volume_revisions_[id] = feedback_.Begin(id);
      return service_.SetReceiverVolume(id, value);
    });
  }
  std::erase_if(volumes_, [this](auto const& entry) {
    return std::none_of(snapshot_.receivers.begin(), snapshot_.receivers.end(),
                        [&](auto const& receiver) {
                          return receiver.receiver_id == entry.first;
                        });
  });
  projection_->Post(snapshot_.session);
}
std::string PlaybackViewModel::Error() const {
  auto value = feedback_.Error();
  if (snapshot_.session &&
      snapshot_.session->last_command.status == CommandStatus::failed)
    value = snapshot_.session->last_command.message;
  for (auto const& error : {snapshot_.local_error, snapshot_.credential_error,
                            snapshot_.discovery_error})
    if (!error.empty()) {
      if (!value.empty()) value += "\n";
      value += error;
    }
  return value;
}
std::string PlaybackViewModel::BlockingError() const {
  std::string value;
  auto add = [&](std::string const& part) {
    if (part.empty() || value.find(part) != std::string::npos) return;
    if (!value.empty()) value += '\n';
    value += part;
  };
  add(snapshot_.source.error);
  add(snapshot_.local_error);
  return value;
}
void PlaybackViewModel::Connect(size_t receiver, SecureBuffer pin) {
  auto visible = ReceiverViews(snapshot_);
  if (receiver >= visible.size()) {
    feedback_.Report("Select an available receiver");
    return;
  }
  ConnectIdentity(visible[receiver].receiver.receiver_id, std::move(pin));
}
void PlaybackViewModel::Disconnect() {
  auto identity = snapshot_.session
                      ? snapshot_.session->intent.selected_receiver_id
                      : std::string{};
  feedback_.Submit(service_.Disconnect(), identity);
}
void PlaybackViewModel::Source(int index) {
  if (index < 0 || size_t(index) > snapshot_.sources.size())
    throw std::runtime_error("Selected source is unavailable");
  AudioSourceConfig source;
  if (index) {
    source.follow_system_default = false;
    source.endpoint_id = snapshot_.sources[size_t(index) - 1].id;
  }
  feedback_.Submit(service_.SetSource(std::move(source)));
}
void PlaybackViewModel::Local(bool value) {
  feedback_.Submit(service_.SetLocalPlayback(value));
}
void PlaybackViewModel::Add(std::string host, unsigned port) {
  feedback_.Submit(service_.AddReceiver(std::move(host), port));
}
void PlaybackViewModel::ConnectIdentity(std::string identity,
                                        SecureBuffer pin) {
  feedback_.Submit(service_.Connect(identity, std::move(pin)), identity);
}
void PlaybackViewModel::Volume(std::string identity, double value) {
  auto& input = volumes_[identity];
  input.Edit(value);
  for (auto& receiver : snapshot_.receivers)
    if (receiver.receiver_id == identity) receiver.volume = value;
  input.Send([this, &identity](double next) {
    volume_revisions_[identity] = feedback_.Begin(identity);
    return service_.SetReceiverVolume(identity, next);
  });
}
void PlaybackViewModel::Mute(std::string identity, bool value) {
  feedback_.Submit(service_.SetReceiverMuted(identity, value), identity);
}
void PlaybackViewModel::Update(ReceiverRecord const& receiver) {
  feedback_.Submit(
      service_.UpdateReceiver(receiver.receiver_id, receiver.name,
                              receiver.favorite, receiver.hidden,
                              receiver.auto_connect, receiver.offset_ms),
      receiver.receiver_id);
}
void PlaybackViewModel::Endpoint(std::string identity, std::string host,
                                 unsigned port) {
  feedback_.Submit(
      service_.EditReceiverEndpoint(identity, std::move(host), port), identity);
}
void PlaybackViewModel::Delete(std::string identity) {
  feedback_.Submit(service_.DeleteReceiver(identity), identity);
}
void PlaybackViewModel::Forget(std::string identity) {
  feedback_.Submit(service_.ForgetPairing(identity), identity);
}
void PlaybackViewModel::ResetAdaptiveLatency(std::string identity) {
  feedback_.Submit(service_.ResetAdaptiveLatency(identity), identity);
}
}  // namespace Win2Air
