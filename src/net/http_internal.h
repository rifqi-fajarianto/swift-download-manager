#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "net/http.h"

// Helpers shared by the http translation units (http.cpp, http_request.cpp,
// http_error.cpp, http_probe.cpp, http_segment.cpp, http_stream.cpp). They are
// internal to the net layer; net/http.h stays the only public interface.
namespace swiftdm::detail {

constexpr int kIoTimeoutMs = 100;         // curl multi poll timeout
constexpr int64_t kReportIntervalMs = 250;
constexpr int kMaxRetries = 5;

// Failure messages carry the host's own explanation (rate limits, captcha
// requirements, hotlink protection) instead of a bare "HTTP/1.1 403 Forbidden".
constexpr size_t kErrorBodyLimit = 1024;   // body bytes kept per request
constexpr size_t kErrorDetailLimit = 300;  // bytes shown in the message

int64_t now_ms();
void ensure_curl_global();
std::string http_error_text(long code);
std::string curl_url_error(CURLcode code);
curl_slist* build_headers(const std::vector<HttpHeader>& headers);

struct RequestOriginGuard {
    std::string original_origin;
    bool guarded = false;
    bool crossed_origin = false;
    CURL* handle = nullptr;
};

bool request_has_sensitive_headers(const HttpOptions& options);
std::string request_origin(const char* url);
int redirect_origin_guard(void* userdata, char* url, char* redirect, int length, int flag);
void apply_common_options(CURL* handle, const HttpOptions& o, curl_slist* headers,
                          RequestOriginGuard* origin_guard);
std::string parse_content_disposition_filename(const std::string& value);
bool parse_content_range(const std::string& value, uint64_t* start, uint64_t* end,
                         uint64_t* total);
bool starts_with_ci(const std::string& text, const char* prefix);
std::string condense_error_body(const std::string& body, const std::string& content_type);
std::string http_error_with_detail(long code, const std::string& body,
                                   const std::string& content_type);

}  // namespace swiftdm::detail
