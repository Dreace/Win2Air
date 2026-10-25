#pragma once
#include "../Settings/AppSettings.h"
namespace Win2Air {
class ReceiverRegistry {
  std::vector<ReceiverRecord> records_;

 public:
  explicit ReceiverRegistry(std::vector<ReceiverRecord> saved = {});
  void Merge(ReceiverRecord observed, ReceiverEndpoint endpoint);
  void RemoveEndpoint(std::string const& identity, std::string const& address,
                      unsigned port);
  void Authenticate(std::string const& selected,
                    std::string const& authenticated);
  ReceiverRecord const& Find(std::string const& identity) const;
  void Update(ReceiverRecord record);
  void Remove(std::string identity);
  void ForgetAuthentication(std::string const& identity);
  std::vector<ReceiverRecord> const& Records() const { return records_; }
};
}  // namespace Win2Air
