#include "core/browser_token.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "core/util.h"
#include "store/settings.h"

namespace swiftdm {
namespace {

// base64url without padding: 32 bytes -> 43 characters.
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(const std::array<uint8_t, 32>& bytes) {
    std::string out;
    out.reserve(43);
    size_t index = 0;
    while (index + 2 < bytes.size()) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[index]) << 16) |
                               (static_cast<uint32_t>(bytes[index + 1]) << 8) |
                               static_cast<uint32_t>(bytes[index + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
        out.push_back(kAlphabet[chunk & 0x3f]);
        index += 3;
    }
    // 32 = 3*10 + 2, so two bytes are left: 3 characters, no padding.
    const uint32_t tail = (static_cast<uint32_t>(bytes[index]) << 8) |
                          static_cast<uint32_t>(bytes[index + 1]);
    out.push_back(kAlphabet[(tail >> 10) & 0x3f]);
    out.push_back(kAlphabet[(tail >> 4) & 0x3f]);
    out.push_back(kAlphabet[(tail << 2) & 0x3f]);
    return out;
}

bool read_random(std::array<uint8_t, 32>* out) {
    std::random_device device;
    try {
        for (size_t i = 0; i < out->size(); i += 4) {
            const unsigned int value = device();
            for (size_t byte = 0; byte < 4 && i + byte < out->size(); ++byte) {
                (*out)[i + byte] = static_cast<uint8_t>((value >> (8 * byte)) & 0xff);
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

std::string BrowserToken::default_path() {
    const std::string config = default_config_path();
    if (config.empty()) return {};
    return path_to_utf8(path_from_utf8(config).parent_path() / "browser-token");
}

std::string BrowserToken::generate() {
    std::array<uint8_t, 32> bytes{};
    if (!read_random(&bytes)) return {};
    return base64url(bytes);
}

std::string BrowserToken::load(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) return {};
    std::string token((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return trim(token);
}

std::string BrowserToken::create(const std::string& path, std::string* error) {
    if (path.empty()) {
        if (error) *error = "no configuration directory for the pairing token";
        return {};
    }
    const std::filesystem::path target = path_from_utf8(path);
    if (target.has_parent_path()) {
        std::string directory_error;
        if (!ensure_private_directory(path_to_utf8(target.parent_path()), &directory_error)) {
            if (error) *error = directory_error;
            return {};
        }
    }
    const std::string token = generate();
    if (token.empty()) {
        if (error) *error = "cannot generate a random pairing token";
        return {};
    }
    // 0600: the token is a secret, readable by its owner only.
    if (!write_file_atomic(path, token, 0600, error)) return {};
    return token;
}

bool BrowserToken::verify(const std::string& expected, const std::string& candidate) {
    // An unpaired app has no token yet: never accept a connection as "paired".
    if (expected.empty() || candidate.empty()) return false;
    // Constant-time comparison: the length is allowed to differ (it is not a
    // secret worth hiding beyond that), but the bytes never short-circuit.
    unsigned char difference = expected.size() == candidate.size() ? 0 : 1;
    const size_t length = expected.size() < candidate.size() ? expected.size()
                                                             : candidate.size();
    for (size_t i = 0; i < length; ++i) {
        difference |= static_cast<unsigned char>(expected[i]) ^
                      static_cast<unsigned char>(candidate[i]);
    }
    return difference == 0;
}

}  // namespace swiftdm
