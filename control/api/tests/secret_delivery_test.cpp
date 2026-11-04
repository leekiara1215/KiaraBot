#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "kiarabot/control/application/target/secret_delivery.hpp"
#include "kiarabot/control/target/secret_file_delivery.hpp"

namespace {
namespace c = kiarabot::control::target;
namespace t = kiarabot::core::target;
namespace fs = std::filesystem;

void require(bool value, std::string_view message) {
  if (!value) {
    throw std::runtime_error(std::string{message});
  }
}

template <typename Value> Value take(t::Outcome<Value> result) {
  require(std::holds_alternative<Value>(result), "expected successful delivery result");
  return std::move(std::get<Value>(result));
}
template <typename Value> void rejects(t::Outcome<Value> result, t::ErrorCode code) {
  require(std::holds_alternative<t::Failure>(result), "expected stable refusal");
  require(std::get<t::Failure>(result).code == code, "unexpected stable refusal code");
}

struct Temporary {
  fs::path path;
  Temporary() {
    auto pattern = (fs::canonical(fs::temp_directory_path()) / "kiarabot-delivery-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    require(::mkdtemp(bytes.data()) != nullptr, "temporary directory");
    path = bytes.data();
  }
  ~Temporary() {
    (void)::chmod(path.c_str(), 0700);
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
c::SecretFileRecipient recipient(const fs::path& root) {
  return {"recipient",
          {t::ResourceKind::Instance, {"instance"}},
          "instance",
          root.string(),
          static_cast<std::uint32_t>(::geteuid()),
          static_cast<std::uint32_t>(::getegid())};
}
c::SecretDeliveryIdentity identity(std::uint64_t version = 1) {
  return {"delivery-" + std::to_string(version),
          {t::ResourceKind::Instance, {"instance"}},
          "recipient",
          version};
}
std::unique_ptr<c::SecretDeliveryPort> sink(const fs::path& path,
                                            c::SecretFileCheckpoint checkpoint = nullptr) {
  return take(c::make_secret_file_delivery({recipient(path)}, checkpoint));
}
std::string read(const fs::path& path) {
  std::ifstream stream{path};
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
void write(const fs::path& path, std::string_view text, mode_t mode = 0600) {
  std::ofstream stream{path};
  stream << text;
  stream.close();
  require(::chmod(path.c_str(), mode) == 0, "set fixture permissions");
}
const std::string kFirst(64, 'a');
const std::string kSecond(64, 'b');

void ordinary_and_cas(const char* python, const char* helper) {
  Temporary root;
  auto port = sink(root.path);
  c::SecretDeliveryApplication app{*port};
  require(!take(app.query(identity())).has_value(), "unknown delivery has no receipt");
  const auto first = take(app.store({identity(), 0, kFirst}));
  require(first.state == c::SecretDeliveryState::Delivered && !first.receipt_id.empty(),
          "first known installed receipt");
  require(read(root.path / "instance.bootstrap") == kFirst + "\n", "fixed secret convention");
  const auto again = take(app.store({identity(), 0, kFirst}));
  require(first == again, "same identity and material returns original receipt");
  const auto queried = take(app.query(identity()));
  require(queried && *queried == first, "lost acknowledgement recovers secret-free receipt");
  rejects(app.store({identity(), 0, kSecond}), t::ErrorCode::Conflict);
  rejects(app.store({identity(3), 2, kSecond}), t::ErrorCode::Conflict);
  auto second_port = sink(root.path);
  c::SecretDeliveryApplication second_app{*second_port};
  const auto second = take(second_app.store({identity(2), 1, kSecond}));
  require(second.identity.credential_version == 2, "second process view honors version CAS");
  rejects(app.store({identity(), 0, kFirst}), t::ErrorCode::Conflict);
  rejects(app.query(identity()), t::ErrorCode::Conflict);
  require(read(root.path / "instance.bootstrap") == kSecond + "\n",
          "late old worker cannot overwrite");
  for (const auto& entry : fs::directory_iterator(root.path)) {
    struct stat information {};
    require(::lstat(entry.path().c_str(), &information) == 0 && S_ISREG(information.st_mode) &&
                (information.st_mode & 0777) == 0600 && information.st_nlink == 1,
            "all durable files are private regular files");
    if (entry.path().extension() != ".bootstrap") {
      const auto metadata = read(entry.path());
      require(metadata.find(kFirst) == std::string::npos &&
                  metadata.find(kSecond) == std::string::npos,
              "intent/receipt/lock never stores secret plaintext");
    }
  }
  // Real existing deployment helper accepts the sink's fixed 0600 file convention.
  const auto child = ::fork();
  require(child >= 0, "fork helper verifier");
  if (child == 0) {
    ::execl(python, python, helper, "verify", "--directory", root.path.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(111);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "existing helper verifies delivered file without plaintext output");
}

void input_and_boundaries() {
  Temporary root;
  auto port = sink(root.path);
  c::SecretDeliveryApplication app{*port};
  auto traversal = identity();
  traversal.recipient_id = "../other";
  rejects(app.store({traversal, 0, kFirst}), t::ErrorCode::InvalidArgument);
  rejects(app.store({identity(), 0, std::string(64, 'Z')}), t::ErrorCode::InvalidArgument);
  rejects(app.store({identity(), 1, kFirst}), t::ErrorCode::InvalidArgument);
  auto foreign = identity();
  foreign.resource.id.value = "other-instance";
  rejects(app.store({foreign, 0, kFirst}), t::ErrorCode::PermissionDenied);
  foreign = identity();
  foreign.recipient_id = "unregistered";
  rejects(app.query(foreign), t::ErrorCode::PermissionDenied);
  require(fs::is_empty(root.path), "invalid inputs have no filesystem side effect");
  auto bad = recipient(root.path);
  bad.peer_id = "../../outside";
  rejects(c::make_secret_file_delivery({bad}), t::ErrorCode::InvalidArgument);
  bad = recipient(root.path);
  rejects(c::make_secret_file_delivery({bad, bad}), t::ErrorCode::InvalidArgument);
  require(::chmod(root.path.c_str(), 0755) == 0, "change directory mode");
  rejects(c::make_secret_file_delivery({bad}), t::ErrorCode::PermissionDenied);
  require(::chmod(root.path.c_str(), 0700) == 0, "restore directory mode");
  Temporary other;
  const auto alias = root.path / "alias";
  fs::create_directory_symlink(other.path, alias);
  bad.directory = alias.string();
  rejects(c::make_secret_file_delivery({bad}), t::ErrorCode::PermissionDenied);
  bad.directory = (alias / "child").string();
  rejects(c::make_secret_file_delivery({bad}), t::ErrorCode::PermissionDenied);
  bad.directory = (root.path / ".." / root.path.filename()).string();
  rejects(c::make_secret_file_delivery({bad}), t::ErrorCode::InvalidArgument);
}

void unsafe_files() {
  for (const auto kind : {"symlink", "hardlink", "mode", "directory", "owner"}) {
    Temporary root;
    Temporary outside;
    const auto target = root.path / "instance.bootstrap";
    const auto untouched = outside.path / "outside";
    write(untouched, "outside-canary");
    if (std::string_view{kind} == "symlink") {
      fs::create_symlink(untouched, target);
    } else if (std::string_view{kind} == "hardlink") {
      fs::create_hard_link(untouched, target);
    } else if (std::string_view{kind} == "directory") {
      fs::create_directory(target);
    } else {
      write(target, kFirst, std::string_view{kind} == "mode" ? 0640 : 0600);
    }
    auto config = recipient(root.path);
    if (std::string_view{kind} == "owner") {
      ++config.owner_uid;
    }
    auto port = take(c::make_secret_file_delivery({config}));
    c::SecretDeliveryApplication app{*port};
    rejects(app.store({identity(), 0, kFirst}), t::ErrorCode::PermissionDenied);
    require(read(untouched) == "outside-canary", "unsafe file never writes outside binding");
  }
  Temporary root;
  write(root.path / ".delivery.instance.intent", "malformed-private-metadata");
  auto port = sink(root.path);
  c::SecretDeliveryApplication app{*port};
  const auto result = app.query(identity());
  rejects(result, t::ErrorCode::Internal);
  require(std::get<t::Failure>(result).acceptance == t::Acceptance::Unknown,
          "malformed durable state remains unknown");
}

void crash_intent(c::SecretFilePhase phase) {
  if (phase == c::SecretFilePhase::IntentDurable) {
    ::_exit(93);
  }
}
void crash_temporary(c::SecretFilePhase phase) {
  if (phase == c::SecretFilePhase::TargetTemporaryDurable) {
    ::_exit(93);
  }
}
void crash_target(c::SecretFilePhase phase) {
  if (phase == c::SecretFilePhase::TargetDurable) {
    ::_exit(93);
  }
}
void crash_receipt(c::SecretFilePhase phase) {
  if (phase == c::SecretFilePhase::ReceiptDurable) {
    ::_exit(93);
  }
}
void stopped_intent(c::SecretFilePhase phase) {
  if (phase == c::SecretFilePhase::IntentDurable) {
    (void)::raise(SIGSTOP);
  }
}

void crash_recovery() {
  const std::vector<c::SecretFileCheckpoint> points{crash_intent, crash_temporary, crash_target,
                                                    crash_receipt};
  for (std::size_t index = 0; index < points.size(); ++index) {
    Temporary root;
    const auto child = ::fork();
    require(child >= 0, "fork crash fixture");
    if (child == 0) {
      auto port = sink(root.path, points[index]);
      c::SecretDeliveryApplication app{*port};
      (void)app.store({identity(), 0, kFirst});
      ::_exit(112);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 93,
            "actual process exited at durable boundary");
    auto port = sink(root.path);
    c::SecretDeliveryApplication app{*port};
    const auto recovered = take(app.query(identity()));
    require(recovered.has_value(), "durable intent survives process crash");
    require(recovered->state ==
                (index < 2 ? c::SecretDeliveryState::Unknown : c::SecretDeliveryState::Delivered),
            "query confirms installed bytes only, never intent alone");
    require(take(app.store({identity(), 0, kFirst})).state == c::SecretDeliveryState::Delivered,
            "same-identity retry resolves after crash without new version");
    for (const auto& entry : fs::directory_iterator(root.path)) {
      require(entry.path().filename().string().find(".tmp.") == std::string::npos,
              "crash recovery removes only own orphan temporaries");
    }
  }
}

void descriptor_exhaustion_is_bounded() {
  Temporary root;
  const auto child = ::fork();
  require(child >= 0, "fork descriptor exhaustion fixture");
  if (child == 0) {
    auto port = sink(root.path);
    c::SecretDeliveryApplication app{*port};
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      ::_exit(115);
    }
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur, 64);
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
      ::_exit(116);
    }
    std::vector<int> descriptors;
    for (;;) {
      const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (descriptor < 0) {
        break;
      }
      descriptors.push_back(descriptor);
    }
    if (descriptors.size() < 2) {
      ::_exit(117);
    }
    for (int index = 0; index < 2; ++index) {
      (void)::close(descriptors.back());
      descriptors.pop_back();
    }
    // Two slots allow the peer lock and scan directory; dup must really fail with EMFILE.
    for (int attempt = 0; attempt < 2; ++attempt) {
      const auto result = app.query(identity());
      if (!std::holds_alternative<t::Failure>(result) ||
          std::get<t::Failure>(result).code != t::ErrorCode::UpstreamUnavailable) {
        ::_exit(118);
      }
      const int first = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      const int second = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      const int third = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (first < 0 || second < 0 || third >= 0) {
        ::_exit(119);
      }
      (void)::close(first);
      (void)::close(second);
    }
    ::_exit(0);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "real child EMFILE refusal never passes negative fd or leaks scan/lock descriptors");
}

void io_failure_and_busy() {
  Temporary root;
  const auto child = ::fork();
  require(child >= 0, "fork syscall failure fixture");
  if (child == 0) {
    auto port = sink(root.path, stopped_intent);
    c::SecretDeliveryApplication app{*port};
    const auto result = app.store({identity(), 0, kFirst});
    if (const auto* failure = std::get_if<t::Failure>(&result);
        failure && failure->acceptance == t::Acceptance::Unknown &&
        failure->recovery == t::Recovery::QuerySameIdentity) {
      ::_exit(94);
    }
    ::_exit(113);
  }
  int status = 0;
  require(::waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status),
          "stop after durable intent");
  const auto obstruction = root.path / "instance.bootstrap";
  fs::create_directory(obstruction);
  write(obstruction / "obstruction", "fixed-IO-failure-fixture");
  require(::kill(child, SIGCONT) == 0, "resume failed syscall fixture");
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 94,
          "IO failure after intent preserves unknown acceptance");
  fs::remove_all(obstruction);
  auto port = sink(root.path);
  c::SecretDeliveryApplication app{*port};
  require(take(app.query(identity()))->state == c::SecretDeliveryState::Unknown,
          "IO failure is not delivery success");
  (void)take(app.store({identity(), 0, kFirst}));
  const auto lock_path = root.path / ".delivery.instance.lock";
  const int lock = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
  require(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "hold external peer lock");
  const auto busy = app.query(identity());
  rejects(busy, t::ErrorCode::UpstreamUnavailable);
  require(std::get<t::Failure>(busy).recovery == t::Recovery::Backoff,
          "busy lock has bounded retry semantics");
  (void)::close(lock);
  require(take(app.query(identity()))->state == c::SecretDeliveryState::Delivered,
          "unlock recovers original receipt");
}
} // namespace

void kind_level_recipients() {
  Temporary root;
  const auto owner_uid = static_cast<std::uint32_t>(::geteuid());
  const auto owner_gid = static_cast<std::uint32_t>(::getegid());
  const c::SecretFileRecipient kind_level{
      "kind-recipient", {t::ResourceKind::Instance, {""}}, "", root.path.string(), owner_uid,
      owner_gid};
  auto port = take(c::make_secret_file_delivery({kind_level}));
  c::SecretDeliveryApplication app{*port};
  const auto identity_for = [](std::string_view instance, std::uint64_t version) {
    return c::SecretDeliveryIdentity{"delivery-" + std::string{instance} + "-" +
                                         std::to_string(version),
                                     {t::ResourceKind::Instance, {std::string{instance}}},
                                     "kind-recipient",
                                     version};
  };
  const auto first = take(app.store({identity_for("instance-a", 1), 0, kFirst}));
  require(first.state == c::SecretDeliveryState::Delivered, "kind-level first delivery");
  require(read(root.path / "instance-a.bootstrap") == kFirst + "\n",
          "kind-level file name derives from the exact resource id");
  const auto other = take(app.store({identity_for("instance-b", 1), 0, kSecond}));
  require(other.state == c::SecretDeliveryState::Delivered, "kind-level second resource delivery");
  require(read(root.path / "instance-b.bootstrap") == kSecond + "\n",
          "per-resource file is independent");
  rejects(app.store(
              {{"delivery-account", {t::ResourceKind::Account, {"account-a"}}, "kind-recipient", 1},
               0,
               kFirst}),
          t::ErrorCode::PermissionDenied);
  take(app.store({identity_for("instance-a", 2), 1, kSecond}));
  rejects(app.store({identity_for("instance-a", 1), 0, kFirst}), t::ErrorCode::Conflict);
  require(read(root.path / "instance-a.bootstrap") == kSecond + "\n",
          "late old worker cannot overwrite a kind-level resource");

  const c::SecretFileRecipient exact_a{
      "exact-recipient", {t::ResourceKind::Instance, {"instance-c"}},
      "instance-c",      root.path.string(),
      owner_uid,         owner_gid};
  auto exact_port = take(c::make_secret_file_delivery({exact_a}));
  c::SecretDeliveryApplication exact_app{*exact_port};
  rejects(
      exact_app.store(
          {{"delivery-mismatch", {t::ResourceKind::Instance, {"instance-d"}}, "exact-recipient", 1},
           0,
           kFirst}),
      t::ErrorCode::PermissionDenied);

  rejects(c::make_secret_file_delivery({kind_level,
                                        {"kind-recipient-two",
                                         {t::ResourceKind::Instance, {}},
                                         "",
                                         root.path.string(),
                                         owner_uid,
                                         owner_gid}}),
          t::ErrorCode::InvalidArgument);
  rejects(c::make_secret_file_delivery({{"kind-with-peer",
                                         {t::ResourceKind::Instance, {}},
                                         "peer",
                                         root.path.string(),
                                         owner_uid,
                                         owner_gid}}),
          t::ErrorCode::InvalidArgument);
}

int main(int argc, char* argv[]) {
  try {
    require(argc == 3, "test needs pinned Python and canonical helper arguments");
    ordinary_and_cas(argv[1], argv[2]);
    input_and_boundaries();
    unsafe_files();
    crash_recovery();
    io_failure_and_busy();
    descriptor_exhaustion_is_bounded();
    kind_level_recipients();
    std::cout << "Secret delivery module: CAS, actual crash/IO recovery, file boundaries and "
                 "helper compatibility passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
