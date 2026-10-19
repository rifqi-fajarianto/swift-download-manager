#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace swiftdm {

// Parsed HLS playlist (m3u8). Master playlists carry variants; media playlists
// carry segment URIs. `EXT-X-KEY` with a real encryption method is surfaced so
// callers can refuse encrypted streams instead of writing garbage.
struct HlsPlaylist {
    bool master = false;
    bool encrypted = false;
    std::vector<std::pair<int64_t, std::string>> variants;  // bandwidth, URI (master)
    std::vector<std::string> segments;
    std::string init_segment;  // EXT-X-MAP URI (fMP4), may be empty
};

// Parses an m3u8 body. Returns false (with a reason) when the text is not a
// usable playlist.
bool parse_hls_playlist(const std::string& text, HlsPlaylist* out, std::string* error);

// Resolves a playlist URI against the absolute URL of the playlist that
// referenced it (absolute, protocol-relative, root-relative or relative).
std::string resolve_playlist_uri(const std::string& base_url, const std::string& uri);

// True when the URL path ends with .m3u8 or the MIME type is an HLS playlist
// type (application/vnd.apple.mpegurl, application/x-mpegURL).
bool is_hls_url(const std::string& url, const std::string& mime_type);

}  // namespace swiftdm
