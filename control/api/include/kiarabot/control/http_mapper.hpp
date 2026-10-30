#pragma once

#include <map>

#include "kiarabot/core/control_application.hpp"

namespace kiarabot::control {
struct HttpRequest {
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers; // Lowercase; duplicate fields rejected by transport.
  std::string body;
};
struct HttpReply {
  int status{200};
  std::string body;
  std::map<std::string, std::string> headers;
};
HttpReply handle_http(core::IControlApplication& application, const HttpRequest& request);
} // namespace kiarabot::control
