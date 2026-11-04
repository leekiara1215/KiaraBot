#pragma once

#include <atomic>

namespace kiarabot::control::target {

// Composition-owned observations for the Control readiness port. The HTTPS endpoint updates the
// flags from real TLS/Identity channel state; the P02 process probe only reads them (no network
// I/O in the readiness port itself).
struct ControlReadiness {
  std::atomic<bool> business{false};
  std::atomic<bool> credentials{false};
  std::atomic<bool> identity{false};
};

} // namespace kiarabot::control::target
