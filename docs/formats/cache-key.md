# Build cache keys

Status: draft.

The build cache stores built output under keys that hash every input that can change
it, and nothing else. Identical inputs are built once per machine and shared by projects; a changed
input never reuses a stale build.

## 1. Two levels

The cache has two levels of entries, so that adding a platform to a project builds only the new slices:

| Level | Stores | Directory |
|---|---|---|
| **slice** | The `slice.prefix` of one slice build and what `build.lua` returned | `<cache-dir>/builds/<library>/<version>-r<revision>/slices/<slice key>/` |
| **package** | The packaged output for a set of slices: XCFrameworks, wrappers, `Package.swift` (Apple) | `<cache-dir>/builds/<library>/<version>-r<revision>/packages/<package key>/` |

A tool-dependency build is a slice entry for the tool slice.

## 2. Slice key

The SHA-256 of a canonical description (3) of:

| Input | Why |
|---|---|
| key format version | Bumped when the meaning of the key changes. |
| toolchain profile version | Bumped when otok changes the flags or environment it gives builds (`build-lua-api.md` 3.1, 5), since that changes the output without any user-visible input changing. |
| target OS, slice name (`iossim-arm64`) | |
| library, version, revision | |
| `recipe-sha256` | The whole recipe: `build.lua`, patches, `[vars]`. |
| source pin (`sha256`, or `commit` and `submodules`) and resource pins | For a local source inside the project: its tree hash (`recipe-hash.md`). For a local source outside the project: its tree hash at build time. |
| merged options, canonical (`recipe-toml.md` 8) | |
| the project's compiler and linker flags for the library (`otok-toml.md` 4.3) | Their order matters, so they are kept as given. |
| configuration | `release` / `debug`. |
| deployment version of the slice's platform | |
| toolchain identity | Apple: Xcode build version (`27A266a`) and SDK version of the slice. Plus the version found of every toolchain the recipe declares (`recipe-toml.md` 13). |
| slice keys of the active dependencies for the same slice | A rebuilt dependency rebuilds its dependents. |
| keys of the tool dependencies | A rebuilt tool rebuilds what it generated. |
| the `build_tools` slice key, when the recipe defines `build_tools` | |
| the entry point, `build` or `build_tools` | `build_tools` has an entry of its own on the tool slice. |

## 3. Canonical description

A UTF-8 text of `key = value` lines, one per input, sorted by key; lists sorted and comma-separated.
The names:

| Name | Value |
|---|---|
| `key-format`, `toolchain-profile` | the two version numbers |
| `os`, `slice` | the target and the slice: `apple`, `iossim-arm64` |
| `environment` | the environment the slice is built in (common 4.2), its host's system and the compiler: `linux/clang-cl`; also for a recipe that says `*/ndk`, so machines of two systems do not share slices |
| `host-libc` | `musl` on a Linux build machine whose C library is musl (`recipe-toml.md` 9.1), absent elsewhere: the programs of its tool slices run there only, and its compiler links musl |
| `library`, `version`, `revision` | |
| `recipe-sha256` | the locked hash; for a recipe outside the project, its tree hash at build time |
| `source` | `archive:<sha256>`, `git:<commit>` (`+submodules`), `path:<tree hash>`, `local:<tree hash at build time>` |
| `resource.<name>` | as `source`; only resources whose `when` holds (`recipe-toml.md` 5) |
| `option.<name>` | the merged value as `recipe-toml.md` 8 writes it; a `file` option: `sha256:<hex>` of the file's contents, or `none` |
| `flags.cflags`, `flags.cxxflags`, `flags.ldflags` | the project's flags for this library (`otok-toml.md` 4.3), in their order, as a TOML array (`["-DA=1", "-O3"]`); absent when empty |
| `configuration`, `linkage`, `deployment` | `release`/`debug`, `static`/`dynamic` (`otok-toml.md` 5); `deployment` only for platforms that have one |
| `c-standard` | the C standard of this library's target (`otok-toml.md` 5.1), `11`: the project's or the lowest the recipes allow; absent when there is none |
| `cxx-standard` | the C++ standard of this library's target (`otok-toml.md` 5.1), `23`: the project's or the lowest the recipes allow; absent when there is none |
| `toolchain.<name>` | the environment's compiler identity: `toolchain.xcode` and `toolchain.sdk` on Apple; `toolchain.ndk` and, in dynamic slices, `toolchain.stl` (`shared` or `static`, `otok-toml.md` 4.7) on Android; `toolchain.msvc` (`windows/msvc`) or `toolchain.clang-cl` (`*/clang-cl`), with `toolchain.crt` and `toolchain.windows-sdk`, on Windows; `toolchain.gcc` or `toolchain.clang` on Linux |
| `toolchains.<name>` | the version found of each toolchain the recipe declares and whose `when` holds (`recipe-toml.md` 13): `toolchains.cmake = 3.31.6`; not for a toolchain a tool recipe builds (`recipe-toml.md` 13.1), which is in the key as `tool.<name>` |
| `dep.<library>`, `tool.<library>`, `build-tools` | the keys of dependencies, tool dependencies and the `build_tools` entry; for a dev slice (`otok-dev-toml.md` 4), `dev:<tree hash of its prefix/>` |
| `entry` | `build` or `build_tools` |
| `tool` | `true` in the slices of a tool package, absent elsewhere: `build.lua` sees it as `slice.is_tool` (`build-lua-api.md` 3), and a library that is a tool too builds its programs there only (`recipe-toml.md` 13.2) |

The key is the SHA-256 of that text. Directory names use its first 32 hex digits (128 bits), which keeps paths
short on Windows. The text is stored next to the entry (`key.txt`), so `otok` can explain why two
builds differ.

### 3.1 Sources key

A recipe that delivers its source (`recipe-toml.md` 12.1) builds no slices. Its patched source is one
entry, `<cache-dir>/builds/<library>/<version>-r<revision>/sources/<sources key>/`, holding `source/` and
`key.txt`, for every platform. The sources key is the SHA-256 of a description of the form of 3 with
`key-format`, `entry = sources`, `library`, `version`, `revision`, `recipe-sha256` (the patches) and
`source`.

## 4. Package key

The SHA-256 of a canonical description of: the key format version, the packager version (bumped when
otok changes how it packages), the slice keys of all slices in the package, the recipe's
`recipe-sha256` (products, wrappers, privacy manifest), the merged options (wrapper compilation
conditions). `Package.swift`, `otok-config.cmake`, `otok.props` and `otok.targets` are not part of any entry: they are
written into the project from the entries of all libraries (`otok-toml.md` 4, 4.11), which is cheap.

The description has the form of 3, with the names:

| Name | Value |
|---|---|
| `key-format`, `packager` | the two version numbers |
| `os`, `library`, `version`, `revision`, `recipe-sha256` | as in 3 |
| `option.<name>` | as in 3 |
| `slice.<slice>.<configuration>` | the slice key of each slice in the package (`slice.linux-x86_64.debug`) |
| `sources` | the sources key (3.1) of a recipe that delivers its source, which has no slices |
| `bundle-data` | `true` with `bundle-data` of the library's `[deps]` entry (`otok-toml.md` 6); absent otherwise |
| `choice.<key>` | a key of the target table that changes the packages and not the builds: `choice.bundle-id-prefix` on Apple (`otok-toml.md` 4) |

Directory names again use the first 32 hex digits. A package entry holds what the target's output
generator made of the slices, laid out as it goes into the project, `licenses/` with the recipe's
`license-files` copied from the source (whose hash is in the slice keys), `data/` with the files
`build` returned as `data` (`build-lua-api.md` 8), and `key.txt`. For `linux`, `windows` and `android`
that is `include/`, `lib/`, `bin/` and `debug/` as each lays them out (`otok-toml.md` 4.1, 4.7, 4.8) and `otok-package.toml`
naming their files (`otok-toml.md` 4.11), for Apple one XCFramework per library output name (and one of its headers for a dynamic library, `otok-toml.md` 4.10),
`shims/`, `wrappers/` and `undefined-symbols/` (`otok-toml.md` 4, 4.4). Product `defines` and option values are written into
the entry or `Package.swift` after evaluating `when`, so the merged options are part of the key. Tool
packages have no package entry.

## 5. Not part of any key

Machine settings (`config.md`) — `jobs`, `cache-dir`, mirrors, proxy —, absolute paths, the time,
the user name, the profile name (only its effect on the inputs above counts), the otok version as such
(only the two version numbers above, which change rarely, so upgrading otok does not throw away the
cache).

## 6. Entries

An entry is written to a temporary directory and renamed into place when complete, so an interrupted
build never leaves a half-written entry. Concurrent otok processes building the same key wait for
each other through a lock file next to the entry. `otok clean` removes entries; garbage collection of
entries unused for a long time is described with the cache module.
