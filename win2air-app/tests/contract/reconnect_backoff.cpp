#include "../../src/App/PowerNotifications.h"
#include "../../src/Streaming/ReceiverSession.h"
#include "../../src/Streaming/ReconnectController.h"
#include "test_support.h"
using namespace Win2Air;
void TestReconnectBackoff() {
  ReconnectController retry;
  uint64_t now = 1000000000;
  for (unsigned seconds : {2, 5, 10, 30, 60, 60, 60}) {
    retry.Schedule(now, 7, "Controlled network interruption");
    auto due = retry.Snapshot()->next_due_monotonic;
    Require(due - now == uint64_t(seconds) * 1000000000 &&
                !retry.Due(due - 1, 7) && retry.Due(due, 7) &&
                !retry.Due(due, 8),
            "Reconnect uses exact bounded delays and the current intent");
    now = due;
    retry.Cancel();
    Require(!retry.Due(now, 7), "Cancel removes a due retry");
  }
  retry.Reset();
  retry.Schedule(now, 8, "New connection intent");
  Require(retry.Snapshot()->attempt == 1 &&
              retry.Snapshot()->next_due_monotonic - now == 2000000000,
          "Successful connection resets the next interruption delay");
  retry.Reset();
  Require(!retry.Snapshot(), "Reset clears the recovery projection");
  retry.Schedule(now, 8, "Failure before suspend");
  retry.Cancel();
  retry.Resume(now + 1000000000, 8, "System resumed");
  Require(retry.Snapshot()->attempt == 1 &&
              retry.Snapshot()->next_due_monotonic == now + 3000000000,
          "Resume re-arms the same upcoming attempt without consuming backoff");
  retry.Reset();
  Rejects([&] { retry.Schedule(UINT64_MAX, 1, "Failure"); },
          "Deadline overflow rejects before scheduling");
  Rejects([&] { retry.Schedule(now, 0, "Failure"); },
          "A recovery wait requires a valid cancellation epoch");
  Require(ClassifyRecovery(SENDER_AUTHENTICATION, 0) ==
                  RecoveryDisposition::authentication &&
              ClassifyRecovery(SENDER_NETWORK, 470) ==
                  RecoveryDisposition::authentication &&
              ClassifyRecovery(SENDER_CAPABILITY, 451) ==
                  RecoveryDisposition::terminal &&
              ClassifyRecovery(SENDER_CAPABILITY, 453) ==
                  RecoveryDisposition::retry &&
              ClassifyRecovery(SENDER_TIMEOUT, 0) == RecoveryDisposition::retry,
          "Authentication and capability failures stop while busy and network "
          "failures can retry");
  Require(PowerEvent(PBT_APMSUSPEND) == SystemEvent::suspend &&
              PowerEvent(PBT_APMRESUMEAUTOMATIC) == SystemEvent::resume &&
              !PowerEvent(PBT_APMRESUMESUSPEND) &&
              !PowerEvent(PBT_APMPOWERSTATUSCHANGE),
          "Power projection handles the universal resume event once");
}
