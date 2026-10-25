#include "SenderLibrary.h"

#include <cstring>
#include <stdexcept>
namespace Win2Air {
template <class T>
T SenderLibrary::Resolve(char const* name) {
  auto result = GetProcAddress(module_, name);
  if (!result)
    throw std::runtime_error(std::string("Required sender export missing: ") +
                             name);
  return reinterpret_cast<T>(result);
}
SenderLibrary::SenderLibrary(std::filesystem::path const& path) {
  if (!path.is_absolute() || path.filename() != L"airplay2_sender.dll" ||
      !std::filesystem::is_regular_file(path))
    throw std::runtime_error(
        "Sender DLL requires an absolute package-controlled path");
  module_ = LoadLibraryExW(
      path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module_)
    throw std::runtime_error("Load sender DLL failed: " +
                             std::to_string(GetLastError()));
  try {
    wchar_t loaded[32768]{};
    auto length = GetModuleFileNameW(module_, loaded, 32768);
    if (!length || length >= 32768 ||
        !std::filesystem::equivalent(path, std::filesystem::path(loaded)))
      throw std::runtime_error(
          "Loaded DLL path differs from package-controlled input");
    query_ = Resolve<decltype(query_)>("sender_get_abi");
    create_ = Resolve<decltype(create_)>("sender_create_ex");
    connect_ = Resolve<decltype(connect_)>("sender_connect");
    poll_ = Resolve<decltype(poll_)>("sender_poll");
    end_ = Resolve<decltype(end_)>("sender_end");
    destroy_ = Resolve<decltype(destroy_)>("sender_destroy");
    probe_ = Resolve<decltype(probe_)>("sender_probe_target");
    submit_ = Resolve<decltype(submit_)>("sender_submit_timed");
    flush_ = Resolve<decltype(flush_)>("sender_flush");
    state_ = Resolve<decltype(state_)>("sender_get_state");
    timing_ = Resolve<decltype(timing_)>("sender_get_timing");
    snapshot_ = Resolve<decltype(snapshot_)>("sender_get_snapshot");
    volume_ = Resolve<decltype(volume_)>("sender_set_volume");
    receiver_volume_ =
        Resolve<decltype(receiver_volume_)>("sender_get_receiver_volume");
    media_command_ =
        Resolve<decltype(media_command_)>("sender_take_media_command");
    now_playing_ = Resolve<decltype(now_playing_)>("sender_set_now_playing");
    presentation_ = Resolve<decltype(presentation_)>("sender_set_presentation");
    retarget_ = Resolve<decltype(retarget_)>("sender_retarget_presentation");
    for (auto name : {"sender_create", "sender_validate_target",
                      "sender_submit", "sender_flush", "sender_get_state"})
      Resolve<FARPROC>(name);
    auto expected = sender_header_abi();
    if (query_(SENDER_ABI_VERSION, sizeof(abi_), &abi_) != SENDER_OK ||
        std::memcmp(&abi_, &expected, sizeof(abi_)))
      throw std::runtime_error(
          "Sender DLL ABI version, architecture or public layout mismatch");
  } catch (...) {
    FreeLibrary(module_);
    module_ = nullptr;
    throw;
  }
}
SenderLibrary::~SenderLibrary() {
  if (module_ && !retain_module_) FreeLibrary(module_);
}
bool SenderLibrary::HasExport(char const* name) const noexcept {
  return GetProcAddress(module_, name) != nullptr;
}
}  // namespace Win2Air
