#pragma once
#include "../../../airplay2-sender-core/include/sender.h"
#include "SessionModels.h"
namespace Win2Air {
bool ApplySenderState(ReceiverSession&, uint64_t, sender_state,
                      sender_error const*);
bool ApplySenderStep(ReceiverSession&, uint64_t, sender_step);
enum class RecoveryDisposition { retry, authentication, terminal };
RecoveryDisposition ClassifyRecovery(int code, int peer) noexcept;
}  // namespace Win2Air
