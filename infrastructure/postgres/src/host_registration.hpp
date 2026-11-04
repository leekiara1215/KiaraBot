#pragma once

#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_host_application.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the P08b-1 Runtime Host registration/heartbeat port. The deployment
// certificate allowlist is injected (never queried from the wire); host records use the existing
// Runtime hosts relation and the current connection generation/liveness is persisted in
// kiarabot.host_connections. Borrows the owning Runtime UoW: no commit, no network.
class HostRegistration final : public core::target::HostRegistrationPort {
public:
  HostRegistration(core::target::UnitOfWork& work, SqlWork& sql,
                   std::vector<core::target::HostCertificateBinding> allowlist)
      : work_(work), sql_(sql), allowlist_(std::move(allowlist)) {}

  core::target::Outcome<std::optional<core::target::HostCertificateBinding>>
  certificate_binding(std::string_view certificate_sha256) override;
  core::target::Outcome<std::optional<core::target::HostRegistration>>
  find_host(const core::target::ResourceId& host) override;
  core::target::Status save_host(const core::target::HostRegistration& registration,
                                 std::optional<core::target::ResourceVersion> expected) override;
  core::target::Outcome<std::optional<core::target::HostConnectionState>>
  connection(const core::target::ResourceId& host) override;
  core::target::Outcome<core::target::HostConnectionState>
  activate_connection(const core::target::HostConnectionState& connection) override;
  core::target::Status save_connection(const core::target::HostConnectionState& connection,
                                       std::uint64_t expected_generation) override;

private:
  core::target::UnitOfWork& work_;
  SqlWork& sql_;
  std::vector<core::target::HostCertificateBinding> allowlist_;
};

// Runtime-owned Host admission gate rows (R21 runtime_host_admission_gates.hpp). Borrows the same
// owner UoW; no commit, no network, no business authorization here.
class HostAdmissionGateRecords final : public core::target::HostAdmissionGateStore {
public:
  explicit HostAdmissionGateRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<std::optional<core::target::HostAdmissionGateRecord>>
  current(const core::target::ResourceId& host) override;
  core::target::Outcome<core::target::HostAdmissionGateRecord>
  save(const core::target::HostAdmissionGateRecord& value,
       std::optional<std::uint64_t> expected_gate_version) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
