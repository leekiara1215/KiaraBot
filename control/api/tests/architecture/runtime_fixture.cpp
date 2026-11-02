// P12e-2 real-mTLS Runtime owner fixture: only the read-only ListAudit/ListLogs projection.
// It exercises Control's cross-owner merge/pagination over a second authoritative source; it is
// a transport fixture, not Runtime policy/PG evidence.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/v1/runtime.grpc.pb.h"

namespace {
namespace wire = kiarabot::v1;

std::string read(const char* path) {
  std::ifstream input(path);
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}
struct AuditRow {
  std::string id;
  std::string actor = "bob";
  std::int64_t nanos = 0;
  std::string method = "UpdateAccount";
};
std::vector<AuditRow> audit_rows() {
  return {{"20", "bob", 4'500'000'000, "UpdateAccount"},
          {"40", "bob", 3'000'000'000, "SendMessage"},
          {"19", "bob", 2'000'000'000, "GetAccount"}};
}
std::int64_t timestamp_nanos(const google::protobuf::Timestamp& value) {
  return value.seconds() * 1'000'000'000LL + value.nanos();
}
std::optional<std::pair<std::int64_t, std::string>> audit_token(std::string_view value) {
  const auto colon = value.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  std::int64_t nanos = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + colon, nanos);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + colon) {
    return std::nullopt;
  }
  return std::pair{nanos, std::string{value.substr(colon + 1)}};
}
template <typename Filter> std::vector<AuditRow> filter_rows(const Filter& filter) {
  std::vector<AuditRow> rows;
  const auto cursor = filter.page().page_token().empty()
                          ? std::optional<std::pair<std::int64_t, std::string>>{}
                          : audit_token(filter.page().page_token());
  for (const auto& row : audit_rows()) {
    if (!filter.actor().empty() && row.actor != filter.actor()) {
      continue;
    }
    if (filter.has_resource() &&
        (filter.resource().kind() != wire::RESOURCE_CATALOG ||
         filter.resource().id() != "catalog")) {
      continue;
    }
    if (filter.has_occurred_from() && row.nanos < timestamp_nanos(filter.occurred_from())) {
      continue;
    }
    if (filter.has_occurred_until() && row.nanos >= timestamp_nanos(filter.occurred_until())) {
      continue;
    }
    if (cursor && (row.nanos > cursor->first ||
                   (row.nanos == cursor->first && row.id >= cursor->second))) {
      continue;
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [](const AuditRow& left, const AuditRow& right) {
    return left.nanos != right.nanos ? left.nanos > right.nanos : left.id > right.id;
  });
  return rows;
}
void stamp(std::int64_t nanos, google::protobuf::Timestamp* output) {
  output->set_seconds(nanos / 1'000'000'000LL);
  output->set_nanos(static_cast<std::int32_t>(nanos % 1'000'000'000LL));
}
void fill_audit(const AuditRow& row, wire::RuntimeAuditRecord* output) {
  output->set_entry_id(row.id);
  output->set_actor(row.actor);
  output->mutable_resource()->set_kind(wire::RESOURCE_CATALOG);
  output->mutable_resource()->set_id("catalog");
  output->set_request_id("request-" + row.id);
  output->set_trace_id("trace-" + row.id);
  output->set_method(row.method);
  output->set_rejection(wire::ERROR_CODE_UNSPECIFIED);
  output->set_authorization_version(9);
  stamp(row.nanos, output->mutable_occurred_at());
}
void fill_log(const AuditRow& row, wire::RuntimeLogRecord* output) {
  output->set_entry_id(row.id);
  output->set_severity("info");
  output->set_component("runtime");
  output->set_method(row.method);
  output->set_actor(row.actor);
  output->mutable_resource()->set_kind(wire::RESOURCE_CATALOG);
  output->mutable_resource()->set_id("catalog");
  output->set_request_id("request-" + row.id);
  output->set_trace_id("trace-" + row.id);
  output->set_message("request.recorded");
  stamp(row.nanos, output->mutable_occurred_at());
}
class Fixture final : public wire::RuntimeService::Service {
public:
  grpc::Status ListAudit(grpc::ServerContext* context,
                         const wire::RuntimeListAuditRequest* input,
                         wire::RuntimeListAuditResponse* output) override {
    if (!input->has_filter() || input->filter().scope().resources_size() != 1 ||
        input->context().delegation().empty()) {
      return {grpc::StatusCode::PERMISSION_DENIED, "fixture requires delegation", {}};
    }
    auto rows = filter_rows(input->filter());
    const auto size = std::min<std::size_t>(
        rows.size(), input->filter().page().page_size() == 0
                         ? 20
                         : static_cast<std::size_t>(input->filter().page().page_size()));
    for (std::size_t index = 0; index < size; ++index) {
      fill_audit(rows[index], output->add_entries());
    }
    if (rows.size() > size) {
      output->mutable_page()->set_next_page_token(std::to_string(rows[size - 1].nanos) + ":" +
                                                  rows[size - 1].id);
    }
    output->mutable_page()->set_page_size(static_cast<std::uint32_t>(size));
    output->mutable_checked_at()->set_seconds(1'700'000'000);
    return grpc::Status::OK;
  }
  grpc::Status ListLogs(grpc::ServerContext* context, const wire::RuntimeListLogsRequest* input,
                        wire::RuntimeListLogsResponse* output) override {
    if (!input->has_filter() || input->filter().scope().resources_size() != 1 ||
        input->context().delegation().empty()) {
      return {grpc::StatusCode::PERMISSION_DENIED, "fixture requires delegation", {}};
    }
    auto rows = filter_rows(input->filter());
    const auto size = std::min<std::size_t>(
        rows.size(), input->filter().page().page_size() == 0
                         ? 20
                         : static_cast<std::size_t>(input->filter().page().page_size()));
    for (std::size_t index = 0; index < size; ++index) {
      fill_log(rows[index], output->add_entries());
    }
    if (rows.size() > size) {
      output->mutable_page()->set_next_page_token(std::to_string(rows[size - 1].nanos) + ":" +
                                                  rows[size - 1].id);
    }
    output->mutable_page()->set_page_size(static_cast<std::uint32_t>(size));
    output->mutable_checked_at()->set_seconds(1'700'000'000);
    return grpc::Status::OK;
  }
};
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    return 2;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = read(argv[1]);
  options.pem_key_cert_pairs.push_back({read(argv[3]), read(argv[2])});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  Fixture fixture;
  builder.RegisterService(&fixture);
  auto server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  std::cout << port << '\n' << std::flush;
  server->Wait();
}
