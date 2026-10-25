#include <windows.h>
#undef GetCurrentTime

#include <dwrite.h>
#include <psapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "../../src/UI/LocalizedResources.h"

using namespace Win2Air;
using namespace winrt::Microsoft::Windows::ApplicationModel::Resources;
namespace {
size_t PrivateBytes() {
  PROCESS_MEMORY_COUNTERS_EX value{};
  winrt::check_bool(GetProcessMemoryInfo(
      GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),
      sizeof(value)));
  return value.PrivateUsage;
}
void CheckTextMemory(LocalizedResources const& resources) {
  winrt::com_ptr<IDWriteFactory> factory;
  winrt::check_hresult(
      DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                          reinterpret_cast<IUnknown**>(factory.put())));
  for (auto language : {"zh-CN", "en-US"}) {
    resource_language = language;
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::check_hresult(factory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
        resources.TextLanguage().c_str(), format.put()));
    auto draw = [&](int index) {
      auto text = L"\u5ba2\u5385 \u5df2\u8fde\u63a5 " + std::to_wstring(index);
      winrt::com_ptr<IDWriteTextLayout> layout;
      winrt::check_hresult(factory->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format.get(), 400,
          100, layout.put()));
      DWRITE_TEXT_METRICS metrics{};
      winrt::check_hresult(layout->GetMetrics(&metrics));
    };
    for (int index = 0; index < 1000; ++index) draw(index);
    auto before = PrivateBytes();
    for (int index = 0; index < 20000; ++index) draw(index);
    auto after = PrivateBytes();
    auto growth = after > before ? after - before : 0;
    std::cout << "text_language=" << language
              << " layouts=20000 private_growth_bytes=" << growth << '\n';
    if (growth > 2 * 1024 * 1024)
      throw std::runtime_error("Repeated text layout retains native memory");
  }
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2)
      throw std::invalid_argument("App runtime directory required");
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto root = std::filesystem::absolute(argv[1]);
    auto module = LoadLibraryExW(
        (root / L"Microsoft.Windows.ApplicationModel.Resources.dll").c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    winrt::check_bool(module);
    auto activate = reinterpret_cast<HRESULT(__stdcall*)(void*, void**)>(
        GetProcAddress(module, "DllGetActivationFactory"));
    winrt::check_bool(activate != nullptr);
    winrt::Windows::Foundation::IActivationFactory factory{nullptr};
    winrt::hstring name{
        L"Microsoft.Windows.ApplicationModel.Resources.ResourceManager"};
    winrt::check_hresult(
        activate(winrt::get_abi(name), winrt::put_abi(factory)));
    auto create = factory.as<IResourceManagerFactory>();
    auto manager = create.CreateInstance((root / L"resources.pri").wstring());
    LocalizedResources resources(manager);
    CheckTextMemory(resources);
    resource_language = "en-US";
    auto english = resources.GetString(L"Connect");
    resource_language = "zh-CN";
    auto chinese = resources.GetString(L"Connect");
    if (english.empty() || chinese.empty() || english == chinese)
      throw std::runtime_error("Explicit resource language selection failed");
    bool rejected = false;
    try {
      resources.GetString(L"MissingResourceForMemoryRegression");
    } catch (winrt::hresult_error const&) {
      rejected = true;
    }
    if (!rejected) throw std::runtime_error("Missing resource was accepted");
    for (int index = 0; index < 1000; ++index) resources.GetString(L"Connect");
    auto before = PrivateBytes();
    constexpr int lookups = 1000000;
    for (int index = 0; index < lookups; ++index) {
      resource_language = index % 2 ? "en-US" : "zh-CN";
      auto expected = index % 2 ? english : chinese;
      if (resources.GetString(L"Connect") != expected)
        throw std::runtime_error("Resource language changed during lookup");
    }
    auto after = PrivateBytes();
    auto growth = after > before ? after - before : 0;
    std::cout << "lookups=" << lookups << " private_growth_bytes=" << growth
              << '\n';
    if (growth > 2 * 1024 * 1024)
      throw std::runtime_error(
          "Repeated resource lookup retains native memory");
    std::cout
        << "Resource language, missing key and memory regression passed\n";
    return 0;
  } catch (winrt::hresult_error const& error) {
    std::cerr << winrt::to_string(error.message()) << '\n';
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
