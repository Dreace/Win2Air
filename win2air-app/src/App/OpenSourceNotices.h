#pragma once
#include <string_view>

namespace Win2Air {
struct OpenSourceNotice {
  std::wstring_view name, license, project, terms;
};
inline constexpr std::wstring_view kProjectRepository{
    L"https://github.com/Dreace/Win2Air"};
inline constexpr std::wstring_view kProjectLicense{L"GPL-3.0-or-later"};
inline constexpr OpenSourceNotice kOpenSourceNotices[] = {
    {L"Win2Air", kProjectLicense, kProjectRepository,
     L"https://github.com/Dreace/Win2Air/blob/main/LICENSE"},
    {L"OwnTone", L"GPL-2.0-or-later",
     L"https://github.com/owntone/owntone-server",
     L"https://github.com/owntone/owntone-server/blob/master/COPYING"},
    {L"pair_ap", L"MIT", L"https://github.com/ejurgensen/pair_ap",
     L"https://github.com/ejurgensen/pair_ap/blob/master/LICENSE"},
    {L"libairptp", L"MIT", L"https://github.com/owntone/libairptp",
     L"https://github.com/owntone/libairptp/blob/master/LICENSE"},
    {L"evrtsp", L"BSD-3-Clause",
     L"https://github.com/owntone/owntone-server/tree/master/src/evrtsp",
     L"https://github.com/owntone/owntone-server/blob/master/src/evrtsp/"
     L"rtsp.c"},
    {L"libevent", L"BSD-3-Clause", L"https://libevent.org/",
     L"https://github.com/libevent/libevent/blob/release-2.1.12-stable/"
     L"LICENSE"},
    {L"libsodium", L"ISC", L"https://libsodium.org/",
     L"https://github.com/jedisct1/libsodium/blob/1.0.18/LICENSE"},
    {L"Libgcrypt", L"LGPL-2.1-or-later",
     L"https://gnupg.org/software/libgcrypt/",
     L"https://gnupg.org/documentation/manuals/gcrypt/Library-Copying.html"},
    {L"Libgpg-error", L"LGPL-2.1-or-later",
     L"https://gnupg.org/software/libgpg-error/",
     L"https://github.com/gpg/libgpg-error/blob/master/COPYING.LIB"},
    {L"libplist", L"LGPL-2.1-or-later",
     L"https://github.com/libimobiledevice/libplist",
     L"https://github.com/libimobiledevice/libplist/blob/master/"
     L"COPYING.LESSER"},
    {L"FFmpeg", L"LGPL-2.1-or-later", L"https://ffmpeg.org/",
     L"https://ffmpeg.org/legal.html"},
    {L"LiveCharts2", L"MIT", L"https://github.com/Live-Charts/LiveCharts2",
     L"https://github.com/Live-Charts/LiveCharts2/blob/2.0.4/LICENSE"},
    {L"SkiaSharp / HarfBuzzSharp", L"MIT", L"https://github.com/mono/SkiaSharp",
     L"https://github.com/mono/SkiaSharp/blob/main/LICENSE.md"},
    {L"Skia", L"BSD-3-Clause", L"https://skia.org/",
     L"https://skia.googlesource.com/skia/+/main/LICENSE"},
    {L"HarfBuzz", L"MIT-Old", L"https://harfbuzz.github.io/",
     L"https://github.com/harfbuzz/harfbuzz/blob/main/COPYING"},
    {L"ANGLE", L"BSD-3-Clause",
     L"https://chromium.googlesource.com/angle/angle/",
     L"https://chromium.googlesource.com/angle/angle/+/main/LICENSE"},
    {L"C++/WinRT", L"MIT", L"https://github.com/microsoft/cppwinrt",
     L"https://github.com/microsoft/cppwinrt/blob/master/LICENSE"},
    {L"C#/WinRT", L"MIT", L"https://github.com/microsoft/CsWinRT",
     L"https://github.com/microsoft/CsWinRT/blob/master/LICENSE"},
    {L".NET", L"MIT", L"https://github.com/dotnet/runtime",
     L"https://github.com/dotnet/runtime/blob/main/LICENSE.TXT"},
    {L"Windows App SDK", L"MIT", L"https://github.com/microsoft/WindowsAppSDK",
     L"https://github.com/microsoft/WindowsAppSDK/blob/main/LICENSE"},
    {L"WinUI", L"MIT", L"https://github.com/microsoft/microsoft-ui-xaml",
     L"https://github.com/microsoft/microsoft-ui-xaml/blob/main/LICENSE"},
};
}  // namespace Win2Air
