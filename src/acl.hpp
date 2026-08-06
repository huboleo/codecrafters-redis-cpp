#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace acl {

struct User {
    bool enabled = false;
    bool no_password = false;

    std::unordered_set<std::string> password_hashes;

    bool all_commands = false;
    std::unordered_set<std::string> allowed_commands;
    std::unordered_set<std::string> denied_commands;

    bool all_keys = false;
    std::vector<std::string> key_patterns;

    bool all_channels = false;
    std::vector<std::string> channel_patterns;
};

} // namespace acl
