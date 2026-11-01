// clang-format off
#include "kiarabot/infrastructure/identity_crypto/key_files.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/pem.h>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;
class Directory {
public:
  Directory() {
    char pattern[] = "build/key-files-XXXXXX";
    char* path = mkdtemp(pattern);
    assert(path);
    path_ = path;
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  std::filesystem::path file(const char* name) const {
    return path_ / name;
  }

private:
  std::filesystem::path path_;
};
void write_file(const std::filesystem::path& file, std::string_view content, mode_t mode = 0600) {
  const int descriptor = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  assert(descriptor >= 0);
  assert(fchmod(descriptor, mode) == 0);
  assert(write(descriptor, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
  assert(close(descriptor) == 0);
}
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
void write_pem(const std::filesystem::path& file, BIO* pem) {
  char* bytes = nullptr;
  const long size = BIO_get_mem_data(pem, &bytes);
  assert(size > 0);
  write_file(file, {bytes, static_cast<std::size_t>(size)});
}
class Clock final : public t::SecurityClockPort {
public:
  t::Outcome<t::SecurityClockSample> sample() override {
    return t::SecurityClockSample{
        t::TimePoint{std::chrono::seconds{1000}}, {}, std::chrono::milliseconds{0}, true};
  }
};
int main() try {
  Directory directory;
  const auto symmetric = directory.file("symmetric");
  constexpr std::string_view kHex =
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  const auto password_path = directory.file("bootstrap-password");
  for (const auto& text : {std::string{"test-password"}, std::string{"test-password\n"},
                           std::string(1024, 'x') + "\n"}) {
    write_file(password_path, text);
    auto password = crypto::load_bootstrap_password_file(password_path);
    const auto bytes = std::get<t::OwnedSecret>(password)->bytes();
    const auto length = text.back() == '\n' ? text.size() - 1 : text.size();
    assert(bytes.size() == length);
    for (std::size_t index = 0; index < length; ++index) {
      assert(std::to_integer<unsigned char>(bytes[index]) ==
             static_cast<unsigned char>(text[index]));
    }
  }
  for (const auto& text : {std::string{}, std::string{"\n"}, std::string{"a\n\n"},
                           std::string{"a\rb"}, std::string{"a\nb"}, std::string(1, '\0'),
                           std::string(1025, 'x'), std::string(1026, 'x')}) {
    write_file(password_path, text);
    assert(std::get<t::Failure>(crypto::load_bootstrap_password_file(password_path)).code ==
           t::ErrorCode::InvalidArgument);
  }
  write_file(password_path, "test-password", 0644);
  assert(std::get<t::Failure>(crypto::load_bootstrap_password_file(password_path)).code ==
         t::ErrorCode::InvalidArgument);
  assert(
      std::get<t::Failure>(crypto::load_bootstrap_password_file(directory.file("absent"))).code ==
      t::ErrorCode::UpstreamUnavailable);
  write_file(symmetric, kHex);
  auto loaded = crypto::load_symmetric_key_file({{"key"}, symmetric});
  const auto& key = std::get<crypto::SymmetricKey>(loaded);
  assert(key.material->bytes().size() == 32);
  for (unsigned index = 0; index < 32; ++index) {
    assert(std::to_integer<unsigned>(key.material->bytes()[index]) == index);
  }
  write_file(symmetric, std::string{kHex} + "\n");
  assert(std::holds_alternative<crypto::SymmetricKey>(
      crypto::load_symmetric_key_file({{"key"}, symmetric})));
  for (const auto& malformed :
       {std::string{}, std::string{kHex} + "\n\n", std::string(64, 'A'), std::string(64, 'g'),
        std::string(66, 'a'), std::string(64, '\0')}) {
    write_file(symmetric, malformed);
    assert(
        std::holds_alternative<t::Failure>(crypto::load_symmetric_key_file({{"key"}, symmetric})));
  }
  write_file(symmetric, kHex, 0644);
  auto invalid_permissions = crypto::load_symmetric_key_file({{"key"}, symmetric});
  assert(std::get<t::Failure>(invalid_permissions).code == t::ErrorCode::InvalidArgument);
  write_file(symmetric, kHex);
  assert(symlink("symmetric", directory.file("link").c_str()) == 0);
  assert(std::holds_alternative<t::Failure>(
      crypto::load_symmetric_key_file({{"key"}, directory.file("link")})));
  assert(std::holds_alternative<t::Failure>(
      crypto::load_symmetric_key_file({{"key"}, directory.file(".")})));
  assert(mkfifo(directory.file("fifo").c_str(), 0600) == 0);
  assert(symlink("bootstrap-password", directory.file("password-link").c_str()) == 0);
  assert(std::get<t::Failure>(crypto::load_bootstrap_password_file(directory.file("password-link")))
             .code == t::ErrorCode::InvalidArgument);
  assert(std::get<t::Failure>(crypto::load_bootstrap_password_file(directory.file("."))).code ==
         t::ErrorCode::InvalidArgument);
  const auto password_start = std::chrono::steady_clock::now();
  assert(std::get<t::Failure>(crypto::load_bootstrap_password_file(directory.file("fifo"))).code ==
         t::ErrorCode::InvalidArgument);
  assert(std::chrono::steady_clock::now() - password_start < std::chrono::seconds{1});
  const auto start = std::chrono::steady_clock::now();
  assert(std::holds_alternative<t::Failure>(
      crypto::load_symmetric_key_file({{"key"}, directory.file("fifo")})));
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{1});
  auto missing = crypto::load_symmetric_key_file({{"key"}, directory.file("missing")});
  assert(std::get<t::Failure>(missing).code == t::ErrorCode::UpstreamUnavailable);
  std::string nul_path = symmetric.native();
  nul_path.push_back('\0');
  nul_path += "ignored";
  assert(std::holds_alternative<t::Failure>(crypto::load_symmetric_key_file({{"key"}, nul_path})));
  Key first(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  Key second(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  Bio pub(BIO_new(BIO_s_mem()), BIO_free), priv(BIO_new(BIO_s_mem()), BIO_free),
      other(BIO_new(BIO_s_mem()), BIO_free);
  assert(first && second && pub && priv && other);
  assert(PEM_write_bio_PUBKEY(pub.get(), first.get()) == 1);
  assert(PEM_write_bio_PrivateKey(priv.get(), first.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
         1);
  assert(PEM_write_bio_PUBKEY(other.get(), second.get()) == 1);
  write_pem(directory.file("public"), pub.get());
  write_pem(directory.file("private"), priv.get());
  for (const auto version : {std::uint64_t{0}, std::uint64_t{1}}) {
    const auto expiry =
        version == 0 ? t::TimePoint{std::chrono::seconds{10000}} : t::TimePoint::max();
    assert(
        std::get<t::Failure>(
            crypto::load_delegation_key_files(
                {{"sign"}, directory.file("public"), directory.file("private"), {version}, expiry}))
            .code == t::ErrorCode::InvalidArgument);
  }
  assert(std::get<t::Failure>(
             crypto::load_delegation_key_files(
                 {{"sign"}, directory.file("public"), directory.file("private"), {1}, {}}))
             .code == t::ErrorCode::InvalidArgument);
  auto delegation = crypto::load_delegation_key_files({{"sign"},
                                                       directory.file("public"),
                                                       directory.file("private"),
                                                       {1},
                                                       t::TimePoint{std::chrono::seconds{10000}}});
  assert(std::get<crypto::DelegationKey>(delegation).private_key_pem);
  assert(std::get<crypto::DelegationKey>(delegation).version.value == 1);
  assert(std::get<crypto::DelegationKey>(delegation).valid_until ==
         t::TimePoint{std::chrono::seconds{10000}});
  assert(chmod(directory.file("public").c_str(), 0644) == 0);
  assert(std::holds_alternative<crypto::DelegationKey>(crypto::load_delegation_key_files(
      {{"verify"}, directory.file("public"), {}, {1}, t::TimePoint{std::chrono::seconds{10000}}})));
  for (const auto mode : {0664, 0666}) {
    assert(chmod(directory.file("public").c_str(), static_cast<mode_t>(mode)) == 0);
    auto rejected = crypto::load_delegation_key_files(
        {{"verify"}, directory.file("public"), {}, {1}, t::TimePoint{std::chrono::seconds{10000}}});
    assert(std::get<t::Failure>(rejected).code == t::ErrorCode::InvalidArgument);
  }
  assert(chmod(directory.file("public").c_str(), 0644) == 0);
  auto verification = crypto::load_delegation_key_files(
      {{"verify"}, directory.file("public"), {}, {1}, t::TimePoint{std::chrono::seconds{10000}}});
  assert(!std::get<crypto::DelegationKey>(verification).private_key_pem);
  assert(chmod(directory.file("private").c_str(), 0640) == 0);
  assert(std::holds_alternative<t::Failure>(
      crypto::load_delegation_key_files({{"sign"},
                                         directory.file("public"),
                                         directory.file("private"),
                                         {1},
                                         t::TimePoint{std::chrono::seconds{10000}}})));
  assert(chmod(directory.file("private").c_str(), 0600) == 0);
  write_file(directory.file("private"), std::string(8193, 'a'));
  assert(std::holds_alternative<t::Failure>(
      crypto::load_delegation_key_files({{"sign"},
                                         directory.file("public"),
                                         directory.file("private"),
                                         {1},
                                         t::TimePoint{std::chrono::seconds{10000}}})));
  write_file(directory.file("private"), std::string(64, '\0'));
  assert(std::holds_alternative<t::Failure>(
      crypto::load_delegation_key_files({{"sign"},
                                         directory.file("public"),
                                         directory.file("private"),
                                         {1},
                                         t::TimePoint{std::chrono::seconds{10000}}})));
  write_pem(directory.file("private"), priv.get());
  assert(symlink("public", directory.file("public-link").c_str()) == 0);
  assert(std::holds_alternative<t::Failure>(
      crypto::load_delegation_key_files({{"verify"},
                                         directory.file("public-link"),
                                         {},
                                         {1},
                                         t::TimePoint{std::chrono::seconds{10000}}})));
  write_pem(directory.file("public"), other.get());
  assert(std::holds_alternative<t::Failure>(
      crypto::load_delegation_key_files({{"sign"},
                                         directory.file("public"),
                                         directory.file("private"),
                                         {1},
                                         t::TimePoint{std::chrono::seconds{10000}}})));
  // Existing factory independently rejects a mismatched pair even when file validation is bypassed.
  char* public_bytes = nullptr;
  const auto public_size = BIO_get_mem_data(other.get(), &public_bytes);
  auto mismatched = std::move(std::get<crypto::DelegationKey>(delegation));
  mismatched.public_key_pem.assign(public_bytes, static_cast<std::size_t>(public_size));
  crypto::DelegationConfig config{
      "issuer", {}, {1}, {t::ResourceKind::Catalog, {"00000000-0000-4000-8000-000000000001"}}};
  config.keys.push_back(std::move(mismatched));
  Clock clock;
  assert(
      std::holds_alternative<t::Failure>(crypto::make_delegation_crypto(std::move(config), clock)));
} catch (...) {
  return 1;
}
