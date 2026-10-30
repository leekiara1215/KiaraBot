#pragma once

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/services.grpc.pb.h"

namespace kiarabot::core {

class AdapterIngress;
class PluginHub;
class ActionRouter;
class HostDispatcher;

enum class BridgeRole { Adapter, Plugin };
struct BridgeCredential {
  BridgeRole role;
  std::string identity;
  std::string token;
};

// Owns immutable credentials and services. Lifecycle methods are single-owner;
// request handlers run concurrently and never mutate credentials. Local-only transport.
class BridgeServer {
public:
  explicit BridgeServer(std::vector<BridgeCredential> credentials,
                        std::shared_ptr<AdapterIngress> ingress = {},
                        std::shared_ptr<PluginHub> plugins = {},
                        std::shared_ptr<ActionRouter> actions = {},
                        std::shared_ptr<HostDispatcher> host = {});
  ~BridgeServer();
  BridgeServer(const BridgeServer&) = delete;
  BridgeServer& operator=(const BridgeServer&) = delete;
  bool start(const std::string& address);
  void stop();
  int port() const;

private:
  class AdapterService;
  class PluginService;
  std::vector<BridgeCredential> credentials_;
  std::unique_ptr<AdapterService> adapters_;
  std::unique_ptr<PluginService> plugins_;
  std::unique_ptr<grpc::Server> server_;
  int port_{0};
};

} // namespace kiarabot::core
