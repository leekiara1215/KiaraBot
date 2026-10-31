#pragma once

#include <map>
#include <string>
#include <vector>

#include "kiarabot/core/application/control_application.hpp"

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
// Maps one HTTP request to a Core port operation. The transport owns protocol concerns that do not
// depend on the application port: connection reuse, CORS, rate limiting and access logging.
HttpReply handle_http(core::IControlApplication& application, const HttpRequest& request);
} // namespace kiarabot::control
