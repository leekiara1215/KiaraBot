#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/host.grpc.pb.h"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"
#include "kiarabot/v1/services.grpc.pb.h"

namespace {

class GrpcProcessLifetime final {
public:
  GrpcProcessLifetime() {
    grpc_init();
  }
  ~GrpcProcessLifetime() {
    // Keep the final shutdown on main, after every channel/context/stub is destroyed.
    // Otherwise the last worker can start detached cleanup while OpenSSL exits.
    grpc_shutdown();
  }
  GrpcProcessLifetime(const GrpcProcessLifetime&) = delete;
  GrpcProcessLifetime& operator=(const GrpcProcessLifetime&) = delete;
};

} // namespace

std::string read(const char* path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int main(int argc, char* argv[]) {
  const GrpcProcessLifetime grpc_lifetime;
  if (argc != 7) {
    return 2;
  }
  const std::string service = argv[1];
  grpc::SslCredentialsOptions options;
  options.pem_root_certs = read(argv[3]);
  const bool authenticated = std::string(argv[6]) == "authenticated";
  if (authenticated) {
    options.pem_cert_chain = read(argv[4]);
    options.pem_private_key = read(argv[5]);
  }
  const auto channel = grpc::CreateChannel(argv[2], grpc::SslCredentials(options));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{3});
  grpc::Status result;
  if (service == "identity") {
    kiarabot::v1::IdentityLoginRequest request;
    kiarabot::v1::IdentityLoginResponse response;
    result = kiarabot::v1::IdentityService::NewStub(channel)->Login(&context, request, &response);
  } else if (service == "runtime") {
    kiarabot::v1::RuntimePublishEventRequest request;
    kiarabot::v1::RuntimePublishEventResponse response;
    result =
        kiarabot::v1::RuntimeService::NewStub(channel)->PublishEvent(&context, request, &response);
  } else if (service == "host") {
    kiarabot::v1::HostRegisterHostRequest request;
    kiarabot::v1::HostRegisterHostResponse response;
    result =
        kiarabot::v1::HostService::NewStub(channel)->RegisterHost(&context, request, &response);
  } else if (service == "scheduler") {
    kiarabot::v1::SchedulerGetTaskRequest request;
    kiarabot::v1::SchedulerGetTaskResponse response;
    result =
        kiarabot::v1::SchedulerService::NewStub(channel)->GetTask(&context, request, &response);
  } else if (service == "model") {
    kiarabot::v1::ModelGatewayChatRequest request;
    kiarabot::v1::ModelGatewayChatResponse response;
    result = kiarabot::v1::ModelService::NewStub(channel)->Chat(&context, request, &response);
  } else if (service == "adapter-bridge") {
    const auto stream = kiarabot::v1::AdapterBridge::NewStub(channel)->Connect(&context);
    stream->WritesDone();
    result = stream->Finish();
  } else if (service == "plugin-bridge") {
    const auto stream = kiarabot::v1::PluginBridge::NewStub(channel)->Connect(&context);
    stream->WritesDone();
    result = stream->Finish();
  } else if (service == "host-bridge") {
    const auto stream = kiarabot::v1::HostBridge::NewStub(channel)->Connect(&context);
    stream->WritesDone();
    result = stream->Finish();
  } else {
    return 2;
  }
  if (authenticated && result.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
    return 0;
  }
  if (!authenticated && (result.error_code() == grpc::StatusCode::UNAVAILABLE ||
                         result.error_code() == grpc::StatusCode::UNAUTHENTICATED)) {
    return 0;
  }
  std::cerr << "Unexpected RPC status " << result.error_code() << '\n';
  return 1;
}
