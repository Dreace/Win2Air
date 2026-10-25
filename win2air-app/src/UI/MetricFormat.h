#pragma once
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace Win2Air {
inline std::wstring FormatCount(double value) {
  auto number = std::llround(value);
  bool negative = number < 0;
  auto digits = std::to_wstring(negative ? -number : number);
  std::wstring grouped;
  int placed = 0;
  for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit) {
    if (placed && placed % 3 == 0) grouped.push_back(L',');
    grouped.push_back(*digit);
    ++placed;
  }
  std::reverse(grouped.begin(), grouped.end());
  if (negative) grouped.insert(grouped.begin(), L'-');
  return grouped;
}
inline std::wstring FormatQuantity(double value, std::wstring_view unit) {
  if (unit.empty())
    throw std::invalid_argument("A displayed quantity requires a unit");
  return FormatCount(value) + L" " + std::wstring(unit);
}
inline std::wstring FormatBytes(double bytes) {
  struct Unit {
    double size;
    wchar_t const* suffix;
  };
  for (auto unit : {Unit{1e9, L" GB"}, Unit{1e6, L" MB"}, Unit{1e3, L" KB"}}) {
    if (std::abs(bytes) >= unit.size)
      return std::format(L"{:.1f}{}", bytes / unit.size, unit.suffix);
  }
  return FormatCount(bytes) + L" B";
}
inline std::wstring FormatBitrate(double bits_per_second) {
  if (std::abs(bits_per_second) >= 1000000)
    return std::format(L"{:.2f} Mbps", bits_per_second / 1000000);
  return std::format(L"{:.0f} kbps", bits_per_second / 1000);
}
inline std::wstring FormatPercent(double fraction) {
  return std::format(L"{:.0f}%", fraction * 100);
}
inline std::wstring FormatRate(double per_second) {
  if (std::abs(per_second - std::round(per_second)) < 0.05)
    return std::format(L"{:.0f} /s", per_second);
  return std::format(L"{:.1f} /s", per_second);
}
struct AxisScale {
  double maximum;
  double step;
  double divisor;
  std::wstring unit;
};
inline double NiceStep(double rough) {
  double safe = std::max(rough, 1e-9);
  double exponent = std::pow(10.0, std::floor(std::log10(safe)));
  double fraction = safe / exponent;
  double nice = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
  return nice * exponent;
}
inline AxisScale BitrateAxis(double bits_per_second) {
  double step = NiceStep(std::max(bits_per_second, 1.0) / 4.0);
  double maximum = step * 4;
  if (maximum >= 1000000) return {maximum, step, 1000000, L"Mbps"};
  return {maximum, step, 1000, L"kbps"};
}
inline AxisScale RateAxis(double rate) {
  double step = std::max(1.0, NiceStep(std::max(rate, 1.0) / 4.0));
  return {step * 4, step, 1, L"/s"};
}
inline std::wstring FormatAxisTick(double value, AxisScale const& scale) {
  if (value <= 0) return L"0";
  if (scale.unit == L"/s") return std::format(L"{:.0f} /s", value);
  if (scale.unit == L"Mbps" && value < 1000000)
    return std::format(L"{:.0f} kbps", value / 1000);
  double scaled = value / scale.divisor;
  bool whole = std::abs(scaled - std::round(scaled)) < 0.05;
  if (scale.unit == L"Mbps" || !whole)
    return std::format(L"{:.1f} {}", scaled, scale.unit);
  return std::format(L"{:.0f} {}", scaled, scale.unit);
}
inline std::vector<unsigned> ChartTicks(unsigned window_seconds) {
  if (window_seconds == 600) return {600, 480, 360, 240, 120, 0};
  std::vector<unsigned> ticks;
  for (unsigned index = 0; index <= 6; ++index)
    ticks.push_back(window_seconds * (6 - index) / 6);
  return ticks;
}
inline std::wstring FormatWindowAge(unsigned seconds, unsigned window) {
  if (seconds == 0) return L"0s";
  if (window >= 120 && seconds % 60 == 0)
    return std::to_wstring(seconds / 60) + L" min";
  return std::to_wstring(seconds) + L"s";
}
inline std::wstring FormatAge(unsigned seconds) {
  if (seconds == 0) return L"0s";
  if (seconds % 3600 == 0) return std::to_wstring(seconds / 3600) + L" h";
  if (seconds >= 120 && seconds % 60 == 0)
    return std::to_wstring(seconds / 60) + L" min";
  return std::to_wstring(seconds) + L"s";
}
}  // namespace Win2Air
