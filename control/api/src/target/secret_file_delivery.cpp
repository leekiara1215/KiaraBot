#include "kiarabot/control/target/secret_file_delivery.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace kiarabot::control::target {
namespace {
namespace core = kiarabot::core::target;
using Json = nlohmann::json;

core::Failure failure(core::ErrorCode code, bool uncertain = false) {
  return {code,
          uncertain ? core::Acceptance::Unknown : core::Acceptance::NotAccepted,
          uncertain ? core::Recovery::QuerySameIdentity : core::Recovery::ReadCurrent,
          {},
          {}};
}
struct FileFailure {
  core::Failure value;
};
[[noreturn]] void refuse(core::ErrorCode code, bool uncertain = false) {
  throw FileFailure{failure(code, uncertain)};
}

class FileDescriptor {
public:
  explicit FileDescriptor(int value = -1) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      (void)::close(value_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        (void)::close(value_);
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  int get() const {
    return value_;
  }
  int release() noexcept {
    return std::exchange(value_, -1);
  }

private:
  int value_;
};

void sync_directory(int directory) {
  if (::fsync(directory) != 0) {
    refuse(core::ErrorCode::UpstreamUnavailable, true);
  }
}

FileDescriptor open_directory(std::string_view path) {
  if (path.empty() || path.front() != '/' || path == "/") {
    refuse(core::ErrorCode::InvalidArgument);
  }
  FileDescriptor current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (current.get() < 0) {
    refuse(core::ErrorCode::UpstreamUnavailable);
  }
  std::size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto component =
        path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
    if (component.empty() || component == "." || component == ".." ||
        component.find('\0') != std::string_view::npos) {
      refuse(core::ErrorCode::InvalidArgument);
    }
    FileDescriptor next{::openat(current.get(), std::string{component}.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (next.get() < 0) {
      refuse(core::ErrorCode::PermissionDenied);
    }
    current = std::move(next);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  struct stat info {};
  if (::fstat(current.get(), &info) != 0 || info.st_uid != ::geteuid() ||
      (info.st_mode & 0777) != 0700) {
    refuse(core::ErrorCode::PermissionDenied);
  }
  return current;
}

void check_file(int descriptor, uid_t uid, gid_t gid) {
  struct stat info {};
  if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
      info.st_uid != uid || info.st_gid != gid || (info.st_mode & 0777) != 0600 ||
      info.st_size < 0 || info.st_size > 4096) {
    refuse(core::ErrorCode::PermissionDenied);
  }
}

std::optional<std::string> read_file(int directory, const std::string& name, uid_t uid, gid_t gid) {
  FileDescriptor file{
      ::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (file.get() < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    refuse(core::ErrorCode::PermissionDenied);
  }
  check_file(file.get(), uid, gid);
  std::string result;
  std::array<char, 512> buffer{};
  for (;;) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      refuse(core::ErrorCode::UpstreamUnavailable, true);
    }
    if (count == 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
    if (result.size() > 4096) {
      refuse(core::ErrorCode::PermissionDenied);
    }
  }
  return result;
}

std::string sha256(std::string_view value) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
      size != 32) {
    refuse(core::ErrorCode::Internal);
  }
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (unsigned int index = 0; index < size; ++index) {
    result.push_back(kHex[digest[index] >> 4]);
    result.push_back(kHex[digest[index] & 15]);
  }
  return result;
}

bool hex_secret(std::string_view value) {
  if (value.size() != 64) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

struct Record {
  SecretDeliveryIdentity identity;
  std::string digest;
  std::string receipt_id;
};
Json record_json(const Record& record) {
  return {{"schema", 1},
          {"delivery_id", record.identity.delivery_id},
          {"resource_kind",
           record.identity.resource.kind == core::ResourceKind::Account ? "account" : "instance"},
          {"resource_id", record.identity.resource.id.value},
          {"recipient_id", record.identity.recipient_id},
          {"credential_version", record.identity.credential_version},
          {"digest", record.digest},
          {"receipt_id", record.receipt_id}};
}
Record decode_record(const std::string& text) {
  const auto value = Json::parse(text, nullptr, false);
  if (!value.is_object() || value.size() != 8 || value.value("schema", 0) != 1 ||
      !value.contains("credential_version") || !value["credential_version"].is_number_unsigned()) {
    refuse(core::ErrorCode::Internal, true);
  }
  const auto kind = value.at("resource_kind").get<std::string>();
  if (kind != "account" && kind != "instance") {
    refuse(core::ErrorCode::Internal, true);
  }
  Record result{{value.at("delivery_id").get<std::string>(),
                 {kind == "account" ? core::ResourceKind::Account : core::ResourceKind::Instance,
                  {value.at("resource_id").get<std::string>()}},
                 value.at("recipient_id").get<std::string>(),
                 value.at("credential_version").get<std::uint64_t>()},
                value.at("digest").get<std::string>(),
                value.at("receipt_id").get<std::string>()};
  if (!valid_delivery_identity(result.identity) || !hex_secret(result.digest) ||
      result.receipt_id.empty() || result.receipt_id.size() > 128) {
    refuse(core::ErrorCode::Internal, true);
  }
  return result;
}

struct Binding {
  SecretFileRecipient recipient;
  FileDescriptor directory;
  std::string target;
  std::string prefix;
};

class SecretFileDelivery final : public SecretDeliveryPort {
public:
  SecretFileDelivery(std::vector<Binding> bindings, SecretFileCheckpoint checkpoint)
      : bindings_(std::move(bindings)), checkpoint_(checkpoint) {}

  core::Outcome<SecretDeliveryReceipt> store(const SecretDeliveryRequest& request) override {
    bool uncertain = false;
    try {
      if (!valid_delivery_identity(request.identity) || !hex_secret(request.secret) ||
          request.expected_version == std::numeric_limits<std::uint64_t>::max() ||
          request.identity.credential_version != request.expected_version + 1) {
        return failure(core::ErrorCode::InvalidArgument);
      }
      auto* found = locate(request.identity);
      if (found == nullptr) {
        return failure(core::ErrorCode::PermissionDenied);
      }
      auto& binding = *found;
      auto lock = lock_peer(binding);
      cleanup_temporaries(binding);
      auto intent = read_record(binding, ".intent");
      auto receipt = read_record(binding, ".receipt");
      const auto head = current(binding, intent, receipt);
      const auto digest = sha256(request.secret);
      if (head && head->identity == request.identity) {
        if (head->digest != digest) {
          return failure(core::ErrorCode::Conflict);
        }
        uncertain = true;
        if (target_matches(binding, *head)) {
          return finish(binding, *head, intent.has_value());
        }
        if (!intent) {
          refuse(core::ErrorCode::Internal, true);
        }
      } else {
        if ((head ? head->identity.credential_version : 0) != request.expected_version) {
          return failure(core::ErrorCode::Conflict);
        }
        // Validate an existing target before installing an intent; never overwrite an unmanaged
        // or unsafe pre-existing file merely because its name matches a configured peer.
        const auto previous = read_file(binding.directory.get(), binding.target,
                                        binding.recipient.owner_uid, binding.recipient.owner_gid);
        if (!head && previous) {
          return failure(core::ErrorCode::Conflict);
        }
      }
      Record next{request.identity, digest, {}};
      next.receipt_id = sha256(record_json(next).dump());
      uncertain = true;
      atomic_write(binding, binding.prefix + ".intent", record_json(next).dump(), ::geteuid(),
                   ::getegid());
      checkpoint(SecretFilePhase::IntentDurable);
      atomic_write(binding, binding.target, std::string{request.secret} + "\n",
                   binding.recipient.owner_uid, binding.recipient.owner_gid);
      checkpoint(SecretFilePhase::TargetDurable);
      return finish(binding, next, true);
    } catch (const FileFailure& rejected) {
      auto result = rejected.value;
      if (uncertain && result.code != core::ErrorCode::Conflict) {
        result.acceptance = core::Acceptance::Unknown;
        result.recovery = core::Recovery::QuerySameIdentity;
      }
      return result;
    } catch (...) {
      return failure(core::ErrorCode::Internal, uncertain);
    }
  }

  core::Outcome<std::optional<SecretDeliveryReceipt>>
  query(const SecretDeliveryIdentity& identity) override {
    try {
      if (!valid_delivery_identity(identity)) {
        return failure(core::ErrorCode::InvalidArgument);
      }
      auto* found = locate(identity);
      if (found == nullptr) {
        return failure(core::ErrorCode::PermissionDenied);
      }
      auto& binding = *found;
      auto lock = lock_peer(binding);
      cleanup_temporaries(binding);
      const auto intent = read_record(binding, ".intent");
      const auto receipt = read_record(binding, ".receipt");
      const auto head = current(binding, intent, receipt);
      if (!head) {
        return std::optional<SecretDeliveryReceipt>{};
      }
      if (head->identity != identity) {
        return failure(core::ErrorCode::Conflict);
      }
      if (target_matches(binding, *head)) {
        return std::optional<SecretDeliveryReceipt>{finish(binding, *head, intent.has_value())};
      }
      return std::optional<SecretDeliveryReceipt>{
          SecretDeliveryReceipt{identity, SecretDeliveryState::Unknown, {}}};
    } catch (const FileFailure& rejected) {
      return rejected.value;
    } catch (...) {
      return failure(core::ErrorCode::Internal, true);
    }
  }

private:
  Binding* locate(const SecretDeliveryIdentity& identity) {
    for (auto& binding : bindings_) {
      if (binding.recipient.recipient_id == identity.recipient_id) {
        if (binding.recipient.resource != identity.resource) {
          return nullptr;
        }
        return &binding;
      }
    }
    return nullptr;
  }

  FileDescriptor lock_peer(const Binding& binding) {
    const auto name = binding.prefix + ".lock";
    FileDescriptor lock{::openat(binding.directory.get(), name.c_str(),
                                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
    if (lock.get() < 0) {
      refuse(core::ErrorCode::PermissionDenied);
    }
    check_file(lock.get(), ::geteuid(), ::getegid());
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
      throw FileFailure{{core::ErrorCode::UpstreamUnavailable,
                         core::Acceptance::NotAccepted,
                         core::Recovery::Backoff,
                         {},
                         {}}};
    }
    return lock;
  }

  std::optional<Record> read_record(const Binding& binding, std::string_view suffix) {
    const auto text = read_file(binding.directory.get(), binding.prefix + std::string{suffix},
                                ::geteuid(), ::getegid());
    return text ? std::optional<Record>{decode_record(*text)} : std::nullopt;
  }

  std::optional<Record> current(const Binding& binding, const std::optional<Record>& intent,
                                const std::optional<Record>& receipt) {
    for (const auto* value : {&intent, &receipt}) {
      if (*value && ((*value)->identity.resource != binding.recipient.resource ||
                     (*value)->identity.recipient_id != binding.recipient.recipient_id)) {
        refuse(core::ErrorCode::Internal, true);
      }
    }
    if (intent && receipt &&
        (intent->identity.credential_version < receipt->identity.credential_version ||
         (intent->identity.credential_version == receipt->identity.credential_version &&
          record_json(*intent) != record_json(*receipt)))) {
      refuse(core::ErrorCode::Internal, true);
    }
    return intent ? intent : receipt;
  }

  bool target_matches(const Binding& binding, const Record& record) {
    auto content = read_file(binding.directory.get(), binding.target, binding.recipient.owner_uid,
                             binding.recipient.owner_gid);
    if (!content) {
      return false;
    }
    if (content->size() == 65 && content->back() == '\n') {
      content->pop_back();
    }
    return hex_secret(*content) && sha256(*content) == record.digest;
  }

  void checkpoint(SecretFilePhase phase) const {
    if (checkpoint_ != nullptr) {
      checkpoint_(phase);
    }
  }

  SecretDeliveryReceipt finish(const Binding& binding, const Record& record, bool has_intent) {
    if (has_intent) {
      atomic_write(binding, binding.prefix + ".receipt", record_json(record).dump(), ::geteuid(),
                   ::getegid());
      checkpoint(SecretFilePhase::ReceiptDurable);
      if (::unlinkat(binding.directory.get(), (binding.prefix + ".intent").c_str(), 0) != 0 &&
          errno != ENOENT) {
        refuse(core::ErrorCode::UpstreamUnavailable, true);
      }
      sync_directory(binding.directory.get());
    }
    return {record.identity, SecretDeliveryState::Delivered, record.receipt_id};
  }

  void atomic_write(const Binding& binding, const std::string& name, const std::string& bytes,
                    uid_t uid, gid_t gid) {
    const bool target = name == binding.target;
    const auto temporary = binding.prefix + (target ? ".tmp.target." : ".tmp.meta.") +
                           std::to_string(::getpid()) + "." + std::to_string(++temporary_sequence_);
    FileDescriptor file{::openat(binding.directory.get(), temporary.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.get() < 0) {
      refuse(core::ErrorCode::UpstreamUnavailable, true);
    }
    try {
      if (::fchown(file.get(), uid, gid) != 0 || ::fchmod(file.get(), 0600) != 0) {
        refuse(core::ErrorCode::PermissionDenied, true);
      }
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          refuse(core::ErrorCode::UpstreamUnavailable, true);
        }
        offset += static_cast<std::size_t>(count);
      }
      if (::fsync(file.get()) != 0) {
        refuse(core::ErrorCode::UpstreamUnavailable, true);
      }
      if (target) {
        checkpoint(SecretFilePhase::TargetTemporaryDurable);
      }
      if (::renameat(binding.directory.get(), temporary.c_str(), binding.directory.get(),
                     name.c_str()) != 0) {
        refuse(core::ErrorCode::UpstreamUnavailable, true);
      }
      sync_directory(binding.directory.get());
    } catch (...) {
      (void)::unlinkat(binding.directory.get(), temporary.c_str(), 0);
      throw;
    }
  }

  void cleanup_temporaries(const Binding& binding) {
    // Independent directory stream; dup() would share its offset across repeated queries.
    FileDescriptor scan{::openat(binding.directory.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    FileDescriptor duplicated{scan.get() < 0 ? -1 : ::dup(scan.get())};
    if (duplicated.get() < 0) {
      refuse(core::ErrorCode::UpstreamUnavailable);
    }
    DIR* raw = ::fdopendir(duplicated.get());
    if (raw == nullptr) {
      refuse(core::ErrorCode::UpstreamUnavailable);
    }
    const std::unique_ptr<DIR, decltype(&::closedir)> directory{raw, ::closedir};
    (void)duplicated.release(); // DIR owns this descriptor only after fdopendir succeeds.
    std::size_t count = 0;
    const auto prefix = binding.prefix + ".tmp.";
    while (const auto* entry = ::readdir(directory.get())) {
      const std::string name{entry->d_name};
      if (!name.starts_with(prefix)) {
        continue;
      }
      if (++count > 64) {
        refuse(core::ErrorCode::RateLimited);
      }
      const bool target = name.starts_with(binding.prefix + ".tmp.target.");
      if (!target && !name.starts_with(binding.prefix + ".tmp.meta.")) {
        refuse(core::ErrorCode::PermissionDenied);
      }
      const auto content = read_file(binding.directory.get(), name,
                                     target ? binding.recipient.owner_uid : ::geteuid(),
                                     target ? binding.recipient.owner_gid : ::getegid());
      if (content && ::unlinkat(binding.directory.get(), name.c_str(), 0) != 0) {
        refuse(core::ErrorCode::UpstreamUnavailable, true);
      }
    }
    if (count != 0) {
      sync_directory(binding.directory.get());
    }
  }

  std::vector<Binding> bindings_;
  SecretFileCheckpoint checkpoint_;
  std::atomic<std::uint64_t> temporary_sequence_{0};
};
} // namespace

core::Outcome<std::unique_ptr<SecretDeliveryPort>>
make_secret_file_delivery(std::vector<SecretFileRecipient> recipients,
                          SecretFileCheckpoint checkpoint_for_testing) {
  try {
    if (recipients.empty() || recipients.size() > 127) {
      refuse(core::ErrorCode::InvalidArgument);
    }
    std::vector<Binding> bindings;
    std::set<std::string> names;
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::string>> targets;
    for (auto& recipient : recipients) {
      const SecretDeliveryIdentity identity{recipient.peer_id, recipient.resource,
                                            recipient.recipient_id, 1};
      if (!valid_delivery_identity(identity) || !names.insert(recipient.recipient_id).second) {
        refuse(core::ErrorCode::InvalidArgument);
      }
      auto directory = open_directory(recipient.directory);
      struct stat info {};
      if (::fstat(directory.get(), &info) != 0 ||
          !targets
               .emplace(static_cast<std::uint64_t>(info.st_dev),
                        static_cast<std::uint64_t>(info.st_ino), recipient.peer_id)
               .second) {
        refuse(core::ErrorCode::InvalidArgument);
      }
      const auto peer = recipient.peer_id;
      bindings.push_back(
          {std::move(recipient), std::move(directory), peer + ".bootstrap", ".delivery." + peer});
    }
    return std::unique_ptr<SecretDeliveryPort>{
        std::make_unique<SecretFileDelivery>(std::move(bindings), checkpoint_for_testing)};
  } catch (const FileFailure& rejected) {
    return rejected.value;
  } catch (...) {
    return failure(core::ErrorCode::Internal);
  }
}
} // namespace kiarabot::control::target
