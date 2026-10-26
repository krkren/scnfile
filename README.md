# scnfile

A Kirikiri Z plugin (SDL3 version) that reads and writes PSB files and opens encrypted ones, for
the compiled scenarios of [KAGEX](https://github.com/krkren/kagex).

A KAGEX game's developer keeps writing ordinary `.ks` scripts. At release, kagex-release compiles
each one into an encrypted `.scn` file (a PSB) and ships only those, so the readable scripts never
leave the developer's machine. In the game, this plugin decrypts and reads the `.scn` files, and
KAGEX's scene player plays them.

scnfile does PSB and the decryption of `.scn` files, plus one tool the other way: scn2ks turns a
compiled scenario back into a `.ks` script. The compiler and the scene player are TJS in KAGEX
(`template/system/SceneParser.tjs` and `KAGEnvPlayer.tjs`, described in its `doc/scn.txt`); encrypted
game archives are [xp3crypt](https://github.com/krkren/xp3crypt)'s.

## Name and ground rules

This is not Wamsoft's psbfile, and doesn't use its name, so nobody mistakes it for their code. The
repository, the plugin and its TJS class (`SCNFile`) have names of their own. "PSB" is used only to say
which file format it reads and writes.

To keep it clean:

- It's written from the PSB format itself: not from Wamsoft's `psbfile.dll`, and not from FreeMote's
  source, whose license (CC BY-NC-SA 4.0) doesn't allow use in commercial games.
- It doesn't decrypt M2's encrypted PSB files (E-mote data). Breaking another company's encryption can
  fall under anti-circumvention law. It only decrypts files encrypted with its own scheme, below.
- Test files are made by its own writer. Other tools may be run locally to cross-check output, but
  nothing they produce is committed.

## How it fits together

```
developer:     .ks ──► KAGEX's compiler (TJS, in the engine) ──► .scn (plain PSB, SCNFile.save)
kagex-release: .scn ──► encrypted with the game's key ──► package (without the compiled .ks)
               key ──► written into the scnfile plugin of each package's engine
player:        .scn ──► scnfile decrypts and parses it ──► SCNFile.root ──► KAGEX's scene player
```

## What it reads and writes

- **Plain PSB:** M2's binary format for structured data: a key-name table, a string table, binary
  resource chunks and a tree of values (null, booleans, integers, reals, strings, resources, lists
  and objects). It reads versions 2 to 4 and the zlib-compressed `mdf` wrapper, and writes version 2.
  `docs/format.md` records the layout as it's implemented and tested.
- **Encrypted `.scn`:** a header (magic `KSCN`, format version, flags, nonce) around a
  zlib-compressed PSB, encrypted with XChaCha20-Poly1305 using [Monocypher](https://monocypher.org/).
  Its tag also refuses corrupted or edited files. The same file and key always encrypt to the same
  bytes, so an unchanged scenario doesn't show up in a patch.

## scn2ks: back to a script

`scn2ks` reads a compiled scenario as JSON, the `<script>.scn.json` KAGEX's compiler writes without
the plugin (or with `format=json` in `parseroption.txt`), and writes a `.ks` script that compiles back
to the same scenes: the same texts, names, voices, waits, steps, save points, choices and jumps. A
plain `.scn` holds the same tree and is read too; an encrypted one isn't.

```
scn2ks char.ks.scn.json              # writes char.ks
scn2ks --linemode=vn in.json out.ks
```

It writes the texts the way the line mode in force puts messages together (`【name/display】`, line
breaks, empty lines, `[l]`, `[p]`, `[r]`, `[ruby]`), the steps as tags, `[if]` blocks, `[iscript]`
blocks, `&` entities and `cond=`, and the choices and jumps that end each scene. The line mode comes
from the `[linemode]` steps in the file; the one the script starts in is worked out from its texts
(`--linemode=` sets it: a script compiled with `parseroption.txt`'s `linemode=` starts in that one).

To get that right it models the compiler's own rules (the line modes, names, the waits it adds at
line ends, voice counters, the state each `[if]` branch starts from), tries each way of writing a
text or a step, and keeps the first the model compiles to what the file holds. What no way of
writing it gives exactly (a text ending in `[l]` in page mode, a step that isn't a tag) is written as
near as it can be, marked in the script with a `; scn2ks:` comment and listed when it runs.

What doesn't come back, because the compiled file doesn't hold it:

- Comments, macros (their tags are written expanded; a step that used `mp` gets a one-line macro,
  `scn2ks_mp`, with the arguments it had), `[voice]`, `[nextvoice]` and `[parse]` (the voices they
  gave are given again by `[voice name=N]` where the tags written don't already give them), and the
  original layout of lines.
- Line numbers and the source key (`hash`), which come from the script and its settings.

`tests/run_scn2ks_test.py` checks this against KAGEX itself: it compiles KAGEX's sample scripts and
`tests/scn2ks/` (every line mode) in the engine, turns each back into a script, compiles those, and
compares the two compiles.

## TJS API

| Member | Does |
|---|---|
| `new SCNFile(storageOrOctet)` | opens a plain or encrypted file, or data already in memory |
| `root` | the contents as ordinary TJS dictionaries and arrays, the same as `PSBFile.root` |
| `SCNFile.save(value, storage)` | writes a value tree as a plain PSB (the compiler uses it) |
| `SCNFile.toOctet(value)` | the same, into memory |
| `SCNFile.hasKey` | whether a key has been written into this plugin |

`root` matches what KAGEX's scene player reads from Wamsoft's `PSBFile`, so the player uses
whichever class is there.

## The key

Games ship prebuilt engines, so a game's key can't be compiled in. The plugin carries a 32-byte slot
behind a marker, which kagex-release (or `scnseal setkey`) fills in for each game's copy of the
plugin, or of an engine with the plugin linked in. Without a key the plugin reads plain files only.

The key ships with the game, as any key a game reads its own files with has to, so this keeps the
scripts from being read with a text editor or a PSB viewer, not from someone determined.

## Platforms

It's C++17 on tp_stub and ncbind, with no Windows APIs: a shared library in `plugin/` on Windows,
Linux and macOS, and linked into the engine on the web, Android and iOS, as krkreffekseer is. It's
built and tested on Linux. The other platforms are built by KAGEX's CI, which builds this
repository as a plugin folder next to krkrz_dev's.

## Layout

```
plugins/scnfile/   the plugin: psb.cpp (the PSB codec), seal.cpp (encrypted files and the key
                   slot) and monocypher/ (Monocypher 4.0.3, unmodified), none of which need the
                   engine, and main.cpp (SCNFile)
tools/scnseal.cpp  encrypts compiled scenarios and writes a game's key into a plugin
tools/scn2ks.cpp   turns a compiled scenario back into a script; scnks.cpp is how (no engine
                   needed)
docs/format.md     the PSB layout, the encrypted files and the key slot
tests/             psb_test.cpp, seal_test.cpp and scnks_test.cpp (no engine), engine/ and
                   run_engine_test.py (SCNFile in the engine, with and without a key),
                   run_scn2ks_test.py and scn2ks/ (scn2ks round trips through KAGEX's compiler)
```

`plugins/` is a plugin folder for the engine build (`TVP_PLUGIN_FOLDERS`, listed after krkrz_dev's
plugins, since tp_stub resolves ncbind from the first plugin folder).

## Building and testing

The plugin needs a [krkrz_dev](https://github.com/wamsoft/krkrz_dev) checkout for tp_stub and ncbind,
and zlib to read `mdf` (compressed) files and to compress encrypted ones. On Linux:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRKRZ_DEV_DIR=/path/to/krkrz_dev
cmake --build build
ctest --test-dir build                     # the codec's, the encryption's and scn2ks's tests
python3 tests/run_engine_test.py --engine /path/to/krkrz --plugin build/plugins/scnfile/libscnfile.so \
    --scnseal build/scnseal
python3 tests/run_scn2ks_test.py --engine /path/to/krkrz --scn2ks build/scn2ks --kagex /path/to/kagex \
    --plugin build/plugins/scnfile/libscnfile.so
```

`-DSCNFILE_SANITIZE=ON` builds the tests with AddressSanitizer and UBSan, and
`-DSCNFILE_PLUGIN=OFF` builds only them, `scnseal` and `scn2ks` (no krkrz_dev needed). A game links the plugin as
`plugin/scnfile.dll` (KAGEX does, in `Initialize.tjs`), which the engine finds as
`plugin/libscnfile.so`.

kagex-release encrypts a game's scenarios and writes its key when it builds the packages. By hand:

```
scnseal keygen game.key                       # once; keep it out of the game's repository
scnseal seal -k game.key data/scenario/*.scn  # encrypts them where they are
scnseal setkey -k game.key plugin/libscnfile.so
```

## License

[0BSD](LICENSE): use it for anything, with no conditions and no notice to include. The libraries the
plugin builds in keep their own licenses: Monocypher (CC0 or BSD-2-Clause), zlib (zlib license), and
tp_stub and ncbind (Kirikiri Z's BSD-3-Clause, which a game already meets by shipping the engine's
license).
