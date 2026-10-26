# scnfile

A Kirikiri Z plugin (SDL3 version) that reads and writes PSB files and opens encrypted ones, for
the compiled scenarios of [KAGEX](https://github.com/krkren/kagex).

A KAGEX game's developer keeps writing ordinary `.ks` scripts. At release, kagex-release compiles
each one into an encrypted `.scn` file (a PSB) and ships only those, so the readable scripts never
leave the developer's machine. In the game, this plugin decrypts and reads the `.scn` files, and
KAGEX's scene player plays them.

scnfile does PSB and the decryption of `.scn` files, and nothing else. The compiler and the scene
player are TJS in KAGEX (`template/system/SceneParser.tjs` and `KAGEnvPlayer.tjs`, described in its
`doc/scn.txt`); encrypted game archives are [xp3crypt](https://github.com/krkren/xp3crypt)'s.

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
docs/format.md     the PSB layout, the encrypted files and the key slot
tests/             psb_test.cpp and seal_test.cpp (no engine), engine/ and run_engine_test.py
                   (SCNFile in the engine, with and without a key)
```

`plugins/` is a plugin folder for the engine build (`TVP_PLUGIN_FOLDERS`, listed after krkrz_dev's
plugins, since tp_stub resolves ncbind from the first plugin folder).

## Building and testing

The plugin needs a [krkrz_dev](https://github.com/wamsoft/krkrz_dev) checkout for tp_stub and ncbind,
and zlib to read `mdf` (compressed) files and to compress encrypted ones. On Linux:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRKRZ_DEV_DIR=/path/to/krkrz_dev
cmake --build build
ctest --test-dir build                     # the codec's and the encryption's tests
python3 tests/run_engine_test.py --engine /path/to/krkrz --plugin build/plugins/scnfile/libscnfile.so \
    --scnseal build/scnseal
```

`-DSCNFILE_SANITIZE=ON` builds the tests with AddressSanitizer and UBSan, and
`-DSCNFILE_PLUGIN=OFF` builds only them and `scnseal` (no krkrz_dev needed). A game links the plugin as
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
