# SwiftDM Firefox extension

Firefox build of the SwiftDM browser integration, for Firefox and the Firefox
based browsers that accept Firefox add-ons (LibreWolf, Waterfox, Zen, Floorp,
Firefox ESR). It uses Manifest V3 with an event page, because Firefox does not
run extension service workers. The popup, the background logic and the protocol
are the same as the Chromium extension: `ext/shared/` holds the single copy of
those files and both browser directories are assembled from it.

Right-click actions for a link or the current page, a popup button for the
current page, automatic HTTP(S) download interception (on by default, switched
off from the popup), the live app status, detected HLS playlists and adaptive
video+audio pairs all behave exactly as documented for the Chromium extension in
[`../chromium/README.md`](../chromium/README.md). This file covers what is
different on Firefox: installation, permissions and troubleshooting.

## Install from a release

Every `vX.Y.Z` release carries two artifacts with the same extension version:

| File | Use |
|---|---|
| `swiftdm-firefox-extension-<version>.zip` | Unpacked directory for `about:debugging` (temporary install) |
| `swiftdm-firefox-extension-<version>.xpi` | Signed by Mozilla, installs permanently (only when the release was signed) |

The unpacked ZIP is always published. The signed XPI is produced only when the
release workflow has the `WEB_EXT_API_KEY`/`WEB_EXT_API_SECRET` secrets
configured, because a permanent Firefox install requires a Mozilla signature.

### Temporary install (unpacked ZIP)

1. Download and extract the ZIP to a folder you keep, for example
   `~/swiftdm-firefox`. Extract it *as a folder*: Firefox needs a directory.
2. Open `about:debugging#/runtime/this-firefox` and choose **Load Temporary
   Add-on…**, then select `manifest.json` inside that folder.
3. Start SwiftDM and use the popup or the right-click actions.

A temporary add-on is removed when Firefox closes, so the folder has to be
loaded again after a restart. Use the signed XPI below for a permanent install.

### Permanent install (signed XPI)

1. Download `swiftdm-firefox-extension-<version>.xpi`.
2. Open `about:addons`, click the gear icon, choose **Install Add-on From File…**
   and select the XPI. Firefox accepts it because Mozilla signed it.
3. Alternatively open the XPI URL directly in Firefox and confirm the prompt.

An unsigned XPI cannot be installed permanently: Firefox refuses it with
"This add-on could not be installed because it appears to be corrupt" unless the
browser is a Developer Edition/Nightly build with
`xpinstall.signatures.required` set to `false`.

### Signing a build yourself

If a release has no XPI, or you build from a checkout:

```sh
node tools/extension_pack.mjs build --browser firefox
npx web-ext@8 sign --source-dir ext/firefox --channel unlisted
```

`--channel unlisted` keeps the add-on off the public listing and returns a
signed XPI. The AMO credentials come from
`WEB_EXT_API_KEY`/`WEB_EXT_API_SECRET` or an interactive login.

## Install for development

1. Build and start SwiftDM. The app listens on `127.0.0.1:10007` while its GUI
   is running.
2. Assemble the loadable directory, which copies `ext/shared/` next to the
   Firefox manifest:

   ```sh
   node tools/extension_pack.mjs build --browser firefox
   ```

   Rerun the command after changing anything in `ext/shared/`. The generated
   copies are git-ignored; `build/extensions/firefox` holds the same directory
   after a CMake build.
3. Open `about:debugging#/runtime/this-firefox`, choose **Load Temporary
   Add-on…** and select `ext/firefox/manifest.json`.
4. Open an HTTP(S) page, then use the extension popup or right-click a link and
   select **Download link with SwiftDM**.

`npx web-ext@8 run --source-dir ext/firefox` starts a throwaway Firefox profile
with the extension loaded and reloads it on file changes.

## Permissions on Firefox

Firefox asks for the permissions in one install prompt: `downloads`, `storage`,
`webRequest`, `contextMenus`, plus HTTP(S) host access. Host access is what lets
the extension observe POST bodies, `x-*` request headers and response content
types for automatic handoffs; those observations expire after 30 seconds and are
never sent anywhere except your local SwiftDM.

- **Host access can be revoked per site.** Firefox lets the user allow or block
  an extension's site access (`about:addons` → the add-on → **Permissions**).
  Without host access `webRequest` does not fire, so automatic interception and
  the detected-media list stop working for that site while the popup button and
  the right-click actions keep working.
- **The cookie permission is optional and separate.** Turn on **Send browser
  cookies** in the popup and accept the prompt, or use a right-click action,
  which asks for the target site. The background context can only read cookies
  after that grant; it can never prompt on its own.
- **`moz-extension://` origins cannot be pinned.** Firefox generates the
  extension UUID per profile, so SwiftDM validates the origin shape instead of a
  fixed ID. The accepted origin is logged and shown in the app's **Extensions**
  dialog. Only the loopback port is reachable, and the handshake still requires
  the `swiftdm.v1` subprotocol.

## Troubleshooting

- **The popup says the connection failed.** Start SwiftDM and open its
  **Extensions** toolbar button; it reports whether `127.0.0.1:10007` is
  listening, the OS error when it is not, and the origin of the last rejected
  handshake. Use **Retry connection** in the popup after fixing the app.
- **The add-on disappeared after restarting Firefox.** A temporary add-on only
  lives for the session. Install the signed XPI, or load the folder again.
- **The add-on was disabled after a Firefox restart.** Firefox disables
  extensions whose files changed while they were loaded; reload it from
  `about:debugging`.
- **Downloads still open in the browser.** Check that **Automatically use
  SwiftDM for downloads** is on in the popup and that the add-on still has site
  access for that page.
- **Port 10007 is already in use.** A previous SwiftDM window may still be
  running in the notification tray. Quit it from the tray (or check with
  `ss -ltnp | grep 10007`), then start SwiftDM again. Closing the window only
  hides it.
- **Private or signed-in downloads are refused (403, or a sign-in page saved as
  a file).** Open the file's page in the browser, turn on **Send browser
  cookies** in the popup and allow the prompt, then press **Download this
  page**; later automatic handoffs reuse the grant.
- **No media is listed in the popup.** Detected media come from `webRequest`
  response headers, so they need site access for that page. Reload the page
  after granting access.

## Firefox-specific limitations

- The background context is an event page, not a service worker. It is unloaded
  when idle, so the status socket reconnects when the popup is opened or an
  event wakes the page; the toolbar icon is restored from storage on every
  start.
- `DownloadItem` on Firefox has no `finalUrl`, so an automatic handoff forwards
  the item's original URL. Chromium additionally matches observations against
  the post-redirect address.
- Container tabs (`contextualIdentities`) are not handled specially: cookies are
  read for the download URL, not for the tab's cookie store.
- Manifest V2 builds, Firefox for Android and the AMO public listing are out of
  scope for this release.
