#pragma once

#include <vector>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::target {

enum class Dependency { BusinessImplementation, DatabaseSchema, Identity, Credentials, Runtime };
enum class DependencyState { Unverified, Available, Unavailable, NotImplemented };
struct DependencyStatus {
  Dependency dependency;
  DependencyState state;
};
struct ServiceReadiness {
  bool ready;
  std::vector<DependencyStatus> dependencies;
};
// Immutable/thread-safe query; borrows no transport objects and performs no network I/O.
// P02 implementations always report BusinessImplementation/NotImplemented and ready=false.
// Listening/live is a process property and must never be inferred from this result.
class ServiceReadinessPort {
public:
  virtual ~ServiceReadinessPort() = default;
  virtual Service service() const noexcept = 0;
  virtual ServiceReadiness readiness() const = 0;
};

} // namespace kiarabot::core::target
