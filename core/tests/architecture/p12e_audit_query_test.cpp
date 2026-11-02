// P12e read-only audit/log projection contract helpers: opaque keyset cursors are canonical and
// reject malformed/zero identity. Real pagination/filter/redaction and permission behavior are
// covered end-to-end by p03_postgres_database plus the Identity/Runtime RPC adapters.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/audit_query.hpp"

namespace {
namespace t = kiarabot::core::target;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::fprintf(stderr, "P12e audit query assertion: %s\n", scenario);
    std::exit(1);
  }
}
} // namespace

int main() {
  const t::AuditCursor cursor{t::TimePoint{std::chrono::nanoseconds{1234567890123456789LL}}, 42};
  const auto encoded = t::encode_audit_cursor(cursor);
  check(encoded == "1234567890123456789:42", "canonical cursor encoding");
  const auto decoded = t::decode_audit_cursor(encoded);
  check(decoded.has_value() && decoded->id == 42 &&
            decoded->occurred_at == cursor.occurred_at,
        "cursor round trip");

  for (const std::string_view malformed :
       {"", ":", "1:", ":2", "0:1", "1:0", "-1:2", "1:-2", "01:2", "1:02", "1:2:3",
        "9999999999999999999:1", "1:9999999999999999999"}) {
    check(!t::decode_audit_cursor(malformed).has_value(), "malformed cursor rejected");
  }
  std::string oversized(65, '1');
  oversized[64] = ':';
  check(!t::decode_audit_cursor(oversized).has_value(), "oversized cursor rejected");
  return 0;
}
