#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/host_records.hpp"
#include "kiarabot/core/ports/target/host_ports.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::target {

struct HostServiceDependencies {
  HostJournal& journal;
  ProcessSupervisor& supervisor;
  HostBridgeTransport& bridge;
  HostClock& clock;
  HostIdentity identity;
  std::vector<LaunchProfile> profiles;
  // Deployment-provided values for non-secret allow-listed environment names.
  std::vector<std::pair<std::string, std::string>> environment;
  std::string credential_directory;
  std::chrono::milliseconds stop_grace{std::chrono::seconds{10}};
  std::size_t observation_batch{64};
};

// P08a Plugin Host application. Owns process reality and the Host-local journal only; desired
// state, registration and config-application facts stay with Runtime. Commands arrive on the
// authenticated HostBridge stream and every observation carries the journal's monotonic sequence.
class HostService final : public ServiceReadinessPort, public HostBridgeHandler {
public:
  explicit HostService(HostServiceDependencies dependencies);
  ~HostService() override;
  HostService(const HostService&) = delete;
  HostService& operator=(const HostService&) = delete;

  Status start();
  void stop();

  Service service() const noexcept override;
  ServiceReadiness readiness() const override;

  Outcome<HostReceipt> apply_desired_process(const HostProcessSpec& process,
                                             std::string_view command_id,
                                             std::string_view payload_sha256) override;
  Outcome<HostReceipt> stop_execution(std::string_view instance_id,
                                      std::uint64_t execution_generation, std::uint32_t grace_ms,
                                      std::string_view command_id,
                                      std::string_view payload_sha256) override;
  Outcome<HostReceipt> drain_host(std::uint32_t grace_ms, std::string_view command_id,
                                  std::string_view payload_sha256) override;
  void bridge_ready() override;
  void bridge_resync() override;

private:
  struct ResolvedProcess {
    ManagedProcessSpec spec;
    const HostPackage* package = nullptr;
    const LaunchProfile* profile = nullptr;
  };
  Outcome<ResolvedProcess> resolve_locked(const HostProcessSpec& process);
  Outcome<HostReceipt> apply_running_locked(const HostCommandRecord& command,
                                           const HostProcessSpec& process,
                                           const ResolvedProcess& resolved);
  Outcome<HostReceipt> apply_stopped_locked(const HostCommandRecord& command,
                                            const HostProcessSpec& process);
  Outcome<HostReceipt> stop_instance_locked(std::string_view instance_id,
                                            std::uint64_t execution_generation,
                                            std::chrono::milliseconds grace,
                                            std::string_view command_id);
  void recover_locked();
  void publish_locked(const ManagedExecution& execution, std::string_view command_id);
  void flush_pending_locked();
  void mark_command_locked(HostCommandRecord& command, HostCommandState state,
                           std::optional<ManagedExecution> execution);
  HostReceipt receipt_locked(const HostCommandRecord& command) const;
  const HostPackage* package_for(std::string_view sha256) const;
  const LaunchProfile* profile_for(std::string_view name) const;
  std::string deployment_environment(std::string_view name) const;
  Outcome<std::string> credential_for(std::string_view instance_id) const;

  HostServiceDependencies dependencies_;
  mutable std::mutex mutex_;
  bool journal_open_ = false;
  bool started_ = false;
  bool stopped_ = false;
};

} // namespace kiarabot::core::target
