#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
#
# Builds the KRDC AVD Flatpak (io.github.manicminer.krdc) from a source tree into an UNSIGNED
# OSTree repository and packs it for the central manicminer Flatpak publisher, which alone signs.
# No secrets are used or accepted here: this step compiles arbitrary source code.
#
#   build.sh --source DIR --out DIR [--tag TAG] [--commit SHA] [--arch x86_64]
#            [--packaging-commit SHA] [--state-dir DIR] [--run-tests] [--bundle-gpg-key FILE]
#
# The packaging (this directory) may be newer than the source: a source tree that predates Flatpak
# support is only built with patches from patches/<tag>/ for exactly that tag; otherwise this fails
# and a new tag has to be made from a commit that has the support. Tags are never changed.
#
# Output in --out: repo/ (unsigned archive-z2 repo), repo.tar, SOURCE.json and, with
# --bundle-gpg-key, io.github.manicminer.krdc-<arch>.flatpak (update origin: the manicminer remote).
# BUILDER.json records the org.flatpak.Builder used. If the build fails, diagnostics/ gets the
# builder provenance, an inventory of the build tree, its metadata files and a full AppStream report.
#
# Builder: always the org.flatpak.Builder Flatpak from Flathub, in the default per-user installation,
# run through its own wrapper, like CI. Distribution flatpak-builder packages are not used: they bring
# a different flatpak-builder and AppStream. Run it in a private D-Bus session (dbus-run-session): the
# Builder reaches the host flatpak through that bus. The org.kde.Sdk and org.kde.Platform of the
# manifest must be installed in the same installation (see ci-install-deps.sh).
set -euo pipefail
umask 022

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=packaging/flatpak/lib.sh
source "$here/lib.sh"

source_dir="" out_dir="" tag="" commit="" arch="x86_64" state_dir="" run_tests=0 bundle_key="" packaging_commit=""
while (($#)); do
    case "$1" in
        --source) source_dir="$2"; shift 2 ;;
        --out) out_dir="$2"; shift 2 ;;
        --tag) tag="$2"; shift 2 ;;
        --commit) commit="$2"; shift 2 ;;
        --packaging-commit) packaging_commit="$2"; shift 2 ;;
        --arch) arch="$2"; shift 2 ;;
        --state-dir) state_dir="$2"; shift 2 ;;
        --run-tests) run_tests=1; shift ;;
        --bundle-gpg-key) bundle_key="$2"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[[ -n "$source_dir" && -n "$out_dir" ]] || die "usage: build.sh --source DIR --out DIR [--tag TAG] [--commit SHA]"
[[ "$arch" == "x86_64" ]] || die "only x86_64 is built so far"
source_dir="$(cd "$source_dir" && pwd -P)"
[[ -f "$source_dir/CMakeLists.txt" && -f "$source_dir/main.cpp" ]] || die "$source_dir is not a KRDC source tree"

if [[ -n "$tag" ]]; then
    validate_tag "$tag" || die "invalid release tag: $tag"
    version="${tag#v}"
else
    version="0.0.0.dev"
fi
if [[ -n "$commit" ]]; then
    [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || die "commit must be a full SHA-1"
fi
if git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
    head="$(git -C "$source_dir" rev-parse HEAD)"
    [[ -z "$commit" || "$commit" == "$head" ]] || die "source is at $head, not $commit"
    commit="$head"
    source_date="$(git -C "$source_dir" log -1 --format=%cs HEAD)"
else
    source_date="${SOURCE_DATE:-1970-01-01}"
fi
[[ "$source_date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || die "bad source date"
# The packaging revision (this directory's commit; in CI the run's commit), recorded with digests
# of every packaging file used so the publisher can check them against the repository.
packaging_root="$(cd "$here/../.." && pwd -P)"
if [[ -z "$packaging_commit" ]]; then
    packaging_commit="$(git -C "$packaging_root" rev-parse HEAD 2>/dev/null)" || die "--packaging-commit is required outside a git checkout"
fi
[[ "$packaging_commit" =~ ^[0-9a-f]{40}$ ]] || die "packaging commit must be a full SHA-1"

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd -P)"
work="$out_dir/work"
rm -rf "$work" "$out_dir/repo" "$out_dir/repo.tar" "$out_dir/SOURCE.json" "$out_dir/BUILDER.json" "$out_dir/diagnostics" "$out_dir"/*.flatpak
mkdir -p "$work"
state_dir="${state_dir:-$out_dir/.flatpak-builder}"

# Packaging patches: only when the source lacks Flatpak support, only for an exact tag.
patches=()
if ! grep -q 'KRDC_AVD_PACKAGED_HANDLER' "$source_dir/CMakeLists.txt"; then
    [[ -n "$tag" ]] || die "source predates Flatpak support and no --tag selects packaging patches"
    patch_dir="$here/patches/$tag"
    [[ -d "$patch_dir" ]] || die "source of $tag predates Flatpak support and packaging has no patches for it; tag a newer commit of master instead"
    while IFS= read -r -d '' p; do patches+=("$p"); done < <(find "$patch_dir" -maxdepth 1 -type f -name '*.patch' -print0 | sort -z)
    ((${#patches[@]})) || die "no patches in $patch_dir"
    for p in "${patches[@]}"; do
        (cd "$source_dir" && git apply --check "$p") || die "$(basename "$p") does not apply to the source of $tag"
    done
fi

# Stage everything the manifest uses inside its directory: sandboxed builds read nothing outside.
mkdir -p "$work/src" "$work/packaging" "$work/patches"
if git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
    # Tracked and untracked-but-not-ignored files as they are on disk: no build trees, no IDE state.
    git -C "$source_dir" ls-files -z --cached --others --exclude-standard \
        | (cd "$source_dir" && tar --null --no-recursion -T - -cf -) | tar -C "$work/src" -xf -
else
    tar -C "$source_dir" --exclude=./.git --exclude='./build*' --exclude='./cmake-build*' --exclude=./.idea -cf - . | tar -C "$work/src" -xf -
fi
cp "$here/$APP_ID.json" "$here/brand-metadata.py" "$work/packaging/"
staged_patches=()
for p in ${patches[@]+"${patches[@]}"}; do
    cp "$p" "$work/patches/"
    staged_patches+=("--patch=$work/patches/$(basename "$p")")
done

manifest="$work/$APP_ID.json"
python3 -I "$here/render-manifest.py" --manifest-dir "$work" \
    --template "$work/packaging/$APP_ID.json" --packaging "$work/packaging" --source "$work/src" \
    --version "$version" --date "$source_date" --run-tests "$run_tests" \
    ${staged_patches[@]+"${staged_patches[@]}"} > "$manifest"

# The Builder's wrapper uses the default per-user installation (and the host flatpak binary).
[[ -z "${FLATPAK_USER_DIR:-}" || "${FLATPAK_USER_DIR%/}" == "${XDG_DATA_HOME:-$HOME/.local/share}/flatpak" ]] \
    || die "org.flatpak.Builder uses the default per-user installation; unset FLATPAK_USER_DIR"
[[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]] || die "no D-Bus session: run this under dbus-run-session"
flatpak --user info org.flatpak.Builder >/dev/null 2>&1 || die "org.flatpak.Builder is not installed (flatpak --user install flathub org.flatpak.Builder)"
builder=(flatpak run --user org.flatpak.Builder)
in_builder() { local cmd="$1"; shift; flatpak run --user --command="$cmd" org.flatpak.Builder "$@"; }

# Which Builder (and so which flatpak-builder and AppStream) made this build.
python3 -I -c '
import json, sys
keys = ("ref", "commit", "version", "flatpak_builder", "appstreamcli")
json.dump(dict(zip(keys, (v.strip() for v in sys.argv[1:]))), sys.stdout, indent=2, sort_keys=True)
print()' \
    "$(flatpak --user info --show-ref org.flatpak.Builder)" \
    "$(flatpak --user info --show-commit org.flatpak.Builder)" \
    "$(flatpak --user info org.flatpak.Builder | sed -n 's/^ *Version: //p')" \
    "$(in_builder flatpak-builder --version)" \
    "$(in_builder appstreamcli --version)" > "$out_dir/BUILDER.json"
echo "builder:"; cat "$out_dir/BUILDER.json"

# On failure: what the build tree held and what AppStream says about it (no secrets exist here).
collect_diagnostics() {
    local diag="$out_dir/diagnostics" files="$work/build/files"
    mkdir -p "$diag"
    cp "$out_dir/BUILDER.json" "$manifest" "$diag/"
    [[ -d "$files" ]] || { echo "no build tree at $files" > "$diag/inventory.txt"; return 0; }
    # type, mode, size, path and (for symlinks) target
    (cd "$files" && find . -printf '%y %M %s %p\t%l\n' | LC_ALL=C sort -k4) > "$diag/inventory.txt" || true
    local meta=() d
    for d in share/metainfo share/appdata share/applications share/icons; do [[ -e "$files/$d" ]] && meta+=("$d"); done
    ((${#meta[@]})) && { tar -C "$files" -cf "$diag/metadata.tar" "${meta[@]}" || true; }
    # Same AppStream and arguments flatpak-builder uses, into a scratch result (the tree is left alone).
    in_builder appstreamcli compose --print-report=full --prefix=/ --origin="$APP_ID" \
        --result-root="$diag/appstream" --data-dir="$diag/appstream/xmls" --icons-dir="$diag/appstream/icons" \
        --components="$APP_ID,$APP_ID.desktop" "$files" > "$diag/appstream-report.txt" 2>&1 || true
    echo "build failed; diagnostics in $diag" >&2
}

mkdir -p "$state_dir"
# --sandbox: the manifest cannot grant itself network or host access during the build.
rc=0
"${builder[@]}" --user --sandbox --force-clean --disable-rofiles-fuse \
    --arch="$arch" --default-branch="$BRANCH" \
    --state-dir="$state_dir" --repo="$out_dir/repo" \
    --subject="$APP_ID $version (${commit:-unversioned})" \
    "$work/build" "$manifest" || rc=$?
if ((rc)); then
    collect_diagnostics
    exit "$rc"
fi

# The publisher regenerates appstream data for the whole repository; drop the build's own.
while IFS= read -r ref; do
    ostree --repo="$out_dir/repo" refs --delete "$ref"
done < <(ostree --repo="$out_dir/repo" refs | grep -E '^appstream2?/' || true)
ostree --repo="$out_dir/repo" prune --refs-only >/dev/null
# Only the refs the publisher accepts; anything else means the manifest changed shape.
mapfile -t refs < <(ostree --repo="$out_dir/repo" refs | sort)
for ref in "${refs[@]}"; do
    allowed_ref "$ref" "$arch" || die "unexpected ref in build repo: $ref"
done
printf '%s\n' "${refs[@]}" | grep -qx "app/$APP_ID/$arch/$BRANCH" || die "app ref missing"

check_exports "$out_dir/repo" "$arch"

tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@0" -C "$out_dir" -cf "$out_dir/repo.tar" repo
pkg_files=()
for f in "$APP_ID.json" brand-metadata.py render-manifest.py build.sh lib.sh source-json.py; do
    pkg_files+=(--packaging-file "packaging/flatpak/$f")
done
for p in ${patches[@]+"${patches[@]}"}; do
    pkg_files+=(--packaging-file "packaging/flatpak/patches/$tag/$(basename "$p")")
done
python3 -I "$here/source-json.py" --repo "$out_dir/repo" --tar "$out_dir/repo.tar" --app-id "$APP_ID" \
    --arch "$arch" --branch "$BRANCH" --tag "$tag" --commit "$commit" --version "$version" \
    --packaging-root "$packaging_root" --packaging-commit "$packaging_commit" --rendered-manifest "$manifest" \
    "${pkg_files[@]}" > "$out_dir/SOURCE.json"

if [[ -n "$bundle_key" ]]; then
    [[ -s "$bundle_key" ]] || die "bundle key $bundle_key is missing"
    flatpak build-bundle --arch="$arch" \
        --repo-url="$REPO_URL" --runtime-repo="$RUNTIME_REPO" --gpg-keys="$bundle_key" \
        "$out_dir/repo" "$out_dir/$APP_ID-$arch.flatpak" "$APP_ID" "$BRANCH"
fi
rm -rf "$work"
echo "built $APP_ID $version for $arch into $out_dir"
