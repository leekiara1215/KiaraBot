#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sodium.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

#include <boost/version.hpp>
#include <curl/curl.h>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include <jwt-cpp/jwt.h>
#include <openssl/crypto.h>
#include <pqxx/pqxx>
#include <pqxx/version>

namespace {
std::string read_file(const char* path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("key_file_unavailable");
  }
  return {std::istreambuf_iterator<char>(stream), {}};
}
} // namespace

static_assert(GOOGLE_PROTOBUF_VERSION == 5029003);
static_assert(BOOST_VERSION == 108700);
static_assert(LIBCURL_VERSION_NUM == 0x080d00);
static_assert(SQLITE_VERSION_NUMBER == 3049001);

int main(int argc, char* argv[]) {
  const char* stage = "versions";
  try {
    if (argc != 3 || sodium_init() < 0) {
      return 2;
    }
    if (std::string(PQXX_VERSION) != "7.10.0" || grpc::Version() != "1.71.0" ||
        std::string(sodium_version_string()) != "1.0.20" ||
        std::string(OpenSSL_version(OPENSSL_VERSION)).find("OpenSSL 3.5.0 ") != 0) {
      return 7;
    }
    stage = "keys";
    const auto public_key = read_file(argv[1]);
    const auto private_key = read_file(argv[2]);
    stage = "jwt-sign";
    const auto token = jwt::create()
                           .set_issuer("toolchain-test")
                           .set_audience("toolchain-test")
                           .sign(jwt::algorithm::ed25519(public_key, private_key));
    stage = "jwt-verify";
    jwt::verify()
        .allow_algorithm(jwt::algorithm::ed25519(public_key))
        .with_issuer("toolchain-test")
        .with_audience("toolchain-test")
        .verify(jwt::decode(token));
    bool rejected = false;
    try {
      jwt::verify()
          .allow_algorithm(jwt::algorithm::ed25519(public_key))
          .with_audience("wrong-audience")
          .verify(jwt::decode(token));
    } catch (const std::exception&) {
      rejected = true;
    }
    if (!rejected) {
      return 3;
    }
    const char* dsn = std::getenv("KIARABOT_TOOLCHAIN_TEST_DSN");
    if (dsn == nullptr) {
      std::cerr << "dedicated_temporary_pg_fixture_required\n";
      return 4;
    }
    stage = "pg-connect";
    pqxx::connection connection(dsn);
    stage = "pg-transaction";
    pqxx::work transaction(connection);
    if (transaction.exec("SHOW server_version_num")[0][0].as<int>() != 160004) {
      return 5;
    }
    transaction.exec("CREATE TEMP TABLE toolchain_probe (id integer PRIMARY KEY)");
    transaction.exec("INSERT INTO toolchain_probe VALUES (1)");
    if (transaction.exec("SELECT count(*) FROM toolchain_probe")[0][0].as<int>() != 1) {
      return 6;
    }
    transaction.abort();
    std::cout << "PG16.4 TLS/libpqxx/JWT Ed25519 sign/verify/rejection passed\n";
    std::cout << "OpenSSL=" << OpenSSL_version(OPENSSL_VERSION)
              << " SQLite=" << sqlite3_libversion() << " curl=" << curl_version()
              << " protobuf=" << GOOGLE_PROTOBUF_VERSION << " gRPC=" << grpc::Version()
              << " Boost=" << BOOST_LIB_VERSION << " sodium=" << sodium_version_string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::string category = "other";
    const std::string diagnostic = error.what();
    for (const auto* candidate :
         {"digest", "certificate", "SSL", "channel binding", "authentication", "No such file",
          "refused", "timeout", "invalid connection option"}) {
      if (diagnostic.find(candidate) != std::string::npos) {
        category = candidate;
        break;
      }
    }
    // Never emit a DSN, token, key or a dependency exception containing connection material.
    std::cerr << "dependency_smoke_failed stage=" << stage << " category=" << category << '\n';
    return 1;
  }
}
