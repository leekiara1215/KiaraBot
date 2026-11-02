#pragma once

#include <memory>

#include "kiarabot/core/ports/target/authorization_method_policy.hpp"

namespace google::protobuf {
class DescriptorPool;
}

namespace kiarabot::core::platform {

// Trusted external resolver for the P04 online-authorization use case. It reads ONLY the generated
// canonical contract descriptors (method options: required_permission, scope_kinds, deadlines and
// requires_online_authorization) from the supplied descriptor pool; there is no handwritten method
// table, no inference from request fields, method spelling, permissions or service names.
// Unannotated/unknown methods return an empty optional so callers fail closed with InvalidArgument;
// missing contract extensions or malformed annotated metadata return a stable Failure.
// The default pool is the generated contract pool. A different pool exists only for fixtures.
target::Outcome<std::unique_ptr<target::AuthorizationMethodPolicyPort>>
make_descriptor_authorization_method_policy(
    const google::protobuf::DescriptorPool* descriptor_pool = nullptr);

} // namespace kiarabot::core::platform
