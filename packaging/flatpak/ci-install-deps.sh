#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
#
# CI only (GitHub's Ubuntu runners): flatpak, flatpak-builder and the KDE SDK of the manifest,
# per user. Locally, install org.flatpak.Builder and org.kde.Sdk/Platform yourself (README.md).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runtime_version="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime-version"])' "$here/io.github.manicminer.krdc.json")"
sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends flatpak flatpak-builder ostree dbus gnupg
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y --noninteractive flathub "org.kde.Sdk//$runtime_version" "org.kde.Platform//$runtime_version"
