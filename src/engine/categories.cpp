#include "engine/categories.h"

#include <set>
#include <string>

#include "core/util.h"

namespace swiftdm {
namespace {

bool in(const std::set<std::string>& set, const std::string& value) {
    return set.count(value) > 0;
}

}  // namespace

const std::vector<std::string>& category_names() {
    static const std::vector<std::string> kNames = {
        "Video", "Audio", "Compressed", "Document", "Application", "Misc"};
    return kNames;
}

bool is_category_name(std::string_view name) {
    for (const std::string& known : category_names()) {
        if (name == known) return true;
    }
    return false;
}

std::string category_folder_for(const std::string& filename) {
    const std::string ext = extension_of(filename);
    if (ext.empty()) return "Misc";

    static const std::set<std::string> kVideo = {
        "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg",
        "ts", "m2ts", "vob", "3gp", "ogv", "rm", "rmvb", "divx"};
    static const std::set<std::string> kAudio = {
        "mp3", "wav", "flac", "aac", "ogg", "oga", "m4a", "wma", "opus", "aiff",
        "alac", "mid", "midi"};
    static const std::set<std::string> kCompressed = {
        "zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst", "lz", "lzma",
        "cab", "iso", "img", "dmg", "apk", "deb", "rpm", "msi", "msix", "appx",
        "jar", "war", "epub"};
    static const std::set<std::string> kDocument = {
        "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "rtf", "odt",
        "ods", "odp", "csv", "md", "mobi", "fb2", "djvu", "tex", "json", "xml"};
    static const std::set<std::string> kApplication = {
        "exe", "appimage", "flatpak", "snap", "bin", "run", "sh", "bat", "cmd",
        "com", "pkg", "dmg", "app", "elf", "so", "dll"};

    if (in(kVideo, ext)) return "Video";
    if (in(kAudio, ext)) return "Audio";
    if (in(kCompressed, ext)) return "Compressed";
    if (in(kDocument, ext)) return "Document";
    if (in(kApplication, ext)) return "Application";
    return "Misc";
}

}  // namespace swiftdm
