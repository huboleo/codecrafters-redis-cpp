#pragma once

#include <string>
#include <unordered_set>

namespace acl {

struct User {
    bool no_password = false;

    std::unordered_set<std::string> password_hashes;
};

} // namespace acl
