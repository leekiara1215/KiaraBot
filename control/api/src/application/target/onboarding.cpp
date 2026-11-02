#include "kiarabot/control/application/target/onboarding.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace kiarabot::control::target::onboarding {
namespace {
core::target::Status invalid() {
  return core::target::Failure{core::target::ErrorCode::InvalidArgument,
                               core::target::Acceptance::NotAccepted,
                               core::target::Recovery::CorrectInput, std::nullopt, std::nullopt};
}

bool same_selection(const Selection& a, const Selection& b) {
  return a.channel == b.channel && a.protocol == b.protocol &&
         a.implementation == b.implementation && a.adapter == b.adapter && a.mode == b.mode;
}

bool matches(FieldKind kind, const FieldValue& value) {
  switch (kind) {
  case FieldKind::Text:
    return std::holds_alternative<std::string>(value);
  case FieldKind::Boolean:
    return std::holds_alternative<bool>(value);
  case FieldKind::SecretReference:
    return std::holds_alternative<SecretReference>(value) &&
           !std::get<SecretReference>(value).id.empty();
  }
  return false;
}
} // namespace

core::target::Status validate(const Capabilities& capabilities, const Selection& selection,
                              Step step, const std::vector<ConfigValue>& config) {
  if (selection.channel.empty() || selection.protocol.empty() || selection.implementation.empty() ||
      selection.adapter.empty() || !same_selection(capabilities.selection, selection) ||
      (selection.mode != Selection::Mode::Managed && selection.mode != Selection::Mode::Existing)) {
    return invalid();
  }
  const auto forbidden = [](Step candidate) {
    return candidate == Step::Install || candidate == Step::Start || candidate == Step::Stop;
  };
  if ((selection.mode == Selection::Mode::Existing &&
       std::any_of(capabilities.steps.begin(), capabilities.steps.end(), forbidden)) ||
      std::find(capabilities.steps.begin(), capabilities.steps.end(), step) ==
          capabilities.steps.end()) {
    return invalid();
  }
  std::set<std::string> declared;
  for (const auto& field : capabilities.fields) {
    if (field.key.empty() || !declared.insert(field.key).second ||
        (field.kind != FieldKind::Text && field.kind != FieldKind::Boolean &&
         field.kind != FieldKind::SecretReference)) {
      return invalid();
    }
  }
  std::set<std::string> supplied;
  for (const auto& value : config) {
    const auto field = std::find_if(capabilities.fields.begin(), capabilities.fields.end(),
                                    [&](const ConfigField& item) { return item.key == value.key; });
    if (!supplied.insert(value.key).second || field == capabilities.fields.end() ||
        !matches(field->kind, value.value)) {
      return invalid();
    }
  }
  for (const auto& field : capabilities.fields) {
    if (field.required && !supplied.contains(field.key)) {
      return invalid();
    }
  }
  return std::monostate{};
}
} // namespace kiarabot::control::target::onboarding
