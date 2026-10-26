# scnfile

A PSB library for the SDL3 version of Kirikiri Z, and a scenario compiler built on it, for
[KAGEX](https://github.com/krkren/kagex).

A game's developer keeps writing ordinary `.ks` scripts. At release, kagex-release compiles each one into
an encrypted binary `.scn` file and ships only those, so the readable `.ks` files never leave the
developer's machine. In the game, the scnfile plugin decrypts and reads the `.scn` files, and KAGEX's
scene player plays them.

**Status:** milestones 1 and 2 (below). The compiler lives in KAGEX
(`template/system/SceneParser.tjs`, with the scene player's changes), since it's TJS that every game
made from the template carries; this repository has the plugin, which reads and writes plain PSB on
Linux, and its tests. Encryption, the other platforms and kagex-release's part are still plans. The
sections below set out the three plans: the library, the compiler, and the changes KAGEX needs.

## Why

KAGEX's scene player (`template/system/KAGEnvPlayer.tjs`: the `[scenestart]` tag and the World's
`playerExecMode`) already plays compiled scenarios. It needs two pieces from Wamsoft that aren't
public: the `psbfile` plugin to read compiled scenes, and a native `SceneParser` to make them. This
repository supplies both, as our own, for every platform the SDL3 engine runs on. With them, a game
gets:

- scripts that stay out of plain view in the shipped game
- jump to any line with the screen rebuilt (testing, or scene replay from the middle of a chapter)
- a thumbnail of any line (chapter select, a flowchart)
- search across all dialogue, with jump to the result (`goToSearch`)
- rollback to earlier choices
- read tracking per line, so skipping stops exactly at unread text
- one text table per scene, a clean hand-off point for translation

## Name and ground rules

This is not Wamsoft's psbfile, and doesn't use its name, so nobody mistakes it for their code. The
repository, the plugin and its TJS class (`SCNFile`) have names of their own. "PSB" is used only to say
which file format it reads and writes.

To keep it clean:

- It's written from the PSB format itself: not from Wamsoft's `psbfile.dll`, and not from FreeMote's
  source, whose license (CC BY-NC-SA 4.0) doesn't allow use in commercial games.
- It doesn't decrypt M2's encrypted PSB files (E-mote data). Breaking another company's encryption can
  fall under anti-circumvention law. It only decrypts files encrypted with our own scheme, below.
- Test files are made by our own writer. Other tools may be run locally to cross-check output, but
  nothing they produce is committed.

## How it fits together

```
developer:     .ks ──► compiler (TJS, in the engine) ──► .scn (plain PSB)
kagex-release: .scn ──► encrypt with the game's key ──► package (without the compiled .ks)
               key ──► written into the scnfile plugin of each package's engine
player:        .scn ──► scnfile plugin decrypts and parses ──► SCNFile.root ──► KAGEnvPlayer plays it
```

## Plan 1: the library (the scnfile plugin)

A native Kirikiri Z plugin, C++17, built on tp_stub and ncbind, with no Windows APIs.

**Files it reads and writes**

- **Plain PSB:** M2's binary format for structured data: a key-name table, a string table, binary
  resource chunks and a tree of values (null, booleans, integers, reals, strings, resources, lists and
  objects). It reads the PSB versions Kirikiri's `.scn` files use; `docs/format.md` records the exact
  layout as it is implemented and tested.
- **Encrypted `.scn` (ours):** a header (magic `KSCN`, format version, flags, nonce) around a
  zlib-compressed PSB, encrypted with XChaCha20-Poly1305 using [Monocypher](https://monocypher.org/)
  (one C file, CC0 or BSD-2-Clause). Its tag also rejects corrupted or edited files.

**TJS API**

| Member | Does |
|---|---|
| `new SCNFile(storageOrOctet)` | opens a plain or encrypted file, or data already in memory |
| `root` | the contents as ordinary TJS dictionaries and arrays, the same as `PSBFile.root` |
| `SCNFile.save(value, storage)` | writes a value tree as a plain PSB `.scn` (the compiler uses it) |
| `SCNFile.toOctet(value)` | the same, into memory (screen states saved inside a scene) |

`root` matches what KAGEnvPlayer reads from Wamsoft's `PSBFile`, so the player only needs to pick
whichever class is present.

**The key**

kagex-release ships prebuilt engines, so the key can't be compiled in. The plugin carries a reserved
32-byte slot behind a marker, which kagex-release fills in for each game. Without a key, the plugin reads
plain files only.

**Platforms**

Every platform kagex ships: a shared library in `plugin/` on Windows, Linux and macOS, and linked
statically into the web, Android and iOS engines, as krkreffekseer is.

## Plan 2: the compiler

A `SceneParser` class in TJS with the methods KAGEnvPlayer calls: `prepare`, `parse`, `isNeedParse`,
`initDepend`, `loadDepend`, `saveDepend`, `getAllDependStorageList`, `getTargetList` and
`getCurrentInfo`. It runs inside the engine, because the screen states only exist once the World has
actually run the tags.

For each script listed in `!scnlist.txt`:

1. Read it with KAGParserEx, expanding macros (`parsermacro.ks`, `parseroption.txt`).
2. Run the World tags through `KAGEnvForParse` (the headless World in `KAGEnvPlayer.tjs`) and record
   `env.onStore()` at each save point.
3. Split the script at labels into scenes, turn choice and jump endings into `nexts`, and collect
   speakers, text and voices.
4. Save the result. Each compiled file holds the key of what it was compiled from (the script, the
   macros, the options, `envinit.tjs`), so only changed scripts recompile, with no `depend.txt`. The
   compiler writes `.scn` with `SCNFile.save` when the plugin is there, and `<script>.scn.json` with the
   json plugin otherwise (or with `format=json` in `parseroption.txt`, for reading it). Screen states are
   stored as plain dictionaries, which the player already accepts.

As built (KAGEX's `doc/scn.txt` has the details), the compiler reads scripts with a tokenizer of its own
rather than KAGParserEx, which works out `[if]`, `cond=` and `&` entities while it reads: here those are
decided when the game plays the scene. The rules are in KAGEX's `doc/scn.txt` rather than a
`docs/rules.md` here.

**Output** (what KAGEnvPlayer reads):

| Where | Field | Contents |
|---|---|---|
| root | `hash` | identifies this compile, so saves from an older one can be detected |
| root | `scenes` | one per label, in order |
| scene | `label`, `title`, `firstLine` | the label, its title, its first source line |
| scene | `preevals`, `postevals` | TJS expressions run before and after the scene |
| scene | `texts` | one per message: `[name, displayed name, text, voice, flag, state]` |
| scene | `lines` | the steps: a number shows that text; a string sets a read flag; an array is a tag (`[tagname, flag attributes…, 0, name, value…]`); a save point is `[point, state, text, force, line, text flag, label]` |
| scene | `nexts` | where it goes afterwards: `type` (below 0 exit, 0 next, 1 `select`, 2 `mselect`), `storage`, `target`, `exp`, `eval`, and for choices `text`, `onceflag`, `tag` |

**The hard part** is the rules the player doesn't show: which tags fold into a saved state and which
stay as steps, where save points fall, and what the text and save flags mean. We define our own and
document them in `docs/rules.md`. Tags that act outside the World (KAG message-layer settings, sounds the
World doesn't manage, `[iscript]` blocks) stay as steps.

## Plan 3: KAGEX reading from it

**In the framework**

- `KAGEnvPlayer.tjs` opens a compiled script as `.scn` with `SCNFile` when the plugin is there
  (`PSBFile` otherwise), and as `.scn.json` with `Scripts.evalJSONStorage` when there's no `.scn`. The
  JSON path works on every current build with no new plugin, so the compiler and player can be built
  and tested before the library exists. It's for development only: JSON is plain text and slower to
  load.
- The template ships the compiler and default `parsermacro.ks` and `parseroption.txt`.
- The sample plays its demos compiled, and `tools/smoketest.py` covers that.
- `doc/` describes compiling, the settings and the rules.

**In the engine builds**

- `linux-config.json`, `web-config.json`, `app-config.json` and `ios-config.json` add the plugin, from
  this repository as a plugin folder (listed after krkrz_dev's, since tp_stub resolves ncbind from the
  first plugin folder).
- `.github/workflows/platforms.yml` and the release workflow build it for every platform, and fail when
  it doesn't load, as they do for krkreffekseer.

**In kagex-release**

New `scenes` settings: `compile` (on or off) and `key` (or a key file kept out of the game's
repository). On each build it:

1. runs the engine headless with `-scncompile`, which compiles every listed script and exits
2. encrypts each `.scn` with the key
3. leaves the compiled `.ks` files out of the package
4. writes the key into the scnfile plugin of each engine, before signing (macOS, iOS, Android) and before
   the web build's id is computed

## Limits

- The key has to ship with the game, so this stops casual reading, not a determined person. The same is
  true of any encryption a game does on its own files.
- Recompiling a script can move its save points, so a save made in it may not resume at the same spot.
  The root `hash` lets the player notice and fall back to the scene's start.
- Only scripts in `!scnlist.txt` are compiled. The rest (system screens, macro files) stay `.ks`.
- Encrypted `.scn` files open only with this plugin and the game's key; plain ones are ordinary PSB.

## Milestones

1. **Compiler to JSON** (done, in KAGEX): text, names, voices, labels, choices and World tags, written
   as `.scn.json`; KAGEX's player reads JSON; the sample plays its demos compiled and the smoke test
   covers it. No plugin needed.
2. **Library, plain PSB** (done, Linux): read and write every TJS type, with round-trip tests; the
   compiler switches its output to `.scn`.
3. **Encryption:** the encrypted container, the key slot and the kagex-release step.
4. **Every platform:** CI builds for Windows, Linux, macOS, the web, Android and iOS; release packages.
5. **Features on top:** jump to line, scene replay, dialogue search and rollback in the sample's UI.

## Layout

```
plugins/scnfile/   the plugin: psb.cpp (the PSB codec, no engine needed) and main.cpp (SCNFile)
docs/format.md     the PSB layout as the plugin reads and writes it
tests/             psb_test.cpp (the codec), engine/ and run_engine_test.py (SCNFile in the engine)
```

`plugins/` is a plugin folder for the engine build (`TVP_PLUGIN_FOLDERS`, listed after krkrz_dev's
plugins, since tp_stub resolves ncbind from the first plugin folder).

## Building and testing

The plugin needs a [krkrz_dev](https://github.com/wamsoft/krkrz_dev) checkout for tp_stub and ncbind,
and zlib to read `mdf` (compressed) files. On Linux:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRKRZ_DEV_DIR=/path/to/krkrz_dev
cmake --build build
ctest --test-dir build                     # the codec's tests
python3 tests/run_engine_test.py --engine /path/to/krkrz --plugin build/plugins/scnfile/libscnfile.so
```

`-DSCNFILE_SANITIZE=ON` builds the codec's tests with AddressSanitizer and UBSan, and
`-DSCNFILE_PLUGIN=OFF` builds only those (no krkrz_dev needed). A game links the plugin as
`plugin/scnfile.dll` (KAGEX does: `Initialize.tjs`), which the engine finds as `plugin/libscnfile.so`.

## License

[0BSD](LICENSE): use it for anything, with no conditions and no notice to include. The libraries the
plugin will build in keep their own licenses: Monocypher (CC0 or BSD-2-Clause), zlib (zlib license), and
tp_stub and ncbind (Kirikiri Z's BSD-3-Clause, which a game already meets by shipping the engine's
license).
