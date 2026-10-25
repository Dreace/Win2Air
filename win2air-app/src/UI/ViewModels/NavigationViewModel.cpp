#include "NavigationViewModel.h"
#include <stdexcept>
namespace Win2Air {
void NavigationViewModel::Navigate(AppPage page) {
  if (page != AppPage::playback && page != AppPage::devices &&
      page != AppPage::statistics && page != AppPage::settings)
    throw std::runtime_error("Navigation target is unsupported");
  page_ = page;
}
}  // namespace Win2Air
