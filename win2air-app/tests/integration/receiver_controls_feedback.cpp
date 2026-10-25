#include "receiver_controls_support.h"

namespace Win2Air::Tests {

void PairingPromptScenario() {
  PairingPrompt prompt;
  auto snapshot = std::make_shared<SessionSnapshot>();
  snapshot->session.emplace();
  snapshot->session->receiver_id = "MacBook";
  snapshot->session->intent_epoch = 1;
  snapshot->session->failure_kind = "authentication";
  Require(prompt.Observe(snapshot) == "MacBook" && !prompt.Observe(snapshot),
          "A pairing request opens once for the failing connection attempt");
  prompt.Dismiss(snapshot, "MacBook");
  Require(!prompt.Observe(snapshot),
          "Dismissing a pairing request keeps it dismissed for that attempt");
  snapshot->session->intent_epoch = 2;
  Require(
      prompt.Observe(snapshot) == "MacBook",
      "An immediate failed retry can prompt without an intermediate UI tick");
}
void OperationFeedbackScenario() {
  OperationFeedback feedback;
  std::promise<CommandResult> mac, speaker;
  feedback.Submit(mac.get_future(), "MacBook");
  feedback.Submit(speaker.get_future(), "HomePod");
  CommandResult denied;
  denied.status = CommandStatus::failed;
  denied.message = "The Mac receiver rejected pairing";
  mac.set_value(denied);
  feedback.Refresh();
  Require(feedback.Error("MacBook") == denied.message &&
              feedback.Error("HomePod").empty() && feedback.Error().empty(),
          "A Mac pairing failure belongs only to the Mac receiver");
  CommandResult applied;
  applied.status = CommandStatus::applied;
  speaker.set_value(applied);
  feedback.Refresh();
  Require(feedback.Error("MacBook") == denied.message,
          "Another receiver succeeding preserves the original failure");
  std::promise<CommandResult> old_attempt, new_attempt;
  feedback.Submit(old_attempt.get_future(), "MacBook");
  feedback.Submit(new_attempt.get_future(), "MacBook");
  new_attempt.set_value(applied);
  feedback.Refresh();
  old_attempt.set_value(denied);
  feedback.Refresh();
  Require(feedback.Error("MacBook").empty() && !feedback.Pending(),
          "Late failure from a superseded attempt cannot replace success");
  feedback.Report("Enter a valid port", "HomePod");
  feedback.Report("Audio source is unavailable");
  Require(feedback.Error("HomePod") == "Enter a valid port" &&
              feedback.Error("MacBook").empty() &&
              feedback.Error() == "Audio source is unavailable",
          "Field validation and application failures retain their own scope");
}
}  // namespace Win2Air::Tests
