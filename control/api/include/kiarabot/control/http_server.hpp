#pragma once

#include <memory>
#include <string>

#include "kiarabot/core/control_application.hpp"

namespace kiarabot::control {
class HttpServer {
public:
  explicit HttpServer(core::IControlApplication& application);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  bool start(const std::string& address);
  void stop();
  unsigned short port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace kiarabot::control
