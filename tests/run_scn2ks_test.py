#!/usr/bin/env python3
"""Round trips of scn2ks through KAGEX's compiler: .ks -> .scn.json -> scn2ks -> .ks -> .scn.json.

usage: run_scn2ks_test.py --engine /path/to/krkrz --scn2ks build/scn2ks --kagex /path/to/kagex
                          [--plugin build/plugins/scnfile/libscnfile.so] [--keep DIR]

It runs KAGEX's sample game (the kagex checkout's data/ and template/) in the engine, adds the
scripts in tests/scn2ks/ to its !scnlist.txt, and compiles them all to JSON (-scncompile=all,
format=json in parseroption.txt). Each compiled file is turned back into a script with scn2ks,
the scripts are replaced by those, and they're compiled again. The two compiles must hold the
same scenes: the same texts, steps, save points, states and choices. Line numbers, the source
key (hash) and the macro name in *mp are left out of the comparison: a script written back
can't keep them. scn2ks must also report nothing it couldn't write exactly.

When the sample is in more than one language (KAGEX's doc/scn.txt, "Languages"), each script's
translations (data/lang/<code>/scenario/) are written back from the first compile too
(scn2ks --lang=<code>) and put in place of the sample's, and a third compile must hold the same
scenes again.

With --plugin (the scnfile plugin), the scripts are compiled once more to .scn, and scn2ks must
write the same scripts from those as from the JSON."""

import argparse
import glob
import json
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


def compile_all(game, engine, timeout, fmt):
    """Compiles every listed script; returns the compiler's summary and warnings"""
    scenario = os.path.join(game, "data", "scenario")
    for f in glob.glob(os.path.join(scenario, "*.scn*")):
        os.remove(f)
    option = os.path.join(scenario, "parseroption.txt")
    if fmt == "json":
        with open(option, "w", encoding="utf-8") as f:
            f.write("format=json\n")
    elif os.path.exists(option):
        os.remove(option)
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
    save = os.path.join(game, "savedata")
    shutil.rmtree(save, ignore_errors=True)
    os.makedirs(save)
    p = subprocess.run([os.path.join(game, os.path.basename(engine)), "-datapath=savedata/", "-scncompile=all"],
                       cwd=game, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    out = p.stdout.decode("utf-8", "replace")
    summary = [l for l in out.splitlines() if "Compiled scenarios:" in l]
    warnings = [l.split("scn: warning: ", 1)[1] for l in out.splitlines() if "scn: warning: " in l]
    if p.returncode != 0 or not summary:
        raise RuntimeError("compiling failed (exit %d):\n%s" % (p.returncode, "\n".join(out.splitlines()[-40:])))
    return summary[0].split("Compiled scenarios:", 1)[1].strip(), warnings


def strip_parens(e):
    while len(e) >= 2 and e[0] == "(" and e[-1] == ")":
        depth = 0
        for i, c in enumerate(e):
            depth += c == "("
            depth -= c == ")"
            if depth == 0 and i < len(e) - 1:
                return e
        e = e[1:-1]
    return e


def normal(root):
    """The tree without what a script written back can't keep"""
    root = dict(root)
    root.pop("hash", None)
    scenes = []
    for s in root.get("scenes", []):
        s = dict(s)
        for k in ("firstLine", "startLine", "lastLine"):
            s.pop(k, None)
        lines = []
        for item in s.get("lines", []):
            if isinstance(item, list) and item and isinstance(item[0], int) and not isinstance(item[0], bool):
                item = item[:4] + item[5:]  # a save point's line
            elif isinstance(item, list) and item and item[0] == "*script":
                item = item[:4]
            if isinstance(item, list):
                item = [({k: v for k, v in x.items() if k != "tagname"} if isinstance(x, dict) and i > 0 and item[i - 1] == "*mp" else x)
                        for i, x in enumerate(item)]
            lines.append(item)
        s["lines"] = lines
        if "nexts" in s:
            s["nexts"] = [dict(n, eval=strip_parens(n["eval"])) if isinstance(n.get("eval"), str) else n for n in s["nexts"]]
        scenes.append(s)
    root["scenes"] = scenes
    return root


def diff(a, b, path="", out=None, limit=8):
    out = [] if out is None else out
    if len(out) >= limit:
        return out
    if type(a) != type(b):
        out.append("%s: %r != %r" % (path, a, b))
    elif isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append("%s.%s: only in %s" % (path, k, "the original" if k in a else "the round trip"))
            else:
                diff(a[k], b[k], path + "." + k, out, limit)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append("%s: %d items != %d" % (path, len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, "%s[%d]" % (path, i), out, limit)
            if len(out) >= limit:
                break
    elif a != b:
        out.append("%s: %r != %r" % (path, a, b))
    return out


def scn2ks(tool, src, dst, lang=None):
    """Runs scn2ks (in a language: a translation); returns its report and whether it wrote the
    script exactly"""
    p = subprocess.run([tool] + (["--lang=" + lang] if lang else []) + [src, dst], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report = p.stdout.decode("utf-8", "replace")
    return report, p.returncode == 0 and "don't compile back exactly" not in report


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine", required=True, help="the engine (krkrz)")
    ap.add_argument("--scn2ks", required=True, help="the scn2ks tool")
    ap.add_argument("--kagex", required=True, help="a krkren/kagex checkout (its sample game is compiled)")
    ap.add_argument("--plugin", help="the scnfile plugin (libscnfile.so), to check .scn input as well")
    ap.add_argument("--keep", help="build the game in this folder and leave it there")
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()

    tmp = args.keep or tempfile.mkdtemp(prefix="scn2ks-test-")
    if args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
    try:
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        for d in ("data", "template"):
            shutil.copytree(os.path.join(args.kagex, d), os.path.join(game, d))
        shutil.copy2(os.path.join(args.kagex, "krkrz.cf"), game)
        engine_dir = os.path.dirname(os.path.abspath(args.engine))
        place(args.engine, os.path.join(game, os.path.basename(args.engine)))
        for f in glob.glob(os.path.join(engine_dir, "libSDL3.so*")):
            place(f, os.path.join(game, os.path.basename(f)))
        if os.path.isdir(os.path.join(engine_dir, "plugin")):
            shutil.copytree(os.path.join(engine_dir, "plugin"), os.path.join(game, "plugin"), symlinks=True)
        os.makedirs(os.path.join(game, "plugin"), exist_ok=True)
        if args.plugin:
            shutil.copy2(args.plugin, os.path.join(game, "plugin", "libscnfile.so"))

        scenario = os.path.join(game, "data", "scenario")
        listed = [l.strip() for l in open(os.path.join(scenario, "!scnlist.txt"), encoding="utf-8-sig")
                  if l.strip() and not l.startswith("#")]
        for ks in sorted(glob.glob(os.path.join(HERE, "scn2ks", "*.ks"))):
            shutil.copy2(ks, scenario)
            listed.append(os.path.basename(ks))
        with open(os.path.join(scenario, "!scnlist.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(listed) + "\n")
        sources = os.path.join(tmp, "sources")
        os.makedirs(sources)
        for name in listed:
            shutil.copy2(os.path.join(scenario, name), sources)

        failures = 0
        summary, warnings = compile_all(game, args.engine, args.timeout, "json")
        print("first compile:", summary)
        for w in warnings:
            print("  compiler warning:", w)
        first, written = os.path.join(tmp, "first"), os.path.join(tmp, "written")
        os.makedirs(first)
        os.makedirs(written)
        originals = {}
        for name in listed:
            path = os.path.join(scenario, name + ".scn.json")
            if not os.path.exists(path):
                raise RuntimeError("%s wasn't compiled" % name)
            shutil.copy2(path, first)
            with open(path, encoding="utf-8-sig") as f:
                originals[name] = normal(json.load(f))
            report, exact = scn2ks(args.scn2ks, os.path.join(first, name + ".scn.json"), os.path.join(written, name))
            if not exact:
                failures += 1
                print("FAIL %s: scn2ks says:\n%s" % (name, report))
            shutil.copy2(os.path.join(written, name), os.path.join(scenario, name))

        summary, warnings2 = compile_all(game, args.engine, args.timeout, "json")
        print("second compile:", summary)
        for w in warnings2:
            print("  compiler warning:", w)
        if len(warnings2) != len(warnings):
            failures += 1
            print("FAIL the scripts written back compile with %d warnings, the originals with %d" % (len(warnings2), len(warnings)))
        for name in listed:
            path = os.path.join(scenario, name + ".scn.json")
            if not os.path.exists(path):
                failures += 1
                print("FAIL %s: the script written back didn't compile" % name)
                continue
            with open(path, encoding="utf-8-sig") as f:
                again = normal(json.load(f))
            d = diff(originals[name], again)
            if d:
                failures += 1
                print("FAIL %s: the round trip differs:\n  %s" % (name, "\n  ".join(d)))
            else:
                n = sum(len(s.get("texts", [])) for s in again["scenes"])
                print("ok   %s (%d scenes, %d texts)" % (name, len(again["scenes"]), n))

        # the translations, written back from the first compile in place of the sample's
        translations = []
        for name in listed:
            with open(os.path.join(first, name + ".scn.json"), encoding="utf-8-sig") as f:
                codes = json.load(f).get("languages") or []
            for code in codes:
                path = os.path.join(game, "data", "lang", code, "scenario", name)
                if os.path.exists(path):
                    translations.append((name, code, path))
        if translations:
            for name, code, path in translations:
                report, exact = scn2ks(args.scn2ks, os.path.join(first, name + ".scn.json"), path, code)
                if not exact:
                    failures += 1
                    print("FAIL %s in %s: scn2ks says:\n%s" % (name, code, report))
            summary, warnings3 = compile_all(game, args.engine, args.timeout, "json")
            print("compile with the translations written back:", summary)
            if len(warnings3) != len(warnings):
                failures += 1
                print("FAIL with the translations written back the scripts compile with %d warnings, the originals with %d"
                      % (len(warnings3), len(warnings)))
                for w in warnings3:
                    print("  compiler warning:", w)
            for name in sorted(set(t[0] for t in translations)):
                with open(os.path.join(scenario, name + ".scn.json"), encoding="utf-8-sig") as f:
                    d = diff(originals[name], normal(json.load(f)))
                if d:
                    failures += 1
                    print("FAIL %s: the round trip of its translations differs:\n  %s" % (name, "\n  ".join(d)))
                else:
                    print("ok   %s in %s" % (name, ", ".join(c for n, c, _ in translations if n == name)))

        if args.plugin:
            # the original scripts again, compiled to .scn: the same scripts come out
            for name in listed:
                shutil.copy2(os.path.join(sources, name), scenario)
            summary, _ = compile_all(game, args.engine, args.timeout, "scn")
            print("compile to .scn:", summary)
            for name in listed:
                path = os.path.join(scenario, name + ".scn")
                if not os.path.exists(path):
                    failures += 1
                    print("FAIL %s wasn't compiled to .scn (is the plugin loading?)" % name)
                    continue
                out = os.path.join(tmp, name + ".from-scn")
                report, exact = scn2ks(args.scn2ks, path, out)
                a = open(out, encoding="utf-8").read().split("\n", 1)[1]
                b = open(os.path.join(written, name), encoding="utf-8").read().split("\n", 1)[1]
                if not exact or a != b:
                    failures += 1
                    print("FAIL %s: the script from the .scn isn't the one from the JSON\n%s" % (name, report))
                else:
                    print("ok   %s from .scn" % name)

        if failures:
            print("%d failures" % failures)
            return 1
        print("all %d scripts round trip" % len(listed))
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
