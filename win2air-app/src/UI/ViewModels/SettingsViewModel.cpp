#include "SettingsViewModel.h"
namespace Win2Air {
void SettingsViewModel::Submit(
    std::function<std::future<CommandResult>()> action,
    std::function<void()> applied, std::function<void()> rejected) {
  if (pending_)
    throw std::runtime_error("Wait for the current setting to complete");
  result_ = {};
  pending_.emplace(action());
  applied_ = std::move(applied);
  rejected_ = std::move(rejected);
}
void SettingsViewModel::Present(PlaybackSnapshot snapshot) {
  snapshot_ = std::move(snapshot);
  auto const& saved = snapshot_.preferences;
  draft_->theme.Update(saved.theme);
  draft_->language.Update(saved.language);
  draft_->source.Update(saved.audio_source.follow_system_default
                            ? ""
                            : saved.audio_source.endpoint_id);
}
void SettingsViewModel::Refresh() {
  if (pending_ && pending_->wait_for(std::chrono::seconds(0)) ==
                      std::future_status::ready) {
    result_ = pending_->get();
    if (result_.status == CommandStatus::applied && applied_) applied_();
    if (result_.status != CommandStatus::applied && rejected_) rejected_();
    applied_ = {};
    rejected_ = {};
    pending_.reset();
  }
  snapshot_ = service_.Snapshot();
  auto const& saved = snapshot_.preferences;
  draft_->theme.Update(saved.theme);
  draft_->language.Update(saved.language);
  draft_->source.Update(saved.audio_source.follow_system_default
                            ? ""
                            : saved.audio_source.endpoint_id);
}
void SettingsViewModel::Appearance(std::string theme, std::string language) {
  auto rejected = [draft = draft_, saved = snapshot_.preferences,
                   tv = draft_->theme.Version(),
                   lv = draft_->language.Version()] {
    draft->theme.Reject(tv, saved.theme);
    draft->language.Reject(lv, saved.language);
  };
  auto applied = [draft = draft_, theme, language, tv = draft_->theme.Version(),
                  lv = draft_->language.Version()] {
    draft->theme.Acknowledge(tv, theme);
    draft->language.Acknowledge(lv, language);
  };
  Submit(
      [&] {
        return service_.SetAppearance(std::move(theme), std::move(language));
      },
      std::move(applied), std::move(rejected));
}
void SettingsViewModel::Source(AudioSourceConfig source) {
  auto const& saved = snapshot_.preferences.audio_source;
  auto rejected = [draft = draft_,
                   key = saved.follow_system_default ? std::string{}
                                                     : saved.endpoint_id,
                   version = draft_->source.Version()] {
    draft->source.Reject(version, key);
  };
  auto applied = [draft = draft_,
                  key = source.follow_system_default ? std::string{}
                                                     : source.endpoint_id,
                  version = draft_->source.Version()] {
    draft->source.Acknowledge(version, key);
  };
  Submit([&] { return service_.SetSource(std::move(source)); },
         std::move(applied), std::move(rejected));
}
void SettingsViewModel::Local(bool enabled) {
  Submit([&] { return service_.SetLocalPlayback(enabled); });
}
void SettingsViewModel::Lifecycle(bool close_to_tray, bool minimized,
                                  bool restore_previous) {
  Submit([&] {
    return service_.SetLifecycle(close_to_tray, minimized, restore_previous);
  });
}
}  // namespace Win2Air
