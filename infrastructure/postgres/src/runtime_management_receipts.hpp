#pragma once
#include "record_access.hpp"
namespace kiarabot::infrastructure::postgres::detail {
core::target::Outcome<std::optional<core::target::RuntimeManagementReceipt>>
find_management_receipt(SqlWork& work, const core::target::ReceiptKey& key);
core::target::Status save_management_receipt(SqlWork& work, const core::target::ReceiptKey& key,
                                             const core::target::RuntimeManagementReceipt& receipt);
} // namespace kiarabot::infrastructure::postgres::detail
