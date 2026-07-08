#!/usr/bin/env python3
"""Generate flurry.unistore for Universal-Updater.

Produces two store entries from the same install logic:
  - "Flurry"           -> tracks the latest STABLE release (includePrereleases: false)
  - "Flurry (Nightly)" -> tracks the latest release of any kind (includePrereleases: true)

The version strings are what Universal-Updater displays and diffs against the
user's installed version to detect updates, so CI re-runs this on every release
to stamp the current tags. downloadRelease always fetches by asset filename, so
the CIA names must stay stable across releases.
"""

import argparse
import json

REPO = "JohnRamberger/Flurry"
RAW_URL = "https://raw.githubusercontent.com/JohnRamberger/Flurry/main/flurry.unistore"


def _download(fname, out, msg, prereleases):
    return {
        "type": "downloadRelease",
        "repo": REPO,
        "file": fname,
        "output": out,
        "includePrereleases": prereleases,
        "message": msg,
    }


def _install(path, msg):
    return {"type": "installCia", "file": path, "message": msg}


def _delete(path):
    return {"type": "deleteFile", "file": path, "message": "Cleaning up..."}


def _entry(title, description, version, prereleases):
    core = [
        _download("Flurry.cia", "sdmc:/Flurry.cia", "Downloading Flurry...", prereleases),
        _install("sdmc:/Flurry.cia", "Installing Flurry..."),
        _delete("sdmc:/Flurry.cia"),
        _download("FlurryLoad.cia", "sdmc:/FlurryLoad.cia", "Downloading Flurry Loader...", prereleases),
        _install("sdmc:/FlurryLoad.cia", "Installing Flurry Loader..."),
        _delete("sdmc:/FlurryLoad.cia"),
    ]
    himem = [
        _download("FlurryLoad_HIMEM.cia", "sdmc:/FlurryLoad_HIMEM.cia", "Downloading Flurry HIMEM Loader...", prereleases),
        _install("sdmc:/FlurryLoad_HIMEM.cia", "Installing Flurry HIMEM Loader..."),
        _delete("sdmc:/FlurryLoad_HIMEM.cia"),
    ]
    return {
        "info": {
            "title": title,
            "author": "JohnRamberger",
            "description": description,
            "category": ["utility"],
            "console": ["3DS"],
            "icon_index": 0,
            "version": version,
        },
        "Install Flurry + Loader": core,
        "Install HIMEM Loader (Old 3DS only)": himem,
    }


def build(stable, nightly, revision):
    return {
        "storeInfo": {
            "title": "Flurry",
            "author": "JohnRamberger",
            "description": "3DS screen streaming over WiFi, built for Chokistream.",
            "url": RAW_URL,
            "file": "flurry.unistore",
            "revision": revision,
            "version": 3,
        },
        "storeContent": [
            _entry(
                "Flurry",
                "The Flurry streaming system module plus its loader (stable release). "
                "Install both, then launch Flurry Loader from the Home Menu.",
                stable,
                False,
            ),
            _entry(
                "Flurry (Nightly)",
                "Bleeding-edge prerelease builds from the latest commit on main, for testers. "
                "Install both, then launch Flurry Loader from the Home Menu.",
                nightly,
                True,
            ),
        ],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stable", required=True, help="latest stable release tag")
    ap.add_argument("--nightly", required=True, help="latest release tag of any kind")
    ap.add_argument("--revision", type=int, required=True, help="store revision (monotonic)")
    ap.add_argument("--out", default="flurry.unistore")
    a = ap.parse_args()

    with open(a.out, "w") as f:
        json.dump(build(a.stable, a.nightly, a.revision), f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
