#include "core/util.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace swiftdm {

std::string sanitize_filename(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || uc < 0x20) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // Trim leading/trailing dots and spaces (Windows-style safety, harmless on Linux).
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    size_t b = 0;
    while (b < out.size() && (out[b] == ' ')) ++b;
    out = out.substr(b);
    // A leading dot would make the name hidden: a server-controlled name such
    // as ".bashrc" must not drop a dotfile into the download directory (or
    // over a dotfile in the home directory). ".." is covered as well, as the
    // dot is not part of that trim.
    if (!out.empty() && out.front() == '.') out.insert(0, "_");
    // Windows reserves the device names even with an extension ("CON.txt").
    if (!out.empty() && is_windows_reserved_name(out)) out.insert(0, "_");
    // A .desktop file in the download directory can be launched by the desktop
    // environment (the "untrusted" marking of the desktop entry spec is not
    // enforced everywhere), so the extension is neutralised.
    if (ends_with(to_lower(out), ".desktop")) out += "_";
    // A single path component is limited by the filesystem, so a long name
    // (a token in the URL path, for example) has to be shortened.
    out = limit_filename_length(out, kMaxFileNameBytes);
    // Shortening can leave a trailing dot or space behind.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

std::string_view utf8_truncate(std::string_view text, size_t bytes) {
    if (bytes >= text.size()) return text;
    size_t cut = bytes;
    // Walk back over the continuation bytes so a multi-byte sequence is not
    // split in half.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

namespace {

// Splits a name into the stem and the extension. A leading dot does not start
// an extension (".bashrc") and an over-long extension is dropped instead of
// eating the budget of the stem.
std::pair<std::string, std::string> split_extension(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {std::string(name), std::string()};
    }
    if (name.size() - dot > kMaxExtensionBytes) {
        return {std::string(name), std::string()};
    }
    return {std::string(name.substr(0, dot)), std::string(name.substr(dot))};
}

}  // namespace

std::string limit_filename_length(std::string_view name, size_t max_bytes) {
    if (name.size() <= max_bytes) return std::string(name);

    const auto [stem, ext] = split_extension(name);
    const std::string suffix = "-" + hex_hash64(fnv1a_hash(name));
    size_t budget = 0;
    if (max_bytes > suffix.size() + ext.size()) {
        budget = max_bytes - suffix.size() - ext.size();
    }
    std::string head(utf8_truncate(stem, budget));
    // A shortened stem must not end with a dot or a space (Windows rejects
    // both at the end of a name).
    while (!head.empty() && (head.back() == ' ' || head.back() == '.')) head.pop_back();
    if (head.empty()) head = "file";

    std::string out = head + suffix + ext;
    if (out.size() > max_bytes) {
        // Only reachable for an absurdly small budget: drop the extension.
        const size_t room = max_bytes > suffix.size() ? max_bytes - suffix.size() : 0;
        std::string trimmed(utf8_truncate(head, room));
        if (trimmed.empty()) trimmed = "f";
        out = trimmed + suffix;
    }
    return out;
}

bool is_windows_reserved_name(std::string_view name) {
    // Windows checks the part before the first dot, ignoring trailing spaces.
    std::string_view base = name.substr(0, name.find('.'));
    while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.remove_suffix(1);
    if (base.empty()) return false;
    const std::string lower = to_lower(base);
    static const char* kReserved[] = {
        "con", "prn", "aux", "nul",  "clock$",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
        // The superscript variants are reserved as well.
        "com\xc2\xb9", "com\xc2\xb2", "com\xc2\xb3",
        "lpt\xc2\xb9", "lpt\xc2\xb2", "lpt\xc2\xb3",
    };
    for (const char* reserved : kReserved) {
        if (lower == reserved) return true;
    }
    return false;
}

uint64_t fnv1a_hash(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_hash64(uint64_t value) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::string file_name_from_url(std::string_view url) {
    // Strip query/fragment.
    std::string_view u = url;
    auto q = u.find_first_of("?#");
    if (q != std::string_view::npos) u = u.substr(0, q);
    // Strip trailing slashes.
    while (!u.empty() && u.back() == '/') u.remove_suffix(1);
    auto slash = u.find_last_of('/');
    std::string_view name = (slash == std::string_view::npos) ? u : u.substr(slash + 1);
    // Strip scheme prefix (e.g. "https:" with no path).
    auto colon = name.find(':');
    if (colon != std::string_view::npos) name = name.substr(colon + 1);
    return sanitize_filename(percent_decode(name));
}

std::string extension_of(std::string_view filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) return "";
    return to_lower(filename.substr(dot + 1));
}

std::string errno_string(int err) { return std::strerror(err); }

}  // namespace swiftdm
