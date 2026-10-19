#pragma once

#include <string>

namespace swiftdm {

// Download interstitials (docs/spec/04-protocols.md §5). Some hosts answer a
// file link with a small HTML page instead of the file. Google Drive is the
// known case: viewer links serve the Drive web app, files that exceed Google's
// virus-scan limit answer with a "can't scan this file for viruses" notice that
// has to be confirmed, and private files redirect to a sign-in wall. Saving
// those pages would store a 2 KB document under a name derived from the URL
// (the Drive id), so the probe resolves the page or fails with a reason.
//
// The helpers below keep that site knowledge out of the transfer code and are
// unit-testable without a network.

// Hosts that serve Google Drive links (drive.google.com,
// drive.usercontent.google.com) and Google Docs editor links (docs.google.com).
bool is_google_drive_host(const std::string& url);
bool is_google_docs_host(const std::string& url);

// True for accounts.google.com, where a private Drive file redirects.
bool is_google_accounts_host(const std::string& url);

// True when `html` is Google's sign-in page (the title/link text a browser
// shows for a Drive link that needs an account).
bool is_google_signin_page(const std::string& html);

// File id of a Drive file/viewer link: "/file/d/<id>[/view]", "/open?id=<id>",
// "/uc?id=<id>", "/download?id=<id>", with an optional "/drive/u/<n>" prefix.
// "" when `url` is not such a link.
std::string google_drive_file_id(const std::string& url);

// Folder id of a Drive folder link ("/drive[/u/<n>]/folders/<id>"). "" when
// `url` is not a folder link.
std::string google_drive_folder_id(const std::string& url);

// Direct download URL for a Drive file id, carrying `authuser` when the link
// named an account. This is the URL Drive itself redirects to.
std::string google_drive_download_url(const std::string& file_id);

// Copies the handoff parameters of `link` that `url` does not carry yet:
// `authuser` (account), `resourcekey` (security-updated links, which need the
// key to resolve the file) and `confirm` (the confirmation flag a browser's own
// download URL carries, so those URLs resolve in one step). Other query data of
// the link is dropped.
std::string keep_drive_handoff_params(const std::string& url, const std::string& link);

// Value of a query parameter ("name=value" pairs, no percent decoding). ""
// when the parameter is absent.
std::string url_query_param(const std::string& url, const std::string& name);

// Direct URL of a download-confirmation page: the first form that carries
// hidden fields naming a file id and a confirmation flag (Drive's virus-scan
// notice, whose "Download anyway" button submits `confirm=t`). "" when `html`
// holds no such form. The session-scoped `uuid` field is dropped so the URL
// stays stable across runs (Drive ignores the value; verified against the live
// host), which keeps the resumed partial file matching.
std::string download_confirmation_url(const std::string& html);

// Failure messages for links that are not a downloadable file.
inline constexpr const char* kDriveFolderError =
    "Google Drive folder link - folders cannot be downloaded as a single file; "
    "open the folder in a browser and download the files individually";
inline constexpr const char* kSignInError =
    "the link needs a signed-in Google account - the file is private or was "
    "removed; share it as 'Anyone with the link' and try again, or hand the "
    "download over from the browser so the session comes along";
inline constexpr const char* kDrivePageError =
    "Google Drive returned a web page instead of the file";
inline constexpr const char* kDocsEditorError =
    "Google Docs editor link - use 'File > Download' in the browser or the "
    "file's export link";
inline constexpr const char* kConfirmationPendingError =
    "download confirmation page could not be resolved - the link still returns "
    "a web page";

// Reason for an HTML response that is a web page rather than the file. Empty
// when the page is not a known site page and may be downloaded as-is: Drive
// serves every file with Content-Disposition, so a page from those hosts is
// never the file itself.
std::string page_error_reason(const std::string& url, const std::string& final_url);

// Reason when the answer is a sign-in wall, either reached through a redirect
// (`effective_url` is accounts.google.com) or recognized in the page text
// (`html`, may be empty). "" when neither applies.
std::string signin_error_reason(const std::string& effective_url, const std::string& html);

}  // namespace swiftdm
