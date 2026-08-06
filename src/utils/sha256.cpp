#include "sha256.hpp"
#include <array>
#include <cstddef>
#include <expected>
#include <openssl/evp.h>
#include <string>
#include <string_view>

std::expected<std::string, std::string> crypto_utils::sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;

    const int result = EVP_Digest(input.data(), input.size(), digest.data(), &digest_length,
                                  EVP_sha256(), nullptr);

    if (result != 1) {
        return std::unexpected("OpenSSL failed to calculate a SHA-256 digest");
    }

    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(digest_length) * 2);

    for (std::size_t i = 0; i < digest_length; ++i) {
        encoded.push_back(hexadecimal[digest[i] >> 4]);
        encoded.push_back(hexadecimal[digest[i] & 0x0f]);
    }

    return encoded;
}
