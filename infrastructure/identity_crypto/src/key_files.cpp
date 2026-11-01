// clang-format off
#include "kiarabot/infrastructure/identity_crypto/key_files.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <sodium.h>
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
t::Failure failure(t::ErrorCode code = t::ErrorCode::InvalidArgument) {
  ERR_clear_error();
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
bool valid_ref(const t::CryptoKeyRef& ref) {
  return !ref.value.empty() && ref.value.size() <= 1024 &&
         ref.value.find('\0') == std::string::npos;
}
class Descriptor {
public:
  explicit Descriptor(int descriptor) : descriptor_(descriptor) {}
  ~Descriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const {
    return descriptor_;
  }

private:
  int descriptor_;
};
class SecureFile final : public t::SecretBuffer {
public:
  explicit SecureFile(std::size_t capacity)
      : allocation_(static_cast<std::byte*>(sodium_malloc(capacity))) {
    if (!allocation_) {
      throw std::bad_alloc{};
    }
  }
  ~SecureFile() override {
    sodium_free(allocation_);
  }
  SecureFile(const SecureFile&) = delete;
  SecureFile& operator=(const SecureFile&) = delete;
  std::span<const std::byte> bytes() const noexcept override {
    return {allocation_, size_};
  }
  std::byte* data() {
    return allocation_;
  }
  void resize(std::size_t size) {
    size_ = size;
  }

private:
  std::byte* allocation_;
  std::size_t size_ = 0;
};
bool metadata(const struct stat& status, bool secret, std::size_t maximum) {
  return S_ISREG(status.st_mode) && status.st_size > 0 &&
         static_cast<std::uintmax_t>(status.st_size) <= maximum &&
         (secret ? (status.st_uid == geteuid() && (status.st_mode & 077) == 0)
                 : ((status.st_uid == geteuid() || status.st_uid == 0) &&
                    (status.st_mode & 022) == 0));
}
t::Outcome<std::unique_ptr<SecureFile>> read_file(const std::filesystem::path& path, bool secret,
                                                  std::size_t maximum) {
  if (path.empty() || path.native().find('\0') != std::string::npos) {
    return failure();
  }
  Descriptor descriptor(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (descriptor.get() < 0) {
    return failure(errno == ELOOP ? t::ErrorCode::InvalidArgument
                                  : t::ErrorCode::UpstreamUnavailable);
  }
  struct stat before {};
  if (fstat(descriptor.get(), &before) != 0) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  if (!metadata(before, secret, maximum)) {
    return failure();
  }
  auto content = std::make_unique<SecureFile>(maximum + 1);
  std::size_t size = 0;
  while (size <= maximum) {
    const ssize_t count = read(descriptor.get(), content->data() + size, maximum + 1 - size);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (count == 0) {
      break;
    }
    size += static_cast<std::size_t>(count);
  }
  struct stat after {};
  if (fstat(descriptor.get(), &after) != 0) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
#if defined(__APPLE__)
  const auto before_modified = before.st_mtimespec;
  const auto after_modified = after.st_mtimespec;
  const auto before_changed = before.st_ctimespec;
  const auto after_changed = after.st_ctimespec;
#else
  const auto before_modified = before.st_mtim;
  const auto after_modified = after.st_mtim;
  const auto before_changed = before.st_ctim;
  const auto after_changed = after.st_ctim;
#endif
  if (!metadata(after, secret, maximum) || size != static_cast<std::size_t>(before.st_size) ||
      before.st_size != after.st_size || before.st_uid != after.st_uid ||
      before.st_mode != after.st_mode || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before_modified.tv_sec != after_modified.tv_sec ||
      before_modified.tv_nsec != after_modified.tv_nsec ||
      before_changed.tv_sec != after_changed.tv_sec ||
      before_changed.tv_nsec != after_changed.tv_nsec) {
    return failure();
  }
  content->resize(size);
  return content;
}
bool trailing_space(BIO* input) {
  char* remaining = nullptr;
  const long size = BIO_get_mem_data(input, &remaining);
  if (size < 0) {
    return false;
  }
  for (long index = 0; index < size; ++index) {
    const char value = remaining[index];
    if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
      return false;
    }
  }
  return true;
}

bool pem_shape(t::SecretView input, std::string_view prefix) {
  const std::string_view value(reinterpret_cast<const char*>(input.data()), input.size());
  return value.starts_with(prefix) && value.find('\0') == std::string_view::npos;
}
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
} // namespace

t::Outcome<t::OwnedSecret> load_bootstrap_password_file(const std::filesystem::path& trusted_path) {
  try {
    if (sodium_init() < 0) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    auto loaded = read_file(trusted_path, true, 1025);
    if (const auto* error = std::get_if<t::Failure>(&loaded)) {
      return *error;
    }
    auto& input = std::get<std::unique_ptr<SecureFile>>(loaded);
    auto bytes = input->bytes();
    if (!bytes.empty() && bytes.back() == std::byte{'\n'}) {
      bytes = bytes.first(bytes.size() - 1);
    }
    if (bytes.empty() || bytes.size() > 1024) {
      return failure();
    }
    for (const auto value : bytes) {
      if (value == std::byte{0} || value == std::byte{'\r'} || value == std::byte{'\n'}) {
        return failure();
      }
    }
    input->resize(bytes.size());
    return t::OwnedSecret{std::move(input)};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
}

t::Outcome<SymmetricKey> load_symmetric_key_file(const TrustedSymmetricKeyFile& file) {
  try {
    if (sodium_init() < 0) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (!valid_ref(file.reference)) {
      return failure();
    }
    auto loaded = read_file(file.hex_file, true, 65);
    if (const auto* error = std::get_if<t::Failure>(&loaded)) {
      return *error;
    }
    auto& input = std::get<std::unique_ptr<SecureFile>>(loaded);
    const auto bytes = input->bytes();
    if (bytes.size() != 64 && !(bytes.size() == 65 && bytes[64] == std::byte{'\n'})) {
      return failure();
    }
    for (auto value : bytes.first(64)) {
      const auto character = std::to_integer<unsigned char>(value);
      if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
        return failure();
      }
    }
    auto decoded = std::make_unique<SecureFile>(32);
    decoded->resize(32);
    std::size_t size = 0;
    if (sodium_hex2bin(reinterpret_cast<unsigned char*>(decoded->data()), 32,
                       reinterpret_cast<const char*>(bytes.data()), 64, nullptr, &size,
                       nullptr) != 0 ||
        size != 32) {
      return failure();
    }
    return SymmetricKey{file.reference, t::OwnedSecret{std::move(decoded)}};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
}
t::Outcome<DelegationKey> load_delegation_key_files(const TrustedDelegationKeyFiles& files) {
  try {
    if (sodium_init() < 0) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (!valid_ref(files.reference) || files.version.value == 0 ||
        files.valid_until <= t::TimePoint{} || files.valid_until == t::TimePoint::max()) {
      return failure();
    }
    auto public_result = read_file(files.public_pem_file, false, 8192);
    if (const auto* error = std::get_if<t::Failure>(&public_result)) {
      return *error;
    }
    const auto& public_file = std::get<std::unique_ptr<SecureFile>>(public_result);
    if (!pem_shape(public_file->bytes(), "-----BEGIN PUBLIC KEY-----\n")) {
      return failure();
    }
    Bio public_bio(
        BIO_new_mem_buf(public_file->bytes().data(), static_cast<int>(public_file->bytes().size())),
        BIO_free);
    if (!public_bio) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    Key public_key(PEM_read_bio_PUBKEY(public_bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!public_key || !trailing_space(public_bio.get()) ||
        EVP_PKEY_is_a(public_key.get(), "ED25519") != 1) {
      return failure();
    }
    t::OwnedSecret private_material;
    if (files.private_pem_file) {
      auto private_result = read_file(*files.private_pem_file, true, 8192);
      if (const auto* error = std::get_if<t::Failure>(&private_result)) {
        return *error;
      }
      auto& private_file = std::get<std::unique_ptr<SecureFile>>(private_result);
      if (!pem_shape(private_file->bytes(), "-----BEGIN PRIVATE KEY-----\n")) {
        return failure();
      }
      Bio private_bio(BIO_new_mem_buf(private_file->bytes().data(),
                                      static_cast<int>(private_file->bytes().size())),
                      BIO_free);
      if (!private_bio) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      Key private_key(PEM_read_bio_PrivateKey(private_bio.get(), nullptr, nullptr, nullptr),
                      EVP_PKEY_free);
      if (!private_key || !trailing_space(private_bio.get()) ||
          EVP_PKEY_is_a(private_key.get(), "ED25519") != 1 ||
          EVP_PKEY_eq(public_key.get(), private_key.get()) != 1) {
        return failure();
      }
      private_material = std::move(private_file);
    }
    return DelegationKey{files.reference,
                         std::string(reinterpret_cast<const char*>(public_file->bytes().data()),
                                     public_file->bytes().size()),
                         std::move(private_material), files.version, files.valid_until};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
