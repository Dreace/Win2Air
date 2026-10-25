#pragma once
#include <memory>
#include "Activation.h"
namespace Win2Air {
class InstanceBackend {
 public:
  virtual ~InstanceBackend() = default;
  virtual bool Claim(std::wstring const& key) = 0;
  virtual void Redirect(ActivationRequest const& request) = 0;
};
std::wstring InstanceKey(std::wstring const& user_sid,
                         std::wstring const& identity);
bool ClaimInstance(InstanceBackend& backend, std::wstring const& user_sid,
                   std::wstring const& identity,
                   ActivationRequest const& request);
class NativeInstance {
  struct Impl;
  std::shared_ptr<Impl> impl_;

 public:
  NativeInstance();
  ~NativeInstance();
  bool Primary() const;
  ActivationRequest Initial() const;
  std::shared_ptr<ActivationQueue> Queue() const;
  std::string TakeError();
};
}  // namespace Win2Air
