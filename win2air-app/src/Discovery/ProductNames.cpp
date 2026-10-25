#include "ProductNames.h"
#include <cctype>
#include <unordered_map>

namespace Win2Air {
namespace {
struct ProductNameEntry {
  char const* id;
  char const* manufacturer;
  char const* en;
  char const* zh;
};
ProductNameEntry const kProductNames[] = {
#include "ReceiverModels.inc"
};
struct BrandAlias {
  char const* alias;
  char const* brand;
};
BrandAlias const kBrands[] = {
#include "ReceiverBrands.inc"
};
std::string Normalize(std::string_view text) {
  std::string result;
  bool space = false;
  for (unsigned char value : text) {
    if (std::isspace(value)) {
      space = !result.empty();
      continue;
    }
    if (space) result += ' ';
    result += static_cast<char>(std::tolower(value));
    space = false;
  }
  return result;
}
}  // namespace
std::string ProductName(std::string_view identifier, bool chinese,
                        std::string_view manufacturer) {
  static auto const products = [] {
    std::unordered_map<std::string, ProductNameEntry const*> result;
    for (auto const& product : kProductNames)
      result.emplace(
          Normalize(product.manufacturer) + "|" + Normalize(product.id),
          &product);
    return result;
  }();
  static auto const brands = [] {
    std::unordered_map<std::string, std::string> result;
    for (auto const& alias : kBrands)
      result.emplace(Normalize(alias.alias), Normalize(alias.brand));
    return result;
  }();
  auto id = Normalize(identifier);
  if (id.empty()) return std::string(identifier);
  auto found = products.find("apple|" + id);
  if (found == products.end() && !manufacturer.empty()) {
    auto brand = Normalize(manufacturer);
    if (auto alias = brands.find(brand); alias != brands.end())
      brand = alias->second;
    found = products.find(brand + "|" + id);
  }
  // Model labels describe identity. Audio support still requires probing.
  return found == products.end() ? std::string(identifier)
         : chinese               ? found->second->zh
                                 : found->second->en;
}
}  // namespace Win2Air
