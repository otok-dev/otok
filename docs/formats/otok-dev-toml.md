# `otok.dev.toml` — working trees for incremental builds, format 1

Status: draft. Common rules are in [README.md](README.md); the manifest in [otok-toml.md](otok-toml.md), the lock in
[otok-lock.md](otok-lock.md), the cache in [cache.md](cache.md).

`otok.dev.toml` is written by the user next to `otok.toml` and **not** committed (the project's `.gitignore` lists it).
It names libraries of the project that the developer works on, with the working trees to build them from: the source,
the resources, the recipe, as they are on disk, with uncommitted changes. otok builds such a library incrementally in a
work directory it keeps (4), so that a change of a few files recompiles those files only, as the library's own build
does in its checkout (`docs/PLAN.md` decision 67).

The file changes neither the resolution nor the lock (3): without it, or with `--no-dev`, the project builds exactly
what `otok.lock` says.

## 1. Example

```toml
# engine and the modules it is built with, from the workspace beside this project
[deps.engine]
source = "../../modules/engine"
# the recipe's checkout too, with its uncommitted changes
recipe = "../../../recipes/engine"

[deps.engine.resources]
core = "../../modules/core"
parser = "../../modules/parser"
math = "../../modules/math"
utf = "../../modules/utf"
```

## 2. Keys

The file is TOML (common 1.1) and strict (common 1.2). Paths are relative to its directory and may point outside the
project (common 6); they must name existing directories.

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | `1`, the version of this document (common 1.4). Optional: absent is `1`. |
| `[metadata]` | table | For other tools; otok never reads it (common 1.2). |
| `[deps.<library>]` | table | A library of the lock, a dependency or a tool dependency, built from working trees. At least one of its keys. |
| `deps.<library>.source` | path | The source instead of the locked one (the recipe's `[source]`, or `source` of `otok.toml` 6.3). A git checkout's submodules are taken as they are in it. |
| `deps.<library>.resources` | table: resource name → path | Resources instead of the locked ones, by their `name` in `recipe.toml` (5); each is placed at its `dest` as a fetched one is. A resource of the recipe not named here is the locked one. |
| `deps.<library>.recipe` | path | The recipe directory instead of the locked recipe: `build.lua`, patches, files, `[vars]`, products (3). |
| `deps.<library>.quiet` | boolean | `true`: the output of its builds is not shown as it comes (4), only, as for other libraries, the end of the log of a failed build. Default `false`. |

## 3. Resolution and the lock

The project is resolved from `otok.toml` and `otok.lock` as without the file, and the lock is written as without it:
`otok install --locked` passes, and a later `otok install` without the file finds the lock up to date.

A `recipe` of `otok.dev.toml` replaces the locked recipe of its library for the build only. It must agree with the
lock: the same library and version, and, resolved in its place without writing the lock, the same graph (the same
dependencies with the same locked versions, options merged to the same values for every other library). Otherwise
the command fails, naming what differs: a change of the graph is a change of the project, made by editing the recipe
reference of `otok.toml` (a `path` recipe, otok-toml.md 6.1) and running `otok install`.

`otok install`, `update` and `fetch` read the file, also with `--locked`, `--frozen` and `--offline` (inputs it names
are not fetched); `--no-dev` ignores it. `otok tree` marks its libraries `(dev)`. `otok publish`, `otok index-*`,
`otok metadata` and `otok audit` never read it.

## 4. Builds

Every slice of a library of the file is a **dev slice**: built in a work directory kept between runs, never stored
in the cache's `builds/`, whatever else its key holds.

- **Work directory.** `<cache-dir>/dev/<project key>/<library>-<slice>/` (`-debug`, `-tool` and `-tools` as in
  `cache.md` 7.1), where the project key is the first 32 hex digits of the SHA-256 of the project directory's absolute
  path. It has the layout of a build's work directory (`source/`, `build/`, `prefix/`, `home/`, `tmp/`,
  `toolchain.cmake`, `build.log`) and `dev.toml`, otok's record of it (4.1). While a slice builds, otok holds the lock
  `<work directory>.lock`.
- **Dev key.** The slice key of `cache-key.md` 3 with `source`, `resource.<name>` of the named resources and, with
  `recipe`, `recipe-sha256` written as `dev`: what decides a build besides the working trees (options, flags,
  toolchains, the keys of the dependencies). When it differs from the one `dev.toml` records, `build/` and `source/`
  are emptied first: the next build configures from scratch, as a toolchain or an option that changed requires.
- **Source.** `source/` is synchronized, not copied afresh: the source tree and the named resources at their `dest`, a
  file copied only when its size or modification time differs from those `dev.toml` records for it, with the
  modification time of the working tree, so that the build tool sees only the files that changed as newer; a file
  copied before and gone from the working tree is removed. Of a working tree that is a git checkout, the files git does not
  ignore (`git ls-files --cached --others --exclude-standard`, and so in its submodules), as a commit could hold them, not the
  build directories beside them; of another directory, every file but `.git` entries. Files that the build wrote into `source/` (build.lua
  copying a file there) are left. Locked resources are copied as for any build when `source/` is new. Patches: a file
  a patch touches is copied and the patches applied again when that file or a patch changed (otherwise it is left
  patched).
- **Build.** `prefix/` is emptied, then build.lua runs as for any slice, in the kept `build/`. `otok.copy` and
  `otok.write` leave a file whose contents would not change, so a recipe that copies its build files into the source
  does not make the build tool configure again (`build-lua-api.md` 6).
- **Result.** The result is checked as for any slice and used from the work directory. Dependents see the dev slice's
  key as `dep.<library> = dev:<tree hash of prefix/>` (`recipe-hash.md`): they rebuild, and are cached, only when its
  output changed.
- **Packaging.** As for any slice: the package key holds the dev slice's key (`cache-key.md` 4), so an output that did not
  change keeps its package, and the output directory takes only what changed (`otok-toml.md` 4.2).
- **Display.** The output of a dev slice's build is shown as it comes, unless `quiet`: what its commands print, the files
  the build tool compiles, the warnings of the compiler, each line after the slice it comes from,
  since the slices build at once (`[engine/android-arm64-v8a] [1/3] Building CXX object …`); `-vv` shows it for every slice
  (`docs/PLAN.md` decision 59). A dev slice is named so: `built engine 0.1.0 for android-arm64-v8a (dev:
  ../../modules/engine)`.
- **Failure.** A failed dev build keeps its work directory as it is; only the log is copied into `failed/`.

`otok clean` removes `dev/` as the rest of the cache. Garbage collection (`cache.md` 9) ages a work directory by the
time of its last build.

### 4.1 `dev.toml`

Written by otok into the work directory once `source/` is synchronized and patched, before build.lua runs:

```toml
key = "…"                         # the dev key's description (cache-key.md 3), whose change empties build/
patches = "…"                     # the SHA-256 of the patches applied; absent: none, or applied again next time
[files]                           # every file synchronized into source/: size and modification time, in the units
"src/a.cpp" = [1234, 134064264451234567]  # of the host's file system clock; a link: [0, 0]
```

## 5. Errors

- the file is not valid TOML, a key is unknown, a value has the wrong type (common 1.2);
- `format` is not `1`;
- a library that is not in the lock; a resource name the library's recipe does not have;
- a path that is not an existing directory;
- a `recipe` whose library or version differ from the locked one, or whose graph differs (3);
- a library whose recipe delivers its source (`recipe-toml.md` 12.1): it builds no slices.
