// WS-12 received-message feed identity vectors: deterministic UUIDv5 derivation from fixed in-repo
// namespaces. Any change to a namespace, the component order or the separator is a breaking feed
// identity change and must update these vectors together with the frozen documentation.
#include <iostream>
#include <stdexcept>
#include <string>

#include "kiarabot/core/domain/target/observation_feed_identity.hpp"

namespace {
namespace t = kiarabot::core::target;
const std::string kAccount = "11111111-1111-4111-8111-111111111111";
const std::string kAdapter = "22222222-2222-4222-8222-222222222222";

void require(const std::string& actual, const std::string& expected, const char* what) {
  if (actual != expected) {
    throw std::runtime_error(std::string{"feed identity drift: "} + what + " expected " + expected +
                             " got " + actual);
  }
}
} // namespace

int main() {
  try {
    require(t::message_feed_id(kAccount, kAdapter, "platform-message-1"),
            "1a7fa4d0-4987-5241-aee3-f620b70cf9f1", "message vector");
    require(t::conversation_feed_id(kAccount, kAdapter, "platform-conversation-1"),
            "3dc8ee9e-3cb1-5ca7-819a-85031b9bdf37", "conversation vector");
    // Empty platform suffixes still derive a stable, non-platform identity (unit separator makes
    // concatenation unambiguous: account|adapter|'' differs from account|adapter|platform).
    require(t::message_feed_id(kAccount, kAdapter, ""), "7b0b765e-966d-5bf7-ba27-532b464821da",
            "empty suffix vector");
    // Namespace separation: the same tuple yields different message and conversation identities.
    if (t::message_feed_id(kAccount, kAdapter, "same") ==
        t::conversation_feed_id(kAccount, kAdapter, "same")) {
      throw std::runtime_error("feed namespaces must be disjoint");
    }
    // No platform raw id leakage in the derived identity.
    const auto derived = t::message_feed_id(kAccount, kAdapter, "platform-message-1");
    if (derived.find("platform-message-1") != std::string::npos || derived.size() != 36) {
      throw std::runtime_error("derived feed identity must be an opaque uuid");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "observation feed identity: ok\n";
  return 0;
}
