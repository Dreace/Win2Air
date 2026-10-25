#pragma once
#include "PresentationConfig.h"
namespace Win2Air {
PresentationConfig RequestedPresentation(std::string mode, int custom_ms,
                                         int offset_ms);
}
