#include <cstdlib>
#include <type_traits>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace target = kiarabot::core::target;
static_assert(std::is_same_v<decltype(target::SessionSecurity::control_peer),
                             std::optional<target::SessionControlPeer>>);
void check(bool condition) {
  if (!condition) {
    std::abort();
  }
}
int main() {
  const target::SessionControlPeer peer{{"control"}, {"stable-certificate-credential"}};
  check(target::valid_session_control_peer(peer));
  check(!target::valid_session_control_peer({{}, peer.credential}));
  check(!target::valid_session_control_peer({peer.subject, {}}));
  target::SessionSecurity original{};
  check(!original.control_peer.has_value());
  original.control_peer = peer;
  auto refreshed = original;
  ++refreshed.rotation;
  ++refreshed.version.value;
  check(refreshed.control_peer == original.control_peer);
  check(*refreshed.control_peer != target::SessionControlPeer{{"other"}, peer.credential});
  check(*refreshed.control_peer != target::SessionControlPeer{peer.subject, {"other-credential"}});
}
