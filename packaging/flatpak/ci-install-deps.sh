#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
#
# CI only (GitHub's Ubuntu runners): flatpak, and per user from Flathub org.flatpak.Builder and the
# KDE SDK/Platform of the manifest: the same Builder (flatpak-builder + AppStream) as local builds,
# not Ubuntu's older flatpak-builder package. Locally, install them yourself (README.md).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runtime_version="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime-version"])' "$here/io.github.manicminer.krdc.json")"
sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends flatpak ostree dbus gnupg
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y --noninteractive flathub org.flatpak.Builder//stable \
    "org.kde.Sdk//$runtime_version" "org.kde.Platform//$runtime_version"
flatpak --user info org.flatpak.Builder
