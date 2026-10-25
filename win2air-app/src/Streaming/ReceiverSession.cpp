#include "ReceiverSession.h"
namespace Win2Air {
RecoveryDisposition ClassifyRecovery(int code, int peer) noexcept {
  if (code == SENDER_AUTHENTICATION || peer == 401 || peer == 403 ||
      peer == 470)
    return RecoveryDisposition::authentication;
  if (code == SENDER_NETWORK || code == SENDER_TIMEOUT ||
      code == SENDER_PLATFORM || peer == 453)
    return RecoveryDisposition::retry;
  return RecoveryDisposition::terminal;
}
bool ApplySenderState(ReceiverSession &session, uint64_t generation,
                      sender_state state, sender_error const *error) {
  if (generation != session.generation) return false;
  if (state == SENDER_READY)
    session.state = SessionState::ready;
  else if (state == SENDER_STREAMING)
    session.state = SessionState::streaming;
  else if (state == SENDER_ENDED)
    session.state = SessionState::disconnected;
  else if (state == SENDER_ENDING)
    session.state = SessionState::ending;
  else if (state == SENDER_FAILED) {
    session.state = SessionState::error;
    CommandResult result;
    result.status = CommandStatus::failed;
    result.stage = SessionState::error;
    result.generation = generation;
    result.intent_epoch = session.intent_epoch;
    if (error) {
      result.platform_code = error->platform_code;
      result.peer_code = error->peer_code;
      result.sender_code = error->code;
      result.message = std::string(error->operation) + ": " + error->message;
    }
    session.error = std::move(result);
  }
  return true;
}
bool ApplySenderStep(ReceiverSession &session, uint64_t generation,
                     sender_step step) {
  if (generation != session.generation) return false;
  session.state = step == SENDER_STEP_PAIRING ? SessionState::pairing
                                              : SessionState::setting_up;
  return true;
}
}  // namespace Win2Air
