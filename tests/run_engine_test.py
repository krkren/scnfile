#!/usr/bin/env python3
"""Runs tests/engine/startup.tjs in a Kirikiri Z engine with the scnfile plugin.

usage: run_engine_test.py --engine /path/to/krkrz --plugin build/plugins/scnfile/libscnfile.so

The engine only loads plugins from its own folder or the plugin/ folder next to it, so the test
runs a copy of it (hard links where it can) in a temporary folder with the plugin in plugin/,
headless (SDL's offscreen video driver). It fails when the engine exits with anything but 0
or reports a failure."""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def place(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine", required=True, help="the engine (krkrz)")
    ap.add_argument("--plugin", required=True, help="the plugin (libscnfile.so)")
    ap.add_argument("--timeout", type=float, default=120)
    opts = ap.parse_args()
    env = dict(os.environ)
    env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    engine = os.path.abspath(opts.engine)
    plugin = os.path.abspath(opts.plugin)
    # next to the plugin, so the hard links stay on one file system
    with tempfile.TemporaryDirectory(prefix="scnfile-test-", dir=os.path.dirname(plugin)) as tmp:
        place(engine, os.path.join(tmp, os.path.basename(engine)))
        for lib in glob.glob(os.path.join(os.path.dirname(engine), "libSDL3.so*")) + \
                glob.glob(os.path.join(os.path.dirname(engine), "SDL3.dll")):
            place(lib, os.path.join(tmp, os.path.basename(lib)))
        os.makedirs(os.path.join(tmp, "plugin"))
        shutil.copy2(plugin, os.path.join(tmp, "plugin", os.path.basename(plugin)))
        os.makedirs(os.path.join(tmp, "savedata"))
        args = [os.path.join(tmp, os.path.basename(engine)), os.path.join(HERE, "engine"),
                "-datapath=" + os.path.join(tmp, "savedata") + "/", "-scnplugin=scnfile.dll"]
        try:
            p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                               timeout=opts.timeout, cwd=tmp)
        except subprocess.TimeoutExpired:
            print("FAIL the engine didn't finish within %ss" % opts.timeout)
            return 1
    out = p.stdout.decode("utf-8", "replace")
    lines = [l.split("scntest: ", 1)[1] for l in out.splitlines() if "scntest: " in l]
    for l in lines:
        print(l)
    if p.returncode != 0 or not lines or any(l.startswith("FAIL") for l in lines):
        if not lines:
            print("\n".join(out.splitlines()[-40:]))
        print("engine test: FAILED (exit code %s)" % p.returncode)
        return 1
    print("engine test: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
