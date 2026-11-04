#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace kiarabot::infrastructure {

// Infrastructure-only bounded adapter around the locked upstream SSE byte parser. Event/data
// views are borrowed during the callback only. False stops delivery permanently, including any
// later events in the same input chunk. EOF never flushes an unfinished SSE event.
class ModelSseParser {
public:
  enum class Fault { None, ResponseTooLarge, LineTooLarge, EventTooLarge, InvalidText, Stopped };
  using Receiver = std::function<bool(std::string_view event, std::string_view data)>;

  explicit ModelSseParser(Receiver receiver);
  ~ModelSseParser();
  ModelSseParser(const ModelSseParser&) = delete;
  ModelSseParser& operator=(const ModelSseParser&) = delete;
  bool feed(std::string_view bytes) noexcept;
  Fault fault() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace kiarabot::infrastructure
