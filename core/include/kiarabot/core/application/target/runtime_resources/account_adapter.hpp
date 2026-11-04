#pragma once

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target {

// Narrow Runtime resource use cases. Each call owns its local UoW; borrowed ports outlive calls.
class RuntimeAccountAdapterApplication {
public:
  virtual ~RuntimeAccountAdapterApplication() = default;

  virtual Outcome<BotAccount> create_account(const CreateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> update_account(const UpdateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> disable_account(const DisableAccountRequest& request) = 0;
  virtual Outcome<BotAccount> get_account(const AccountQuery& request) = 0;
  virtual Outcome<ListResult<BotAccount>> list_accounts(const AccountListQuery& request) = 0;
  virtual Outcome<BindingMutation> bind_adapter(const BindAdapterRequest& request) = 0;
  virtual Outcome<BindingMutation> unbind_adapter(const UnbindAdapterRequest& request) = 0;
  virtual Outcome<AdapterRegistrationResult>
  register_adapter(const AdapterRegistrationCommand& request) = 0;
  virtual Outcome<HeartbeatResult> heartbeat(const HeartbeatCommand& request) = 0;
  virtual Outcome<AdapterRecord> get_adapter(const AdapterQuery& request) = 0;
  virtual Outcome<ListResult<AdapterRecord>> list_adapters(const AdapterListQuery& request) = 0;
};

} // namespace kiarabot::core::target
