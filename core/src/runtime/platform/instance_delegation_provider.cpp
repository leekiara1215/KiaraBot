#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"

#include <array>
#include <new>
#include <optional>
#include <string_view>
#include <future>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
std::string cache_key(std::string_view kind, const std::string& instance,
                      const std::string& installation, std::string_view model,
                      t::Permission permission) {
  return std::string{kind} + "|" + instance + "|" + installation + "|" + std::string{model} + "|" +
         std::to_string(static_cast<int>(permission));
}
std::optional<t::Sha256Digest> certificate_fingerprint(std::string_view pem) {
  BIO* raw = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (raw == nullptr) {
    return std::nullopt;
  }
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw, BIO_free);
  X509* raw_cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
  if (raw_cert == nullptr) {
    return std::nullopt;
  }
  std::unique_ptr<X509, decltype(&X509_free)> certificate(raw_cert, X509_free);
  t::Sha256Digest digest;
  unsigned int size = 0;
  if (X509_digest(certificate.get(), EVP_sha256(),
                  reinterpret_cast<unsigned char*>(digest.bytes.data()), &size) != 1 ||
      size != digest.bytes.size()) {
    return std::nullopt;
  }
  return digest;
}
} // namespace

target::TimePoint delegation_provider_now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

RuntimeInstanceDelegationProvider::RuntimeInstanceDelegationProvider(
    InstanceDelegationProviderOptions options, t::Sha256Digest leaf_fingerprint)
    : options_(std::move(options)), leaf_fingerprint_(leaf_fingerprint) {}

namespace {
// F-5 best-effort: cached child proofs are short-lived secrets; overwrite their buffer before
// releasing capacity. This cannot erase allocator copies or core dumps; deployment guidance still
// requires disabled core dumps / restricted ptrace on the host trust boundary.
void zeroize(std::string& secret) {
  if (!secret.empty()) {
    OPENSSL_cleanse(secret.data(), secret.size());
  }
  secret.clear();
}
} // namespace

RuntimeInstanceDelegationProvider::~RuntimeInstanceDelegationProvider() {
  std::lock_guard lock(mutex_);
  for (auto& [key, cached] : cache_) {
    (void)key;
    zeroize(cached.proof);
  }
  cache_.clear();
}

t::Outcome<std::unique_ptr<RuntimeInstanceDelegationProvider>>
RuntimeInstanceDelegationProvider::create(InstanceDelegationProviderOptions options) {
  if (!options.source || options.service_certificate_pem.empty() || options.identity == nullptr ||
      options.cache_ttl <= std::chrono::milliseconds::zero() ||
      options.cache_ttl > std::chrono::seconds{56}) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto fingerprint = certificate_fingerprint(options.service_certificate_pem);
  if (!fingerprint) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<RuntimeInstanceDelegationProvider>(
        new RuntimeInstanceDelegationProvider(std::move(options), *fingerprint));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

t::Outcome<std::string> RuntimeInstanceDelegationProvider::single_flight(
    const std::string& key, const std::function<t::Outcome<std::string>()>& loader) {
  std::shared_future<t::Outcome<std::string>> waiter;
  std::promise<t::Outcome<std::string>> promise;
  bool leader = false;
  {
    std::lock_guard lock(mutex_);
    const auto found = inflight_.find(key);
    if (found != inflight_.end()) {
      waiter = found->second;
    } else {
      waiter = promise.get_future().share();
      inflight_.emplace(key, waiter);
      leader = true;
    }
  }
  if (!leader) {
    return waiter.get();
  }
  t::Outcome<std::string> result = failure(t::ErrorCode::Internal);
  try {
    result = loader();
  } catch (const std::bad_alloc&) {
    result = failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    result = failure(t::ErrorCode::Internal);
  }
  {
    std::lock_guard lock(mutex_);
    inflight_.erase(key);
  }
  promise.set_value(result);
  return result;
}

t::Outcome<std::string>
RuntimeInstanceDelegationProvider::scheduler_task_delegation(const t::SchedulerTaskContext& context,
                                                             t::Permission permission) {
  if (context.owner.value.empty() || context.installation.value.empty() ||
      (permission != t::Permission::TaskSchedule && permission != t::Permission::TaskRead)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto key =
      cache_key("task", context.owner.value, context.installation.value, {}, permission);
  const auto now = delegation_provider_now();
  {
    std::lock_guard lock(mutex_);
    const auto found = cache_.find(key);
    if (found != cache_.end() && found->second.expires_at > now) {
      return found->second.proof;
    }
  }
  return single_flight(key, [this, &context, permission, key]() -> t::Outcome<std::string> {
    {
      std::lock_guard lock(mutex_);
      const auto found = cache_.find(key);
      if (found != cache_.end() && found->second.expires_at > delegation_provider_now()) {
        return found->second.proof;
      }
    }
    auto parent = options_.source->acquire(context, permission);
    if (const auto* rejected = std::get_if<t::Failure>(&parent)) {
      return *rejected;
    }
    auto& delegation = std::get<InstanceParentDelegation>(parent);
    const auto started = delegation_provider_now();
    if (delegation.expires_at <= started) {
      return unauthenticated();
    }
    if (delegation.token.empty() || delegation.token.size() > 32768) {
      return unauthenticated();
    }
    for (const unsigned char character : delegation.token) {
      if (character < 33 || character > 126) {
        return unauthenticated();
      }
    }
    std::vector<t::ResourceRef> scope{
        t::ResourceRef{t::ResourceKind::Instance, t::ResourceId{context.owner.value}},
        t::ResourceRef{t::ResourceKind::Installation, t::ResourceId{context.installation.value}}};
    auto exchanged = options_.identity->exchange_delegation(
        delegation.token, t::Service::Scheduler, scope, {permission}, leaf_fingerprint_);
    if (const auto* rejected = std::get_if<t::Failure>(&exchanged)) {
      return *rejected;
    }
    const auto& child = std::get<ExchangedDelegation>(exchanged);
    const auto expires_at =
        std::min({started + options_.cache_ttl, child.expires_at, delegation.expires_at,
                  delegation_provider_now() + std::chrono::seconds{56}});
    if (expires_at <= delegation_provider_now()) {
      return unauthenticated();
    }
    {
      std::lock_guard lock(mutex_);
      auto& slot = cache_[key];
      zeroize(slot.proof);
      slot = Cached{child.proof, expires_at, context.installation.value};
      if (cache_.size() > 256) {
        for (auto& [old_key, old] : cache_) {
          (void)old_key;
          zeroize(old.proof);
        }
        cache_.clear();
        cache_[key] = Cached{child.proof, expires_at, context.installation.value};
      }
    }
    return child.proof;
  });
}

t::Outcome<std::string> RuntimeInstanceDelegationProvider::model_chat_delegation(
    const t::SchedulerTaskContext& context, std::string_view model_id, t::Permission permission) {
  if (context.owner.value.empty() || context.installation.value.empty() || model_id.empty() ||
      (permission != t::Permission::ModelChat && permission != t::Permission::ModelCallInspect)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto key =
      cache_key("model", context.owner.value, context.installation.value, model_id, permission);
  const auto now = delegation_provider_now();
  {
    std::lock_guard lock(mutex_);
    const auto found = cache_.find(key);
    if (found != cache_.end() && found->second.expires_at > now) {
      return found->second.proof;
    }
  }
  return single_flight(key, [this, &context, model_id, permission, key]()
                           -> t::Outcome<std::string> {
    {
      std::lock_guard lock(mutex_);
      const auto found = cache_.find(key);
      if (found != cache_.end() && found->second.expires_at > delegation_provider_now()) {
        return found->second.proof;
      }
    }
    auto parent = options_.source->acquire_model(context, model_id, permission);
    if (const auto* rejected = std::get_if<t::Failure>(&parent)) {
      return *rejected;
    }
    auto& delegation = std::get<InstanceParentDelegation>(parent);
    const auto started = delegation_provider_now();
    if (delegation.expires_at <= started || delegation.token.empty() ||
        delegation.token.size() > 32768) {
      return unauthenticated();
    }
    for (const unsigned char character : delegation.token) {
      if (character < 33 || character > 126) {
        return unauthenticated();
      }
    }
    std::vector<t::ResourceRef> scope{
        t::ResourceRef{t::ResourceKind::Model, t::ResourceId{std::string{model_id}}}};
    auto exchanged = options_.identity->exchange_delegation(
        delegation.token, t::Service::Model, scope, {permission}, leaf_fingerprint_);
    if (const auto* rejected = std::get_if<t::Failure>(&exchanged)) {
      return *rejected;
    }
    const auto& child = std::get<ExchangedDelegation>(exchanged);
    const auto expires_at =
        std::min({started + options_.cache_ttl, child.expires_at, delegation.expires_at,
                  delegation_provider_now() + std::chrono::seconds{56}});
    if (expires_at <= delegation_provider_now()) {
      return unauthenticated();
    }
    {
      std::lock_guard lock(mutex_);
      auto& slot = cache_[key];
      zeroize(slot.proof);
      slot = Cached{child.proof, expires_at, context.installation.value};
      if (cache_.size() > 256) {
        for (auto& [old_key, old] : cache_) {
          (void)old_key;
          zeroize(old.proof);
        }
        cache_.clear();
        cache_[key] = Cached{child.proof, expires_at, context.installation.value};
      }
    }
    return child.proof;
  });
}

} // namespace kiarabot::core::platform
