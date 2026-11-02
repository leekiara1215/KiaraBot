#include <cstdlib>
#include <iostream>
#include <variant>

#include "kiarabot/control/application/target/onboarding.hpp"

namespace onboarding = kiarabot::control::target::onboarding;
namespace core = kiarabot::core::target;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool accepted(const core::Status& status) {
  return std::holds_alternative<std::monostate>(status);
}

int main() {
  const onboarding::Selection managed{"qq", "onebot-v11", "napcat", "nonebot",
                                      onboarding::Selection::Mode::Managed};
  onboarding::Capabilities capabilities{managed,
                                        {onboarding::Step::Detect, onboarding::Step::Install,
                                         onboarding::Step::Configure, onboarding::Step::Start,
                                         onboarding::Step::Login, onboarding::Step::CheckConnection,
                                         onboarding::Step::Stop},
                                        {onboarding::InteractionKind::QrCode},
                                        {{"token", onboarding::FieldKind::SecretReference, true},
                                         {"enabled", onboarding::FieldKind::Boolean, false}}};
  const std::vector<onboarding::ConfigValue> config{
      {"token", onboarding::SecretReference{"local/token"}}, {"enabled", true}};
  require(accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install, config)),
          "managed selection");
  require(!accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install, {})),
          "required secret");
  require(!accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install,
                                         {{"token", std::string{"secret"}}})),
          "plaintext secret must be rejected");
  require(!accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install,
                                         {{"token", onboarding::SecretReference{""}}})),
          "empty secret reference");
  require(!accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install,
                                         {{"token", onboarding::SecretReference{"x"}},
                                          {"token", onboarding::SecretReference{"y"}}})),
          "duplicate value");
  require(!accepted(onboarding::validate(
              capabilities, managed, onboarding::Step::Install,
              {{"token", onboarding::SecretReference{"x"}}, {"shell", std::string{"cmd"}}})),
          "undeclared command");
  auto other = managed;
  other.protocol = "other";
  require(!accepted(onboarding::validate(capabilities, other, onboarding::Step::Install, config)),
          "mismatched protocol");
  capabilities.fields.push_back({"token", onboarding::FieldKind::Text, false});
  require(!accepted(onboarding::validate(capabilities, managed, onboarding::Step::Install, config)),
          "duplicate schema");

  const onboarding::Selection existing{"qq", "onebot-v11", "compatible", "nonebot",
                                       onboarding::Selection::Mode::Existing};
  onboarding::Capabilities external{
      existing,
      {onboarding::Step::Detect, onboarding::Step::Configure, onboarding::Step::CheckConnection},
      {},
      {{"endpoint", onboarding::FieldKind::Text, true},
       {"token", onboarding::FieldKind::SecretReference, true}}};
  const std::vector<onboarding::ConfigValue> connection{
      {"endpoint", std::string{"wss://localhost"}},
      {"token", onboarding::SecretReference{"external/token"}}};
  require(accepted(onboarding::validate(external, existing, onboarding::Step::CheckConnection,
                                        connection)),
          "existing service connection");
  require(!accepted(onboarding::validate(external, existing, onboarding::Step::Stop, connection)),
          "undeclared stop");
  external.steps.push_back(onboarding::Step::Start);
  const auto result =
      onboarding::validate(external, existing, onboarding::Step::CheckConnection, connection);
  require(!accepted(result) &&
              std::get<core::Failure>(result).code == core::ErrorCode::InvalidArgument &&
              std::get<core::Failure>(result).acceptance == core::Acceptance::NotAccepted,
          "existing service process control must fail closed");
}
