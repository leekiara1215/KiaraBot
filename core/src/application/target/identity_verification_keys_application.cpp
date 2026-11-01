// clang-format off
#include "kiarabot/core/application/target/identity_verification_keys_application.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;
using detail::failure;
bool catalog_uuid(const ResourceRef& resource) {
  const auto& value = resource.id.value;
  if (resource.kind != ResourceKind::Catalog || value.size() != 36 || value[14] != '4' ||
      std::string_view{"89ab"}.find(value[19]) == std::string_view::npos) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') {
        return false;
      }
    } else if (!((value[index] >= '0' && value[index] <= '9') ||
                 (value[index] >= 'a' && value[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}
Status valid_material(const VerificationKeySet& material) {
  if (material.keys.size() != 1 || material.version.value == 0 ||
      !catalog_uuid(material.keyset_resource)) {
    return failure(ErrorCode::Internal);
  }
  const auto& key = material.keys.front();
  if (!detail::bounded_text(key.reference.value) || key.version.value == 0 ||
      key.valid_until <= TimePoint{} || key.valid_until == TimePoint::max()) {
    return failure(ErrorCode::Internal);
  }
  return std::monostate{};
}
class Application final : public IdentityVerificationKeysApplication {
public:
  Application(IdentitySecurityUnitOfWorkFactory& transactions,
              VerificationKeyMaterialPort& material)
      : transactions_(transactions), material_(material) {}
  Outcome<GetVerificationKeysReply>
  get_verification_keys(const GetVerificationKeysRequest& request) override {
    try {
      return execute(request);
    } catch (const std::bad_alloc&) {
      return failure(ErrorCode::UpstreamUnavailable);
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }

private:
  Outcome<GetVerificationKeysReply> execute(const GetVerificationKeysRequest& request) {
    auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (request.known_version && request.known_version->value == 0) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto initial = read_current_peer(request);
    if (const auto* error = std::get_if<Failure>(&initial)) {
      return *error;
    }
    // The first readonly UoW has been destroyed; the material provider may invoke its crypto clock.
    auto snapshot = material_.snapshot();
    if (const auto* error = std::get_if<Failure>(&snapshot)) {
      return *error;
    }
    auto& material = std::get<VerificationKeySet>(snapshot);
    auto valid = valid_material(material);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto current = detail::read_peer(*work.transaction, request.caller);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(current);
    auto peer = detail::check_peer(authority, request.caller, work.now, false);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    auto authorized = authorize_exact(authority.subject.principal, Permission::IdentityKeysRead,
                                      material.keyset_resource, authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&authorized)) {
      return *error;
    }
    const auto& previous = std::get<detail::PeerAuthority>(initial);
    if (previous.credential.ref != authority.credential.ref ||
        previous.subject.principal.id != authority.subject.principal.id ||
        previous.subject.principal.version != authority.subject.principal.version ||
        previous.subject.authorization.version != authority.subject.authorization.version) {
      return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
    if (work.now >= material.keys.front().valid_until) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    auto checkpoint =
        detail::final_check(work, request.context,
                            std::min(authority.credential.expires_at.value_or(TimePoint::min()),
                                     request.caller.binding.certificate.not_after),
                            true);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    if (work.now >= material.keys.front().valid_until) {
      return failure(ErrorCode::UpstreamUnavailable);
    }
    // Authentication/current exact authorization precede the caller's version comparison.
    if (request.known_version && request.known_version->value > material.version.value) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return GetVerificationKeysReply{std::move(material), work.now};
  }
  Outcome<detail::PeerAuthority> read_current_peer(const GetVerificationKeysRequest& request) {
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& work = std::get<detail::Work>(opened);
    auto current = detail::read_peer(*work.transaction, request.caller);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(current);
    auto peer = detail::check_peer(authority, request.caller, work.now, false);
    if (const auto* error = std::get_if<Failure>(&peer)) {
      return *error;
    }
    return std::move(std::get<detail::PeerAuthority>(current));
  }
  IdentitySecurityUnitOfWorkFactory& transactions_;
  VerificationKeyMaterialPort& material_;
};
} // namespace
Outcome<std::unique_ptr<IdentityVerificationKeysApplication>>
make_identity_verification_keys_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                            VerificationKeyMaterialPort& material) {
  try {
    return std::unique_ptr<IdentityVerificationKeysApplication>{
        std::make_unique<Application>(transactions, material)};
  } catch (const std::bad_alloc&) {
    return failure(ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::target
