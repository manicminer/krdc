#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
"""Give an installed KRDC tree the identity of this unofficial fork.

Run by the Flatpak manifest after `cmake --install` (and by the tests on a
fake tree). Installs the desktop file, metainfo and icons under the fork's
application ID only, so nothing claims to be, provide or replace KDE's
org.kde.krdc. Upstream translations of the name are dropped: they say "KRDC",
which is the official application.
"""
import argparse
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET

UPSTREAM_ID = "org.kde.krdc"
DISPLAY_NAME = "KRDC AVD"
SUMMARY = "Unofficial KRDC fork with Azure Virtual Desktop workspaces"
HOMEPAGE = "https://github.com/manicminer/krdc"
XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"
APP_ID_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*(\.[A-Za-z_][A-Za-z0-9_-]*){2,}$")
VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def fail(message):
    print(f"brand-metadata: {message}", file=sys.stderr)
    sys.exit(1)


def brand_desktop(source, target, app_id):
    lines = []
    with open(source, encoding="utf-8") as f:
        for line in f.read().splitlines():
            key = line.split("=", 1)[0]
            # Translated names and comments describe the official application.
            if re.match(r"^(Name|GenericName|Comment|Keywords)\[", key):
                continue
            if key == "Name":
                line = f"Name={DISPLAY_NAME}"
            elif key == "Comment":
                line = f"Comment={SUMMARY}"
            elif key == "Icon":
                line = f"Icon={app_id}"
            elif key == "StartupWMClass":
                line = f"StartupWMClass={app_id}"
            elif key == "X-DocPath":
                continue
            lines.append(line)
    if not any(l.startswith("Name=") for l in lines):
        fail(f"{source} has no Name")
    with open(target, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def brand_metainfo(source, target, app_id, version, date):
    tree = ET.parse(source)
    root = tree.getroot()

    def only(tag):
        found = root.findall(tag)
        if len(found) != 1:
            fail(f"{source}: expected one <{tag}>, found {len(found)}")
        return found[0]

    only("id").text = app_id
    for tag in ("name", "summary"):
        for element in root.findall(tag):
            if element.get(XML_LANG):
                root.remove(element)
    only("name").text = DISPLAY_NAME
    only("summary").text = SUMMARY
    only("launchable").text = f"{app_id}.desktop"
    developer = only("developer")
    developer.set("id", "io.github.manicminer")
    for child in list(developer):
        developer.remove(child)
    ET.SubElement(developer, "name").text = "manicminer (unofficial KRDC fork)"
    # Translated descriptions describe the official application; replace all of it.
    description = only("description")
    for child in list(description):
        description.remove(child)
    ET.SubElement(description, "p").text = (
        "KRDC AVD is an unofficial fork of KDE's KRDC remote desktop client that adds experimental "
        "Azure Virtual Desktop workspaces and Microsoft Entra ID sign-in. It is not made, endorsed or "
        "supported by KDE or Microsoft."
    )
    ET.SubElement(description, "p").text = (
        "It connects with RDP or VNC to other computers, and lists and opens Azure Virtual Desktop full desktops."
    )
    for tag in ("url", "project_group", "update_contact", "screenshots", "releases"):
        for element in root.findall(tag):
            root.remove(element)
    for kind, url in (("homepage", HOMEPAGE), ("bugtracker", HOMEPAGE + "/issues"), ("vcs-browser", HOMEPAGE)):
        ET.SubElement(root, "url", {"type": kind}).text = url
    releases = ET.SubElement(root, "releases")
    ET.SubElement(releases, "release", {"version": version, "date": date})
    # Never provide or replace the official application's ID.
    for provides in root.findall("provides"):
        for element in list(provides):
            if element.tag == "id":
                provides.remove(element)
    for tag in ("replaces",):
        for element in root.findall(tag):
            root.remove(element)
    ET.indent(tree)
    tree.write(target, encoding="utf-8", xml_declaration=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--date", required=True)
    args = parser.parse_args()
    if not APP_ID_RE.match(args.app_id) or args.app_id == UPSTREAM_ID:
        fail(f"invalid application ID {args.app_id!r}")
    if not VERSION_RE.match(args.version) or not DATE_RE.match(args.date):
        fail("invalid version or date")
    share = os.path.join(args.prefix, "share")

    apps = os.path.join(share, "applications")
    upstream_desktop = os.path.join(apps, f"{UPSTREAM_ID}.desktop")
    if not os.path.isfile(upstream_desktop):
        fail(f"missing {upstream_desktop}")
    brand_desktop(upstream_desktop, os.path.join(apps, f"{args.app_id}.desktop"), args.app_id)
    os.remove(upstream_desktop)

    metainfo_dir = os.path.join(share, "metainfo")
    upstream_meta = os.path.join(metainfo_dir, f"{UPSTREAM_ID}.appdata.xml")
    if not os.path.isfile(upstream_meta):
        fail(f"missing {upstream_meta}")
    brand_metainfo(upstream_meta, os.path.join(metainfo_dir, f"{args.app_id}.metainfo.xml"), args.app_id, args.version, args.date)
    os.remove(upstream_meta)

    # Icons: krdc.* -> <app id>.*; Flatpak exports only icons named after the application.
    icons = os.path.join(share, "icons", "hicolor")
    renamed = 0
    for directory, _, files in os.walk(icons):
        for name in files:
            stem, ext = os.path.splitext(name)
            if stem == "krdc" and os.path.basename(directory) == "apps":
                os.replace(os.path.join(directory, name), os.path.join(directory, args.app_id + ext))
                renamed += 1
    if renamed == 0:
        fail("no krdc application icon found")

    # Upstream's MIME definitions are only installed with SPICE; never export them under KDE's name.
    upstream_mime = os.path.join(share, "mime", "packages", f"{UPSTREAM_ID}-mime.xml")
    if os.path.exists(upstream_mime):
        shutil.move(upstream_mime, os.path.join(share, "mime", "packages", f"{args.app_id}-mime.xml"))


if __name__ == "__main__":
    main()
