---
name: otok
description: Add, build, update and check native C and C++ libraries with otok, which builds them from source and hands them to Xcode and SwiftPM, CMake, Visual Studio or Gradle. Use when the project has an otok.toml, when the user wants a native library (zlib, OpenSSL, Boost, curl, libpng, ...) for an Apple, Android, Windows or Linux app, or when an otok command fails.
---

# otok

otok builds native libraries from source on this machine and writes them into one output directory (`Otok/`) that
the project's build uses: a Swift package and XCFrameworks for Apple, `otok-config.cmake` for CMake on every target,
`otok.props`/`otok.targets` for Visual Studio. Everything is declared in `otok.toml`; `otok.lock` pins every version
and input by its hash.

Documentation: https://otok-dev.github.io (index for agents: https://otok-dev.github.io/llms.txt). The recipes of the
public index: https://otok-dev.github.io/index/.

## Ground rules

- Pass `--format json` to every command you read the output of. `search`, `info`, `outdated`, `audit`, `why` and
  `log --list` print one JSON document; `install`, `update` and `fetch` print JSON lines, one event per line. Never
  parse the text output: it is for people and changes between releases. The documents are specified in
  https://github.com/otok-dev/otok/blob/main/docs/formats/cli-json.md.
- Errors and warnings are in `diagnostics` (or `diagnostic` events), each with `severity` and `message`. The exit
  code is 0 on success.
- `--format json` needs otok 0.5.34 or later (`search` and `info`: 0.5.33). Check with `otok --version`; if it is
  older, tell the user to update otok (Homebrew `brew upgrade otok`, Scoop `scoop update otok`, Linux: run the
  install script again). `otok agent-skill` writes this skill as the otok of the machine has it: run it again after
  updating otok.
- Edit `otok.toml`, never `otok.lock`: otok writes the lock. Commit both. `Otok/` is generated: keep it in
  `.gitignore`.
- The first `otok install` builds every library and the tools they need from source and can take many minutes (Boost,
  OpenSSL, ICU, Skia: much longer). Let it run; do not stop it because it is quiet. Later runs take builds from the
  machine's cache.
- otok needs `git`, and the build tools of the target: Xcode for Apple, Visual Studio with the C++ workload for
  Windows, a compiler and `bwrap` (bubblewrap) for Linux, the NDK for Android. otok builds CMake, Ninja, Python,
  Meson and the like from their own recipes: do not install them for otok.

## Add a library

1. Find the recipe:
   `otok search <name> --format json` → `recipes[]` with `id` (`"apotocki/libpng"`), `version` (the newest),
   `targets` (`apple`, `android`, `windows`, `linux`).
2. Read it:
   `otok info <id> --format json` → `recipe.versions[]` (published, advisories), `recipe.variants[]` with each
   target's `platforms`, `linkage`, `options[]` (`name`, `type`, `values`, `default`, `doc`) and `dependencies`.
   Pick only options whose `doc` says they are needed.
3. Add it to `otok.toml`:

   ```toml
   [deps.curl]
   recipe = "apotocki/curl"
   version = "~> 8.23"            # 8.23 or a later 8.x; ">= 8.20, < 9" also works
   options = { http2 = true }     # only options the recipe declares
   ```

   The table's name is the library name. A recipe's own dependencies (nghttp2 for curl with `http2`, zlib for libpng)
   come by themselves: add them only to set their options or version.
4. Build: `otok install --format json`. Read the events until `end`; `end.exit` is the exit code.
5. Use the output in the project's build (see below).

## A new project

`otok.toml` at the root of the project, with a table for each target the app is built for:

```toml
[otok]
format = 1

[apple]                                   # XCFrameworks and Otok/Package.swift
platforms = ["macosx", "ios", "iossim"]
deployment = { macos = "14.0", ios = "17.0" }

[android]
architectures = { android = ["arm64-v8a", "x86_64"] }
deployment = { android = "24" }

[windows]
build-on = ["windows/msvc"]               # or "windows/clang-cl"
architectures = { windows = ["x86_64"] }
configurations = ["debug", "release"]

[linux]
build-on = ["linux/gcc"]                  # or "linux/clang"
configurations = ["debug", "release"]

[deps.lz4]
recipe = "apotocki/lz4"
version = "~> 1.10"
```

Keep only the targets the user needs. A machine builds the targets it can (Apple only on a Mac, Windows on Windows)
and skips the others with a `skipped` event that says why: that is not an error. Libraries are static unless a target
table says `linkage = "dynamic"`, or `[deps.<library>] linkage = "dynamic"` for one library.

## Use the output

- **CMake** (every target):

  ```cmake
  find_package(Otok CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/Otok" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  target_link_libraries(app PRIVATE otok::LZ4)   # otok::<product>
  ```

  The product names are in `Otok/otok-config.cmake` and in `otok metadata` (`packages[].outputs[].products[].name`).
  A project that already calls `find_package(lz4 CONFIG)` keeps it: configure with
  `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=Otok/otok-provider.cmake` instead.
- **Xcode / SwiftPM**: add `Otok` as a local package (`.package(path: "Otok")`), depend on its products
  (`.product(name: "LZ4", package: "Otok")`).
- **Visual Studio**: import `Otok\otok.props` before the project's settings and `Otok\otok.targets` at the end, and
  list the products in `<OtokProducts>LZ4</OtokProducts>`.
- **Android (Gradle + CMake)**: the CMake way above from the module's `CMakeLists.txt`; otok's file packs the shared
  objects.

`otok metadata` prints the whole project as JSON (always JSON): targets, outputs, every package with its version,
options, linkage and products.

## When something fails

- Read `diagnostics` (or the `diagnostic` events): the message says what is wrong and usually what to do.
- A failed build: the `slice` event with `state: "failed"` names its `log` (the file kept in the cache; its end has the
  compiler's or the build system's error) and a `hint` when otok knows what to do (otok 0.5.36 or later). Before that,
  or later on: `otok log <library> --list --format json` → `logs[]`, newest first, with `file`, `failed`, `slice`; read
  the newest `file` with `failed: true`.
  `otok log <library>` prints the newest log itself.
- `otok install -v` shows the commands each build runs; `--keep-build-dirs` keeps the whole work directory of a failed
  build in the cache.
- Option conflicts ("two libraries ask for different values"): `otok why <library> --format json` →
  `packages[].options.<name>.requests[]` says who asked for what.
- Do not work around a failing recipe by editing files in otok's cache or in `Otok/`: they are overwritten. Report it
  to the recipe's publisher, or change options, version or linkage in `otok.toml`.

## Keep it current

- `otok outdated --format json` → `packages[]` with `current`, `compatible` (what `otok update` would take) and
  `latest`; `behind: true` when one differs.
- `otok update [<library>...] --format json`: moves to the newest versions the constraints allow and rewrites the
  lock (the `lock` event lists the changes). Widen the `version` constraint in `otok.toml` to go past it.
- `otok audit --format json` → `findings[]`: advisories (`withdrawn`, `security`, `broken`), deprecated recipes and
  licenses that `[policy]` refuses; exit 1 when one is an error.
- In CI: `otok install --locked` fails when `otok.lock` is missing or out of date instead of changing it.

## Other commands

`otok tree` (the graph, text), `otok fetch` (inputs only, no build), `otok clean` (the cache), `otok help <command>`.

A team may share built slices through a build cache of its own: stores named in the machine's `config.toml`
(`[[build-cache]]`) or `OTOK_BUILD_CACHE*` variables, never in `otok.toml`. `otok cache status` shows where each slice
would come from (the local cache, a store, a build); `otok install --no-build-cache` uses the local cache alone. Do not
put a store's URL or token into the project's files.
