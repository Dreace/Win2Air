#include <iostream>
#include "../../src/Streaming/LatencyController.h"
#include "../contract/test_support.h"
namespace {
struct MarkerMeasurement {
  int64_t observed_ns{}, target_ns{}, difference_ns{};
  uint64_t uncertainty_ns{};
  bool within_tolerance{};
};
MarkerMeasurement Analyze(uint64_t source, uint64_t receiver, int base,
                          int offset, uint64_t uncertainty) {
  Win2Air::RequestedPresentation("custom", base, offset);
  if (!source || receiver < source || receiver - source > 60000000000 ||
      uncertainty > 60000000000)
    throw std::runtime_error(
        "Marker measurement requires aligned monotonic timestamps and bounded "
        "uncertainty");
  MarkerMeasurement result;
  result.observed_ns = static_cast<int64_t>(receiver - source);
  result.target_ns = int64_t(base + offset) * 1000000;
  result.difference_ns = result.observed_ns - result.target_ns;
  result.uncertainty_ns = uncertainty;
  result.within_tolerance =
      uint64_t(std::abs(result.difference_ns)) + uncertainty <= 10000000;
  return result;
}
uint64_t Unsigned(wchar_t const* input) {
  std::wstring text(input);
  size_t used{};
  if (text.empty() ||
      text.find_first_not_of(L"0123456789") != std::wstring::npos)
    throw std::runtime_error(
        "Marker arguments require unsigned decimal integers");
  auto number = std::stoull(text, &used);
  if (used != text.size())
    throw std::runtime_error("Marker numeric argument has trailing input");
  return number;
}
}  // namespace
void TestPresentationMeasurement() {
  auto positive = Analyze(1000000000, 1870000000, 750, 120, 2000000);
  Require(
      positive.within_tolerance && positive.target_ns == 870000000 &&
          positive.difference_ns == 0,
      "Independent aligned markers include positive compensation exactly once");
  auto negative = Analyze(1000000000, 1630000000, 750, -120, 2000000);
  Require(negative.within_tolerance && negative.observed_ns == 630000000,
          "Independent aligned markers measure earlier presentation with "
          "negative compensation");
  Require(!Analyze(1000000000, 1880000000, 750, 120, 2000000).within_tolerance,
          "Measurement uncertainty is included when qualifying the "
          "compensation error");
  Require(!Analyze(1000000000, 1870000000, 750, 120, 11000000).within_tolerance,
          "Insufficient measurement precision never qualifies as verified "
          "compensation");
  Rejects([&] { Analyze(0, 1000000000, 750, 0, 0); },
          "Absent source marker is unavailable, rather than a zero latency");
  Rejects([&] { Analyze(1000000000, 999999999, 750, 0, 0); },
          "Unaligned marker clocks cannot establish output delay");
}
int MeasurePresentation(wchar_t** arguments) {
  auto source = Unsigned(arguments[0]), receiver = Unsigned(arguments[1]);
  auto base = Unsigned(arguments[2]);
  size_t used{};
  auto offset = std::stoi(arguments[3], &used);
  if (used != std::wstring(arguments[3]).size() || base > 5000)
    throw std::runtime_error("Invalid presentation measurement target");
  auto measurement = Analyze(source, receiver, static_cast<int>(base), offset,
                             Unsigned(arguments[4]));
  for (unsigned index = 5; index < 8; ++index)
    if (std::wstring(arguments[index]).empty() ||
        std::wstring(arguments[index]).size() > 256 ||
        std::wstring(arguments[index]).find_first_of(L";=\r\n") !=
            std::wstring::npos)
      throw std::runtime_error(
          "Measurement requires method, source and receiver descriptions");
  std::wcout << L"method=" << arguments[5] << L"; source=" << arguments[6]
             << L"; receiver=" << arguments[7]
             << L"; provenance=caller-supplied-aligned-markers; target_ns="
             << measurement.target_ns << L"; observed_ns="
             << measurement.observed_ns << L"; difference_ns="
             << measurement.difference_ns << L"; uncertainty_ns="
             << measurement.uncertainty_ns << L"; within_tolerance="
             << measurement.within_tolerance << L'\n';
  return measurement.within_tolerance ? 0 : 1;
}
