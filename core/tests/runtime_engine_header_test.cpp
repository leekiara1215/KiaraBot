#include "kiarabot/core/domain/runtime_engine.hpp"

#include <type_traits>

static_assert(std::is_abstract_v<kiarabot::core::detail::RuntimeStatePort>);
static_assert(!std::is_default_constructible_v<kiarabot::core::detail::RuntimeEngine>);
static_assert(std::is_constructible_v<kiarabot::core::detail::RuntimeEngine,
                                      kiarabot::core::detail::RuntimeStatePort&>);
