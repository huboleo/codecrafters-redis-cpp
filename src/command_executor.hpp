#pragma once
#include "resp.hpp"

class CommandExecutor {
  public:
    [[nodiscard]] resp::Response execute(const resp::Command& command);
};
