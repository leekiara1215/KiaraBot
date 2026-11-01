#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pqxx/pqxx>

#include "kiarabot/core/domain/target/persistence.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// libpq TEXT uses zero-terminated parameters; reject NUL before it can alias/truncate data.
// Binary Task payloads/digests are intentionally not subject to this text representation rule.
bool valid_sql_text(std::string_view value) noexcept;
bool valid_sql_text_fields(std::initializer_list<std::string_view> values) noexcept;
bool valid_text(const core::target::Operation& value) noexcept;
bool valid_text(const core::target::HandoffContext& value) noexcept;
bool valid_text(const core::target::ReplayResult& value);
std::string timestamp(core::target::TimePoint value);
core::target::TimePoint time_field(const pqxx::row& row, const char* column);
std::optional<core::target::TimePoint> optional_time(const pqxx::row& row, const char* column);
std::string digest(const core::target::Sha256Digest& value);
core::target::Sha256Digest read_digest(const pqxx::field& field);
std::string time_projection(std::string_view column);
struct RecordSpec {
  std::string_view table;
  std::vector<std::string_view> keys;
  core::target::Service owner;
};
RecordSpec record_spec(RecordKind kind);
std::vector<std::string> record_key(const core::target::ReplayResult& value);
core::target::ResourceVersion record_version(const core::target::ReplayResult& value);
bool store_live(pqxx::transaction_base& transaction, RecordKind kind,
                const core::target::ReplayResult& value,
                std::optional<core::target::ResourceVersion> expected);
std::optional<core::target::ReplayResult>
load_live(pqxx::transaction_base& transaction, RecordKind kind, std::span<const std::string> key);
std::int64_t store_result(pqxx::transaction_base& transaction,
                          const core::target::ReplayResult& result);
core::target::ReplayResult load_result(pqxx::transaction_base& transaction, std::int64_t id);
core::target::Operation read_operation(const pqxx::row& row);
core::target::HandoffContext read_handoff(const pqxx::row& row);
pqxx::params handoff_params(const core::target::HandoffContext& handoff);
inline constexpr std::string_view kHandoffFields =
    "source,message,operation_id,operation_owner,operation_resource_kind,operation_resource_id,"
    "causation,resource_kind,resource_id,resource_version,actor,"
    "authorization_version,payload_hash,created_at,valid_until";
std::string handoff_projection();

} // namespace kiarabot::infrastructure::postgres::detail
