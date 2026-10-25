#pragma once
#include <string>
#include <string_view>
namespace Win2Air {
std::string ProductName(std::string_view identifier, bool chinese,
                        std::string_view manufacturer = {});
}
