#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <signal.h>

#include "kiarabot/core/runtime/plugin_process.hpp"
#endif

#include "kiarabot/core/application/runtime.h"
#include "kiarabot/core/application/runtime_maintenance.hpp"

#ifdef KIARABOT_HAS_GRPC
#include "kiarabot/control/configuration.hpp"
#include "kiarabot/control/http_server.hpp"
#include "kiarabot/core/application/action_router.hpp"
#include "kiarabot/core/application/adapter_ingress.hpp"
#include "kiarabot/core/application/host_dispatcher.hpp"
#include "kiarabot/core/application/plugin_hub.hpp"
#include "kiarabot/core/runtime/bridge_server.hpp"
#endif
#ifdef KIARABOT_HAS_SQLITE
#include "kiarabot/infrastructure/http_model_provider.hpp"
#include "kiarabot/infrastructure/message_projection.hpp"
#include "kiarabot/infrastructure/runtime_log_sink.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"
#include "kiarabot/infrastructure/time_zone_resolver.hpp"
#endif

namespace {

#if defined(__unix__) || defined(__APPLE__)
bool stop_pending(const sigset_t& signals) {
  sigset_t pending;
  if (sigpending(&pending) == 0 &&
      (sigismember(&pending, SIGTERM) == 1 || sigismember(&pending, SIGINT) == 1)) {
    int received = 0;
    return sigwait(&signals, &received) == 0;
  }
  return false;
}

std::chrono::milliseconds process_timeout(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (!value) {
    return std::chrono::milliseconds(fallback);
  }
  const std::string_view text(value);
  int duration = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), duration);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || duration < 10 ||
      duration > 30000) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::milliseconds(duration);
}

#ifdef KIARABOT_HAS_GRPC
bool process_count(const char* name, std::size_t fallback, std::size_t minimum, std::size_t maximum,
                   std::size_t& value) {
  const char* text = std::getenv(name);
  if (text == nullptr) {
    value = fallback;
    return true;
  }
  const std::string_view input(text);
  std::size_t parsed = 0;
  const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
  if (input.empty() || result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
      parsed < minimum || parsed > maximum) {
    return false;
  }
  value = parsed;
  return true;
}

std::string_view storage_code(kiarabot::core::StorageError error) {
  switch (error) {
  case kiarabot::core::StorageError::Conflict:
    return "CONFLICT";
  case kiarabot::core::StorageError::Corrupt:
    return "INTERNAL";
  case kiarabot::core::StorageError::Unavailable:
    return "UPSTREAM_UNAVAILABLE";
  }
  return "INTERNAL";
}

void log_maintenance(const kiarabot::core::MaintenanceStats& stats) {
  std::cerr << "operation=maintenance outcome=ok events=" << stats.events
            << " deliveries=" << stats.deliveries << " actions=" << stats.actions
            << " audit=" << stats.audit << " commands=" << stats.commands
            << " namespace_entries=" << stats.namespace_entries
            << " host_receipts=" << stats.host_receipts << '\n';
}
#endif

#endif

int check_runtime() {
  kiarabot::core::Runtime runtime;
  const auto result =
      runtime.ingest(kiarabot::core::EventRecord{"startup-check", "startup", "", 0, false});
  if (!std::holds_alternative<kiarabot::core::EventResult>(result) ||
      std::get<kiarabot::core::EventResult>(result) != kiarabot::core::EventResult::accepted) {
    return 1;
  }
  std::cout << "kiarabot-core self-check passed; inbox="
            << std::get<std::size_t>(runtime.inbox_size()) << "\n";
  return 0;
}

int run_process(std::string_view listen = {}) {
#if defined(__unix__) || defined(__APPLE__)
  // Block before any future worker creation so all workers inherit this mask.
  // The executable exits after sigwait; no signal handler accesses runtime state.
  sigset_t signals;
  if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
      sigaddset(&signals, SIGTERM) != 0 || pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::cerr << "operation=process_start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
  kiarabot::core::PluginProcess child;
  const char* executable = std::getenv("KIARABOT_PLUGIN_EXECUTABLE");
  const char* manifest = std::getenv("KIARABOT_PLUGIN_MANIFEST");
  const bool managed = executable || manifest;
  const auto start_timeout = process_timeout("KIARABOT_PLUGIN_START_TIMEOUT_MS", 5000);
  const auto stop_timeout = process_timeout("KIARABOT_PLUGIN_STOP_TIMEOUT_MS", 2000);
  if (managed && (listen.empty() || !executable || *executable != '/' || !manifest ||
                  *manifest != '/' || start_timeout.count() == 0 || stop_timeout.count() == 0)) {
    std::cerr << "operation=plugin_config outcome=failed error_code=INVALID_ARGUMENT\n";
    return 1;
  }
#ifdef KIARABOT_HAS_GRPC
  std::size_t queue_limit = 0;
  std::size_t audit_limit = 0;
  std::size_t action_limit = 0;
  std::size_t ack_timeout_ms = 0;
  std::size_t maintenance_interval_ms = 0;
  std::size_t retention_hours = 0;
  if (!process_count("KIARABOT_EVENT_QUEUE_LIMIT", 1024, 1, 100000, queue_limit) ||
      !process_count("KIARABOT_AUDIT_LIMIT", 100000, 1000, 10000000, audit_limit) ||
      !process_count("KIARABOT_ACTION_LIMIT", 10000, 100, 1000000, action_limit) ||
      !process_count("KIARABOT_ACK_TIMEOUT_MS", 30000, 1000, 600000, ack_timeout_ms) ||
      !process_count("KIARABOT_MAINTENANCE_INTERVAL_MS", 60000, 1000, 3600000,
                     maintenance_interval_ms) ||
      !process_count("KIARABOT_RETENTION_HOURS", 168, 24, 8760, retention_hours)) {
    std::cerr << "operation=runtime_config outcome=failed error_code=INVALID_ARGUMENT\n";
    return 1;
  }
  kiarabot::core::RuntimeOptions runtime_options;
  runtime_options.queue_limit = queue_limit;
  runtime_options.retention = std::chrono::hours(retention_hours);
  runtime_options.ack_timeout = std::chrono::milliseconds(ack_timeout_ms);
  kiarabot::core::RuntimeLimits runtime_limits;
  runtime_limits.audit_limit = audit_limit;
  runtime_limits.action_limit = action_limit;
  runtime_limits.command_limit = audit_limit;
#ifdef KIARABOT_HAS_SQLITE
  auto log_sink = std::make_shared<kiarabot::infrastructure::BufferedRuntimeLogSink>();
#endif
  std::unique_ptr<kiarabot::core::RuntimeStore> persistent_store;
  std::unique_ptr<kiarabot::core::BridgeServer> server;
  std::unique_ptr<kiarabot::core::ControlService> control_application;
  std::unique_ptr<kiarabot::control::HttpServer> control_server;
  const char* control_config = std::getenv("KIARABOT_CONTROL_CONFIG");
  if (control_config && listen.empty()) {
    return 1;
  }
  std::jthread scheduling;
  std::jthread maintenance_thread;
  std::shared_ptr<kiarabot::core::RuntimeMaintenance> maintenance;
  if (!listen.empty()) {
    std::vector<kiarabot::core::BridgeCredential> credentials;
    for (const auto role :
         {kiarabot::core::BridgeRole::Adapter, kiarabot::core::BridgeRole::Plugin}) {
      const bool adapter = role == kiarabot::core::BridgeRole::Adapter;
      const char* id = std::getenv(adapter ? "KIARABOT_ADAPTER_ID" : "KIARABOT_PLUGIN_ID");
      const char* token = std::getenv(adapter ? "KIARABOT_ADAPTER_TOKEN" : "KIARABOT_PLUGIN_TOKEN");
      if (id != nullptr || token != nullptr) {
        credentials.push_back({role, id == nullptr ? "" : id, token == nullptr ? "" : token});
      }
    }
    std::shared_ptr<kiarabot::core::AdapterIngress> ingress;
    std::shared_ptr<kiarabot::core::PluginHub> plugins;
    const char* database = std::getenv("KIARABOT_RUNTIME_DB");
    const char* accounts = std::getenv("KIARABOT_ADAPTER_ACCOUNTS");
    if (database != nullptr || accounts != nullptr) {
#ifdef KIARABOT_HAS_SQLITE
      const char* identity = std::getenv("KIARABOT_ADAPTER_ID");
      if (!database || !*database || std::string_view(database) == ":memory:" ||
          std::string_view(database).starts_with("file:") || !accounts || !*accounts || !identity ||
          !*identity) {
        std::cerr << "operation=ingress_config outcome=failed error_code=INVALID_ARGUMENT\n";
        return 1;
      }
      auto opened = kiarabot::infrastructure::SqliteRuntimeStore::open(database);
      if (std::holds_alternative<kiarabot::core::StorageError>(opened)) {
        std::cerr << "operation=storage_open outcome=failed error_code=UPSTREAM_UNAVAILABLE\n";
        return 1;
      }
      persistent_store = std::get<std::unique_ptr<kiarabot::infrastructure::SqliteRuntimeStore>>(
          std::move(opened));
      persistent_store = std::make_unique<kiarabot::core::LoggingRuntimeStore>(
          std::move(persistent_store), log_sink);
      std::vector<std::string> allowed_accounts;
      const std::string list(accounts);
      std::size_t begin = 0;
      do {
        const auto end = list.find(',', begin);
        allowed_accounts.push_back(
            list.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) {
          break;
        }
        begin = end + 1;
      } while (true);
      if (const char* requested_permissions = std::getenv("KIARABOT_PLUGIN_PERMISSIONS")) {
        const char* plugin_id = std::getenv("KIARABOT_PLUGIN_ID");
        if (!plugin_id || !*plugin_id) {
          return 1;
        }
        std::vector<std::string> permissions;
        const std::string values(requested_permissions);
        std::size_t position = 0;
        while (position < values.size()) {
          const auto end = values.find(',', position);
          permissions.push_back(
              values.substr(position, end == std::string::npos ? end : end - position));
          if (end == std::string::npos) {
            break;
          }
          position = end + 1;
        }
        plugins = std::make_shared<kiarabot::core::PluginHub>(
            *persistent_store, std::vector<kiarabot::core::PluginPolicy>{{plugin_id, permissions}},
            kiarabot::core::RuntimeClock{}, runtime_options, runtime_limits);
      }
      ingress = std::make_shared<kiarabot::core::AdapterIngress>(
          *persistent_store,
          std::vector<kiarabot::core::AdapterPolicy>{{identity, allowed_accounts}}, runtime_options,
          plugins);
#else
      std::cerr << "operation=ingress_config outcome=failed error_code=CAPABILITY_NOT_SUPPORTED\n";
      return 1;
#endif
    }
    auto actions = plugins ? std::make_shared<kiarabot::core::ActionRouter>(
                                 *persistent_store, kiarabot::core::RuntimeClock{}, runtime_limits)
                           : nullptr;
    std::shared_ptr<kiarabot::core::IModelProvider> model;
    const char* model_endpoint = std::getenv("KIARABOT_MODEL_ENDPOINT");
    const char* model_key = std::getenv("KIARABOT_MODEL_API_KEY");
    const char* model_name = std::getenv("KIARABOT_MODEL_NAME");
    if (model_endpoint || model_key || model_name) {
#ifdef KIARABOT_HAS_SQLITE
      if (!plugins || !model_endpoint || !model_key || !model_name) {
        std::cerr << "operation=model_config outcome=failed error_code=INVALID_ARGUMENT\n";
        return 1;
      }
      auto provider = kiarabot::infrastructure::HttpModelProvider::open(
          {model_endpoint, model_key, model_name});
      if (std::holds_alternative<kiarabot::v1::Error>(provider)) {
        std::cerr << "operation=model_config outcome=failed error_code=INVALID_ARGUMENT\n";
        return 1;
      }
      model = std::get<std::shared_ptr<kiarabot::infrastructure::HttpModelProvider>>(
          std::move(provider));
#else
      return 1;
#endif
    }
    std::shared_ptr<kiarabot::core::TaskScheduler> scheduler;
#ifdef KIARABOT_HAS_SQLITE
    if (plugins) {
      scheduler = std::make_shared<kiarabot::core::TaskScheduler>(
          *persistent_store, std::make_shared<kiarabot::infrastructure::TimeZoneResolver>(),
          kiarabot::core::RuntimeClock{}, runtime_limits);
      if (std::holds_alternative<kiarabot::v1::Error>(scheduler->recover())) {
        std::cerr << "operation=scheduler_recover outcome=failed error_code=UPSTREAM_UNAVAILABLE\n";
        return 1;
      }
    }
#endif
    kiarabot::core::NamespacePolicy namespace_policy;
    namespace_policy.audit_records = runtime_limits.audit_limit;
    auto host = plugins ? std::make_shared<kiarabot::core::HostDispatcher>(
                              *persistent_store, kiarabot::core::RuntimeClock{}, namespace_policy,
                              model, scheduler)
                        : nullptr;
    if (persistent_store) {
      kiarabot::core::MaintenanceOptions maintenance_options;
      maintenance_options.retention = runtime_options.retention;
      maintenance_options.audit_limit = runtime_limits.audit_limit;
      maintenance_options.command_limit = runtime_limits.command_limit;
      maintenance = std::make_shared<kiarabot::core::RuntimeMaintenance>(*persistent_store,
                                                                         maintenance_options);
      const auto initial = maintenance->run();
      if (const auto* failure = std::get_if<kiarabot::core::StorageError>(&initial)) {
        std::cerr << "operation=maintenance outcome=failed error_code=" << storage_code(*failure)
                  << '\n';
      } else if (std::get<kiarabot::core::MaintenanceStats>(initial).changed()) {
        log_maintenance(std::get<kiarabot::core::MaintenanceStats>(initial));
      }
    }
    if (control_config) {
      if (!persistent_store) {
        return 1;
      }
      auto configuration = kiarabot::control::read_configuration(control_config);
#ifdef KIARABOT_HAS_SQLITE
      configuration.application.logs = [log_sink] { return log_sink->read(); };
      // Metadata-only message projection for the opt-in /ws/messages channel.
      // No body and no persistence: the stream is a notification feed only.
      auto message_projection =
          std::make_shared<kiarabot::infrastructure::BufferedMessageProjection>();
      configuration.application.messages = [message_projection] {
        return message_projection->read();
      };
      if (ingress) {
        ingress->set_message_sink(message_projection);
      }
#endif
      if (const char* id = std::getenv("KIARABOT_ADAPTER_ID")) {
        configuration.application.adapter_id = id;
        configuration.application.adapter_connected = [ingress, identity = std::string(id)] {
          return ingress && ingress->connected(identity);
        };
      }
      if (accounts) {
        std::string list(accounts);
        std::size_t position = 0;
        while (position < list.size()) {
          const auto end = list.find(',', position);
          configuration.application.accounts.push_back(
              list.substr(position, end == std::string::npos ? end : end - position));
          if (end == std::string::npos) {
            break;
          }
          position = end + 1;
        }
      }
      if (model_name) {
        configuration.application.model_name = model_name;
      }
      if (plugins) {
        configuration.application.config_delivery =
            std::make_shared<kiarabot::core::PluginConfigDelivery>(
                *persistent_store, configuration.application.encryption_key);
        plugins->set_configuration(configuration.application.config_delivery);
      }
      configuration.application.audit_limit = runtime_limits.audit_limit;
      control_application = std::make_unique<kiarabot::core::ControlService>(
          *persistent_store, std::move(configuration.application));
      server = std::make_unique<kiarabot::core::BridgeServer>(std::move(credentials), ingress,
                                                              plugins, actions, host);
      if (!server->start(std::string(listen))) {
        return 1;
      }
      control_server = std::make_unique<kiarabot::control::HttpServer>(*control_application);
      if (!control_server->start(configuration.listen)) {
        return 1;
      }
      std::cout << "operation=control_start port=" << control_server->port() << "\n" << std::flush;
    }
    if (!server) {
      server = std::make_unique<kiarabot::core::BridgeServer>(std::move(credentials), ingress,
                                                              plugins, actions, host);
      if (!server->start(std::string(listen))) {
        std::cerr << "operation=grpc_start outcome=failed error_code=INTERNAL\n";
        return 1;
      }
    }
    if (scheduler) {
      scheduling = std::jthread([scheduler](std::stop_token stop) {
        auto previous = kiarabot::v1::ERROR_CODE_UNSPECIFIED;
        while (!stop.stop_requested()) {
          const auto result = scheduler->tick();
          const auto* failure = std::get_if<kiarabot::v1::Error>(&result);
          const auto code = failure ? failure->code() : kiarabot::v1::ERROR_CODE_UNSPECIFIED;
          if (code != previous && failure) {
            std::cerr << "operation=scheduler_tick outcome=failed error_code="
                      << kiarabot::v1::ErrorCode_Name(code) << '\n';
          }
          previous = code;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
    }
    if (maintenance) {
      maintenance_thread =
          std::jthread([maintenance, interval = std::chrono::milliseconds(maintenance_interval_ms)](
                           std::stop_token stop) {
            auto last = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
              if (std::chrono::steady_clock::now() - last < interval) {
                continue;
              }
              last = std::chrono::steady_clock::now();
              const auto result = maintenance->run();
              if (const auto* failure = std::get_if<kiarabot::core::StorageError>(&result)) {
                std::cerr << "operation=maintenance outcome=failed error_code="
                          << storage_code(*failure) << '\n';
                continue;
              }
              const auto& stats = std::get<kiarabot::core::MaintenanceStats>(result);
              if (stats.changed()) {
                log_maintenance(stats);
              }
            }
          });
    }
    std::cout << "transport_listening=true port=" << server->port()
              << " rpc_ready=false adapter_ingress_ready=" << (ingress ? "true" : "false")
              << " plugin_delivery_ready=" << (plugins ? "true" : "false") << "\n";
    if (managed) {
      const char* token = std::getenv("KIARABOT_PLUGIN_TOKEN");
      const char* identity = std::getenv("KIARABOT_PLUGIN_ID");
      if (!plugins || !token || !identity) {
        std::cerr << "operation=plugin_config outcome=failed error_code=INVALID_ARGUMENT\n";
        return 1;
      }
      std::vector<std::string> environment{"PATH=/usr/bin:/bin", "PYTHONUNBUFFERED=1",
                                           "PLUGIN_CORE_ENDPOINT=127.0.0.1:" +
                                               std::to_string(server->port()),
                                           std::string("PLUGIN_AUTH_TOKEN=") + token};
      if (const char* pythonpath = std::getenv("KIARABOT_PLUGIN_PYTHONPATH")) {
        environment.push_back(std::string("PYTHONPATH=") + pythonpath);
      }
      if (!child.start({executable, "-m", "kiarabot_sdk", "--manifest", manifest}, environment)) {
        std::cerr << "operation=plugin_start outcome=failed error_code=UPSTREAM_UNAVAILABLE\n";
        return 1;
      }
      const auto deadline = std::chrono::steady_clock::now() + start_timeout;
      while (!plugins->connected(identity)) {
        if (stop_pending(signals)) {
          child.stop(stop_timeout);
          scheduling.request_stop();
          if (scheduling.joinable()) {
            scheduling.join();
          }
          server->stop();
          return 0;
        }
        if (!child.running() || std::chrono::steady_clock::now() >= deadline) {
          child.stop(stop_timeout);
          std::cerr << "operation=plugin_start outcome=failed error_code=TIMEOUT\n";
          return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::cout << "operation=plugin_start outcome=registered\n";
    }
  }
#else
  (void)listen;
#endif
  std::cout << "operation=process_start outcome=waiting rpc_ready=false\n" << std::flush;
  int received_signal = 0;
  if (managed) {
    bool child_alive = true;
    while (!stop_pending(signals)) {
      if (child_alive && !child.running()) {
        child_alive = false;
        std::cerr << "operation=plugin_exit outcome=failed error_code=UPSTREAM_UNAVAILABLE\n";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const bool forced = child.stop(stop_timeout);
    std::cout << "operation=plugin_stop forced=" << (forced ? "true" : "false") << "\n";
  } else if (sigwait(&signals, &received_signal) != 0) {
    std::cerr << "operation=process_wait outcome=failed error_code=INTERNAL\n";
    return 1;
  }
#ifdef KIARABOT_HAS_GRPC
  if (control_server) {
    control_server->stop();
  }
  if (server) {
    scheduling.request_stop();
    if (scheduling.joinable()) {
      scheduling.join();
    }
    maintenance_thread.request_stop();
    if (maintenance_thread.joinable()) {
      maintenance_thread.join();
    }
    server->stop();
  }
#endif
  std::cout << "operation=process_stop outcome=completed\n";
  return 0;
#else
  (void)listen;
  std::cerr << "Continuous operation requires a POSIX platform; use --check for self-check\n";
  return 1;
#endif
}

} // namespace

int main(int argc, char* argv[]) try {
#ifdef KIARABOT_HAS_GRPC
  if (argc == 3 && std::string_view(argv[1]) == "--grpc-listen") {
    if (std::string_view(argv[2]).empty()) {
      return 2;
    }
    return run_process(argv[2]);
  }
#endif
  if (argc == 1) {
    return run_process();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--check") {
    return check_runtime();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout
        << "Usage: kiarabot-core [--check|--help|--grpc-listen 127.0.0.1:PORT (proto build)]\n";
    return 0;
  }
  std::cerr << "Usage: kiarabot-core [--check|--help|--grpc-listen 127.0.0.1:PORT (proto build)]\n";
  return 2;
} catch (const std::exception&) {
  std::cerr << "operation=process outcome=failed error_code=INTERNAL\n";
  return 1;
}
