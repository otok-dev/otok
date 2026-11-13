# Otok formats

Normative specifications of the files otok reads and writes. Otok is a dependency manager that builds native
libraries from source on the user's machine, following recipes published in an index, and hands them to the
consumer's build. These documents say exactly what each file holds, what it means and how otok checks it, with as
much of the reason as a reader needs to apply a rule; the principles they rely on are in 1.6. Where other
descriptions of otok disagree with them, these documents win.

These documents are licensed under the Creative Commons Attribution 4.0 International License
([LICENSE-CC-BY-4.0.txt](LICENSE-CC-BY-4.0.txt)), not under otok's license, so that other programs may implement
them and their text may be published with the documentation; their examples may be copied without any condition
(CC0 1.0).

| Document | File | Written by |
|---|---|---|
| [otok-toml.md](otok-toml.md) | `otok.toml` — project manifest | the user |
| [recipe-toml.md](recipe-toml.md) | `recipe.toml` — recipe metadata | the recipe publisher |
| [build-lua-api.md](build-lua-api.md) | `build.lua` — recipe build script, API 1 | the recipe publisher |
| [otok-lock.md](otok-lock.md) | `otok.lock` — resolved inputs | otok |
| [otok-dev-toml.md](otok-dev-toml.md) | `otok.dev.toml` — working trees for incremental builds, usually not committed | the user |
| [index.md](index.md) | index entries, library and publisher files, advisories, HTTPS view | `otok publish`, index maintainers |
| [config.md](config.md) | `config.toml` — machine settings | the user |
| [recipe-hash.md](recipe-hash.md) | tree hash: `recipe-sha256`, local sources | — |
| [cache-key.md](cache-key.md) | build cache keys | — |
| [cache.md](cache.md) | the cache directory: layout, entries, locks | otok |
| [metadata.md](metadata.md) | the JSON of `otok metadata` — the project for IDEs and scripts | otok |

Status: draft. Until the first public release the formats may change without bumping their
version numbers; from then on every change bumps them (1.4).

## 1. Common rules

### 1.1 Encoding and syntax

- Files are UTF-8 without a byte order mark. Manifest-like files are [TOML 1.0.0](https://toml.io/en/v1.0.0).
- Keys are lowercase kebab-case (`deny-licenses`, `default-recipe`). Names chosen by users or
  publishers (library names, option names, profile names) follow 1.2.

### 1.2 Strictness

- An unknown key is an error, reported with file, line and column. Typos in a manifest must not be
  silently ignored, since an ignored option changes what is built.
- A value of the wrong type is an error. There are no implicit conversions (a string `"true"` is
  not a boolean, a number `15` is not the version `"15.0"`).
- Every file that allows it has a `[metadata]` table that otok never reads, for other tools.

### 1.3 Diagnostics

Errors name the file, line and column (from the TOML parser's source positions) and, where it
helps, the value that was expected. Errors about a resolved graph (conflicts, policy) name the
dependents involved.

### 1.4 Format versions

Each format has a version number declared in the file (`[otok] format` in `otok.toml`,
`[recipe] api` in `recipe.toml`) or implied by the containing file. Rules:

- Any change after the first public release bumps the number, including additions; there are no
  minor versions.
- A newer otok reads every older version exactly as it was defined.
- An otok that does not know a version fails with a message saying that the file needs a newer otok,
  instead of reporting unknown keys. It cannot name the version: which otok brought a format number is known only
  after it.

### 1.5 Data written for a newer otok

What an index holds is read for years by every otok released before it, so readers of the index and of published
recipes do not fail on what a newer otok writes:

- A version entry this otok cannot read (an unknown `format` or `api`, a library of an unknown version scheme) is
  not there for it, and is an error only when a lock or a constraint needs that version (`index.md` 3).
- Platforms, architectures, environments and targets are open sets in recipes read from an index or a git repository
  (4.1): an unknown name never matches on this otok.
- The mutable files of an index are read without the check of unknown keys of 1.2, and advisories so that they act at
  least as much as they mean (`index.md` 2, 5, 6, 7, 10, 11). Index CI checks every file strictly.

Everything a person writes (`otok.toml`, `config.toml`, a recipe by `path`, what `otok publish` and index CI check)
stays strict.

### 1.6 Principles

The formats rely on these rules of otok as a whole:

1. **No prebuilt binaries are distributed.** An index holds recipes and the hashes of their inputs only; everything
   that runs on the user's machine is readable on the catalog page.
2. **Project configuration is data.** `otok.toml` is never executed; the logic of a build is in recipes, which are
   reviewed, versioned and hashed.
3. **Published versions are immutable.** A version entry pins a recipe commit and the hashes of its inputs and is
   never edited; a version is withdrawn by an advisory, not by an edit.
4. **Builds are offline.** otok downloads every input and checks it against its hash before any build; build processes
   get no network.
5. **One place for each concern, and machine settings never change what is built.** Everything that changes what is
   built for a project is in its `otok.toml` (and the files it includes) or in recipes. Machine settings
   (`config.md`) choose where inputs come from, how many jobs run and which installed toolchain is used (whose own
   version is part of the cache key); they enter no cache key themselves, and anything that would change the output
   is accepted only in `otok.toml` or a recipe.

## 2. Names

| Name | Syntax | Notes |
|---|---|---|
| library | `[a-z0-9][a-z0-9_-]{0,63}` | `openssl`, `libssh2`, `zxing-cpp`. Lowercase only, so a name means one thing on case-insensitive file systems. |
| publisher | GitHub login: `[a-z0-9]([a-z0-9-]{0,38})`, no `--`, no trailing `-` | compared case-insensitively and stored lowercase |
| recipe name | as library | unique per publisher |
| recipe id | `<publisher>/<recipe name>` | `apotocki/openssl` |
| option | `[a-z][a-z0-9-]{0,63}` | `no-deprecated`, `libraries` |
| profile | `[a-z][a-z0-9-]{0,63}` | `ci`, `release-device` |
| product, module | `[A-Za-z_][A-Za-z0-9_]{0,63}` | SPM product and Clang module names |

Library names contain neither `.` nor uppercase, so they never need quoting as TOML keys
(`[deps.libssh2]`). The library name `tools` is reserved (`deps.tools` in `build.lua`).

## 3. Versions and constraints

### 3.1 Version strings

A version is the upstream version, verbatim: one or more characters of
`[A-Za-z0-9._+-]`, starting and ending with a letter or digit, at most 64 characters. Examples:
`3.5.4`, `1.1.1w`, `78.3`, `1.91.0-1`, `2026.10.01`.

Its order is defined by the library's version scheme, declared in the index (`index.md`):

- **`dotted`** (default). Split on `.`, `-`, `_`. Each part is a number optionally followed by a
  letter suffix (`1`, `1w`, `0rc1` → number `0`, suffix `rc1`), or a suffix alone (`beta`, number
  absent). Compare part by part:
  1. numbers numerically (an absent number sorts before any number);
  2. a part without suffix before the same number with one (`1.1.1` < `1.1.1a`);
  3. suffixes in ASCII order (`1.1.1a` < `1.1.1w`);
  4. a missing part sorts first (`1.2` < `1.2.0` < `1.2.1`).

  Separators carry no meaning (`1.91.0-1` and `1.91.0.1` compare equal; a library should not
  publish both). `+` is a plain character of a suffix. A suffix is not a pre-release marker:
  `1.92.0` < `1.92.0.beta1` < `1.92.0.beta2`. Stability is the separate `channel` attribute; a
  library whose pre-release tags must sort before the release uses `semver` or `explicit`.
- **`semver`**. Semantic Versioning 2.0.0 precedence; build metadata (`+…`) is ignored for order.
- **`explicit`**. The order listed in the library file of the index. A version not in the list
  cannot be published.

Two versions that compare equal are the same version: the index rejects publishing the second one.

### 3.2 Constraints

A constraint is a comma-separated list of comparisons, all of which must hold. Whitespace around
operators and commas is ignored.

```text
constraint  = comparison *( "," comparison )
comparison  = [ operator ] version
operator    = "=" / ">=" / ">" / "<=" / "<" / "~>"
```

- A bare version is an exact match: `"1.1.1w"` is `"= 1.1.1w"`. There are no implicit caret or
  tilde ranges.
- `~> V` means `>= V, < N`: only the last given part may grow. N is V without its last part and
  with the number of the new last part incremented (its suffix dropped); for a one-part V, N is
  that part's number incremented. As in RubyGems and CocoaPods:
  `~> 1.1.1` → `>= 1.1.1, < 1.2`; `~> 3.5` → `>= 3.5, < 4`; `~> 3` → `>= 3, < 4` (the same as
  `~> 3.0`); `~> 1.91.0-1` → `>= 1.91.0-1, < 1.91.1`; `~> 1.beta` → `>= 1.beta, < 2`. The part to
  increment must have a number (`~> beta.1` and `~> beta` are errors). An open range is written
  explicitly: `>= 3`.
- In the `explicit` scheme only `=` and bare versions are allowed.
- Pre-releases (`channel = "prerelease"` in the index) match only when the constraint names one with
  `=` or the dependency sets `prerelease = true`.

Examples: `"~> 3.5"`, `"~> 3"`, `">= 3.0, < 3.6"`, `"= 1.92.0.beta1"`, `">= 1.1.1"`.

## 4. Targets, platforms and build environments

A **target** is a unit of delivery: one output, one way of packaging, one integration with consumers. A
**platform** is a unit of binary compatibility inside a target: what a binary runs on. Every platform belongs to
one target. A **build environment** is where and with what a slice is built (4.2).

| Target | Platforms | Environments (4.2) |
|---|---|---|
| `apple` | `ios`, `iossim`, `macosx`, `catalyst`, `tvos`, `tvossim`, `watchos`, `watchossim`, `xros`, `xrossim` | `macos/xcode` |
| `android` | `android` | `macos/ndk`, `linux/ndk`, `windows/ndk` |
| `windows` | `windows` | `windows/msvc`, `windows/clang-cl`, `linux/clang-cl` |
| `linux` | `linux` | `linux/gcc`, `linux/clang` |

### 4.1 Platforms and slices

The platforms of `apple`:

| Platform | Meaning | Architectures (default first) |
|---|---|---|
| `ios` | iOS devices | `arm64` |
| `iossim` | iOS Simulator | `arm64`, `x86_64` |
| `macosx` | macOS | `arm64`, `x86_64` |
| `catalyst` | Mac Catalyst | `arm64`, `x86_64` |
| `tvos` | tvOS devices | `arm64` |
| `tvossim` | tvOS Simulator | `arm64`, `x86_64` |
| `watchos` | watchOS devices | `arm64_32`, `arm64` |
| `watchossim` | watchOS Simulator | `arm64`, `x86_64` |
| `xros` | visionOS devices | `arm64` |
| `xrossim` | visionOS Simulator | `arm64` |

The target `android` has one platform, `android`, whose architectures
are the Android ABIs: `arm64-v8a` (the default), `armeabi-v7a`, `x86_64`, `x86`. Its deployment key
`android` is the API level.

The target `windows` has one platform, `windows`, with the architectures `x86_64`
(the default) and `arm64`: the MSVC ABI, whether built with MSVC or clang-cl; it has no deployment versions.

The target `linux` has one platform, `linux`, with the architectures `x86_64` (the default) and `arm64`: glibc and
the GNU C++ runtime of the build machine; it has no deployment versions. (The development target `native`
of earlier versions, whose platform `native` built for the machine otok ran on, is gone.)

A slice is `<platform>-<arch>`: `iossim-arm64`, `watchos-arm64_32`, `android-arm64-v8a`, `windows-arm64`,
`linux-x86_64`.
Platform names of later targets never change the existing ones.

The tables are maintained by the targets, not by the formats: a newer otok may add platforms,
architectures or environments without a format change, since accepting a new name changes nothing for existing
files. An older otok reading a recipe of an index or a git repository takes a name it does not know as one that
never matches (1.5): `supported` and `archs` keep the platforms and architectures it knows, a `when` or `not` testing
the unknown name is false, and `build-on` cannot build in an unknown environment. In `otok.toml`, a recipe by
`path` and what `otok publish` and index CI check, an unknown name is an error.

### 4.2 Build environments

An environment is written `<host>/<compiler>`:

- The **host** is the system of the build machine: `macos`, `linux` or `windows`, optionally with its architecture
  (`windows-arm64`, `linux-x86_64`; architectures `x86_64` and `arm64`). A host without architecture stands for
  every architecture of that system.
- The **compiler** is a family with what it needs to build (linker, archiver, SDK). Other build tools (CMake,
  Ninja, b2, …) are not part of it; recipes name them in `[toolchains]` (`recipe-toml.md` 13).

| Compiler | What it is |
|---|---|
| `xcode` | Xcode's clang and SDKs |
| `ndk` | the Android NDK's clang and sysroot |
| `msvc` | `cl.exe` and `link.exe` of Visual Studio, with its CRT and the Windows SDK |
| `clang-cl` | clang in the driver mode of `cl.exe`, `lld-link`, the CRT and the Windows SDK (of Visual Studio on Windows, of `xwin splat` elsewhere) |
| `gcc` | the GNU compilers and the system's binutils |
| `clang` | LLVM's clang and lld |

`*` as the host (`*/ndk`) stands for every host of the target with that compiler, those added later too. It is
allowed only for a compiler that builds the same cross build on every host, which the target says: `ndk` only.
`clang-cl`, `gcc` and `clang` build natively on a host of the target's own system and across elsewhere, and a
project's build system takes different paths for each.

An environment **covers** another when its host is `*`, or the same system with no architecture or the same one,
and its compiler is the same: `windows/msvc` covers `windows-arm64/msvc`; `*/ndk` covers `linux/ndk` and `*/ndk`;
`linux/ndk` does not cover `*/ndk`. A machine **has** an environment it is covered by, when the target finds the
compiler there.

Each compiler of a target builds libraries of an **ABI**, which the target says: the
`windows` target's `msvc` and `clang-cl` build MSVC's (one C++ ABI, one CRT), the `linux` target's `gcc` and `clang`
the system's (glibc and libstdc++). Libraries of one ABI link together whatever compiler built them. An environment
**substitutes** for another of the same target when its system and architecture are the same and its compiler is
another of the same ABI: `windows/clang-cl` for `windows/msvc`, `linux/clang` for `linux/gcc`; never across systems
(`linux/clang-cl` for `windows/msvc`) nor for `*` environments. A compiler of an ABI of its own (a MinGW, a clang with
libc++) substitutes for none.

## 5. Licenses

License fields hold [SPDX license expressions](https://spdx.github.io/spdx-spec/v3.0.1/annexes/spdx-license-expressions/)
(`Apache-2.0`, `MIT OR Apache-2.0`, `GPL-2.0-or-later WITH Classpath-exception-2.0`). Policy checks
(`otok-toml.md` 9) evaluate the expression: `A OR B` passes if either side passes, `A AND B` only if
both do.

## 6. Paths

- Paths in files use `/` on every host and are relative to the directory of the file that contains
  them, unless a field says otherwise.
- Paths that leave the project (`../otok-recipes/openssl`) are allowed only where a field says so
  (local recipe and source overrides); they are never written to `otok.lock`.
