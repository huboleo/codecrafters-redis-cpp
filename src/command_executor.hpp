#pragma once
#include "database.hpp"
#include "resp.hpp"

class CommandExecutor {
  public:
    [[nodiscard]] resp::Response execute(const resp::Command& command);

  private:
    Database _database;
};
