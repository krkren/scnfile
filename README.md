# scnfile

Compiled, encrypted scenarios for [KAGEX](https://github.com/krkren/kagex).

A game's developer keeps writing ordinary `.ks` scripts. At release, kagex-release compiles each one into
an encrypted binary `.scn` file and ships only those, so the readable `.ks` files never leave the
developer's machine. In the game, the `scnfile` plugin decrypts and loads the `.scn` files, and KAGEX's
scene player plays them.

**Status: plan.** Nothing is implemented yet.

## Why

- **Scripts stay out of plain view.** A shipped game carries no readable scenario text.
- **Scene playback comes back.** KAGEX's scene player (`template/system/KAGEnvPlayer.tjs`: the
  `[scenestart]` tag and the World's `playerExecMode`) already plays compiled scenes. It needs
  Wamsoft's `psbfile` plugin to read them and their native `SceneParser` to make them, and neither
  is public. This repository supplies both, as our own:
  - jump to any line with the screen rebuilt (testing, or scene replay from the middle of a chapter)
  - a thumbnail of any line (chapter select, a flowchart)
  - search across all dialogue, with jump to the result (`goToSearch`)
  - rollback to earlier choices
  - read tracking per line, so skipping stops exactly at unread text
  - one text table per scene, a clean hand-off point for translation

## How it fits together

```
developer:     .ks ──► compiler (TJS, in the engine) ──► .scn (plain)
kagex-release: .scn (plain) ──► encrypt with the game's key ──► package (no compiled .ks)
               key ──► written into the scnfile plugin of each package's engine
player:        .scn ──► scnfile plugin decrypts ──► SCNFile.root ──► KAGEnvPlayer plays it
```

### 1. The `.scn` format (ours, not PSB)

PSB is M2's format and Wamsoft's reader is private; neither is needed. A `.scn` holds one TJS value
tree (void, integer, real, string, octet, array, dictionary):

- **Header:** magic `KSCN`, format version, flags (compressed, encrypted), nonce.
- **Body:** a string table, then the value tree with strings stored by index; zlib-compressed.
- **Encryption (release builds):** XChaCha20-Poly1305 over the body, using
  [Monocypher](https://monocypher.org/) (one C file, CC0 or BSD-2-Clause). Its tag also rejects
  corrupted or edited files.

Development builds write and read unencrypted `.scn` files.

### 2. The `scnfile` plugin (C++)

- `new SCNFile(storageOrOctet)` and its `root` property: the same surface KAGEnvPlayer uses on
  `PSBFile`, so the player needs a one-line change.
- `SCNFile.save(value, storage)`: writes an unencrypted `.scn`; the compiler uses it.
- **Key:** a reserved 32-byte slot behind a marker in the plugin's binary, which kagex-release fills in
  for each game. kagex-release ships prebuilt engines, so the key can't be compiled in.
- **Platforms:** every one kagex ships. A shared library in `plugin/` on Windows, Linux and macOS; linked
  statically into the web, Android and iOS engines, as krkreffekseer is. Built on tp_stub and ncbind,
  with no Windows APIs.

### 3. The compiler (TJS, runs in the engine)

A `SceneParser` class with the methods KAGEnvPlayer calls: `prepare`, `parse`, `isNeedParse`,
`initDepend`, `loadDepend`, `saveDepend`, `getAllDependStorageList`, `getTargetList` and
`getCurrentInfo`. It runs inside the engine because the screen states only exist once the World has
actually run the tags.

For each script listed in `!scnlist.txt`:

1. Read it with KAGParserEx, expanding macros (`parsermacro.ks`, `parseroption.txt`).
2. Run the World tags through `KAGEnvForParse` (the headless World in `KAGEnvPlayer.tjs`) and record
   `env.onStore()` at each save point.
3. Split the script at labels into scenes; turn choice and jump endings into `nexts`; collect speakers,
   text and voices.
4. Save the result with `SCNFile.save`, and update `setup/sceneparser/depend.txt` so only changed
   scripts recompile.

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

### 4. kagex-release

New `scenes` settings: `compile` (on or off) and `key` (or a key file kept out of the game's
repository). On each build it:

1. runs the engine headless with `-scncompile`, which compiles every listed script and exits
2. encrypts each `.scn` with the key
3. leaves the compiled `.ks` files out of the package
4. writes the key into the scnfile plugin of each engine, before signing (macOS, iOS, Android) and
   before the web build's id is computed

### 5. Changes in KAGEX

- `KAGEnvPlayer.tjs` loads `.scn` files with `SCNFile` when the plugin is there (`PSBFile` otherwise).
- The template ships the compiler and default `parsermacro.ks` and `parseroption.txt`.
- The sample plays its demos compiled, and `tools/smoketest.py` covers that.
- `doc/` describes compiling, the settings and the rules.

## Limits

- The key has to ship with the game, so this stops casual reading, not a determined person. The same is
  true of any encryption a game does on its own files.
- Recompiling a script can move its save points, so a save made in it may not resume at the same spot.
  The root `hash` lets the player notice and fall back to the scene's start.
- Only scripts in `!scnlist.txt` are compiled. The rest (system screens, macro files) stay `.ks`.
- PSB tools such as FreeMote don't read these files, by design. Don't copy FreeMote code: its license is
  CC BY-NC-SA 4.0.

## Milestones

1. **Format and plugin, unencrypted:** read and write every TJS type, with round-trip tests; Linux first.
2. **Compiler for the sample:** text, names, voices, labels, choices and World tags; the sample plays
   compiled; the smoke test covers it.
3. **Encryption:** the key slot and the kagex-release step.
4. **Every platform:** CI builds for Windows, Linux, macOS, the web, Android and iOS; release packages.
5. **Features on top:** jump to line, scene replay, dialogue search and rollback in the sample's UI.

## Planned layout

```
plugin/     the scnfile plugin (C++)
compiler/   SceneParser and its defaults (TJS)
docs/       format, rules, settings
tests/      format round trips, compiled sample scenes
```
