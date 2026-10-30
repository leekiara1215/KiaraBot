#pragma once

#include <mutex>

#include "kiarabot/core/ports.hpp"

namespace kiarabot::infrastructure {
// Configuration belongs to the composition root, never to a plugin request.
struct HttpModelConfig {
  std::string endpoint;
  std::string api_key;
  std::string model;
};
class HttpModelProvider final : public core::IModelProvider {
public:
  static core::PortResult<std::shared_ptr<HttpModelProvider>> open(HttpModelConfig config);
  core::PortResult<kiarabot::v1::ModelChatResponse>
  chat(const kiarabot::v1::ModelChatRequest& request, std::chrono::milliseconds timeout) override;

private:
  explicit HttpModelProvider(HttpModelConfig config);
  HttpModelConfig config_;
  std::mutex mutex_;
};
} // namespace kiarabot::infrastructure
