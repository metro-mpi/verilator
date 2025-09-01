// has to export LIBS="-lsodium" in order to use

#pragma once
#include <sodium.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>

inline std::string blake2b_128_hex(const std::string& input) {
    constexpr size_t OUTLEN = 16;  // 128 bits = 16 bytes
    unsigned char hash[OUTLEN];

    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium init failed");
    }

    if (crypto_generichash(hash, OUTLEN,
                           reinterpret_cast<const unsigned char*>(input.data()), input.size(),
                           nullptr, 0) != 0) {
        throw std::runtime_error("BLAKE2b hashing failed");
    }

    // Convert hash bytes to hex string
    std::ostringstream oss;
    for (size_t i = 0; i < OUTLEN; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}
