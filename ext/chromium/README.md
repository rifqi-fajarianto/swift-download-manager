# SwiftDM Chromium extension

Initial integration supports Chromium-based browsers (Chrome, Chromium, Edge,
Brave) using Manifest V3. It adds right-click actions for a link or the current
page and a popup button for the current page. It does not intercept every
browser download automatically.

## Install for development

1. Build and start SwiftDM. The app listens on `127.0.0.1:10007` while its GUI
   is running.
2. Open the browser's extensions page (for example `chrome://extensions`),
   enable **Developer mode**, and choose **Load unpacked**.
3. Select this `ext/chromium` directory.
4. Open an HTTP(S) page, then use the extension popup or right-click a link and
   select **Download link with SwiftDM**.
5. On the link/page context menu action, Chromium asks for optional cookie
   permission for the target site. Grant it only where authenticated downloads
   are needed; denying it sends the URL without cookies. The popup action does
   not trigger permission prompts and will send without cookies unless already
   granted.

For command-line builds, install Qt 6 Network as well as Qt 6 Widgets (on Debian
and Ubuntu this is included in `qt6-base-dev`).

The app binds only to IPv4 loopback and accepts the `swiftdm.v1` WebSocket
subprotocol from a Chromium extension origin. Browser cookie data is held only
for the active request and is not persisted by SwiftDM. Redirect requests do
not forward sensitive Cookie/Authorization headers to another host.
Partitioned (CHIPS) cookies are currently omitted because they require a
top-level-site context that the downloader does not retain.

## Current scope and limitations

- HTTP and HTTPS links/pages; no automatic download interception yet.
- Cookie permission is optional and requested per target origin.
- Page title is collected for protocol compatibility but is not currently
  stored in the download list.
- `showPanel` and `getVersion` protocol commands are implemented server-side;
  this initial extension does not expose a separate panel UI.
- Extension store packaging/signing and persistent install instructions are
  not part of this development build.
