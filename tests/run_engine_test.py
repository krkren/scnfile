#!/usr/bin/env python3
"""Runs tests/engine/startup.tjs in a Kirikiri Z engine with the scnfile plugin.

usage: run_engine_test.py --engine /path/to/krkrz --plugin build/plugins/scnfile/libscnfile.so
                          --scnseal build/scnseal

The engine only loads plugins from its own folder or the plugin/ folder next to it, so the test
runs a copy of it (hard links where it can) in a temporary folder with the plugin in plugin/,
headless (SDL's offscreen video driver). It runs three times: with the plugin as built (values
to files and back), then with a key written into the plugin by scnseal (files encrypted with
that key open, others don't), then without a key again (encrypted files don't open). It fails
when the engine exits with anything but 0 or reports a failure."""

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


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        raise RuntimeError("%s failed:\n%s" % (" ".join(cmd), p.stdout.decode("utf-8", "replace")))


def engine_pass(tmp, engine, mode, env, timeout):
    """Runs the test project once; returns (ok, the lines it reported)"""
    args = [os.path.join(tmp, os.path.basename(engine)), os.path.join(HERE, "engine"),
            "-datapath=" + os.path.join(tmp, "savedata") + "/", "-scnplugin=scnfile.dll", "-scnmode=" + mode]
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                           timeout=timeout, cwd=tmp)
    except subprocess.TimeoutExpired:
        return False, ["FAIL the engine didn't finish within %ss" % timeout]
    out = p.stdout.decode("utf-8", "replace")
    lines = [l.split("scntest: ", 1)[1] for l in out.splitlines() if "scntest: " in l]
    ok = p.returncode == 0 and lines and not any(l.startswith("FAIL") for l in lines)
    if not lines:
        lines = out.splitlines()[-40:]
    return ok, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine", required=True, help="the engine (krkrz)")
    ap.add_argument("--plugin", required=True, help="the plugin (libscnfile.so)")
    ap.add_argument("--scnseal", required=True, help="the scnseal tool")
    ap.add_argument("--timeout", type=float, default=120)
    opts = ap.parse_args()
    env = dict(os.environ)
    env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    engine = os.path.abspath(opts.engine)
    plugin = os.path.abspath(opts.plugin)
    scnseal = os.path.abspath(opts.scnseal)
    ok = True
    # next to the plugin, so the hard links stay on one file system
    with tempfile.TemporaryDirectory(prefix="scnfile-test-", dir=os.path.dirname(plugin)) as tmp:
        place(engine, os.path.join(tmp, os.path.basename(engine)))
        for lib in glob.glob(os.path.join(os.path.dirname(engine), "libSDL3.so*")) + \
                glob.glob(os.path.join(os.path.dirname(engine), "SDL3.dll")):
            place(lib, os.path.join(tmp, os.path.basename(lib)))
        installed = os.path.join(tmp, "plugin", os.path.basename(plugin))
        os.makedirs(os.path.dirname(installed))
        os.makedirs(os.path.join(tmp, "savedata"))
        save = os.path.join(tmp, "savedata")

        shutil.copy2(plugin, installed)
        passed, lines = engine_pass(tmp, engine, "plain", env, opts.timeout)
        print("plain:\n  " + "\n  ".join(lines))
        ok = ok and passed

        # encrypt what the first pass wrote, with two keys, and write key A into the plugin
        keys = os.path.join(tmp, "keys")
        os.makedirs(keys)
        run([scnseal, "keygen", os.path.join(keys, "a.key")])
        run([scnseal, "keygen", os.path.join(keys, "b.key")])
        for name in ("a", "b"):
            shutil.copy2(os.path.join(save, "sealtest.scn"), os.path.join(save, name + ".kscn"))
            run([scnseal, "seal", "-k", os.path.join(keys, name + ".key"), os.path.join(save, name + ".kscn")])
        run([scnseal, "setkey", "-k", os.path.join(keys, "a.key"), installed])

        passed, lines = engine_pass(tmp, engine, "sealed", env, opts.timeout)
        print("with a key:\n  " + "\n  ".join(lines))
        ok = ok and passed

        shutil.copy2(plugin, installed)
        passed, lines = engine_pass(tmp, engine, "nokey", env, opts.timeout)
        print("without a key:\n  " + "\n  ".join(lines))
        ok = ok and passed
    print("engine test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
