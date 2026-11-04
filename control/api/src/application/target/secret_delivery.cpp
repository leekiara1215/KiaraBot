#include "kiarabot/control/application/target/secret_delivery.hpp"

#include <limits>

namespace kiarabot::control::target {
namespace {
namespace core = kiarabot::core::target;

bool identifier(std::string_view value) {
  if (value.empty() || value.size() > 128 || value == "." || value == "..") {
    return false;
  }
  for (const unsigned char character : value) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_' ||
          character == '.')) {
      return false;
    }
  }
  return true;
}
core::Failure invalid() {
  return {core::ErrorCode::InvalidArgument,
          core::Acceptance::NotAccepted,
          core::Recovery::CorrectInput,
          {},
          {}};
}
} // namespace

bool valid_delivery_identity(const SecretDeliveryIdentity& identity) {
  return identifier(identity.delivery_id) && identifier(identity.resource.id.value) &&
         identifier(identity.recipient_id) && identity.credential_version > 0 &&
         (identity.resource.kind == core::ResourceKind::Account ||
          identity.resource.kind == core::ResourceKind::Instance);
}

core::Outcome<SecretDeliveryReceipt>
SecretDeliveryApplication::store(const SecretDeliveryRequest& request) {
  if (!valid_delivery_identity(request.identity) || request.secret.size() != 64 ||
      request.expected_version == std::numeric_limits<std::uint64_t>::max() ||
      request.identity.credential_version != request.expected_version + 1) {
    return invalid();
  }
  for (const char character : request.secret) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return invalid();
    }
  }
  return port_.store(request);
}

core::Outcome<std::optional<SecretDeliveryReceipt>>
SecretDeliveryApplication::query(const SecretDeliveryIdentity& identity) {
  if (!valid_delivery_identity(identity)) {
    return invalid();
  }
  return port_.query(identity);
}
} // namespace kiarabot::control::target
