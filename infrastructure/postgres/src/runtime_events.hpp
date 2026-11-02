#pragma once

#include "kiarabot/core/ports/target/business_ports.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the Runtime event inbox/delivery ledger. Borrows the owning Runtime UoW
// (SqlWork) so every method participates in the caller's transaction; never opens a connection,
// commits, or performs remote calls. Timestamps passed by the application are authoritative
// samples; recorded created/updated columns still use database clock_timestamp().
class RuntimeEvents final : public core::target::EventRepository {
public:
  explicit RuntimeEvents(SqlWork& sql) : sql_(sql) {}

  core::target::Outcome<core::target::EventAcceptanceResult>
  accept(const core::target::EventRecord& event,
         const std::vector<core::target::DeliveryRecord>& targets,
         const core::target::EventCapacityLimits& limits) override;
  core::target::Outcome<std::vector<core::target::DeliveryClaim>>
  claim(const core::target::DeliveryClaimRequest& request) override;
  core::target::Outcome<core::target::DeliveryReceipt>
  acknowledge(const core::target::DeliveryAcknowledgement& acknowledgement,
              core::target::TimePoint now) override;
  core::target::Outcome<core::target::DeliveryReceipt>
  renew(const core::target::DeliveryAttempt& attempt, core::target::TimePoint now) override;
  core::target::Outcome<core::target::DeliverySweep> sweep(core::target::TimePoint now,
                                                           const core::target::RetryPolicy& policy,
                                                           double jitter_sample) override;
  core::target::Outcome<std::size_t> recover(core::target::TimePoint now,
                                             const core::target::RetryPolicy& policy,
                                             double jitter_sample) override;
  core::target::Outcome<std::vector<core::target::DeadLetter>>
  list_dead_letters(const std::optional<core::target::InstanceId>& instance,
                    std::size_t limit) override;
  core::target::Outcome<std::optional<core::target::DeliveryRecord>>
  replay(const core::target::ReplayRequest& request) override;
  core::target::Outcome<core::target::EventCapacity>
  capacity(const core::target::EventCapacityLimits& limits) override;
  core::target::Outcome<std::size_t> prune_expired(core::target::TimePoint now,
                                                   std::size_t limit) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
