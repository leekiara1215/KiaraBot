#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("P11 model gateway assertion failed at line " +
                             std::to_string(location.line()));
  }
}

t::TimePoint at(std::int64_t seconds) {
  return t::TimePoint{std::chrono::seconds{seconds}};
}

void identifiers() {
  const std::string model = "11111111-1111-4111-8111-111111111111";
  require(t::valid_model_identifier(model));
  require(!t::valid_model_identifier("11111111-1111-1111-8111-111111111111")); // bad version
  require(!t::valid_model_identifier("11111111-1111-4111-c111-111111111111")); // bad variant
  require(!t::valid_model_identifier("111111111111411181111111111111111111")); // no dashes
  require(!t::valid_model_identifier(""));
  require(t::valid_model_alias("default"));
  require(t::valid_model_alias("gpt-4o.mini_1"));
  require(!t::valid_model_alias("Default"));
  require(!t::valid_model_alias("-bad"));
  require(!t::valid_model_alias(""));
  require(!t::valid_model_alias(std::string(129, 'a')));
}

void endpoints() {
  require(t::valid_model_endpoint("https://api.example.com/v1/chat/completions"));
  require(t::valid_model_endpoint("https://api.example.com"));
  require(t::valid_model_endpoint("http://127.0.0.1:8080/v1"));
  require(t::valid_model_endpoint("http://localhost:11434"));
  require(!t::valid_model_endpoint("http://api.example.com/v1"));
  require(!t::valid_model_endpoint("ftp://example.com"));
  require(!t::valid_model_endpoint("https://user:pass@example.com"));
  require(!t::valid_model_endpoint("https://example.com?token=1"));
  require(!t::valid_model_endpoint("https://example.com#fragment"));
  require(!t::valid_model_endpoint("https://example.com/ path"));
  require(!t::valid_model_endpoint(""));
  require(!t::valid_model_endpoint(std::string("https://") + std::string(2049, 'a')));
}

void limits() {
  require(t::valid_provider_model("gpt-4o-mini"));
  require(!t::valid_provider_model(""));
  require(!t::valid_provider_model(std::string(257, 'a')));
  require(t::valid_model_message_role("system"));
  require(t::valid_model_message_role("user"));
  require(t::valid_model_message_role("assistant"));
  require(!t::valid_model_message_role("tool"));
  require(t::valid_provider_request_id("chatcmpl-abc_1:2"));
  require(t::valid_provider_request_id(""));
  require(!t::valid_provider_request_id("has space"));
  require(!t::valid_provider_request_id(std::string(129, 'a')));

  std::array<std::byte, 4> secret{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}};
  require(t::valid_model_secret(secret));
  std::array<std::byte, 1> empty_secret{};
  require(!t::valid_model_secret(empty_secret));
  std::array<std::byte, 1> control{std::byte{'\n'}};
  require(!t::valid_model_secret(control));

  require(t::model_timeout_budget(5000, at(100), at(90)) == 5000);
  require(t::model_timeout_budget(30000, at(100), at(90)) == 9900); // RPC remainder wins
  require(t::model_timeout_budget(5000, at(100), at(99)) == 900);   // safety margin
  require(t::model_timeout_budget(5000, at(100), at(100)) == 0);    // expired budget
  require(t::model_timeout_budget(5000, at(100), at(101)) == 0);    // past deadline
  require(t::model_timeout_budget(5000, at(100), t::TimePoint{std::chrono::milliseconds{99'950}}) ==
          0); // sub-margin remainder
  require(t::model_timeout_budget(100, at(1000), at(998)) == 100);
  require(t::model_timeout_budget(30000, at(1000), at(0)) == 30000);
}

void canonical_hash_input() {
  const std::vector<t::ModelMessage> messages{{"user", "hello"}, {"assistant", "world"}};
  const auto first = t::model_request_canonical("default", messages);
  const auto second = t::model_request_canonical("default", messages);
  require(first == second);
  require(!first.empty());
  const auto changed_model = t::model_request_canonical("other", messages);
  require(first != changed_model);
  auto changed_messages = messages;
  changed_messages[0].text = "hello!";
  require(first != t::model_request_canonical("default", changed_messages));
  // Message count/value boundaries are length-prefixed and cannot alias shifted content.
  const std::vector<t::ModelMessage> split{{"user", "ab"}, {"assistant", "c"}};
  const std::vector<t::ModelMessage> merged{{"user", "a"}, {"assistant", "bc"}};
  require(t::model_request_canonical("m", split) != t::model_request_canonical("m", merged));
  require(t::model_request_canonical("m", {}) != t::model_request_canonical("", {}));
}

void state_projection() {
  require(t::model_operation_state(t::ModelCallState::Started) == t::OperationState::Running);
  require(t::model_operation_state(t::ModelCallState::Succeeded) == t::OperationState::Succeeded);
  require(t::model_operation_state(t::ModelCallState::Failed) == t::OperationState::Failed);
  require(t::model_operation_state(t::ModelCallState::Unknown) == t::OperationState::Blocked);
  require(t::model_execution_outcome(t::ModelCallState::Started) == t::ExecutionOutcome::Unknown);
  require(t::model_execution_outcome(t::ModelCallState::Unknown) == t::ExecutionOutcome::Unknown);
  require(t::model_execution_outcome(t::ModelCallState::Succeeded) ==
          t::ExecutionOutcome::KnownSucceeded);
  require(t::model_execution_outcome(t::ModelCallState::Failed) ==
          t::ExecutionOutcome::KnownFailed);
}

void txn2_mutations() {
  t::ModelProviderReply reply;
  reply.text = "answer";
  reply.provider_request_id = "req-1";
  reply.latency_ms = 42;
  reply.usage = {true, 10, 20};
  auto success = t::model_success_mutation(reply);
  require(success.state == t::ModelCallState::Succeeded);
  require(success.execution_outcome == t::ExecutionOutcome::KnownSucceeded);
  require(success.usage_known && success.input_tokens == 10 && success.output_tokens == 20);
  require(success.may_have_billed && success.provider_request_id == "req-1" &&
          success.latency_ms == 42);

  t::ModelProviderReply no_usage = reply;
  no_usage.usage = {false, 0, 0};
  auto success_unknown_usage = t::model_success_mutation(no_usage);
  require(!success_unknown_usage.usage_known && success_unknown_usage.input_tokens == 0);

  auto definite = t::model_definite_failure_mutation(t::ErrorCode::InvalidArgument);
  require(definite.state == t::ModelCallState::Failed);
  require(definite.execution_outcome == t::ExecutionOutcome::KnownFailed);
  require(!definite.may_have_billed && definite.error == t::ErrorCode::InvalidArgument);

  auto unknown = t::model_unknown_mutation(t::ErrorCode::Timeout);
  require(unknown.state == t::ModelCallState::Unknown);
  require(unknown.execution_outcome == t::ExecutionOutcome::Unknown);
  require(unknown.may_have_billed && !unknown.usage_known);
  require(unknown.error == t::ErrorCode::Timeout);
}

void unknown_resolution() {
  t::ModelIntent call;
  call.owner = {"33333333-3333-4333-8333-333333333333"};
  call.logical_call_id = "44444444-4444-4444-8444-444444444444";
  call.model = {"11111111-1111-4111-8111-111111111111"};
  call.config_version = {1};
  call.state = t::ModelCallState::Unknown;
  call.version = {3};
  call.created_at = at(100);
  call.observed_at = at(100);
  call.may_have_billed = true;

  require(!t::model_unknown_resolution_due(call, at(100 + 30 * 24 * 3600 - 1)));
  require(t::model_unknown_resolution_due(call, at(100 + 30 * 24 * 3600)));
  call.disposition = t::ModelCallDisposition::ExecutedBilled;
  require(!t::model_unknown_resolution_due(call, at(100 + 60 * 24 * 3600)));

  auto abandoned = t::model_abandon_unknown(call, at(200), "system");
  require(abandoned.state == t::ModelCallState::Failed);
  require(abandoned.disposition == t::ModelCallDisposition::Abandoned);
  require(abandoned.may_have_billed);
  require(abandoned.resolved_at == at(200) && abandoned.resolved_by == "system");
  require(abandoned.version.value == 4);

  auto not_executed =
      t::model_resolve_unknown(call, t::ModelCallDisposition::NotExecuted, at(201), "operator");
  require(not_executed.state == t::ModelCallState::Failed);
  require(not_executed.disposition == t::ModelCallDisposition::NotExecuted);
  require(!not_executed.may_have_billed && not_executed.usage_known);
  require(not_executed.version.value == 4);

  auto billed =
      t::model_resolve_unknown(call, t::ModelCallDisposition::ExecutedBilled, at(202), "operator");
  require(billed.may_have_billed);

  require(!t::valid_call_disposition(t::ModelCallDisposition::Unspecified));
  for (const auto disposition :
       {t::ModelCallDisposition::NotExecuted, t::ModelCallDisposition::ExecutedNotBilled,
        t::ModelCallDisposition::ExecutedBilled, t::ModelCallDisposition::Abandoned}) {
    require(t::valid_call_disposition(disposition));
  }

  require(t::model_provider_kind("openai_chat_completions") ==
          t::ModelProviderKind::OpenAiChatCompletions);
  require(!t::model_provider_kind("anthropic"));
  require(t::model_provider_kind_name(t::ModelProviderKind::OpenAiChatCompletions) ==
          "openai_chat_completions");
}

} // namespace

int main() {
  try {
    identifiers();
    endpoints();
    limits();
    canonical_hash_input();
    state_projection();
    txn2_mutations();
    unknown_resolution();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
