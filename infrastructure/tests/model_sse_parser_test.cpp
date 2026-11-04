#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/model_stream.hpp"
#include "model_sse_parser.hpp"

namespace {
using kiarabot::infrastructure::ModelSseParser;
using namespace std::string_literals;

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}

void partition_matrix() {
  struct Sample {
    std::string wire;
    std::vector<std::string> data;
    std::string event = "message";
  };
  const std::vector<Sample> samples{
      {"data: x\n\n", {"x"}},
      {"data: x\r\r", {"x"}},
      {"data: x\r\n\r\n", {"x"}},
      {"data: x\rdata: y\n\r", {"x\ny"}},
      {"\xef\xbb\xbf"
       "data: x\n\n",
       {"x"}},
      {"data: 中文🙂\r\n\r\n", {"中文🙂"}},
      {"data:\ndata: x\ndata:\n\n", {"\nx\n"}},
      {"data:\n\n", {""}},
      {"data\n\n", {""}},
      {"\n:keepalive\r\nunknown: skip\ndata: x\n\n", {"x"}},
      {"event: custom\ndata: x\n\n", {"x"}, "custom"},
      {"event: custom\nevent:\ndata: x\n\n", {"x"}},
      {"data: x\n\ndata: y\r\n\r\n", {"x", "y"}},
      {"data: x\n", {}},
      {"data: x\n\ndata: incomplete", {"x"}},
      {"id: valid\nid: bad\0id\nretry: abc\ndata: x\n\n"s, {"x"}},
      {"data:  two\n\n", {" two"}},
      {"data: \xef\xbb\xbf"
       "x\n\n",
       {"\xef\xbb\xbf"
        "x"}},
  };
  std::size_t runs = 0;
  for (const auto& sample : samples) {
    for (std::size_t step = 1; step <= sample.wire.size() + 1; ++step) {
      std::vector<std::string> actual;
      ModelSseParser parser([&](std::string_view event, std::string_view data) {
        check(event == sample.event, "SSE event name");
        actual.emplace_back(data);
        return true;
      });
      for (std::size_t index = 0; index < sample.wire.size(); index += step) {
        check(parser.feed(std::string_view{sample.wire}.substr(index, step)), "partition feed");
      }
      check(actual == sample.data, "SSE output independent of chunk partition");
      ++runs;
    }
  }
  check(samples.size() == 18 && runs == 337, "frozen 18-case/337-partition matrix");
}

void sticky_failures() {
  std::size_t callbacks = 0;
  const auto receiver = [&](std::string_view, std::string_view) {
    ++callbacks;
    return true;
  };
  ModelSseParser long_line(receiver);
  check(!long_line.feed(":" + std::string(65536, 'x')), "bound ignored/comment line");
  check(long_line.fault() == ModelSseParser::Fault::LineTooLarge, "line fault");
  check(!long_line.feed("\ndata: later\n\n") && callbacks == 0, "line failure stays sticky");

  ModelSseParser large_event(receiver);
  std::string event;
  for (int index = 0; index < 17; ++index) {
    event += "data: " + std::string(4096, 'x') + "\n";
  }
  event += "\ndata: later\n\n";
  check(!large_event.feed(event), "bound multi-line data event");
  check(large_event.fault() == ModelSseParser::Fault::EventTooLarge && callbacks == 0,
        "overflow cannot dispatch later event in same chunk");

  ModelSseParser large_response(receiver);
  const std::string comment = ":" + std::string(1021, 'x') + "\n";
  for (int index = 0; index < 1026; ++index) {
    large_response.feed(comment);
  }
  check(large_response.fault() == ModelSseParser::Fault::ResponseTooLarge, "total bytes bound");

  ModelSseParser utf8(receiver);
  check(!utf8.feed("data: \xff\n\n"), "invalid UTF-8 event fails closed");
  check(utf8.fault() == ModelSseParser::Fault::InvalidText && callbacks == 0, "UTF-8 fault");
  ModelSseParser nul_event(receiver);
  check(!nul_event.feed("event: bad\0event\ndata: x\n\n"s), "NUL event name fails closed");
  check(nul_event.fault() == ModelSseParser::Fault::InvalidText, "event type fault");

  ModelSseParser stop([&](std::string_view, std::string_view) {
    ++callbacks;
    return false;
  });
  check(!stop.feed("data: first\n\ndata: second\n\n") && callbacks == 1,
        "consumer stop suppresses later callback in same feed");
  ModelSseParser throwing([](std::string_view, std::string_view) -> bool {
    throw std::runtime_error("test callback");
  });
  check(!throwing.feed("data: x\n\n"), "callback exceptions stop at infrastructure boundary");
}

} // namespace

int main() {
  try {
    partition_matrix();
    sticky_failures();
    std::cout << "SSE: 18 cases/337 partitions and bounded sticky faults passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
