# KRDC AVD Flatpak (fork only)

`io.github.manicminer.krdc`, shown as **KRDC AVD**: an unofficial fork of KDE's KRDC; not KDE's
`org.kde.krdc` and not on Flathub. Built here **unsigned**; signed and published only by the
shared repository [manicminer/flatpak](https://github.com/manicminer/flatpak) at
<https://manicminer.github.io/flatpak/> (remote name `manicminer`, branch `stable`, x86_64).

## Install / update

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --user --if-not-exists manicminer https://manicminer.github.io/flatpak/manicminer.flatpakrepo
flatpak install --user manicminer io.github.manicminer.krdc
flatpak update --user io.github.manicminer.krdc
# or: flatpak install --user https://manicminer.github.io/flatpak/io.github.manicminer.krdc.flatpakref
```

A `.flatpak` bundle is attached to each GitHub Release (`flatpak install --user <file>`); it sets
`manicminer` as origin, so updates come from the signed repository.

## Files

| File | Purpose |
|---|---|
| `io.github.manicminer.krdc.json` | manifest: KDE 6.11, FreeRDP 3.33.0 (`WITH_AAD=ON`, cJSON), QtKeychain 0.17.0, libssh, libvncserver, krb5, pkcs11-helper; SPICE off |
| `build.sh` | builds a source tree into an unsigned repo + `repo.tar` + `SOURCE.json` (+ bundle with `--bundle-gpg-key`) |
| `render-manifest.py`, `brand-metadata.py`, `source-json.py`, `lib.sh` | helpers (paths, fork identity, build record: source tag/commit, packaging commit and file digests, rendered manifest digest, runtime/SDK commits) |
| `test-sandbox-callback.sh`, `receiver-probe/` | installed-app test of the Entra sign-in return path across sandboxes with KRDC's own receiver built by the app's SDK (test only, not shipped; offline, synthetic) |
| `patches/<tag>/` | packaging patches for tags that predate Flatpak support (only `v26.11.70.r2459.g23ff7258.avd1`) |
| `ci-install-deps.sh` | CI tooling |
| `manicminer-flatpak.gpg` | **to add**: the repository's public key, from manicminer/flatpak's *Export public signing key* run |

## Sandbox design (sign-in return)

- `-DKRDC_AVD_PACKAGED_HANDLER=ON` installs `io.github.manicminer.krdc.avd-signin-handler.desktop`
  (`MimeType=x-scheme-handler/ms-appx-web` only, `Exec=krdc-avd-signin-handler %u`). Flatpak exports
  it at install time as `flatpak run --command=krdc-avd-signin-handler io.github.manicminer.krdc`,
  i.e. the packaged helper in a new sandbox of the same app. No host paths are registered; KRDC
  writes no `mimeapps.list` entries in the Flatpak (setup dialog reports it as set up).
- Inside Flatpak (`/.flatpak-info` and `FLATPAK_ID`), the channel lives in
  `$XDG_RUNTIME_DIR/app/$FLATPAK_ID/krdc-avd/` – the one runtime directory shared by all sandboxes of
  the app – with the same owner-only checks. Native builds are unchanged.
- The browser is the host's, via KIO (OpenURI portal in the sandbox). Keychain: only
  `org.freedesktop.secrets` and `org.kde.kwalletd6`. No `--filesystem=host/home`, no
  `org.freedesktop.Flatpak`.

## Local build and tests

```sh
flatpak --user install flathub org.flatpak.Builder org.kde.Sdk//6.11 org.kde.Platform//6.11
dbus-run-session -- packaging/flatpak/build.sh --source . --out /tmp/krdc-flatpak --run-tests
packaging/flatpak/test-sandbox-callback.sh --repo /tmp/krdc-flatpak/repo --source .
```

`build.sh` always uses org.flatpak.Builder from the default per-user installation, the same as CI
(CI does not use Ubuntu's flatpak-builder package, whose flatpak-builder and AppStream are older).
For a separate installation, point HOME at a private directory instead of setting FLATPAK_USER_DIR.
`out/BUILDER.json` records the Builder's ref, commit and versions. When a build fails,
`out/diagnostics/` gets an inventory of the build tree, its metadata files and a full
`appstreamcli compose` report, and CI uploads it as the `flatpak-build-diagnostics-x86_64` artifact.

`test-sandbox-callback.sh` re-runs itself with a scrubbed environment, private HOME, runtime
directory and `dbus-run-session`, and no display: nothing reaches the desktop session. Run builds
the same way (private HOME/D-Bus) when testing on a desktop: org.flatpak.Builder starts
`flatpak build` through the session bus.

## Releases

Tag a commit on master `v<ver>.r<n>.g<hash>.avd<k>` and push the tag: the workflow in the tagged
commit runs (so the tag must contain `.github/workflows/flatpak.yml`). It builds and attaches the
bundle to a release (needs `manicminer-flatpak.gpg` committed; otherwise the run fails and is not
published). Nothing is requested: manicminer/flatpak's scheduled *Publish* finds the successful run
and publishes it, normally within 15-30 minutes. The existing tag
`v26.11.70.r2459.g23ff7258.avd1` predates the workflow: run *Flatpak* by hand on master with
`source_tag` set; master's packaging and `patches/<tag>/` are used. Tags are never moved; a tag
whose source cannot be built must be superseded by a new tag.

The publisher does not rely on this workflow's "on master" check: it resolves master to its head
SHA itself and requires both the tag's commit and the packaging commit (the run's head) to be that
SHA or its ancestors, and records the SHA in the signed commit. Keep master protected (no force
push or deletion): rewriting it makes earlier builds unpublishable.
