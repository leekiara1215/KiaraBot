#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
bool valid_audit_cursor(std::string_view token) {
  if (token.empty() || token.size() > 160) {
    return false;
  }
  const auto colon = token.find(':');
  if (colon == 0 || colon == std::string_view::npos || colon + 1 >= token.size()) {
    return false;
  }
  const auto number = token.substr(0, colon);
  if (number.size() > 20 || !std::all_of(number.begin(), number.end(),
                                         [](char value) { return value >= '0' && value <= '9'; })) {
    return false;
  }
  const auto id = token.substr(colon + 1);
  return id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char value) {
           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                  (value >= 'A' && value <= 'F') || value == '-';
         });
}
std::string audit_cursor(const AuditLogRow& row) {
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(row.occurred_at.time_since_epoch())
          .count();
  return std::to_string(nanos) + ":" + row.entry_id;
}
// Cross-owner keyset token. A single shared cursor cannot page two independent owner keysets
// without skipping rows: each owner returns at most page_size rows, while the merged page may
// include only a prefix of either page. The Control token therefore carries one cursor (or an
// explicit exhausted marker) per owner, and the next request forwards each owner its own cursor.
struct OwnerAuditCursor {
  bool exhausted = false;
  std::string token;
};
struct AuditPageCursor {
  OwnerAuditCursor identity;
  OwnerAuditCursor runtime;
};
std::string hex_encode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    output.push_back(kHex[byte >> 4U]);
    output.push_back(kHex[byte & 0x0fU]);
  }
  return output;
}
std::optional<std::string> hex_decode(std::string_view value) {
  if (value.empty() || value.size() % 2 != 0 || value.size() > 400) {
    return std::nullopt;
  }
  const auto digit = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    return -1;
  };
  std::string output;
  output.reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    const int high = digit(value[index]);
    const int low = digit(value[index + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    output.push_back(static_cast<char>((high << 4) | low));
  }
  return output;
}
std::optional<OwnerAuditCursor> decode_owner_cursor(std::string_view value) {
  if (value == "~") {
    return OwnerAuditCursor{true, {}};
  }
  if (value == "*") {
    return OwnerAuditCursor{false, {}};
  }
  auto decoded = hex_decode(value);
  if (!decoded || !valid_audit_cursor(*decoded)) {
    return std::nullopt;
  }
  return OwnerAuditCursor{false, std::move(*decoded)};
}
std::optional<AuditPageCursor> parse_audit_page_cursor(std::string_view token) {
  if (token.empty()) {
    return AuditPageCursor{};
  }
  if (!token.starts_with("v1.")) {
    // Legacy fixture/source token: both owners understand the same (occurred_at,id) cursor.
    if (!valid_audit_cursor(token)) {
      return std::nullopt;
    }
    return AuditPageCursor{{false, std::string{token}}, {false, std::string{token}}};
  }
  const auto first = token.find('.', 3);
  if (first == std::string_view::npos || first == 3 || first + 1 >= token.size() ||
      token.find('.', first + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  auto identity = decode_owner_cursor(token.substr(3, first - 3));
  auto runtime = decode_owner_cursor(token.substr(first + 1));
  if (!identity || !runtime) {
    return std::nullopt;
  }
  return AuditPageCursor{std::move(*identity), std::move(*runtime)};
}
std::string encode_audit_page_cursor(const AuditPageCursor& cursor) {
  const auto encode = [](const OwnerAuditCursor& value) {
    if (value.exhausted) {
      return std::string{"~"};
    }
    return value.token.empty() ? std::string{"*"} : hex_encode(value.token);
  };
  return "v1." + encode(cursor.identity) + "." + encode(cursor.runtime);
}
std::optional<OwnerAuditCursor> next_owner_cursor(const AuditLogSourcePage& page,
                                                  const std::vector<const AuditLogRow*>& included,
                                                  std::string_view owner,
                                                  std::string_view previous_token) {
  if (page.rows.empty()) {
    return OwnerAuditCursor{true, {}};
  }
  const AuditLogRow* last_included = nullptr;
  std::size_t included_count = 0;
  for (const auto* row : included) {
    if (row->owner != owner) {
      continue;
    }
    ++included_count;
    if (last_included == nullptr || row->occurred_at <= last_included->occurred_at) {
      last_included = row;
    }
  }
  const bool source_more = !page.next_token.empty();
  if (included_count == 0) {
    // None of this owner's returned rows entered the merged page. Keep its previous keyset
    // cursor so the next page retries the same bounded window; advancing past it would drop rows
    // that are older than the global cut but still not emitted.
    return OwnerAuditCursor{false, std::string{previous_token}};
  }
  if (!source_more && included_count == page.rows.size()) {
    return OwnerAuditCursor{true, {}};
  }
  return OwnerAuditCursor{false, audit_cursor(*last_included)};
}
bool audit_id_greater(std::string_view left, std::string_view right) {
  const auto parse = [](std::string_view value) -> std::optional<std::uint64_t> {
    if (value.empty() || value.size() > 20) {
      return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      return std::nullopt;
    }
    return result;
  };
  const auto left_number = parse(left);
  const auto right_number = parse(right);
  if (left_number && right_number) {
    return *left_number > *right_number;
  }
  return left > right;
}
void sort_audit_rows(std::vector<AuditLogRow>& rows) {
  std::sort(rows.begin(), rows.end(), [](const AuditLogRow& left, const AuditLogRow& right) {
    if (left.occurred_at != right.occurred_at) {
      return left.occurred_at > right.occurred_at;
    }
    if (left.owner != right.owner) {
      return left.owner < right.owner;
    }
    return audit_id_greater(left.entry_id, right.entry_id);
  });
}
std::string json_time_point(core::target::TimePoint value) {
  const auto encoded = codec::to_wire(value);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    return json_time(*timestamp);
  }
  return "";
}
std::string resource_text(const AuditLogRow& row) {
  if (row.resource_id.empty()) {
    return "";
  }
  return std::string{wire::ResourceKind_Name(row.resource_kind)} + ":" + row.resource_id;
}
Json audit_page_json(std::vector<AuditLogRow>& rows, std::uint32_t page_size,
                     const std::optional<std::string>& next_token) {
  sort_audit_rows(rows);
  Json items = Json::array();
  const auto take = std::min<std::size_t>(rows.size(), page_size);
  for (std::size_t index = 0; index < take; ++index) {
    const auto& row = rows[index];
    Json item{{"id", row.entry_id},          {"occurred_at", json_time_point(row.occurred_at)},
              {"owner", row.owner},          {"actor", row.actor},
              {"action", row.method},        {"resource", resource_text(row)},
              {"request_id", row.request_id}};
    if (!row.account_id.empty()) {
      item["account_id"] = row.account_id;
    }
    if (!row.conversation_id.empty()) {
      item["conversation_id"] = row.conversation_id;
    }
    if (!row.rejection.empty() && row.rejection != "ERROR_CODE_UNSPECIFIED") {
      item["rejection"] = row.rejection;
    }
    items.push_back(std::move(item));
  }
  Json page{{"page_size", page_size}};
  if (next_token) {
    page["next_page_token"] = *next_token;
  }
  return {{"items", std::move(items)}, {"page", std::move(page)}};
}
Json logs_page_json(std::vector<AuditLogRow>& rows, std::uint32_t page_size,
                    const std::optional<std::string>& next_token) {
  sort_audit_rows(rows);
  Json items = Json::array();
  const auto take = std::min<std::size_t>(rows.size(), page_size);
  for (std::size_t index = 0; index < take; ++index) {
    const auto& row = rows[index];
    Json item{{"id", row.entry_id},
              {"name", row.component},
              {"occurred_at", json_time_point(row.occurred_at)},
              {"owner", row.owner},
              {"level", row.severity == "rejected" ? "ERROR"
                        : row.severity == "warn"   ? "WARN"
                        : row.severity == "debug"  ? "DEBUG"
                                                   : "INFO"},
              {"component", row.component},
              {"message", row.message}};
    if (!row.trace_id.empty()) {
      item["trace_id"] = row.trace_id;
    }
    items.push_back(std::move(item));
  }
  Json page{{"page_size", page_size}};
  if (next_token) {
    page["next_page_token"] = *next_token;
  }
  return {{"items", std::move(items)}, {"page", std::move(page)}};
}

} // namespace

std::optional<OwnerHttpResult> handle_audit_logs(const RequestContext& context,
                                                 const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  if (path == "/api/v1/logs" || path == "/api/v1/audit") {
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    if (!ports.audit_logs_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    const bool logs = path == "/api/v1/logs";
    const auto kind = logs ? AuditLogKind::Logs : AuditLogKind::Audit;
    const auto permission = logs ? wire::PERMISSION_LOG_READ : wire::PERMISSION_AUDIT_READ;
    auto query = parse_query(request.target);
    if (!query) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    for (const auto& [name, value] : *query) {
      (void)value;
      if (name != "page_size" && name != "page_token" && name != "occurred_from" &&
          name != "occurred_until" && name != "actor" && name != "resource") {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
    }
    AuditLogFilter filter;
    if (const auto size = query->find("page_size"); size != query->end()) {
      std::uint32_t value = 0;
      const auto parsed =
          std::from_chars(size->second.data(), size->second.data() + size->second.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != size->second.data() + size->second.size() ||
          value == 0 || value > 100) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      filter.page_size = value;
    }
    AuditPageCursor page_cursor;
    if (const auto token = query->find("page_token"); token != query->end()) {
      auto parsed = parse_audit_page_cursor(token->second);
      if (!parsed) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      page_cursor = std::move(*parsed);
    }
    const auto time_filter = [&](const char* name) -> std::optional<core::target::TimePoint> {
      const auto found = query->find(name);
      if (found == query->end()) {
        return core::target::TimePoint{};
      }
      google::protobuf::Timestamp timestamp;
      if (!google::protobuf::util::TimeUtil::FromString(found->second, &timestamp)) {
        return std::nullopt;
      }
      const auto decoded = codec::from_wire(timestamp);
      if (const auto* value = std::get_if<core::target::TimePoint>(&decoded)) {
        return *value;
      }
      return std::nullopt;
    };
    auto occurred_from = time_filter("occurred_from");
    auto occurred_until = time_filter("occurred_until");
    if (!occurred_from || !occurred_until) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    if (*occurred_from != core::target::TimePoint{} &&
        *occurred_until != core::target::TimePoint{} && *occurred_from > *occurred_until) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    filter.occurred_from = *occurred_from == core::target::TimePoint{}
                               ? std::nullopt
                               : std::optional<core::target::TimePoint>{*occurred_from};
    filter.occurred_until = *occurred_until == core::target::TimePoint{}
                                ? std::nullopt
                                : std::optional<core::target::TimePoint>{*occurred_until};
    if (const auto actor = query->find("actor"); actor != query->end()) {
      if (actor->second.empty() || actor->second.size() > 256) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      filter.actor = actor->second;
    }
    if (const auto resource = query->find("resource"); resource != query->end()) {
      wire::ResourceRef parsed_resource;
      if (!google::protobuf::util::JsonStringToMessage(resource->second, &parsed_resource).ok() ||
          parsed_resource.id().empty() ||
          parsed_resource.kind() == wire::RESOURCE_KIND_UNSPECIFIED) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      filter.resource = std::move(parsed_resource);
    }
    // Identity enforces its Control peer assignment directly; Runtime requires a fresh one-shot
    // delegation for the same sensitive permission on the Control catalog scope. A source
    // already exhausted by a previous composite token is skipped without a spurious call.
    auto identity_filter = filter;
    identity_filter.page_token = page_cursor.identity.token;
    auto runtime_filter = filter;
    runtime_filter.page_token = page_cursor.runtime.token;
    AuditLogSourcePage identity_source;
    if (!page_cursor.identity.exhausted) {
      auto identity_page = ports.audit_logs_->identity(kind, identity_filter);
      if (const auto* failure = std::get_if<OwnerFailure>(&identity_page)) {
        return context.failure(*failure);
      }
      identity_source = std::move(std::get<AuditLogSourcePage>(identity_page));
    }
    AuditLogSourcePage runtime_source;
    if (!page_cursor.runtime.exhausted) {
      auto delegation =
          context.delegate(ports.delegation_, "runtime", permission, control_catalog_scope());
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      auto runtime_page =
          ports.audit_logs_->runtime(kind, runtime_filter, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&runtime_page)) {
        return context.failure(*failure);
      }
      runtime_source = std::move(std::get<AuditLogSourcePage>(runtime_page));
    }
    auto rows = identity_source.rows;
    rows.insert(rows.end(), runtime_source.rows.begin(), runtime_source.rows.end());
    // Merge order: occurred_at DESC, owner ASC, entry id DESC (numeric when both ids are
    // decimal) for a deterministic cross-source keyset.
    sort_audit_rows(rows);
    const auto take = std::min<std::size_t>(rows.size(), filter.page_size);
    std::vector<AuditLogRow> page_rows(rows.begin(), rows.begin() + take);
    std::vector<const AuditLogRow*> included;
    included.reserve(page_rows.size());
    for (const auto& row : page_rows) {
      included.push_back(&row);
    }
    // Each owner gets its own next keyset cursor. The previous single shared cursor skipped
    // rows when one owner's page contributed only a prefix of the merged page.
    auto identity_cursor =
        page_cursor.identity.exhausted
            ? std::optional<OwnerAuditCursor>{OwnerAuditCursor{true, {}}}
            : next_owner_cursor(identity_source, included, "identity", identity_filter.page_token);
    auto runtime_cursor =
        page_cursor.runtime.exhausted
            ? std::optional<OwnerAuditCursor>{OwnerAuditCursor{true, {}}}
            : next_owner_cursor(runtime_source, included, "runtime", runtime_filter.page_token);
    if (!identity_cursor || !runtime_cursor) {
      return context.error(wire::INTERNAL, false);
    }
    std::optional<std::string> next_token;
    if (!identity_cursor->exhausted || !runtime_cursor->exhausted) {
      next_token = encode_audit_page_cursor({*identity_cursor, *runtime_cursor});
    }
    Json body = logs ? logs_page_json(page_rows, filter.page_size, next_token)
                     : audit_page_json(page_rows, filter.page_size, next_token);
    return OwnerHttpResult{200, body.dump()};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
