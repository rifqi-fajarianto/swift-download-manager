# SwiftDM Chromium extension

Initial integration supports Chromium-based browsers (Chrome, Chromium, Edge,
Brave) using Manifest V3. It adds right-click actions for a link or the current
page and a popup button for the current page. Automatic HTTP(S) download
interception is enabled by default and can be disabled from the extension popup;
a separate **Bypass** popup option keeps new downloads in the browser while the
manual actions keep working. The popup also shows the app's live status (idle or
how many downloads are running).

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
   except your local SwiftDM. On the link/page context menu action, Chromium
   also asks for optional cookie permission for the target site. Grant it only
   where authenticated downloads are needed; denying it sends the URL without
   cookies. Automatic interception and the popup action never prompt for cookie
   permission; they use cookies only for origins previously granted.

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
- **Authenticated downloads fail with 403.** Automatic handoff cannot prompt
  for cookies; use the context-menu action once for that site and allow the
  optional cookie permission.

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
- Automatic interception cannot prompt for cookie permission. Use the
  context-menu action once for a site to grant cookies; later automatic
  handoffs for that origin can reuse the grant.
- Cookie permission is optional and requested per target origin.
- Page title is collected for protocol compatibility but is not currently
  stored in the download list.
- `showPanel` and `getVersion` protocol commands are implemented server-side;
  this initial extension does not expose a separate panel UI.
- Extension store packaging/signing and persistent install instructions are
  not part of this development build.
