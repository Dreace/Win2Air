#pragma once
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// Shared between setup_native.cpp and its support translation units; call
// sites keep the unqualified names the single translation unit used before
// the split.
void check(HRESULT hr, char const *operation);
void validate_icon(std::filesystem::path const &path, UINT size);
struct WindowSearch {
  DWORD process;
  bool visible;
  HWND window{};
};
BOOL CALLBACK find_window(HWND hwnd, LPARAM data);
bool session_locked();
void require_visible_desktop();
void capture_app_window(std::filesystem::path const &path,
                        HWND selected = nullptr,
                        std::filesystem::path const &expected_executable = {});
bool exercise_shell(HWND window, unsigned &step,
                    std::filesystem::path const &screenshot,
                    std::filesystem::path const &executable);
bool exercise_information_page(HWND window, unsigned &step, bool about);
bool exercise_tray(HWND window, unsigned &step);
extern std::optional<bool> expected_caption_dark;
void run_executable(std::filesystem::path const &executable,
                    std::filesystem::path const &screenshot,
                    bool close_early = false, bool full_flow = false,
                    bool statistics = false, bool about = false,
                    bool tray = false);
void activate(std::wstring const &aumid,
              std::wstring const &arguments = L"--smoke-window",
              unsigned timeout = 15,
              std::filesystem::path const &screenshot = {});
void publish_media_fixture(std::filesystem::path const &artwork);
void require_store_png(std::filesystem::path const &path);
void run_store_screenshots(std::filesystem::path const &executable,
                           std::filesystem::path const &output);
