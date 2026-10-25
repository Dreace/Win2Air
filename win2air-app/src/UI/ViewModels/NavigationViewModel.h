#pragma once
#include "../../App/Activation.h"
namespace Win2Air {
class NavigationViewModel {
  AppPage page_{AppPage::playback};

 public:
  AppPage Page() const { return page_; }
  void Navigate(AppPage page);
};
}  // namespace Win2Air
