#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <pqxx/pqxx>

#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::infrastructure::postgres::detail {

std::string timestamp(core::target::TimePoint value);
core::target::TimePoint time_field(const pqxx::row& row, const char* column);
std::optional<core::target::TimePoint> optional_time(const pqxx::row& row, const char* column);
std::string digest(const core::target::Sha256Digest& value);
core::target::Sha256Digest read_digest(const pqxx::field& field);
std::string time_projection(std::string_view column);
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
