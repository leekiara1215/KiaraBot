#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include "kiarabot/infrastructure/file_storage.hpp"

namespace {
void exercise(kiarabot::core::IFileStorage& store) {
  using kiarabot::v1::Error;
  using kiarabot::v1::INVALID_ARGUMENT;
  using kiarabot::v1::NOT_FOUND;
  const std::vector<std::byte> value{std::byte{1}, std::byte{2}};
  assert(std::get<Error>(store.get("key")).code() == NOT_FOUND);
  assert(std::holds_alternative<std::monostate>(store.put("key", value)));
  assert(std::get<std::vector<std::byte>>(store.get("key")) == value);
  assert(std::get<Error>(store.get("../key")).code() == INVALID_ARGUMENT);
  assert(std::holds_alternative<std::monostate>(store.erase("key")));
  assert(std::holds_alternative<std::monostate>(store.erase("key")));
  assert(std::get<Error>(store.get("key")).code() == NOT_FOUND);
  // Boundary: 255 bytes is the POSIX NAME_MAX for one path component; 256 is
  // rejected before touching the filesystem.
  const std::string maximum_key(255, 'a');
  assert(std::holds_alternative<std::monostate>(store.put(maximum_key, value)));
  assert(std::get<std::vector<std::byte>>(store.get(maximum_key)) == value);
  assert(std::holds_alternative<std::monostate>(store.erase(maximum_key)));
  const std::string oversized_key(256, 'a');
  assert(std::get<Error>(store.get(oversized_key)).code() == INVALID_ARGUMENT);
  assert(std::get<Error>(store.put(oversized_key, value)).code() == INVALID_ARGUMENT);
  assert(std::get<Error>(store.erase(oversized_key)).code() == INVALID_ARGUMENT);
}
} // namespace

int main() {
  std::string pattern = (std::filesystem::temp_directory_path() / "kiarabot-files-XXXXXX").string();
  char* directory = ::mkdtemp(pattern.data());
  assert(directory != nullptr);
  kiarabot::infrastructure::FileStorage store(directory);
  kiarabot::infrastructure::FakeFileStorage fake;
  exercise(store);
  exercise(fake);
  const auto root = std::filesystem::path(directory);
  const auto outside = root / "outside.txt";
  {
    std::ofstream output(outside);
    output << "outside";
  }
  std::filesystem::create_symlink(outside, root / "link");
  assert(std::holds_alternative<kiarabot::v1::Error>(store.get("link")));
  assert(std::holds_alternative<std::monostate>(store.put("link", {std::byte{3}})));
  std::ifstream input(outside);
  std::string outside_content;
  input >> outside_content;
  assert(outside_content == "outside");
  fake.fail_next(kiarabot::v1::INTERNAL);
  assert(std::get<kiarabot::v1::Error>(fake.put("key", {})).code() == kiarabot::v1::INTERNAL);
  assert(std::holds_alternative<std::monostate>(fake.put("key", {})));
  std::filesystem::remove_all(directory);
}
