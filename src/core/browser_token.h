#pragma once

#include <string>

namespace swiftdm {

// Trust-on-first-use pairing token for the browser integration
// (docs/spec/08-media-download.md §7).
//
// Binding the WebSocket server to 127.0.0.1 keeps it off the network, but any
// local process can still connect and claim to be the extension. The token is
// what turns "somebody connected" into "the extension the user approved".
//
// The token lives in `$XDG_CONFIG_HOME/swiftdm/browser-token` with mode 0600
// (owner read/write only). Deleting that file revokes every paired extension.
class BrowserToken {
  public:
    // Path of the token file next to the settings file. Empty when the
    // platform has no config directory.
    static std::string default_path();

    // Reads the stored token. Returns an empty string when no token exists yet
    // (not paired) or when the file cannot be read.
    static std::string load(const std::string& path);

    // Creates a new random token and writes it to `path`. Returns the token, or
    // an empty string on failure (with `error` filled when provided).
    static std::string create(const std::string& path, std::string* error = nullptr);

    // True when `candidate` equals `expected`, comparing in constant time so a
    // local process cannot learn the token one byte at a time. An empty
    // `expected` (nothing stored yet) never matches.
    static bool verify(const std::string& expected, const std::string& candidate);

    // 32 random bytes, base64url without padding (43 characters).
    static std::string generate();
};

}  // namespace swiftdm
