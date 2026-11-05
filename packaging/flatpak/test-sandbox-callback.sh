#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
#
# End-to-end, offline test of the AVD sign-in return path of the installed Flatpak:
#
#   1. installs a build (unsigned, test only) into a SCRATCH per-user installation;
#   2. checks the exported ms-appx-web desktop file and the app's permissions;
#   3. builds KRDC's own receiver (Avd::CallbackReceiver from rdp/avd of --source, with the tag's
#      packaging patches) with the app's SDK into a test-only probe, and runs it in one sandbox
#      of the app, waiting for a sign-in exactly as KRDC does;
#   4. runs the Exec line of the exported desktop file, as the desktop does for a link from the
#      browser, with a synthetic link; Flatpak starts the packaged helper in a NEW sandbox of the
#      app, which must reach the receiver. A link for another sign-in reaches nothing, and one
#      written straight to the receiver's socket is rejected while it keeps waiting.
#
# No browser, account, tenant, network or the user's own Flatpak installation, session bus, HOME
# or display is used (it re-runs itself with a private environment and D-Bus session):
#   FLATPAK_USER_DIR=<scratch> test-sandbox-callback.sh --repo DIR --source DIR [--tag TAG]
# The scratch installation needs the app's runtime and SDK (org.kde.Platform, org.kde.Sdk).
set -euo pipefail
if [[ -z "${KRDC_TEST_ISOLATED:-}" ]]; then
    h="$(mktemp -d /tmp/kt.XXXXXX)"; mkdir -m 700 "$h/run"; rc=0
    env -i PATH="$PATH" TMPDIR="$h" HOME="$h" XDG_RUNTIME_DIR="$h/run" LANG=C.UTF-8 KRDC_TEST_ISOLATED=1 \
        FLATPAK_USER_DIR="${FLATPAK_USER_DIR:-}" dbus-run-session -- bash "$0" "$@" || rc=$?
    rm -rf "$h"; exit "$rc"
fi
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$here/lib.sh"

repo="" source="" tag=""
while (($#)); do
    case "$1" in
        --repo) repo="$2"; shift 2 ;;
        --source) source="$2"; shift 2 ;;
        --tag) tag="$2"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[[ -d "$repo" && -f "$source/rdp/avd/avdcallback.cpp" ]] || die "usage: FLATPAK_USER_DIR=<scratch> $0 --repo DIR --source DIR [--tag TAG]"
[[ -n "${FLATPAK_USER_DIR:-}" && "$FLATPAK_USER_DIR" != */.local/share/flatpak ]] \
    || die "set FLATPAK_USER_DIR to a scratch installation; this test never uses your own"
repo="$(cd "$repo" && pwd -P)"
remote="krdc-sandbox-test"
work="$(mktemp -d)"
listener_pid=""
cleanup() {
    [[ -n "$listener_pid" ]] && kill "$listener_pid" 2>/dev/null || true
    flatpak --user uninstall -y --noninteractive "$APP_ID//$BRANCH" >/dev/null 2>&1 || true
    flatpak --user remote-delete --force "$remote" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT
pass=0
ok() { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }

# 1. Install (local, unsigned test repository; the published one is always signed).
flatpak --user remote-delete --force "$remote" >/dev/null 2>&1 || true
flatpak --user remote-add --no-gpg-verify "$remote" "file://$repo"
flatpak --user install -y --noninteractive "$remote" "$APP_ID//$BRANCH" >/dev/null
ok "installed $APP_ID from the build"

# 2. Exported desktop file and permissions.
handler="$FLATPAK_USER_DIR/exports/share/applications/$APP_ID.avd-signin-handler.desktop"
[[ -f "$handler" ]] || die "no exported $APP_ID.avd-signin-handler.desktop"
grep -qx 'MimeType=x-scheme-handler/ms-appx-web;' "$handler" || die "handler claims other types"
exec_line="$(sed -n 's/^Exec=//p' "$handler")"
[[ "$exec_line" == *"--command=krdc-avd-signin-handler"* && "$exec_line" == *" $APP_ID "* && "$exec_line" == *"%u"* ]] \
    || die "exported Exec does not start the packaged helper in $APP_ID: $exec_line"
[[ "$exec_line" != *"--filesystem"* && "$exec_line" != *"/app/"* ]] || die "exported Exec widens access or names sandbox paths: $exec_line"
ok "exported handler: MimeType ms-appx-web only, Exec=$exec_line"
[[ -f "$FLATPAK_USER_DIR/exports/share/applications/$APP_ID.desktop" ]] || die "main desktop file not exported"
! ls "$FLATPAK_USER_DIR/exports/share/applications/" | grep -q 'org\.kde\.krdc' || die "exports under org.kde.krdc"
ok "no exports under KDE's org.kde.krdc"
perms="$(flatpak --user info --show-permissions "$APP_ID//$BRANCH")"
if grep -Eq '^filesystems=.*(host|home)' <<<"$perms" || grep -q 'org.freedesktop.Flatpak=talk' <<<"$perms" \
    || grep -Eq '^sockets=.*(session-bus|system-bus)' <<<"$perms"; then
    die "app has host, home, whole-bus or host-command permissions: $perms"
fi
ok "no host/home filesystem, whole bus or host command permissions"
grep -q 'org.freedesktop.secrets=talk' <<<"$perms" && grep -q 'org.kde.kwalletd6=talk' <<<"$perms" \
    && ok "keychain reachable through org.freedesktop.secrets / org.kde.kwalletd6 only" || die "keychain bus names missing"

# 3. KRDC's receiver, built from the source with the app's SDK, waiting in sandbox A.
mkdir -p "$work/src/rdp"
cp -a "$source/rdp/avd" "$work/src/rdp/"
if ! grep -q KRDC_AVD_PACKAGED_HANDLER "$source/CMakeLists.txt"; then
    [[ -n "$tag" && -d "$here/patches/$tag" ]] || die "source predates Flatpak support: --tag with packaging patches needed"
    for p in "$here/patches/$tag"/*.patch; do
        (cd "$work/src" && git apply --include='rdp/avd/*' "$p") || die "$(basename "$p") does not apply"
    done
fi
sdk="$(flatpak --user info --show-sdk "$APP_ID//$BRANCH")"
flatpak --user run --command=sh --filesystem="$work" --filesystem="$here/receiver-probe:ro" "$sdk" -c \
    'cmake -S "$1" -B "$2" -G Ninja -DKRDC_SOURCE="$3" >/dev/null && cmake --build "$2" >/dev/null' \
    _ "$here/receiver-probe" "$work/probe" "$work/src" >"$work/probe-build.log" 2>&1 \
    || { cat "$work/probe-build.log" >&2; die "building the receiver probe with $sdk failed"; }
ok "KRDC's Avd::CallbackReceiver built with $sdk"

state="krdcSandboxTestState_$(od -An -N12 -tx1 /dev/urandom | tr -d ' \n')"
redirect="ms-appx-web://Microsoft.AAD.BrokerPlugin/a85cf173-4192-42f8-81fa-777a763e6e2c"
link="$redirect?code=synthetic-code&state=$state"
flatpak --user run --command="$work/probe/probe" --filesystem="$work:ro" "$APP_ID//$BRANCH" "$redirect" "$state" \
    >"$work/listener.out" 2>"$work/listener.err" &
listener_pid=$!
for _ in $(seq 1 100); do
    grep -q READY "$work/listener.out" 2>/dev/null && break
    kill -0 "$listener_pid" 2>/dev/null || { cat "$work/listener.out" "$work/listener.err" >&2; die "receiver failed"; }
    sleep 0.2
done
grep -q READY "$work/listener.out" || die "receiver did not start"
ok "KRDC's receiver waiting in sandbox A"

run_exec() { # link: run the exported Exec line as the desktop would (%u = the link, one argument)
    local -a argv
    read -r -a argv <<<"$exec_line"
    local i
    for i in "${!argv[@]}"; do
        [[ "${argv[$i]}" == "%u" ]] && argv[$i]="$1"
    done
    "${argv[@]}"
}
waiting() { ! grep -q 'RECEIVED\|FAILED' "$work/listener.out" && kill -0 "$listener_pid" 2>/dev/null; }

# 4a. A link for another sign-in reaches nothing.
other="${link/state=$state/state=krdcSandboxTestOther_0123456789abcdef}"
if run_exec "$other" >"$work/other.out" 2>&1; then
    die "helper accepted a link for a sign-in that does not exist"
fi
waiting || die "the other link reached the waiting sign-in"
! grep -q 'synthetic-code' "$work/other.out" || die "helper printed the link"
ok "link for another sign-in refused by the helper, receiver untouched, nothing printed"

# 4b. Another sign-in's link written straight to this receiver's socket, from sandbox B: rejected.
answer="$(flatpak --user run --command=python3 "$APP_ID//$BRANCH" -I -c '
import hashlib, os, socket, sys
d = os.path.join(os.environ["XDG_RUNTIME_DIR"], "app", os.environ["FLATPAK_ID"], "krdc-avd")
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(os.path.join(d, hashlib.sha256(b"krdc-avd-signin:" + sys.argv[1].encode()).hexdigest()[:24]))
s.sendall(sys.argv[2].encode() + b"\n")
print(s.recv(64).decode().strip())' "$state" "$other" 2>"$work/inject.err")" || true
[[ "$answer" == REJECTED ]] || die "receiver answered '$answer' to a wrong-state link"
waiting || die "a wrong-state link ended the wait: $(cat "$work/listener.out")"
ok "wrong-state link written to the receiver's socket: REJECTED, still waiting"

# 4c. The real round trip: exported handler in new sandbox B -> KRDC's receiver in sandbox A.
run_exec "$link" >"$work/helper.out" 2>&1 || { cat "$work/helper.out" >&2; die "helper failed"; }
wait "$listener_pid" || true
listener_pid=""
grep -qx 'RECEIVED synthetic-code' "$work/listener.out" || { cat "$work/listener.out" "$work/listener.err" >&2; die "link did not reach sandbox A"; }
! grep -q 'synthetic-code' "$work/helper.out" || die "helper printed the link"
ok "link from the exported handler (sandbox B) accepted by KRDC's receiver (sandbox A), code intact"

printf '\n%d checks passed\n' "$pass"
