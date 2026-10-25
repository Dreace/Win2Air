#include <cstring>

#include "../../src/Streaming/SenderLibrary.h"
#include "../fixtures/ReceiverFixture.h"
#include "test_support.h"
using namespace Win2Air;
void TestSenderAbi(wchar_t const* path) {
  SenderLibrary library(path);
  auto actual = library.Abi();
  auto expected = sender_header_abi();
  Require(actual.entry_count <= std::size(actual.layout),
          "Public ABI descriptor remains within its declared layout array");
  Require(std::memcmp(&actual, &expected, sizeof(actual)) == 0,
          "MSVC and DLL layouts differ");
  sender_abi_info bad{};
  Require(library.QueryAbi(999, sizeof(bad), &bad) == SENDER_INPUT,
          "Bad ABI version accepted");
  Require(library.QueryAbi(SENDER_ABI_VERSION, 1, &bad) == SENDER_INPUT,
          "Bad ABI size accepted");
  sender_error error{};
  sender_runtime* runtime{};
  sender_options options{
      1,      "Win2Air contract", "Win2Air contract", 4096, nullptr, nullptr,
      nullptr};
  sender_create_options extended{SENDER_ABI_VERSION,
                                 sizeof(sender_create_options),
                                 options,
                                 {1000, 0},
                                 nullptr};
  Require(library.Create(extended, &runtime, &error) == SENDER_OK && runtime,
          "Real DLL create");
  Require(library.Poll(runtime, 0, &error) == SENDER_OK, "Idle poll");
  Require(library.Destroy(runtime, &error) == SENDER_OK,
          "Same-module destruction");
  runtime = nullptr;
  extended.version = 999;
  Require(
      library.Create(extended, &runtime, &error) == SENDER_INPUT && !runtime,
      "Bad create version accepted");
  extended.version = SENDER_ABI_VERSION;
  extended.size = 1;
  Require(library.Create(extended, &runtime, &error) == SENDER_INPUT,
          "Short create options accepted");
  extended.size = sizeof(extended);
  Require(library.Create(extended, &runtime, &error) == SENDER_OK,
          "Create runtime for outstanding notification cleanup");
  Tests::ReceiverFixture receiver;
  sender_target target{"127.0.0.1",
                       static_cast<uint16_t>(receiver.port),
                       "AA:BB:CC:DD:EE:FF",
                       "Loopback-only lifecycle",
                       (1ULL << 9) | (1ULL << 41) | (1ULL << 48),
                       0,
                       50};
  sender_audio_format format{44100, 16, 2};
  uint64_t generation{};
  Require(library.Connect(runtime, &target, &format, nullptr, nullptr,
                          &generation, &error) == SENDER_OK,
          "Queued connect owns live runtime");
  Require(library.Destroy(runtime, &error) == SENDER_STATE && runtime,
          "Failed destruction preserves valid live handle");
  Require(library.End(runtime, generation, &error) == SENDER_OK,
          "Queued end after rejected destroy");
  bool destroyed = false;
  for (unsigned i = 0; i < 300 && !destroyed; ++i) {
    library.Poll(runtime, 10, &error);
    if (library.Destroy(runtime, &error) == SENDER_OK) {
      runtime = nullptr;
      destroyed = true;
    }
  }
  Require(destroyed && !runtime,
          "Drain callbacks then same-module destruction succeeds");
  Require(
      library.HasExport("sender_connect") && library.HasExport("sender_submit"),
      "Lifecycle exports absent");
  Require(library.HasExport("sender_set_volume") &&
              library.HasExport("sender_get_receiver_volume") &&
              library.HasExport("sender_set_now_playing") &&
              library.HasExport("sender_retarget_presentation"),
          "Dynamic volume and timeline retarget exports are required");
}
