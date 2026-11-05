<!--
    SPDX-FileCopyrightText: 2026 KRDC contributors
    SPDX-License-Identifier: CC0-1.0
-->

# RDP connection files and Azure Virtual Desktop in KRDC

Status: first milestone, **experimental**. Full desktops only.

KRDC is **not** a Microsoft client and is not supported by Microsoft. To list
Azure Virtual Desktop (AVD) workspaces it signs in with the public client ID of
Microsoft's Remote Desktop client and sends the `X-MS-User-Agent` value of
Microsoft's web client, because the AVD feed refuses unknown clients. FreeRDP
uses the same public client ID for AVD by default. Microsoft can stop accepting
this at any time. All endpoints and these identifiers are kept in one place,
`rdp/avd/avdendpoints.cpp` (`Avd::publicCloud()`); only the public cloud is
defined so far.

## What works

### Opening `.rdp` files

*New Connection*, the command line (`krdc file.rdp`) or the file
manager open `.rdp` files. KRDC reads them with FreeRDP's own parser and applies
**all** their settings to the session, not only address and user name:

- UTF-8 (with or without BOM) and UTF-16 files (with BOM, as Windows writes them).
- Typed values (`:i:`, `:s:`), values containing colons, IPv6 addresses
  (`full address:s:[2001:db8::10]:3389`), `alternate full address`.
- RD Gateway and AVD settings (`gatewayhostname`, `resourceprovider:s:arm`,
  `wvd endpoint pool`, `armpath`, …). A file is treated as AVD if it says
  `resourceprovider:s:arm` or its gateway is below `wvd.microsoft.com`; KRDC then
  uses FreeRDP's ARM gateway transport.
- The logon to the remote computer is the one the file configures. Microsoft
  Entra sign-in to the remote computer is only used with `enablerdsaadauth:i:1`;
  KRDC does not force it because a file is AVD or names a tenant.

With a file, the file decides where and how to connect and log in (address,
gateway, security protocols). KRDC's host settings still decide how the session
is shown locally: resolution and monitors, scaling, colour depth, acceleration,
sound, keyboard layout, proxy, shared folder.

Before connecting through the file's gateway (`gatewayusagemethod` 1 or 2, as
FreeRDP reads it), KRDC asks you to accept it, even for a host with saved
settings. For a host without saved settings, KRDC saves the file's display
size, full screen and monitors, scaling, sound, clipboard opt-out and an
always-used gateway as that host's settings. Workspace launches skip both steps.

KRDC deliberately does **not** take from a file:

- device redirection (`drivestoredirect`, `devicestoredirect`,
  `usbdevicestoredirect`, `camerastoredirect`, `redirectdrives`,
  `redirectcomports`, `redirectposdevices`); share folders in the host settings.
  CR, LF and CRLF all end a record, as they do for FreeRDP, so a record cannot
  hide behind a lone CR;
- FreeRDP command-line options (lines starting with `/`): such files are refused,
  whether or not FreeRDP was built to read them;
- relaxed certificate checks: `IgnoreCertificate` and automatic acceptance stay
  off, and `authentication level:i:0` becomes `2` (warn);
- remote applications (`remoteapplicationmode:i:1`) are refused with a message
  instead of being opened as a desktop. A program name with mode `0` is a desktop.

`.rdpw` workspace files are refused with a message: they are a different format
and FreeRDP has no parser for them.

### Credentials

- A user name in a file is only a suggestion. It fills the login dialog and can
  be changed there; KRDC never adds it to the address, history or bookmarks.
- Passwords for the remote computer and for a gateway are kept apart. A gateway
  prompt names the gateway and never fills in or changes the remote computer's
  login (unless the file or host settings say the gateway uses the same login).
- If the connection is set up for Entra sign-in but the remote computer asks for
  a password, the dialog says so: its policy may require a different logon.
  KRDC does not try to work around that.
- Plain-text `password:s:` values are used for that one connection and never stored.

### Microsoft Entra ID sign-in during a connection

FreeRDP asks for a token for the AVD gateway and, with `enablerdsaadauth:i:1`,
another one for the remote computer (bound to a proof-of-possession key).
FreeRDP builds the requests, keeps the OAuth state and PKCE verifier, and
exchanges the code, including the scope and `req_cnf` of the remote computer.
KRDC only gets the code to FreeRDP, with your default browser (see
[Signing in with the browser](#signing-in-with-the-browser)).

KRDC refuses to open a sign-in whose request from FreeRDP lacks a single
`state`, S256 PKCE, `response_type=code`, exactly one `client_id` equal to
FreeRDP's AVD client ID, or the `ms-appx-web` redirect below.
A FreeRDP sign-in helper is only used if FreeRDP's `AadAuthHelper` setting
names one by absolute path; KRDC no longer lets FreeRDP pick one by itself,
because `freerdp-xdg-aad-helper` would register itself for `ms-appx-web:`
links without asking.

Canceling ends the connection quietly. Errors from Entra ID, such as conditional
access refusals (`AADSTS…`), are shown in the connection error page.

### Azure Virtual Desktop workspaces

*File ▸ Azure Virtual Desktop Workspaces…*

1. *Sign In…* uses the same browser sign-in (authorization code with PKCE,
   `prompt=select_account`).
2. KRDC fetches the feed discovery document and each workspace feed, and shows
   the resources as cards: desktops under *Devices*, remote applications under
   *Apps*. Apps are shown slightly dimmed with a "Coming soon" badge and cannot be
   opened yet. A search field filters both by title and workspace.
3. Nothing is opened until you activate a desktop card (double-click, Enter) or
   press *Connect*. KRDC first makes sure it has an access token that is valid
   for at least five more minutes (renewing it with the stored refresh token if
   needed), then downloads its connection file and opens it like a local `.rdp` file; the feed
   token is handed to FreeRDP as the AVD gateway token for that launch only,
   and only if the file's gateway is an AVD gateway of that cloud (below
   `wvd.microsoft.com`, HTTPS port, not an IP address); otherwise the desktop is
   not opened. During any connection KRDC signs in for an AVD gateway token only
   for such a gateway.

The list is cached per cloud and account for one hour from the fetch (not
extended by reading it; a clock set back makes it stale). Reopening the window
within that hour shows it without the network or the keychain; *Refresh* always
fetches again. Only a complete fetch replaces the cache, so a failed or partial
update leaves the last good list in place, shown with its time and the error.
Complete resources, with their connection file and icon addresses, are kept in
memory only, for the running KRDC. On disk, in
`~/.cache/krdc/avd-workspaces/` (directory 0700, files 0600, names hashed from
cloud and account, at most 2 MiB and 1000 resources, anything else is discarded)
KRDC keeps titles, types, workspaces, tenant IDs, the fetch time and decoded icons:
no tokens, no addresses (they may carry signatures), no connection files. After a
restart, opening a desktop from that list fetches the feed again to find its
current connection file. *Sign Out* deletes the cached list of the account, and
a fetch that began before it is not stored afterwards. KRDC opens one workspace
window at a time; choosing the action again brings the open one forward. Icons
read back from disk are decoded only for listed resources, at most 64×64 pixels,
200 icons and 3.2 MB decoded.

Icons: feeds name them by address only (`<Icons><Icon32 FileType="Png" FileURL=…/>`
… `<IconRaw FileType="Ico" …/>`, [MS-TSWP] 2.2.2.1.6). KRDC takes the largest
PNG up to 64 pixels, otherwise the ICO, only over HTTPS from the feed's own
service (for example `*.wvd.microsoft.com`). It fetches them with the feed token,
follows no redirects, and accepts at most 256 KiB, 10 seconds and 256×256 pixels.
It decodes PNG or ICO only, judged by content, checking image headers before
decoding; it does not change Qt's process-wide image allocation limit. Icons are
optional: failures are only logged, without their address. A desktop's picture
from the feed is shown whole, in its own aspect ratio; apps without an icon get a
matching icon from the icon theme, desktops without one get artwork drawn by
KRDC. Titles are never read as markup, in tooltips or labels. Symbolic icons of
the window's controls that would barely show against the colour scheme (themes
not recoloured by the platform) are redrawn in the scheme's text colour, again
whenever the scheme changes.

The access token used to open a desktop is renewed once its lifetime minus five
minutes has passed by either the wall clock or a monotonic clock, or once the
wall clock goes back past the time it was received; setting the clock back does
not prolong it.

The refresh token is kept in the system keychain through QtKeychain (no
plain-text fallback). Only the account name is written to `krdcrc`
(`[AzureVirtualDesktop] Account=`). *Sign Out* deletes the keychain entry; a
keychain read still pending from before signing out or switching accounts is
ignored.

Network safety: the bearer token is only sent over HTTPS to `microsoft.com` /
`microsoftonline.com` hosts (label-boundary match, no user info in URLs);
redirects are followed by KRDC itself, at most 5 times and only to such hosts;
feed documents are limited to 4 MiB, connection files to 256 KiB, token
responses to 256 KiB; XML with a DTD is refused; tokens are not logged.

Connections opened from a file or a workspace are not added to the history or
to the sessions restored at startup, as the address alone cannot reproduce them.

### Signing in with the browser

FreeRDP knows two redirect URIs for the public client ID KRDC and FreeRDP use:
`https://login.microsoftonline.com/common/oauth2/nativeclient`, a blank page
meant for embedded browsers and terminal use, and
`ms-appx-web://Microsoft.AAD.BrokerPlugin/a85cf173-4192-42f8-81fa-777a763e6e2c`,
its default (`FreeRDP_GatewayAvdAccessTokenFormat`) whenever it does not read the
answer from a terminal. In a normal browser the `nativeclient` page shows a
phishing warning and then `/common/wrongplace`: it is not meant to be copied
from. KRDC therefore uses FreeRDP's `ms-appx-web` redirect for both the
workspace list and connections, and there is nothing to copy or paste. KRDC
adds no redirect of its own (no `localhost`).

1. *Open Sign-in Page in Browser* opens the sign-in page with your default
   browser through KIO, as `xdg-open` does on Plasma. If that fails, the reason
   (for example a browser program that does not exist) is shown in the dialog;
   KRDC does not try another browser behind your back. The address is also shown,
   to open it yourself.
2. You sign in. At the end the browser asks whether to open an `ms-appx-web:`
   link with another application; allow it.
3. The desktop starts `krdc-avd-signin-handler` with that link. It passes the
   link to the waiting KRDC sign-in and exits; the dialog closes on its own.

**One-time setup.** Only one application can open `ms-appx-web:` links for your
user account. The sign-in dialog shows which application opens them now and
offers *Set Up KRDC for Sign-in Links…*. Only after you confirm a dialog naming
the current handler (if any) and the helper's path does KRDC write
`~/.local/share/applications/org.kde.krdc.avd-signin-handler.desktop` (only
`MimeType=x-scheme-handler/ms-appx-web`, hidden from menus) and make it the
default in `~/.config/mimeapps.list`. If a desktop-specific setting such as
`kde-mimeapps.list` still wins, KRDC says so and puts both files back as they
were, byte for byte. Before changing anything KRDC copies both files; if one
exists but cannot be read, or is a symbolic link (which writing would replace),
it changes nothing and says so. If putting a file back fails, the error names
the files to check. When registering succeeds, `mimeapps.list` is written by
KDE's KConfig, as when choosing an application in System Settings. Nothing is registered at install time, at start-up or by the tests. A
build tree registers the helper next to its `krdc`; an installed KRDC the one in
`libexec`; switching between them shows the other KRDC helper and asks again.
Building the `krdc` target alone, as IDE run targets do, also builds the helper.
Paths with characters other than letters, digits and `./_-+` are not registered,
nor is a helper that others can write to, or that is in a folder that others
can rename or replace: every folder up to `/` (after resolving symbolic links)
must belong to root or you and must not be writable by group or others, unless
it has the sticky bit, like `/tmp`. This is checked again right before
registering. To undo it, delete the
`.desktop` file and its line in `mimeapps.list`, or pick another application for
`x-scheme-handler/ms-appx-web` in the file associations.

**Trust boundaries.**

- The helper takes exactly one argument, at most 16 KiB, and only for
  `ms-appx-web://Microsoft.AAD.BrokerPlugin/…` with exactly one well-formed
  `state`. It prints no part of the link, writes no files and keeps nothing.
- Each waiting sign-in listens on its own Unix socket in
  `$XDG_RUNTIME_DIR/krdc-avd/` (mode 0700; KRDC refuses a runtime directory or
  channel directory that is not owned by you and private, a symbolic link, or
  a missing `XDG_RUNTIME_DIR`; there is no fallback to `/tmp`). The socket name
  is 24 hex digits of SHA-256 over the sign-in's state, so the helper reaches the
  one sign-in that started the browser, and nothing in a link can choose
  another path. Both sides check that the other runs as the same user. The
  socket itself is created owner-only, whatever the umask.
- KRDC accepts only the exact redirect URI (scheme and host case-insensitive,
  the path with the client ID exactly),
  no user info, port or fragment, every query parameter at most once and
  unencoded keys, printable ASCII only (and spaces, as KIO decodes `%20` when
  `xdg-open` passes a link on Plasma), valid percent escapes in every value,
  including those KRDC ignores, its own state
  (compared in constant time), and either a code or an error. FreeRDP gets only
  `state=…&code=…` rebuilt from the checked answer, as its own parsing is loose.
- Any program running as you can find the socket without knowing the state,
  so each connection is handled on its own: a malformed, wrong-destination,
  oversized (16 KiB) or stalled (5 seconds per connection) one is refused and
  dropped, and the sign-in keeps waiting for the browser's link. At most 4
  connections are read at once; more are turned away immediately. The first
  valid answer finishes the sign-in. Finishing, canceling, closing the dialog
  and the 10-minute timeout remove the socket and drop all connections, so
  repeated, late and stale links reach nothing.
- Codes, states and links are not logged and not written to settings or
  history. The authority's error description is shown in the error dialog but
  not logged: the log names only a standard OAuth error code; the receive buffer and FreeRDP's request strings are wiped after use.

**What KRDC needs from FreeRDP.** The browser sign-in for connections relies on
these FreeRDP 3 client functions and settings (present in 3.32.2):
`freerdp_client_get_aad_url()` for `FREERDP_CLIENT_AAD_AUTH_REQUEST`,
`FREERDP_CLIENT_AAD_TOKEN_REQUEST` (scope, code, `req_cnf`),
`FREERDP_CLIENT_AAD_AVD_AUTH_REQUEST` and `FREERDP_CLIENT_AAD_AVD_TOKEN_REQUEST`
(code), which add state and PKCE; `freerdp_client_extract_aad_code()`;
`client_common_get_access_token()`; and `FreeRDP_UseCommonStdioCallbacks` set
to false while building the requests so that `FreeRDP_GatewayAvdAccessTokenFormat`
and `FreeRDP_GatewayAvdClientID` give the `ms-appx-web` redirect. FreeRDP's own
helper protocol (`aad_auth_helper_start()`,
`aad_auth_helper_get_access_token_v()`, `aad_auth_helper_stop()`, JSON-RPC
`hello`/`navigate` over pipes) is not needed. A FreeRDP-packaged
`freerdp-xdg-aad-helper` could replace KRDC's own handler only if it stopped
registering itself without consent, stopped deriving socket paths from the
state text, and stopped printing the sign-in URL.

**Browser not opening.** The report that *Open Sign-in Page in Browser* did
nothing could not be reproduced offline: in an isolated Plasma Wayland session
(nested headless KWin, private `HOME`/XDG and D-Bus) both Qt's former
`QDesktopServices::openUrl()` and KIO started a stand-in browser registered
like Klinker (`Exec=… %U`, no `DBusActivatable`) with an activation token. The
dialog now uses KIO's asynchronous launcher, shows *Opening your browser…*,
then either *Waiting for you to finish signing in…* or the launcher's error,
and never launches twice on its own. If the browser still does not appear,
the dialog says whether the launch itself succeeded, which tells apart a
launcher problem from one in the link handler.

## Limitations of this milestone

- Full desktops only; no RemoteApp, no `.rdpw`.
- No RDP Shortpath, Teams optimisation, extra virtual channels or sovereign clouds.
- Each Entra sign-in during a connection goes through the browser (its own
  session usually makes this quick). Signing in needs the one-time setup above.
- Disconnecting while an explicitly configured FreeRDP helper's sign-in window
  is open waits until that window is closed or the helper times out (FreeRDP's
  helper call cannot be interrupted). KRDC's own credential and sign-in dialogs
  are closed and the connection stopped at once.
- The gateway token from the workspace list is reused when retrying a tab; after
  about an hour it expires and the workspace must be opened again.
- Entra ID and conditional access decide which clients and devices may connect.
  KRDC reports refusals; it does not bypass them.
- The interactive sign-in and real AVD connections have not been tested against a
  live tenant as part of this work. In particular, that Entra ID accepts the
  `ms-appx-web` redirect for the workspace feed scope from a system browser, and
  how each browser asks before handing the link over, still need checking with a
  real account.

## Using a connection file safely

Connection files downloaded from a workspace or sent by an administrator can
contain host pool identifiers, tenant IDs and signatures. Keep them local: do
not attach them to bug reports, paste them into chats or upload them. To report
a problem, describe which properties are set (names and `:i:` values), not the
values of addresses and identifiers. KRDC does not save downloaded workspace
files to disk.

## Building

Needs Qt 6, KDE Frameworks 6, extra-cmake-modules, QtKeychain, and FreeRDP and
FreeRDP-Client 3.32.2 or later. CMake requires 3.32.2 because that is the
version tested (with Qt 6.12), not because earlier versions are known to fail.
Microsoft Entra sign-in and AVD need FreeRDP built with `WITH_AAD=ON`.

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
# optional: -DWITH_VNC=OFF when libvncclient is not installed,
#           -DECM_DIR=<prefix>/share/ECM/cmake for a private ECM
cmake --build build
```

## Offline tests

```sh
cd build
ctest -R '^krdc-' --output-on-failure
```

The tests use no network and no real accounts: HTTP is answered by an
in-process fake, fixtures are synthetic (`example.org`, zero GUIDs), and ctest
runs every test with `QT_QPA_PLATFORM=offscreen`, private `HOME`/XDG
directories, a private short `XDG_RUNTIME_DIR` under the build tree, no D-Bus
session and `BROWSER=/bin/false` (`cmake/modules/KrdcTestEnvironment.cmake`).
Tests that use desktop files also switch on Qt's test mode, so the user's
associations stay out of reach. The built `krdc-avd-signin-handler` is run the
way the desktop runs it; the only browser ever started is a shell-script
stand-in, and only after checking it is the registered handler. Run the tests
through ctest, not the binaries directly, to keep that isolation.

| Test | Covers |
|---|---|
| `krdc-connectionlaunchtest` | core launch type, default factory behaviour |
| `krdc-rdplaunchprofiletest` | encodings, IPv6, typed and colon values, AVD detection, signed-file-like AVD fixture, security floor, RemoteApp/`.rdpw` refusal, redirection filtering with CR/LF/mixed line endings, command-line records refused, gateway token only for trusted AVD gateways |
| `krdc-rdpcredentialstest` | host vs gateway credential routing, editable imported user name |
| `krdc-rdpaccesstokentest` | token dispatch: explicit helper only, browser sign-in with FreeRDP's own `ms-appx-web` redirect, scope and `req_cnf` kept, refusal of requests without state/S256 PKCE/that redirect/exactly FreeRDP's `client_id`, only the checked state and code passed to FreeRDP, FreeRDP's real builder and extractor (offline: `client_id`, redirect, S256 challenge matching the verifier in the token request, other state refused), Entra errors, log diagnostics without text from the answer (synthetic secrets), cancel, SIGPIPE from a dead helper, no AVD token for other gateways |
| `krdc-avdcallbacktest` | strict parsing of answers (wrong scheme/host/client/path, client ID case, user info, port, fragment, duplicates, encoded keys, control characters, broken escapes in any value, size; upper-case scheme accepted), owner-only channel directory and socket, the built handler delivering to the live sign-in only, other sign-ins unreachable, malformed, wrong-destination, oversized and stalled clients refused without ending the sign-in, at most 4 clients at once, replay, cancel (also with clients connected), timeout, nothing printed |
| `krdc-avdhandlerregistrationtest` | no implicit registration, registration on request only, reporting the current handler, another KRDC's helper, unusable helpers and paths, group/other-writable grandparent refused, sticky folder accepted, recheck before registering, unreadable files left untouched, byte-for-byte rollback, failed restores reported |
| `krdc-avdfeedtest` | feed parsing, DTD refusal, size limits, trusted hosts, redirects |
| `krdc-avdworkspacecachetest` | one-hour age at the boundary, no sliding, clock set back, account/cloud isolation, empty lists, sign-out removal, icons kept per listed resource, disk metadata without addresses or tokens and owner-only, restart, refusal of malformed/oversized/too long/shared files, icon metadata parsing and origins, bounded PNG/ICO decoding leaving Qt's global allocation limit alone, icons from disk only for listed resources and bounded in size, number and memory, a sign-out outdating earlier fetches, icon download without redirects or untrusted hosts |
| `krdc-avdworkspacedialogtest` | reopening within the hour without network or keychain, refetch after the hour, *Refresh* bypassing the cache, failed and partial updates keeping the last good list marked stale, empty lists cached, sign-out forgetting the account's list and a late feed answer, a sign-out in another window not undone by a late answer, fresh token before launching from a cached list, after expiry or after the clock was set back, launch after a restart through a fresh feed; cards: Devices/Apps, accessible names, keyboard activation, apps not opened with a "coming soon" tooltip, long titles, narrow and wide windows, scrolling, signed-out/empty/error states, search, server icons cached, a desktop's server picture drawn undistorted instead of the artwork, titles/account/search text never markup, control icons legible in light and dark schemes and after switching |
| `krdc-avdoauthtest` | PKCE request with the `ms-appx-web` redirect, answer/state checks, token requests and errors |
| `krdc-avdsignindialogtest` | consent before setup (naming the current handler, declining changes nothing), no paste field, one asynchronous browser launch and its errors, KIO launcher with a stand-in browser (success and missing program), answer from the built handler, cancel invalidating it, wrong destination refused while the sign-in goes on, sign-out during sign-in, main window closed during sign-in, answers of other sign-ins, workspace list, silent refresh, launch only on request, pending keychain reads, no launch through other gateways |
| `krdc-rdpuihandofftest` | cancellable requests from FreeRDP's thread: answered, canceled while queued or open, context destroyed |
| `krdc-rdpplugintest` | built plugin: file import to launch, a single workspace window, view start against a closed local port, closing while a credential request is queued or its dialog is open (local fake server, watchdog) |
