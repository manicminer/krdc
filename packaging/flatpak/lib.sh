# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
# shellcheck shell=bash
# Shared by the packaging scripts; sourced, not run.

APP_ID="io.github.manicminer.krdc"
BRANCH="stable"
# The shared manicminer Flatpak repository (published by github.com/manicminer/flatpak).
REMOTE_NAME="manicminer"
REPO_URL="https://manicminer.github.io/flatpak/repo/"
RUNTIME_REPO="https://dl.flathub.org/repo/flathub.flatpakrepo"

die() {
    printf '%s: error: %s\n' "${0##*/}" "$*" >&2
    exit 1
}

# Release tags of the fork: v<upstream version>.r<count>.g<12 hex at most>.avd<n>, e.g.
# v26.11.70.r2459.g23ff7258.avd1. Strict, so a tag can be used in paths and messages as is.
validate_tag() {
    [[ "${#1}" -le 80 && "$1" =~ ^v[0-9]{1,4}(\.[0-9]{1,4}){2}\.r[0-9]{1,7}\.g[0-9a-f]{7,40}\.avd[0-9]{1,4}$ ]]
}

# The refs a build may contain.
allowed_ref() {
    local ref="$1" arch="$2"
    [[ "$ref" == "app/$APP_ID/$arch/$BRANCH" || "$ref" == "runtime/$APP_ID.Locale/$arch/$BRANCH" ]]
}

# The exported files of the app commit: only names Flatpak exports, and the sign-in handler.
check_exports() {
    local repo="$1" arch="$2" ref listing
    ref="app/$APP_ID/$arch/$BRANCH"
    listing="$(ostree --repo="$repo" ls -R "$ref" /export | awk '{print $NF}')"
    grep -qx "/export/share/applications/$APP_ID.desktop" <<<"$listing" || die "main desktop file not exported"
    grep -qx "/export/share/applications/$APP_ID.avd-signin-handler.desktop" <<<"$listing" || die "sign-in handler desktop file not exported"
    if grep -E '^/export/share/(applications|metainfo|icons/.*/apps)/[^/]+$' <<<"$listing" | grep -v "/$APP_ID[.-]" | grep -q .; then
        die "files exported under names other than $APP_ID"
    fi
    if grep -q 'org\.kde\.krdc' <<<"$listing"; then
        die "files exported under KDE's org.kde.krdc"
    fi
    local handler
    handler="$(ostree --repo="$repo" cat "$ref" "/export/share/applications/$APP_ID.avd-signin-handler.desktop")"
    grep -qx 'MimeType=x-scheme-handler/ms-appx-web;' <<<"$handler" || die "handler desktop file claims other types"
    # Flatpak rewrites Exec to `flatpak run --command=krdc-avd-signin-handler <app>` on installation.
    grep -qx 'Exec=krdc-avd-signin-handler %u' <<<"$handler" || die "handler desktop file does not run the packaged helper"
}
