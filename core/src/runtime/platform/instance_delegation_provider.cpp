#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"

#include <array>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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
std::string cache_key(const std::string& instance, t::Permission permission) {
  return instance + "|" + std::to_string(static_cast<int>(permission));
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

RuntimeInstanceDelegationProvider::~RuntimeInstanceDelegationProvider() = default;

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

t::Outcome<ExchangedDelegation>
RuntimeInstanceDelegationProvider::exchange(const t::SchedulerTaskContext& context,
                                            t::Permission permission) {
  if (context.owner.value.empty() || context.installation.value.empty() ||
      (permission != t::Permission::TaskSchedule && permission != t::Permission::TaskRead)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto parent = options_.source->acquire(context, permission);
  if (const auto* rejected = std::get_if<t::Failure>(&parent)) {
    return *rejected;
  }
  auto& delegation = std::get<InstanceParentDelegation>(parent);
  if (delegation.expires_at <= delegation_provider_now()) {
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
  return options_.identity->exchange_delegation(delegation.token, t::Service::Scheduler, scope,
                                                {permission}, leaf_fingerprint_);
}

t::Outcome<std::string>
RuntimeInstanceDelegationProvider::scheduler_task_delegation(const t::SchedulerTaskContext& context,
                                                             t::Permission permission) {
  if (context.owner.value.empty() || context.installation.value.empty() ||
      (permission != t::Permission::TaskSchedule && permission != t::Permission::TaskRead)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto key = cache_key(context.owner.value, permission);
  const auto now = delegation_provider_now();
  {
    std::lock_guard lock(mutex_);
    const auto found = cache_.find(key);
    if (found != cache_.end() && found->second.expires_at > now) {
      return found->second.proof;
    }
  }
  auto exchanged = exchange(context, permission);
  if (const auto* rejected = std::get_if<t::Failure>(&exchanged)) {
    return *rejected;
  }
  const auto& delegation = std::get<ExchangedDelegation>(exchanged);
  const auto expires_at = std::min({now + options_.cache_ttl, delegation.expires_at,
                                    delegation_provider_now() + std::chrono::seconds{56}});
  if (expires_at <= delegation_provider_now()) {
    return unauthenticated();
  }
  {
    std::lock_guard lock(mutex_);
    cache_[key] = Cached{delegation.proof, expires_at};
    if (cache_.size() > 256) {
      cache_.clear();
      cache_[key] = Cached{delegation.proof, expires_at};
    }
  }
  return delegation.proof;
}

} // namespace kiarabot::core::platform
