#pragma once

#include <memory>

#include "kiarabot/core/ports/target/unit_of_work.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// P11 Model-private repositories borrow the owning common UoW SQL access and never commit.
std::unique_ptr<core::target::ModelConfigurationRepository>
model_configuration_repository(SqlWork& sql);
std::unique_ptr<core::target::ProviderSecretRepository> provider_secret_repository(SqlWork& sql);
std::unique_ptr<core::target::ModelCallRepository> model_call_repository(SqlWork& sql);

} // namespace kiarabot::infrastructure::postgres::detail
