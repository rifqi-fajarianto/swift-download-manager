# SwiftDM Chromium extension

Initial integration supports Chromium-based browsers (Chrome, Chromium, Edge,
Brave) using Manifest V3. It adds right-click actions for a link or the current
page and a popup button for the current page. Automatic HTTP(S) download
interception is enabled by default and can be disabled from the extension popup;
a separate **Bypass** popup option keeps new downloads in the browser while the
manual actions keep working. The popup also shows the app's live status (idle or
how many downloads are running). The popup also lists media it noticed for the
current tab: HLS (`.m3u8`) playlists and adaptive video+audio track pairs, each
downloadable with one click.

## Install for development

1. Build and start SwiftDM. The app listens on `127.0.0.1:10007` while its GUI
   is running.
2. Open the browser's extensions page (for example `chrome://extensions`),
   enable **Developer mode**, and choose **Load unpacked**.
3. Select this `ext/chromium` directory.
4. Open an HTTP(S) page, then use the extension popup or right-click a link and
   select **Download link with SwiftDM**.
5. Chromium asks for the `downloads` and `webRequest` permissions plus HTTP(S)
   host access when the extension is installed or reloaded. Observation is used
   only to remember POST bodies and `x-*` request headers for intercepted
   downloads; those entries expire after 30 seconds and are never sent anywhere
   except your local SwiftDM. Chromium also asks for the optional cookie
   permission when the popup's **Send browser cookies** switch is turned on (for
   the current site) or when the link/page context-menu action runs (for its
   target origin). Grant it where signed-in downloads are needed; denying it
   sends the URL without cookies. Only a user gesture can prompt, so automatic
   interception reads cookies only after such a grant has been given.

For command-line builds, install Qt 6 Network as well as Qt 6 Widgets (on Debian
and Ubuntu this is included in `qt6-base-dev`).

The app binds only to IPv4 loopback and accepts the `swiftdm.v1` WebSocket
subprotocol from a well-formed Chromium extension origin. The exact extension ID
is intentionally not pinned: reloading an unpacked extension from a different
path can change its ID. Browser cookie data is held only for the active request
and is not persisted by SwiftDM. Redirect requests do not forward sensitive
Cookie/Authorization headers to another host. Partitioned (CHIPS) cookies are
currently omitted because they require a top-level-site context that the
downloader does not retain.

## Troubleshooting

- **The popup says the connection failed.** Start SwiftDM and open its
  **Extensions** toolbar button; it reports whether `127.0.0.1:10007` is
  listening, the OS error when it is not, and the origin of the last rejected
  handshake. Use **Retry connection** in the popup after fixing the app.
- **Edge DevTools shows `extensions://...`.** That is Edge's extension-page
  scheme label; the server accepts `extensions://`, `extension://`, and
  `chrome-extension://` origins with a valid extension ID.
- **Port 10007 is already in use.** A previous SwiftDM window may still be
  running in the notification tray. Quit it from the tray (or check with
  `ss -ltnp | grep 10007`), then start SwiftDM again. Closing the window only
  hides it.
- **Downloads still open in the browser.** Reload the extension after changing
  its files, accept the new `downloads` permission if Chromium asks, and make
  sure **Automatically use SwiftDM for downloads** is checked in the popup.
- **Private or signed-in downloads are refused (403, or a sign-in page saved as
  a file).** Open the file's page in the browser, turn on **Send browser
  cookies** in the popup and allow the prompt, then press **Download this
  page**; later automatic handoffs reuse the grant. Sharing the file as "Anyone
  with the link" also works without cookies. The popup switch and the
  context-menu action are the only places that can prompt.

## Current scope and limitations

- HTTP and HTTPS links/pages; automatic interception is enabled by default.
- Automatic interception cancels the browser item before contacting SwiftDM. If
  SwiftDM is unreachable, the extension asks the browser to download the URL
  again. Very small downloads that finish before they can be canceled stay in
  the browser to avoid a duplicate copy.
- If the app is not running, auto-interception briefly shows then restores the
  browser download. Check the popup's **Retry connection** message for details.
- Handoff forwards observed POST bodies and `x-*` request headers when the
  request was seen by `webRequest` (UTF-8 or base64, up to 256 KiB). Multipart
  bodies are captured as raw bytes; body data is only kept in memory for 30
  seconds. `blob:` and `data:` downloads are not forwarded.
- Bypass keeps new downloads in the browser until it is turned off; right-click
  and popup actions still send links/pages to SwiftDM while bypass is on.
- The popup shows live app status (idle, or the number of active downloads)
  pushed over a subscription socket; it reconnects automatically on the next
  popup open or extension event.
- Automatic interception cannot prompt for cookie permission. Grant it once
  from the popup's **Send browser cookies** switch (or a context-menu action);
  later handoffs reuse it, and the popup switch shows the current state.
- Cookie permission is optional. The popup asks for the site it was opened on;
  a single grant covers the HTTP(S) sites the extension serves, because host
  access is already declared in the manifest.
- Browser page URL and title are shown in download Properties and persisted in
  SwiftDM's SQLite history along with POST bodies and forwarded headers; the
  history list is not restored into the GUI yet.
- Detected HLS playlists are sent as ordinary URLs: SwiftDM fetches the
  playlist and its segments itself. Encrypted (`EXT-X-KEY`) playlists are
  refused by the app with a clear error.
- Adaptive pairs are two separate downloads that share a `group` token (shown
  as **Pair** in Properties and stored in `downloads.groupid`). SwiftDM does not
  mux them into one MKV yet; both files are kept side by side.
- FTP has no automatic interception (Chromium no longer loads FTP pages);
  explicit `ftp://` links can be sent from the context menu, and the app's
  manual dialog and CLI accept FTP URLs directly.
- `showPanel` and `getVersion` protocol commands are implemented server-side;
  this initial extension does not expose a separate panel UI.
- Extension store packaging/signing and persistent install instructions are
  not part of this development build.
