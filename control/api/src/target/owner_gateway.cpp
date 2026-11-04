#include "kiarabot/control/target/owner_gateway.hpp"

#include <array>
#include <string_view>

#include "owner_gateway/internal.hpp"

namespace kiarabot::control::target {
namespace wire = kiarabot::v1;

OwnerHttpResult OwnerGateway::live() {
  return {200, R"({"state":"HEALTHY"})"};
}

OwnerHttpResult OwnerGateway::ready(bool ready, std::string_view request_id) {
  if (ready) {
    return {200, R"({"state":"HEALTHY"})"};
  }
  return {503, owner_error_body(wire::UPSTREAM_UNAVAILABLE, request_id, request_id, true)};
}

std::optional<OwnerHttpResult> OwnerGateway::handle(const OwnerHttpRequest& request) {
  try {
    const auto separator = request.target.find('?');
    const owner_gateway::RequestContext context{
        request, std::string_view{request.target}.substr(0, separator),
        owner_gateway::request_identifier(request), owner_gateway::trace_identifier(request)};
    const owner_gateway::Ports ports{
        delegation_, runtime_,         principals_,           health_,
        resources_,  tasks_,           operations_,           model_,
        audit_logs_, secret_delivery_, bootstrap_recipients_, host_management_};
    // Preserve precedence: catalog reads precede command families; unmatched requests remain
    // available to the other HTTPS handlers. Each family retains its own method/error rules.
    constexpr std::array<owner_gateway::Handler, 13> kHandlers{
        owner_gateway::handle_health,     owner_gateway::handle_principals,
        owner_gateway::handle_catalogs,   owner_gateway::handle_operations,
        owner_gateway::handle_bootstrap,  owner_gateway::handle_accounts,
        owner_gateway::handle_instances,  owner_gateway::handle_tasks,
        owner_gateway::handle_audit_logs, owner_gateway::handle_lifecycle,
        owner_gateway::handle_models,     owner_gateway::handle_actions,
        owner_gateway::handle_hosts,
    };
    for (const auto handler : kHandlers) {
      if (auto result = handler(context, ports)) {
        return result;
      }
    }
    return std::nullopt;
  } catch (...) {
    const auto request_id = owner_gateway::random_identifier();
    return OwnerHttpResult{500, owner_error_body(wire::INTERNAL, request_id, request_id, false)};
  }
}

} // namespace kiarabot::control::target
