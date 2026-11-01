#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {
// Descriptor checksum only, not a security primitive. Fixed PG16.4 catalog rendering covers
// all relation columns/defaults/identity, constraints, indexes, sequence settings and domain.
inline constexpr std::string_view kDescriptorQuery = R"sql(
SELECT md5(string_agg(descriptor,E'\n' ORDER BY descriptor)) FROM (
  SELECT 'column:'||c.relname||':'||a.attnum||':'||a.attname||':'||format_type(a.atttypid,a.atttypmod)||':'||
    a.attnotnull||':'||a.attidentity::text||':'||COALESCE(pg_get_expr(d.adbin,d.adrelid),'') AS descriptor
    FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
    JOIN pg_attribute a ON a.attrelid=c.oid AND a.attnum>0 AND NOT a.attisdropped
    LEFT JOIN pg_attrdef d ON d.adrelid=c.oid AND d.adnum=a.attnum
    WHERE n.nspname='kiarabot' AND c.relkind='r'
  UNION ALL
  SELECT 'constraint:'||COALESCE(c.relname,t.typname)||':'||v.contype::text||':'||
    pg_get_constraintdef(v.oid)||':'||v.convalidated||':'||v.condeferrable||':'||v.condeferred
    FROM pg_constraint v JOIN pg_namespace n ON n.oid=v.connamespace
    LEFT JOIN pg_class c ON c.oid=v.conrelid LEFT JOIN pg_type t ON t.oid=v.contypid
    WHERE n.nspname='kiarabot'
  UNION ALL
  SELECT 'index:'||indexdef FROM pg_indexes WHERE schemaname='kiarabot'
  UNION ALL
  SELECT 'domain:'||t.typname||':'||format_type(t.typbasetype,t.typtypmod)||':'||t.typnotnull
    FROM pg_type t JOIN pg_namespace n ON n.oid=t.typnamespace WHERE n.nspname='kiarabot' AND t.typtype='d'
  UNION ALL
  SELECT 'sequence:'||sequencename||':'||data_type||':'||start_value||':'||min_value||':'||max_value||':'||increment_by||':'||cycle
    FROM pg_sequences WHERE schemaname='kiarabot'
) descriptors
)sql";
inline constexpr std::string_view descriptor(core::target::Service owner) {
  switch (owner) {
  case core::target::Service::Identity:
    return "d74a1a8ae6ec77c1178dfae43d904edc";
  case core::target::Service::Runtime:
    return "7d31916ae3ece03e99a9d5b1b3bbaaed";
  case core::target::Service::Scheduler:
    return "e1fc9dd761a457cd6b9b7ea279c9e5a0";
  case core::target::Service::Model:
    return "1e4a6b2e0ed5e62cce290cdd6ba30ce3";
  default:
    return {};
  }
}
inline constexpr int relation_count(core::target::Service owner) {
  switch (owner) {
  case core::target::Service::Identity:
    return 100;
  case core::target::Service::Runtime:
    return 54;
  case core::target::Service::Scheduler:
    return 53;
  case core::target::Service::Model:
    return 48;
  default:
    return 0;
  }
}
} // namespace kiarabot::infrastructure::postgres::schema
