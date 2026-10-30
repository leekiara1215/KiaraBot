#include "kiarabot/core/bridge_server.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <string_view>
#include <thread>
#include <utility>

#include "kiarabot/core/action_router.hpp"
#include "kiarabot/core/adapter_ingress.hpp"
#include "kiarabot/core/host_dispatcher.hpp"
#include "kiarabot/core/plugin_hub.hpp"

namespace kiarabot::core {
namespace {

grpc::Status failure(grpc::StatusCode status, kiarabot::v1::ErrorCode code) {
  kiarabot::v1::Error error;
  error.set_code(code);
  error.set_message(kiarabot::v1::ErrorCode_Name(code));
  return {status, error.message(), error.SerializeAsString()};
}

// No content-dependent early exit when comparing equal-length credentials.
bool token_matches(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  unsigned int difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
  }
  return difference == 0;
}

const BridgeCredential* authenticate(grpc::ServerContext& context,
                                     const std::vector<BridgeCredential>& credentials) {
  const auto& metadata = context.client_metadata();
  if (metadata.count("authorization") != 1) {
    return nullptr;
  }
  const auto value = metadata.find("authorization")->second;
  const std::string_view header(value.data(), value.size());
  if (!header.starts_with("Bearer ")) {
    return nullptr;
  }
  for (const auto& credential : credentials) {
    if (token_matches(header.substr(7), credential.token)) {
      return &credential;
    }
  }
  return nullptr;
}

bool valid_credentials(const std::vector<BridgeCredential>& credentials) {
  if (credentials.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < credentials.size(); ++i) {
    const auto& value = credentials[i];
    if (value.identity.empty() || value.identity.size() > 256 || value.token.size() < 32 ||
        value.token.size() > 4096 ||
        !std::all_of(value.token.begin(), value.token.end(),
                     [](unsigned char character) { return character >= 33 && character <= 126; })) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (value.token == credentials[j].token ||
          (value.role == credentials[j].role && value.identity == credentials[j].identity)) {
        return false;
      }
    }
  }
  return true;
}

template <typename Request, typename Response, typename Identity>
grpc::Status
handle(grpc::ServerContext& context, grpc::ServerReaderWriter<Response, Request>& stream,
       const std::vector<BridgeCredential>& credentials, BridgeRole role, Identity identity) {
  try {
    const auto* principal = authenticate(context, credentials);
    if (principal == nullptr) {
      return failure(grpc::StatusCode::UNAUTHENTICATED, kiarabot::v1::UNAUTHENTICATED);
    }
    if (principal->role != role) {
      return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
    }
    stream.SendInitialMetadata();
    Request request;
    if (!stream.Read(&request)) {
      return context.IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Cancelled")
                                   : grpc::Status::OK;
    }
    if (request.body_case() == Request::BODY_NOT_SET) {
      return failure(grpc::StatusCode::INVALID_ARGUMENT, kiarabot::v1::INVALID_ARGUMENT);
    }
    if (request.has_register_() && identity(request) != principal->identity) {
      return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
    }
    // Later tasks replace this explicit refusal with durable business handlers.
    return failure(grpc::StatusCode::INTERNAL, kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
  } catch (...) {
    return failure(grpc::StatusCode::INTERNAL, kiarabot::v1::INTERNAL);
  }
}

} // namespace

class BridgeServer::AdapterService final : public kiarabot::v1::AdapterBridge::Service {
public:
  AdapterService(const std::vector<BridgeCredential>& credentials,
                 std::shared_ptr<AdapterIngress> ingress, std::shared_ptr<ActionRouter> actions)
      : credentials_(credentials), ingress_(std::move(ingress)), actions_(std::move(actions)) {}
  grpc::Status Connect(grpc::ServerContext* context,
                       grpc::ServerReaderWriter<kiarabot::v1::CoreToAdapter,
                                                kiarabot::v1::AdapterToCore>* stream) override {
    if (!ingress_) {
      return handle(*context, *stream, credentials_, BridgeRole::Adapter,
                    [](const auto& request) { return request.register_().adapter_id(); });
    }
    try {
      const auto* principal = authenticate(*context, credentials_);
      if (!principal) {
        return failure(grpc::StatusCode::UNAUTHENTICATED, kiarabot::v1::UNAUTHENTICATED);
      }
      if (principal->role != BridgeRole::Adapter) {
        return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
      }
      stream->SendInitialMetadata();
      kiarabot::v1::AdapterToCore request;
      if (!stream->Read(&request)) {
        return context->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Cancelled")
                                      : grpc::Status::OK;
      }
      if (!request.has_register_()) {
        return failure(grpc::StatusCode::INVALID_ARGUMENT, kiarabot::v1::INVALID_ARGUMENT);
      }
      if (request.register_().adapter_id() != principal->identity) {
        return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
      }
      auto registration = ingress_->register_adapter(principal->identity, request.register_());
      std::mutex write_mutex;
      // Always disconnect our own instance, including on failed Write/exception.
      struct SessionGuard {
        AdapterIngress& ingress;
        AdapterSession session;
        ActionRouter* actions;
        grpc::ServerContext& context;
        std::mutex done_mutex;
        std::condition_variable done_condition;
        bool done{false};
        std::jthread sender;
        ~SessionGuard() {
          sender.request_stop();
          if (sender.joinable()) {
            std::unique_lock lock(done_mutex);
            if (!done_condition.wait_for(lock, std::chrono::milliseconds(100),
                                         [&] { return done; })) {
              context.TryCancel();
            }
            lock.unlock();
            sender.join();
          }
          if (actions) {
            actions->detach(session);
          }
          ingress.disconnect(session);
        }
      } guard{*ingress_, registration.session, actions_.get(), *context, {}, {}, false, {}};
      kiarabot::v1::CoreToAdapter response;
      *response.mutable_register_result() = registration.response;
      if (!stream->Write(response)) {
        return {grpc::StatusCode::CANCELLED, "Cancelled"};
      }
      if (!registration.response.accepted()) {
        return grpc::Status::OK;
      }
      if (actions_) {
        actions_->attach(registration.session, request.register_());
        guard.sender = std::jthread([&](std::stop_token stop) {
          try {
            while (!stop.stop_requested() && !context->IsCancelled()) {
              const auto next = actions_->next(registration.session);
              if (const auto* error = std::get_if<kiarabot::v1::Error>(&next)) {
                if (error->code() == kiarabot::v1::CONFLICT && error->retryable()) {
                  // Optimistic storage conflict: nothing has been written to the stream.
                  std::this_thread::sleep_for(std::chrono::milliseconds(20));
                  continue;
                }
                context->TryCancel();
                break;
              }
              if (const auto& action =
                      std::get<std::optional<kiarabot::v1::ActionEnvelope>>(next)) {
                kiarabot::v1::CoreToAdapter output;
                *output.mutable_action() = *action;
                std::lock_guard lock(write_mutex);
                if (!stream->Write(output)) {
                  context->TryCancel();
                  break;
                }
              } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
              }
            }
          } catch (...) {
            context->TryCancel();
          }
          {
            std::lock_guard lock(guard.done_mutex);
            guard.done = true;
          }
          guard.done_condition.notify_all();
        });
      }
      while (stream->Read(&request)) {
        response.Clear();
        if (request.has_event()) {
          *response.mutable_event_ack() = ingress_->ingest(registration.session, request.event());
        } else if (request.has_action_result() && actions_) {
          if (!ingress_->current(registration.session)) {
            return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
          }
          const auto completed = actions_->complete(registration.session, request.action_result());
          if (const auto* error = std::get_if<kiarabot::v1::Error>(&completed)) {
            return failure(grpc::StatusCode::INTERNAL, error->code());
          }
          continue;
        } else if (request.has_heartbeat()) {
          if (request.heartbeat().instance_id() != registration.session.instance ||
              !ingress_->current(registration.session)) {
            return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
          }
          response.mutable_heartbeat_ack()->set_received_at_unix_ms(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
        } else {
          return failure(request.has_action_result() ? grpc::StatusCode::INTERNAL
                                                     : grpc::StatusCode::INVALID_ARGUMENT,
                         request.has_action_result() ? kiarabot::v1::CAPABILITY_NOT_SUPPORTED
                                                     : kiarabot::v1::INVALID_ARGUMENT);
        }
        {
          std::lock_guard lock(write_mutex);
          if (!stream->Write(response)) {
            return {grpc::StatusCode::CANCELLED, "Cancelled"};
          }
        }
        if (response.has_event_ack() && response.event_ack().error().retryable()) {
          return grpc::Status::OK; // Backpressure: unread/unconfirmed events stay at sender.
        }
      }
      return context->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Cancelled")
                                    : grpc::Status::OK;
    } catch (...) {
      return failure(grpc::StatusCode::INTERNAL, kiarabot::v1::INTERNAL);
    }
  }

private:
  const std::vector<BridgeCredential>& credentials_;
  std::shared_ptr<AdapterIngress> ingress_;
  std::shared_ptr<ActionRouter> actions_;
};

class BridgeServer::PluginService final : public kiarabot::v1::PluginBridge::Service {
public:
  PluginService(const std::vector<BridgeCredential>& credentials, std::shared_ptr<PluginHub> hub,
                std::shared_ptr<ActionRouter> actions, std::shared_ptr<HostDispatcher> host)
      : credentials_(credentials), hub_(std::move(hub)), actions_(std::move(actions)),
        host_(std::move(host)) {}
  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<kiarabot::v1::CoreToPlugin, kiarabot::v1::PluginToCore>* stream)
      override {
    if (!hub_) {
      return handle(*context, *stream, credentials_, BridgeRole::Plugin,
                    [](const auto& request) { return request.register_().manifest().id(); });
    }
    try {
      const auto* principal = authenticate(*context, credentials_);
      if (!principal) {
        return failure(grpc::StatusCode::UNAUTHENTICATED, kiarabot::v1::UNAUTHENTICATED);
      }
      if (principal->role != BridgeRole::Plugin) {
        return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
      }
      stream->SendInitialMetadata();
      kiarabot::v1::PluginToCore request;
      if (!stream->Read(&request)) {
        return context->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Cancelled")
                                      : grpc::Status::OK;
      }
      if (!request.has_register_()) {
        return failure(grpc::StatusCode::INVALID_ARGUMENT, kiarabot::v1::INVALID_ARGUMENT);
      }
      if (request.register_().manifest().id() != principal->identity) {
        return failure(grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
      }
      auto registration = hub_->attach(principal->identity, request.register_());
      const bool action_enabled =
          actions_ && registration.response.accepted() &&
          std::find(request.register_().supported_features().begin(),
                    request.register_().supported_features().end(),
                    "action.result.v1") != request.register_().supported_features().end();
      if (action_enabled) {
        registration.response.add_enabled_features("action.result.v1");
      }
      const bool storage_enabled =
          host_ && registration.response.accepted() &&
          std::find(request.register_().supported_features().begin(),
                    request.register_().supported_features().end(),
                    "host.storage.v1") != request.register_().supported_features().end();
      if (storage_enabled) {
        registration.response.add_enabled_features("host.storage.v1");
      }
      const bool model_enabled =
          host_ && host_->has_model() && registration.response.accepted() &&
          std::find(request.register_().supported_features().begin(),
                    request.register_().supported_features().end(),
                    "host.model.v1") != request.register_().supported_features().end();
      if (model_enabled) {
        registration.response.add_enabled_features("host.model.v1");
      }
      const bool tasks_enabled =
          host_ && host_->has_scheduler() && registration.response.accepted() &&
          std::find(request.register_().supported_features().begin(),
                    request.register_().supported_features().end(),
                    "host.tasks.v1") != request.register_().supported_features().end();
      if (tasks_enabled) {
        registration.response.add_enabled_features("host.tasks.v1");
      }
      kiarabot::v1::CoreToPlugin response;
      *response.mutable_register_result() = registration.response;
      if (!registration.response.accepted()) {
        stream->Write(response);
        return grpc::Status::OK;
      }
      std::mutex write_mutex;
      struct ConnectionGuard {
        PluginHub& hub;
        PluginSession session;
        grpc::ServerContext& context;
        std::mutex done_mutex;
        std::condition_variable done_condition;
        bool done{false};
        bool closed{false};
        std::jthread sender;
        void close() noexcept {
          if (closed) {
            return;
          }
          closed = true;
          sender.request_stop();
          if (sender.joinable()) {
            std::unique_lock lock(done_mutex);
            if (!done_condition.wait_for(lock, std::chrono::milliseconds(100),
                                         [&] { return done; })) {
              context.TryCancel(); // Unblock a peer that stopped reading a pending Write.
            }
            lock.unlock();
            sender.join();
          }
          (void)hub.detach(session);
        }
        ~ConnectionGuard() {
          close();
        }
      } guard{*hub_, registration.session, *context, {}, {}, false, false, {}};
      if (!stream->Write(response)) {
        return {grpc::StatusCode::CANCELLED, "Cancelled"};
      }
      guard.sender = std::jthread([&](std::stop_token stop) {
        try {
          while (!stop.stop_requested() && !context->IsCancelled()) {
            const auto delivery = hub_->next(registration.session);
            kiarabot::v1::CoreToPlugin output;
            if (const auto* error = std::get_if<kiarabot::v1::Error>(&delivery)) {
              if (error->code() == kiarabot::v1::CONFLICT && error->retryable()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
              }
              *output.mutable_error() = *error;
              {
                std::lock_guard lock(write_mutex);
                stream->Write(output);
              }
              context->TryCancel();
              break;
            }
            const auto& event = std::get<std::optional<kiarabot::v1::EventEnvelope>>(delivery);
            if (event) {
              *output.mutable_event() = *event;
              std::lock_guard lock(write_mutex);
              if (!stream->Write(output)) {
                context->TryCancel();
                break;
              }
            } else {
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
          }
        } catch (...) {
          context->TryCancel();
        }
        {
          std::lock_guard lock(guard.done_mutex);
          guard.done = true;
        }
        guard.done_condition.notify_all();
      });
      while (stream->Read(&request)) {
        response.Clear();
        PortStatus outcome = std::monostate{};
        if (request.has_event_ack()) {
          outcome = hub_->acknowledge(registration.session, request.event_ack());
        } else if (request.has_host_request() &&
                   (request.host_request().has_model_chat() ? model_enabled
                    : request.host_request().has_task_create() ||
                            request.host_request().has_task_get() ||
                            request.host_request().has_task_cancel()
                        ? tasks_enabled
                        : storage_enabled) &&
                   hub_->current(registration.session)) {
          *response.mutable_host_response() =
              host_->dispatch(registration.session, request.host_request());
        } else if (request.has_action() || request.has_action_query() ||
                   request.has_action_cancel()) {
          if (!hub_->current(registration.session)) {
            kiarabot::v1::Error error;
            error.set_code(kiarabot::v1::PERMISSION_DENIED);
            outcome = error;
          } else if (!action_enabled) {
            kiarabot::v1::Error error;
            error.set_code(kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
            outcome = error;
          } else {
            *response.mutable_action_response() =
                request.has_action() ? actions_->submit(registration.session, request.action())
                : request.has_action_query()
                    ? actions_->query(registration.session, request.action_query())
                    : actions_->cancel(registration.session, request.action_cancel());
          }
        } else if (request.has_heartbeat() &&
                   request.heartbeat().instance_id() == registration.session.instance &&
                   hub_->current(registration.session)) {
          response.mutable_heartbeat_ack()->set_received_at_unix_ms(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
        } else {
          kiarabot::v1::Error error;
          error.set_code(request.has_action() || request.has_host_request()
                             ? kiarabot::v1::CAPABILITY_NOT_SUPPORTED
                             : kiarabot::v1::INVALID_ARGUMENT);
          error.set_message(kiarabot::v1::ErrorCode_Name(error.code()));
          outcome = error;
        }
        const auto* rejected = std::get_if<kiarabot::v1::Error>(&outcome);
        if (rejected) {
          *response.mutable_error() = *rejected;
        }
        if (response.body_case() != kiarabot::v1::CoreToPlugin::BODY_NOT_SET) {
          std::lock_guard lock(write_mutex);
          if (!stream->Write(response)) {
            break;
          }
        }
        if (rejected) {
          break;
        }
      }
      guard.close();
      return context->IsCancelled() ? grpc::Status(grpc::StatusCode::CANCELLED, "Cancelled")
                                    : grpc::Status::OK;
    } catch (...) {
      return failure(grpc::StatusCode::INTERNAL, kiarabot::v1::INTERNAL);
    }
  }

private:
  const std::vector<BridgeCredential>& credentials_;
  std::shared_ptr<PluginHub> hub_;
  std::shared_ptr<ActionRouter> actions_;
  std::shared_ptr<HostDispatcher> host_;
};

BridgeServer::BridgeServer(std::vector<BridgeCredential> credentials,
                           std::shared_ptr<AdapterIngress> ingress,
                           std::shared_ptr<PluginHub> plugins,
                           std::shared_ptr<ActionRouter> actions,
                           std::shared_ptr<HostDispatcher> host)
    : credentials_(std::move(credentials)),
      adapters_(std::make_unique<AdapterService>(credentials_, std::move(ingress), actions)),
      plugins_(std::make_unique<PluginService>(credentials_, std::move(plugins), std::move(actions),
                                               std::move(host))) {}
BridgeServer::~BridgeServer() {
  stop();
}
bool BridgeServer::start(const std::string& address) {
  constexpr std::string_view prefix = "127.0.0.1:";
  if (server_ || !valid_credentials(credentials_) || !address.starts_with(prefix)) {
    return false;
  }
  unsigned int requested_port = 0;
  const char* begin = address.data() + prefix.size();
  const char* end = address.data() + address.size();
  const auto parsed = std::from_chars(begin, end, requested_port);
  if (parsed.ec != std::errc{} || parsed.ptr != end || requested_port > 65535) {
    return false;
  }
  grpc::ServerBuilder builder;
  builder.AddChannelArgument("grpc.so_reuseport", 0);
  builder.SetMaxReceiveMessageSize(1024 * 1024);
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &port_);
  builder.RegisterService(adapters_.get());
  builder.RegisterService(plugins_.get());
  server_ = builder.BuildAndStart();
  if (!server_) {
    port_ = 0;
  }
  return server_ != nullptr;
}
void BridgeServer::stop() {
  if (server_) {
    server_->Shutdown(std::chrono::system_clock::now());
    server_->Wait();
    server_.reset();
    port_ = 0;
  }
}
int BridgeServer::port() const {
  return port_;
}

} // namespace kiarabot::core
