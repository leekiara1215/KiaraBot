#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "kiarabot/core/runtime/bridge_server.hpp"

namespace {
void require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
void expect_error(const grpc::Status& status, grpc::StatusCode transport,
                  kiarabot::v1::ErrorCode code) {
  kiarabot::v1::Error error;
  require(status.error_code() == transport, "transport error mismatch");
  require(error.ParseFromString(status.error_details()) && error.code() == code,
          "stable error mismatch");
}
} // namespace

int main() {
  try {
    using kiarabot::core::BridgeCredential;
    using kiarabot::core::BridgeRole;
    using kiarabot::core::BridgeServer;
    const std::string adapter_token(32, 'a');
    const std::string plugin_token(32, 'p');
    const std::vector<BridgeCredential> credentials = {
        {BridgeRole::Adapter, "adapter", adapter_token},
        {BridgeRole::Plugin, "plugin", plugin_token}};
    BridgeServer server(credentials);
    require(!server.start("0.0.0.0:0"), "external binding accepted");
    require(!server.start("127.0.0.1:invalid"), "invalid port accepted");
    require(!server.start("127.0.0.1:65536"), "overflow port accepted");
    BridgeServer empty({});
    require(!empty.start("127.0.0.1:0"), "empty credentials accepted");
    BridgeServer duplicate({credentials[0], credentials[0]});
    require(!duplicate.start("127.0.0.1:0"), "duplicate credentials accepted");
    require(server.start("127.0.0.1:0") && server.port() > 0, "listen failed");
    const std::string address = "127.0.0.1:" + std::to_string(server.port());
    BridgeServer occupied(credentials);
    require(!occupied.start(address), "occupied port accepted");
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto adapters = kiarabot::v1::AdapterBridge::NewStub(channel);
    auto plugins = kiarabot::v1::PluginBridge::NewStub(channel);
    auto call_adapter = [&](const std::string& authorization, const std::string& id,
                            bool duplicate_header = false, bool send = true) {
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      if (!authorization.empty()) {
        context.AddMetadata("authorization", authorization);
        if (duplicate_header) {
          context.AddMetadata("authorization", authorization);
        }
      }
      auto stream = adapters->Connect(&context);
      if (send) {
        kiarabot::v1::AdapterToCore request;
        request.mutable_register_()->set_adapter_id(id);
        stream->Write(request);
      }
      stream->WritesDone();
      kiarabot::v1::CoreToAdapter response;
      require(!stream->Read(&response), "unimplemented operation acknowledged");
      return stream->Finish();
    };
    expect_error(call_adapter("", "adapter"), grpc::StatusCode::UNAUTHENTICATED,
                 kiarabot::v1::UNAUTHENTICATED);
    expect_error(call_adapter("Bearer wrong", "adapter"), grpc::StatusCode::UNAUTHENTICATED,
                 kiarabot::v1::UNAUTHENTICATED);
    expect_error(call_adapter("Bearer " + adapter_token, "adapter", true),
                 grpc::StatusCode::UNAUTHENTICATED, kiarabot::v1::UNAUTHENTICATED);
    expect_error(call_adapter("Bearer " + plugin_token, "adapter"),
                 grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
    expect_error(call_adapter("Bearer " + adapter_token, "other"),
                 grpc::StatusCode::PERMISSION_DENIED, kiarabot::v1::PERMISSION_DENIED);
    expect_error(call_adapter("Bearer " + adapter_token, "adapter"), grpc::StatusCode::INTERNAL,
                 kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
    require(call_adapter("Bearer " + adapter_token, "", false, false).ok(), "half close failed");
    {
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      context.AddMetadata("authorization", "Bearer " + plugin_token);
      auto stream = plugins->Connect(&context);
      kiarabot::v1::PluginToCore request;
      request.mutable_register_()->mutable_manifest()->set_id("plugin");
      stream->Write(request);
      stream->WritesDone();
      kiarabot::v1::CoreToPlugin response;
      require(!stream->Read(&response), "plugin falsely registered");
      expect_error(stream->Finish(), grpc::StatusCode::INTERNAL,
                   kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
    }
    {
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      context.AddMetadata("authorization", "Bearer " + adapter_token);
      auto stream = adapters->Connect(&context);
      stream->Write(kiarabot::v1::AdapterToCore{});
      stream->WritesDone();
      kiarabot::v1::CoreToAdapter response;
      require(!stream->Read(&response), "empty message accepted");
      expect_error(stream->Finish(), grpc::StatusCode::INVALID_ARGUMENT,
                   kiarabot::v1::INVALID_ARGUMENT);
    }
    {
      grpc::ClientContext context;
      context.AddMetadata("authorization", "Bearer " + adapter_token);
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
      auto stream = adapters->Connect(&context);
      kiarabot::v1::CoreToAdapter response;
      require(!stream->Read(&response), "timeout produced response");
      require(stream->Finish().error_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
              "deadline not enforced");
    }
    {
      grpc::ClientContext context;
      context.AddMetadata("authorization", "Bearer " + adapter_token);
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      auto stream = adapters->Connect(&context);
      stream->WaitForInitialMetadata(); // Authorized handler is blocked on Read.
      server.stop();
      kiarabot::v1::CoreToAdapter response;
      require(!stream->Read(&response), "shutdown produced response");
      require(!stream->Finish().ok(), "active stream succeeded during shutdown");
      server.stop();
    }
    require(server.start(address), "port not released on shutdown");
    server.stop();
    std::cout << "Bridge authentication, refusal, deadlines and shutdown passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
