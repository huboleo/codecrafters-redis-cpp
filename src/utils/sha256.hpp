#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace crypto_utils {

[[nodiscard]] std::expected<std::string, std::string> sha256(std::string_view input);

} // namespace crypto_utils
