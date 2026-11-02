#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/action_rules.hpp"
#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Trusted Runtime-to-adapter dispatch boundary (P07a-B). The bridge handler owns the transport: it
// registers one sink per accepted adapter connection and pushes the frames returned by
// claim_due(). The implementation commits the conditional mark_dispatched() before returning, so a
// lost frame can only produce UNKNOWN/EXPIRED evidence, never a second dispatch attempt.
struct ActionDispatchFrame {
  ActionId action_id;
  AttemptId dispatch_attempt_id;
  std::uint32_t dispatch_sequence = 0;
  std::string serialized_envelope;
  TimePoint deadline;
  std::string trace_id;
};

struct ActionDispatchClaim {
  std::vector<ActionDispatchFrame> frames;
};

class ActionDispatchPort {
public:
  virtual ~ActionDispatchPort() = default;
  // Bounded claim for one currently connected adapter instance. Each returned frame already has
  // its dispatch attempt/sequence persisted and its Action row marked Dispatched in the same UoW.
  // A failure leaves every row untouched; the bridge reports it as transient and retries later.
  virtual Outcome<ActionDispatchClaim> claim_due(const InstanceId& adapter_instance,
                                                 std::size_t limit, TimePoint now) = 0;
};

// Trusted adapter -> Runtime result/evidence sink. Terminal results are persisted as one
// conditional update (and its receipt returned); non-terminal evidence never makes an Action
// terminal and is only appended. Callers write the ActionReceipt after the call returns.
struct ActionResultReport {
  InstanceId adapter_instance;
  ActionId action_id;
  std::optional<AttemptId> dispatch_attempt_id;
  ActionTerminalResult result;
  std::string trace_id;
};

struct ActionAdapterEvidence {
  InstanceId adapter_instance;
  ActionId action_id;
  AttemptId dispatch_attempt_id;
  ExecutionOutcome execution_outcome = ExecutionOutcome::Unknown;
  std::optional<ErrorCode> error_code;
  std::optional<std::string> platform_reference;
  TimePoint observed_at;
  std::string trace_id;
};

struct ActionReportReceipt {
  ActionReceiptDisposition disposition = ActionReceiptDisposition::Recorded;
  std::optional<ActionRecord> record;
  std::optional<Failure> failure;
  bool evidence_appended = false;
};

class ActionEvidenceSink {
public:
  virtual ~ActionEvidenceSink() = default;
  virtual Outcome<ActionReportReceipt> record_result(const ActionResultReport& report) = 0;
  virtual Outcome<ActionReportReceipt> record_evidence(const ActionAdapterEvidence& report) = 0;
};

} // namespace kiarabot::core::target
