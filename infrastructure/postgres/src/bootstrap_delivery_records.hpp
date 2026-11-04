#pragma once

#include "kiarabot/core/ports/target/runtime_bootstrap_deliveries.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the BOOT-02 Runtime bootstrap delivery intent/receipt relation. Borrows the
// owning Runtime UoW: every method participates in that transaction, never commits/rolls back and
// never touches the delivery sink, a filesystem or a remote service. Rows carry no secret and no
// digest; those stay in bridge_credentials.
class BootstrapDeliveryRecords final : public core::target::BootstrapDeliveryRepository {
public:
  explicit BootstrapDeliveryRecords(SqlWork& sql) : sql_(sql) {}

  core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>>
  find(const core::target::ResourceRef& resource, const std::string& delivery_id) override;
  core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>>
  current(const core::target::ResourceRef& resource) override;
  core::target::Status insert(const core::target::BootstrapDeliveryRecord& record) override;
  core::target::Status transition(const core::target::BootstrapDeliveryRecord& next,
                                  core::target::BootstrapDeliveryState expected_state) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
