#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
"""Renders the build manifest: the template with absolute paths to the source tree to build
(which may be another checkout than the packaging), packaging patches, version and tests."""
import argparse
import json
import os
import re
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--template", required=True)
    p.add_argument("--packaging", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--date", required=True)
    p.add_argument("--run-tests", choices=["0", "1"], default="0")
    p.add_argument("--patch", action="append", default=[])
    p.add_argument("--manifest-dir", required=True, help="sources are referenced relative to it (sandboxed builds need them inside)")
    a = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}", a.version) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", a.date):
        sys.exit("render-manifest: bad version or date")
    with open(a.template, encoding="utf-8") as f:
        m = json.load(f)
    base = os.path.realpath(a.manifest_dir)

    def rel(path):
        path = os.path.realpath(path)
        if os.path.commonpath([base, path]) != base:
            sys.exit(f"render-manifest: {path} is not inside {base}")
        return os.path.relpath(path, base)

    source = os.path.realpath(a.source)
    packaging = os.path.realpath(a.packaging)

    def module(name):
        found = [x for x in m["modules"] if x.get("name") == name]
        if len(found) != 1:
            sys.exit(f"render-manifest: module {name} not found once")
        return found[0]

    krdc = module("krdc")
    srcs = []
    for s in krdc["sources"]:
        s = dict(s)
        if s["type"] == "dir":
            s["path"] = rel(source)
            # Local build trees and IDE state of a developer checkout are never sources.
            s["skip"] = sorted(set(s.get("skip", [])) | {
                e for e in os.listdir(source)
                if e in (".git", ".idea", ".cache", ".vscode") or e.startswith(("build", "cmake-build"))
            })
        elif s["type"] == "file":
            s["path"] = rel(os.path.join(packaging, s["path"]))
        srcs.append(s)
    for patch in a.patch:
        srcs.append({"type": "patch", "path": rel(patch)})
    krdc["sources"] = srcs
    krdc["post-install"] = [
        'python3 -I "${FLATPAK_BUILDER_BUILDDIR}/brand-metadata.py" --prefix "${FLATPAK_DEST}" '
        f'--app-id "${{FLATPAK_ID}}" --version "{a.version}" --date "{a.date}"'
    ]
    if a.run_tests == "1":
        krdc["config-opts"] = [o for o in krdc["config-opts"] if not o.startswith("-DBUILD_TESTING=")] + ["-DBUILD_TESTING=ON"]
        krdc["run-tests"] = True
        # Offline unit tests only; the test environment isolates HOME, XDG and D-Bus itself.
        krdc["test-rule"] = ""
        # Excluded, as they depend on the build sandbox rather than on KRDC: krdc-rdpaccesstokentest
        # (FreeRDP fetches public Entra OpenID metadata while preparing AVD requests; there is no
        # network here), krdc-krdcstartuptest (LD_LIBRARY_PATH=/app/lib loads the installed core
        # library instead of the build tree's), krdc-avdworkspacedialogtest (icon theme, style), and
        # krdc-avdhandlerregistrationtest/krdc-avdsignindialogtest (they run after installation, so the
        # package's own ms-appx-web desktop file in /app is visible, and KIO uses the absent portal).
        # All of them pass in a native build (ctest), which CI runs as well.
        krdc["test-commands"] = ["ctest --output-on-failure -R '^krdc-' -E '^krdc-(rdpaccesstokentest|krdcstartuptest|avdworkspacedialogtest|avdhandlerregistrationtest|avdsignindialogtest)$'"]
    cnf = module("legacy-openssl-cnf")
    cnf["sources"] = [{"type": "file", "path": rel(os.path.join(source, "legacy-openssl.cnf"))}]
    json.dump(m, sys.stdout, indent=4)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
