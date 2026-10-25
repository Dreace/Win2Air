#include "../src/Shell/TrayService.h"

#include <SystemMediaTransportControlsInterop.h>
#include <objbase.h>
#include <windows.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include "../src/App/SystemMediaSession.h"

#include <UIAutomation.h>
#include <appmodel.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <wtsapi32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#include "native_gui.h"

void check(HRESULT hr, char const *operation) {
  if (FAILED(hr))
    throw std::runtime_error(std::string(operation) + " HRESULT=" +
                             std::to_string(static_cast<unsigned long>(hr)));
}
void validate_icon(std::filesystem::path const &path, UINT size) {
  ComPtr<IWICImagingFactory> factory;
  check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory)),
        "Create WIC factory");
  ComPtr<IWICBitmapDecoder> decoder;
  check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                           WICDecodeMetadataCacheOnLoad,
                                           &decoder),
        "Read application icon");
  ComPtr<IWICBitmapFrameDecode> frame;
  check(decoder->GetFrame(0, &frame), "Read application icon frame");
  UINT width{}, height{};
  check(frame->GetSize(&width, &height), "Read application icon size");
  if (width != size || height != size)
    throw std::runtime_error("Unexpected application icon dimensions: " +
                             path.string());
}
BOOL CALLBACK find_window(HWND hwnd, LPARAM data) {
  auto &search = *reinterpret_cast<WindowSearch *>(data);
  DWORD process{};
  GetWindowThreadProcessId(hwnd, &process);
  wchar_t title[128]{};
  GetWindowTextW(hwnd, title, 128);
  if (process == search.process && IsWindowVisible(hwnd) &&
      std::wstring(title) == L"Win2Air") {
    search.visible = true;
    search.window = hwnd;
  }
  return TRUE;
}
bool session_locked() {
  DWORD session{};
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
    throw std::runtime_error("Read validation session identity failed: " +
                             std::to_string(GetLastError()));

  LPWSTR data{};
  DWORD size{};
  if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session,
                                   WTSSessionInfoEx, &data, &size))
    throw std::runtime_error("Read validation session state failed: " +
                             std::to_string(GetLastError()));

  const auto state = reinterpret_cast<WTSINFOEXW const *>(data);
  const bool valid = data && size >= sizeof(WTSINFOEXW) && state->Level == 1;
  const auto flags = valid ? state->Data.WTSInfoExLevel1.SessionFlags
                           : WTS_SESSIONSTATE_UNKNOWN;
  WTSFreeMemory(data);
  if (!valid ||
      (flags != WTS_SESSIONSTATE_LOCK && flags != WTS_SESSIONSTATE_UNLOCK))
    throw std::runtime_error("Validation session lock state is unavailable");
  return flags == WTS_SESSIONSTATE_LOCK;
}
void require_visible_desktop() {
  if (session_locked())
    throw std::runtime_error(
        "UI visual validation is blocked: unlock the Windows desktop before "
        "capturing the App");
  if (!SetThreadExecutionState(ES_DISPLAY_REQUIRED))
    throw std::runtime_error("Reset display idle timer for GUI capture failed");
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_MOVE;
  if (SendInput(1, &input, sizeof(input)) != 1)
    throw std::runtime_error("Wake display for GUI capture failed: " +
                             std::to_string(GetLastError()));
}
void capture_app_window(std::filesystem::path const &path, HWND selected,
                        std::filesystem::path const &expected_executable) {
  require_visible_desktop();
  HWND window = selected ? selected : FindWindowW(nullptr, L"Win2Air");
  wchar_t title[128]{};
  GetWindowTextW(window, title, 128);
  DWORD process_id{};
  GetWindowThreadProcessId(window, &process_id);
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process)
    throw std::runtime_error("Inspect foreground capture target failed");
  UINT32 size{};
  auto status = GetPackageFullName(process, &size, nullptr);
  std::wstring package(size, L'\0');
  if (status == ERROR_INSUFFICIENT_BUFFER)
    status = GetPackageFullName(process, &size, package.data());
  bool expected_process =
      status == ERROR_SUCCESS && package.starts_with(L"Win2Air.Desktop_");
  if (!expected_executable.empty()) {
    wchar_t image[32768]{};
    DWORD image_size = 32768;
    expected_process =
        QueryFullProcessImageNameW(process, 0, image, &image_size) &&
        std::filesystem::equivalent(expected_executable, image);
  }
  CloseHandle(process);
  if (!expected_process || std::wstring(title) != L"Win2Air")
    throw std::runtime_error(
        "Window capture requires the foreground installed Win2Air window");
  ComPtr<IUIAutomation> automation;
  ComPtr<IUIAutomationElement> root;
  ComPtr<IUIAutomationCondition> condition;
  ComPtr<IUIAutomationElementArray> elements;
  check(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&automation)),
        "Create App accessibility inspector");
  check(automation->ElementFromHandle(window, &root),
        "Inspect App accessibility root");
  check(automation->CreateTrueCondition(&condition),
        "Inspect App control condition");
  check(root->FindAll(TreeScope_Descendants, condition.Get(), &elements),
        "Inspect App controls");
  int count{};
  elements->get_Length(&count);
  for (int i = 0; i < count && i < 128; ++i) {
    ComPtr<IUIAutomationElement> element;
    BSTR name{};
    if (SUCCEEDED(elements->GetElement(i, &element)) &&
        SUCCEEDED(element->get_CurrentName(&name)) && name) {
      std::wcout << L"App control: " << name << L'\n';
      SysFreeString(name);
    }
  }
  using namespace winrt::Windows::Graphics::Capture;
  if (!GraphicsCaptureSession::IsSupported())
    throw std::runtime_error("App window graphics capture is unavailable");
  auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  check(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(),
                                 winrt::put_abi(item)),
        "Select installed App window for capture");
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, device.put(), nullptr,
                          context.put()),
        "Create App capture graphics device");
  auto dxgi = device.as<IDXGIDevice>();
  winrt::com_ptr<::IInspectable> inspectable;
  check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()),
        "Project App capture graphics device");
  auto projected =
      inspectable
          .as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
      projected,
      winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
          B8G8R8A8UIntNormalized,
      2, item.Size());
  auto session = pool.CreateCaptureSession(item);
  session.IsCursorCaptureEnabled(false);
  session.StartCapture();
  Direct3D11CaptureFrame captured{nullptr};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!captured && std::chrono::steady_clock::now() < deadline) {
    captured = pool.TryGetNextFrame();
    if (!captured) Sleep(20);
  }
  if (!captured) {
    session.Close();
    pool.Close();
    throw std::runtime_error(
        "App graphics capture returned no frame before its deadline");
  }
  auto warmup =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < warmup) {
    auto next = pool.TryGetNextFrame();
    if (next) {
      captured.Close();
      captured = next;
    }
    Sleep(20);
  }
  auto access = captured.Surface()
                    .as<::Windows::Graphics::DirectX::Direct3D11::
                            IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  check(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()),
        "Read App capture texture");
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  description.BindFlags = 0;
  description.MiscFlags = 0;
  description.Usage = D3D11_USAGE_STAGING;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  winrt::com_ptr<ID3D11Texture2D> staging;
  check(device->CreateTexture2D(&description, nullptr, staging.put()),
        "Create capture readback texture");
  context->CopyResource(staging.get(), texture.get());
  D3D11_MAPPED_SUBRESOURCE pixels{};
  check(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &pixels),
        "Map App capture pixels");
  try {
    auto content = captured.ContentSize();
    auto width = static_cast<UINT>(content.Width),
         height = static_cast<UINT>(content.Height);
    if (!width || !height || width > description.Width ||
        height > description.Height || height > UINT_MAX / pixels.RowPitch)
      throw std::runtime_error("App capture content or byte bound is invalid");
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "Create screenshot encoder factory");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "Create screenshot stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
          "Open screenshot output");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
          "Create screenshot encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
          "Initialize screenshot encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(&frame, &properties),
          "Create screenshot frame");
    check(frame->Initialize(properties.Get()), "Initialize screenshot frame");
    check(frame->SetSize(width, height), "Set capture frame size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Set capture pixel format");
    check(frame->WritePixels(height, pixels.RowPitch, pixels.RowPitch * height,
                             static_cast<BYTE *>(pixels.pData)),
          "Write App capture pixels");
    check(frame->Commit(), "Commit screenshot frame");
    check(encoder->Commit(), "Commit screenshot encoder");
  } catch (...) {
    context->Unmap(staging.get(), 0);
    captured.Close();
    session.Close();
    pool.Close();
    throw;
  }
  context->Unmap(staging.get(), 0);
  captured.Close();
  session.Close();
  pool.Close();
}
