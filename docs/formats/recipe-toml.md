# `recipe.toml` — recipe metadata, API 1

Status: draft. Common rules are in [README.md](README.md) ("common 1.2" and so on). The build
script is specified in `build-lua-api.md`; the hash of the recipe directory in `recipe-hash.md`.

A recipe is a directory with `recipe.toml`, `build.lua` and the files they reference. `recipe.toml`
is data: everything otok needs to know about a recipe before running it — identity, sources,
options, dependencies, platforms, products — so resolution, fetching and the catalog never execute
Lua. `build.lua` only builds.

## 1. Example

```toml
[recipe]
api = 1
library = "openssl"
name = "openssl"
version = "3.5.4"
revision = 1
license = "Apache-2.0"
license-files = ["LICENSE.txt"]
homepage = "https://www.openssl.org"
description = "OpenSSL (libssl, libcrypto), built from source."
keywords = ["tls", "crypto"]

[source]
url = "https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz"
sha256 = "…"
verify-against = { git = "https://github.com/openssl/openssl", tag = "openssl-3.5.4" }
patches = ["patches/0001-ios-no-fork.patch"]

[platforms]
supported = ["ios", "iossim", "macosx", "catalyst", "tvos", "tvossim", "watchos", "watchossim", "xros", "xrossim"]
build-on = ["macos/xcode"]

[vars]
openssl_target = { ios = "ios64-xcrun", iossim-arm64 = "iossimulator-arm64-xcrun", iossim-x86_64 = "iossimulator-x86_64-xcrun", macosx-arm64 = "darwin64-arm64-cc", macosx-x86_64 = "darwin64-x86_64-cc", default = "ios64-xcrun" }

[options.no-deprecated]
type = "bool"
default = false
doc = "Configure with no-deprecated: removes deprecated APIs."

[options.features]
type = "list"
values = ["no-legacy", "no-quic", "no-engine"]
default = []
doc = "Extra Configure flags."

[options.zlib]
type = "bool"
default = false
additive = true
doc = "Build with zlib compression support."

[deps.zlib]
version = "~> 1.3"
default-recipe = "apotocki/zlib"
when = { option = "zlib" }

[[product]]
name = "OpenSSL"
libraries = ["ssl", "crypto"]
headers = "crypto"
defines = [{ define = "OPENSSL_NO_DEPRECATED", when = { option = "no-deprecated" } }]
link = { libraries = ["z"] }
apple = { module = "COpenSSL", frameworks = ["Security"], wrappers = ["OpenSSLSwift"], privacy-manifest = "PrivacyInfo.xcprivacy" }

[apple]
xcode = ">= 16"

[toolchains]
make = ">= 3.81"
perl = ">= 5.30"

[[publish.check]]
options = { no-deprecated = true, features = ["no-legacy"] }
```

## 2. Top-level tables

| Table | Required | Section |
|---|---|---|
| `[recipe]` | yes | 3 |
| `[tool]` | no | 3.1 |
| `[source]` | yes | 4 |
| `[[resource]]` | no | 5 |
| `[platforms]` | yes | 6 |
| `[vars]` | no | 7 |
| `[options.<name>]` | no | 8 |
| `[deps.<library>]` | no | 9 |
| `[tool-deps.<library>]` | no | 10 |
| `[[product]]` | yes (at least one), except in a tool recipe (3.1) | 11 |
| `[apple]` | no | 12 |
| `[toolchains]` | no | 13 |
| `[publish]` | no | 14 |
| `[metadata]` | no | common 1.2; never read |

The recipe directory is described in section 15.

## 3. `[recipe]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `api` | integer | — (required) | Recipe API version: the format of this file and the `build.lua` API. `1` for this document; API 1 is Lua 5.5. |
| `library` | library name | — (required) | The upstream library this recipe builds. |
| `name` | recipe name | — (required) | With the publisher, forms the recipe id `<publisher>/<name>`. Usually equal to `library`; a publisher with two recipes of one library names them apart (`openssl`, `openssl-fips`). The publisher is not in the file: it is the GitHub account publishing it (`index.md` 6). |
| `variant` | recipe name syntax (common 2) | the target of `[platforms] supported` | One of the recipes of one id, each for its own target and environments (6.1): `apple`, `windows-msvc`, `windows-clang-cl`. Each variant has its own revisions, index entries and advisories (`index.md`). Users name the recipe id only; otok picks the variant by the target and environment it builds for. Required when `supported` spans several targets (a local recipe only, 6.1). |
| `version` | version (common 3.1) | — (required) | The upstream version built. |
| `revision` | integer ≥ 1 | — (required) | Recipe revision for this version. |
| `channel` | `"stable"` or `"prerelease"` | `"stable"` | Stability, set from the upstream release (common 3.2); copied into the index entry. |
| `license` | SPDX expression (common 5) | — (required) | License of the library (not of the recipe). |
| `license-files` | array of paths or `{ recipe = "<path>" }` | — (required, non-empty) | License texts for acknowledgements: a path inside the unpacked source, or a file of the recipe directory when the source has no license file of its own (Lua's release archive carries its license only in `lua.h`). Each is packaged as `licenses/<path>` (`otok-toml.md` 4.2), so no two entries may have one path. |
| `description` | string, ≤ 200 characters, one line | — (required) | |
| `homepage` | URL | — | Upstream project page. |
| `documentation` | URL | — | Upstream documentation, linked from the catalog. |
| `keywords` | array of strings, ≤ 10, each `[a-z0-9-]{1,32}` | `[]` | For search. |
| `linkage` | array of `"static"`, `"dynamic"` | `["static"]` | What the build can produce (`otok-toml.md` 5): `build.lua` reads the chosen one as `slice.linkage` (`build-lua-api.md` 3). A project asking for another fails to resolve. |
| `c-standard`, `cxx-standard` | string | — (any) | The C and C++ standards the library builds with, as constraints of two-digit standard numbers (`">= 99"`, `">= 14, <= 20"`; common 3.2 in the `dotted` scheme), compared by year, so that `">= 99"` allows C11. A project asking for another (`otok-toml.md` 5.1) fails to resolve; without the project's, the lowest standard every recipe of the target allows is chosen. Without them, any standard is tried. Not for a recipe that delivers its source. |

### 3.1 `[tool]`

A tool recipe builds programs that other builds run: CMake, Ninja, `gn`. It is a
tool dependency of the recipes that use it (10), usually through a toolchain that names it (13).

```toml
[tool]
programs = ["ninja"]
```

| Key | Type | Meaning |
|---|---|---|
| `programs` | array of program names, `[a-z0-9][a-z0-9_+-]{0,63}`, non-empty, without repeats | — (required). The programs `build` returns (`build-lua-api.md` 8). Every program a toolchain takes from the recipe (13.1, 13.2) is listed. A program may be a script run by a toolchain of the recipe (Meson by `python`).  |

A tool recipe needs no `[[product]]`. Without products it can be a tool dependency only: a recipe or a project that
has it as a dependency fails to resolve. It may have products too, for a library that is also a tool. `otok publish`
builds one without products in the tool slice of the machine, where it checks that `programs` are built.

Tool slices are built by the target of the build machine (`otok-toml.md` 7): `macosx` of `apple` on macOS, `linux`,
`windows`. A tool recipe is of one of them, like any recipe (6.1), so a tool for every machine is three variants:
`supported = ["macosx"]` with `build-on = ["macos/xcode"]`, `["linux"]` with `["linux/gcc", "linux/clang"]`,
`["windows"]` with `["windows/msvc", "windows/clang-cl"]`.

## 4. `[source]`

Exactly one of the two forms:

| Form | Keys |
|---|---|
| Archive | `url` (https), `sha256` (64 lowercase hex digits of the archive file), `mirrors` (optional: array of https URLs of the same file) |
| Git | `git` (https or ssh URL), `commit` (full 40-character SHA), `tag` (optional: the tag of that commit), `submodules` (boolean, default `false`) |

`file://` URLs are accepted too, for local recipes and test fixtures whose sources are vendored next
to them. `otok publish` to an index on GitHub and its CI reject them (a private index published to
through a checkout may use them, on a shared file system).

A git repository may also be named by ssh, as git names it: `ssh://[user@]host[:port]/path`, or the
short form `[user@]host:path` (a `:` before any `/`; a single letter before it is a Windows drive,
not a host). This is for the private repositories of a team's own libraries: git reaches them with
the user's keys and SSH agent (`cache.md` 4), so only those who may read them can build such a recipe.
Archives (`url`, `mirrors`) are https only. `otok publish` to an index on GitHub and its CI reject ssh
locations as they reject `file://`.

`mirrors` are tried in their order when `url` fails: a network or HTTP error, or a file that does not
match `sha256` (`cache.md` 3.1). The hash decides what is built, so a mirror can make an input
reachable but never change it, and mirrors enter no cache key. Boost publishes its archives on
several hosts; a recipe lists them here rather than reading a list from the network, so every
location of an input is in the reviewed recipe. A URL appears once among `url` and `mirrors`.
Projects and machines may add locations of their own, tried before these (`otok-toml.md` 10,
`config.md` 2.1).

The commit is what is built; `tag` only helps. It shows which release the commit is, and otok fetches
the tag with `--depth 1`, which every git server allows, before falling back to fetching the commit by
its hash, which some servers refuse (`cache.md` 4). A tag that exists but points to another commit is
an error ("the tag was moved"), never a reason to build something else. Tag names follow git's rules
for ref names (no spaces, no `..`, none of `~^:?*[\`).

Both forms also take:

| Key | Type | Meaning |
|---|---|---|
| `patches` | array of paths in the recipe directory | Applied in this order to every fresh copy of the source, as `git apply` does (`-p1`, no fuzz). A patch that does not apply cleanly fails the build. |
| `verify-against` | `{ git = "<url>", tag = "<tag>" }` or `{ git, commit }` | Archive form only: `otok publish` and catalog CI compare the archive with the git tree and list files that exist only in the archive or differ: a release archive may carry generated files that the history of the repository does not have. |

An archive whose contents are a single top-level directory is unpacked with that directory stripped;
otherwise the archive root is the source root. Supported archive types: `.tar.gz`/`.tgz`,
`.tar.xz`, `.tar.bz2`, `.tar.zst`, `.zip`, recognized by content, not by name.

With `submodules = true` the submodules are fetched at the commits recorded in the source commit;
git hashes cover them, so no extra hashes are needed.

## 5. `[[resource]]`

Additional hashed inputs: data files, sub-projects, generated files an upstream ships separately.
Nothing else is downloaded for a build.

| Key | Type | Meaning |
|---|---|---|
| `name` | `[a-z][a-z0-9_]{0,63}` | Unique in the recipe; `build.lua` sees the resource as `slice.resources.<name>`. |
| `url` + `sha256` (+ `mirrors`), or `git` + `commit` | as in 4 | Where it comes from. |
| `unpack` | boolean | Archive resources: unpack (default `true`) or keep the file as is. |
| `dest` | path inside the source tree | Optional: place the resource there in every source copy before patching. Without `dest` the resource stays outside the source tree, read-only. |
| `when` | condition with `option`, `host`, `arch` and `environment` only (9.1) | Without `when` every build uses the resource. An inactive resource is not in `slice.resources` and not in the slice key, and is not fetched when it holds for no slice the configuration could build (its `arch` and `environment` taken as holding); `otok.lock` records every resource all the same (`otok-lock.md` 3), so the lock does not depend on options or on the machine. `host` lets a tool recipe of release binaries fetch only the archive of the build machine: `when = { host = ["windows-x86_64"] }`; for Linux, the one of its C library: `["linux-x86_64"]` (glibc) and `["linux-x86_64-musl"]`. A recipe with no archive for a machine builds the program from source there, finding no resource active (`build-lua-api.md` 3, `slice.host_libc`). |

## 6. `[platforms]`

| Key | Type | Meaning |
|---|---|---|
| `supported` | array of platform names (common 4) | — (required, non-empty). Platforms the recipe builds for; they tell its target (common 4). A project asking for another platform fails resolution with this list in the message. |
| `architectures` | table: platform → array of architectures | Restricts architectures per platform (default: all of common 4). A project's `[apple] architectures` must stay within it; otherwise resolution fails and asks the project to restrict its architectures. otok never drops an architecture silently: the missing slice would surface only when the app links. |
| `build-on` | array of environments (common 4.2) | — (required, non-empty, without repeats). Where and with what the recipe builds: `["macos/xcode"]`, `["*/ndk"]`, `["windows/msvc"]`. Each is an environment of the target of `supported`, `*` only where common 4.2 allows it. |

### 6.1 Targets and environments

A recipe is written for one target and the environments it names. Several environments
are for one way of building that serves them all (lz4 for Android on every host: `["*/ndk"]`); a different way of
building is a different recipe of the same id, a **variant** (3):

```toml
# boost/1.92/windows-msvc/recipe.toml: b2 with cl.exe
[recipe]
name = "boost"
variant = "windows-msvc"
[platforms]
supported = ["windows"]
build-on = ["windows/msvc"]

# boost/1.92/windows-clang-cl/recipe.toml: Boost's CMake with clang-cl, across from Linux
[recipe]
name = "boost"
variant = "windows-clang-cl"
[platforms]
supported = ["windows"]
build-on = ["linux/clang-cl"]
```

- The **cells** of a recipe are its platforms times its environments. Variants of one id published for one version
  share no cell (`index.md`): for every platform and environment at most one of them builds.
- A project builds a library in an environment of its own (`otok-toml.md` 4.9) with the variant whose environments
  cover it (common 4.2) and whose platforms hold every platform of the target the project builds; without one, in an
  environment of the same system and ABI some variant covers. A recipe builds where its
  build system works: one that builds with clang-cl only says so, and serves projects of `windows/msvc` all the same.
- `build.lua` sees the environment of the slice (`slice.environment`, `slice.host`, `build-lua-api.md` 3); the
  machines of `*` differ by `slice.host` and by `when = { host = … }` of resources (5).
- A recipe used only by path or git (`otok-toml.md` 6.1) may support platforms of several targets, each built in the
  environments of `build-on` that are its; it then names its `variant`. `otok publish` takes one target only.

## 7. `[vars]`

Per-slice values for `build.lua`, so recipes do not repeat platform tables in
Lua. Each key is a variable name (`[a-z][a-z0-9_]{0,63}`, a valid Lua identifier); each value is a
table from selectors to values:

| Selector | Matches |
|---|---|
| `<platform>-<arch>` | one slice (`iossim-x86_64`) |
| `<platform>` | every architecture of a platform (`iossim`) |
| `<os>` | every slice of a target OS (`apple`) |
| `default` | every slice |

For a slice, the most specific selector wins (slice, then platform, then OS, then `default`). Values
are strings, integers, booleans or arrays of strings; all values of one variable have the same type.
Validation fails if some supported slice has no value for a variable. `build.lua` reads
`slice.vars.<name>`.

There is deliberately no expression language: anything that needs logic is written in `build.lua`.

## 8. `[options.<name>]`

A typed build parameter set by the user.

| Key | Type | Meaning |
|---|---|---|
| `type` | `"bool"`, `"int"`, `"string"`, `"enum"`, `"list"`, `"file"` | — (required) |
| `default` | value of the type | — (required, except for `file`, which takes none: no file). Must itself be valid. |
| `doc` | string | — (required). Shown in the catalog and in error messages. |
| `values` | array of strings | Required for `enum` (allowed values); optional for `list` (allowed elements; without it, any strings). |
| `additive` | boolean | Default `false`. Only for `bool` and `list` (8.1). |

Values:

| Type | TOML value | In `build.lua` |
|---|---|---|
| `bool` | boolean | boolean |
| `int` | integer | integer |
| `string` | string | string |
| `enum` | string from `values` | string |
| `list` | array of strings (elements from `values`, if declared) | array of strings: duplicates removed, ordered as in `values` (or sorted, without `values`) |
| `file` | string: a path in the project (`otok-toml.md` 6.2; 8.3) | the path of its copy in the work directory, or `false` without a file |

The canonical form of a list (deduplicated, ordered) is what option merging and the cache key use,
so `["json", "filesystem"]` and `["filesystem", "json"]` build once.

### 8.1 Merging requests

Several requests for one option — the project's value, `requires-options` of dependents (9),
profiles — are merged before building:

- `additive = true`: `bool` values merge by OR (any request for `true` wins); `list` values by
  union. Use it only when the option only adds something (a component, a feature), so a union is
  safe for every requester.
- otherwise (every `int`, `string`, `enum`, `file`, and non-additive `bool`/`list`): all requests must be
  equal (a `file` option: the same path). Disagreement fails resolution, naming the requesters and their values.

The project's own value of an additive option is part of the union. An option nobody requests takes
`default`.

### 8.2 In wrappers

Option values reach wrapper sources (11, `apple.wrappers`) as Swift active compilation conditions
and C preprocessor definitions. The name is `OTOK_OPTION_` + the option name upper-cased
with `-` replaced by `_`:

| Type | Defined |
|---|---|
| `bool` | `OTOK_OPTION_NO_DEPRECATED` when true |
| `enum` | `OTOK_OPTION_TLS_OPENSSL` (option + value) |
| `list` | one per element: `OTOK_OPTION_LIBRARIES_JSON` |
| `int` | C definition with the value (`OTOK_OPTION_LEVEL=3`); no Swift condition |
| `string` | C definition with the value as a string literal (`OTOK_OPTION_PREFIX="ab"`); no Swift condition |
| `file` | `OTOK_OPTION_DATA_FILTER` when a file is given |

Enum values and list elements are upper-cased the same way, with every character other than a letter or
a digit replaced by `_`.

### 8.3 File options

A `file` option gives the build a file of the consumer's project: a configuration file, a list of what
to include (ICU's data filter). Only the project sets it, in `otok.toml` and its profiles; `requires-options`
(9) and `[[publish.check]]` (14) cannot, since a recipe has no project to take the file from, and
publication builds without one. Its contents, not its path, are the value that counts: they enter the slice
key (`cache-key.md` 3), so an edited file rebuilds and a file moved elsewhere in the project does not. otok
copies the file into the work directory of each slice, where the build reads it (the sandbox keeps the
build out of the project).

## 9. `[deps.<library>]`

A library this recipe links against, built for the same slices.

| Key | Type | Meaning |
|---|---|---|
| `version` | constraint | — (required). |
| `default-recipe` | recipe id | — (required). Used only when the project does not choose a recipe for the library. |
| `prerelease` | boolean | Default `false`; as in `otok-toml.md` 6. |
| `requires-options` | table: option → value | Values this recipe needs from the dependency; merged as in 8.1; no `file` option (8.3). |
| `linkage` | `"static"` or `"dynamic"` | The dependency's linkage when the project chooses none for it in its `[deps.<library>]` (`otok-toml.md` 5): a recipe whose own library is one DLL with its dependencies inside (Cairo with Pixman and FreeType) asks for `"static"`, the project may still choose otherwise. Dependents asking for different ones fail resolution unless the project chooses. Not in `[tool-deps]`: tools are static. |
| `when` | condition (9.1) | Without `when` the dependency is unconditional. |
| `toolchains` | array of program names (3.1) | Programs of this dependency the build runs: the recipe and version the dependency got, built for the build machine as well (13.2). Not in `[tool-deps]`. |

### 9.1 Conditions

`when` is a table; all its keys must hold. It is evaluated per slice.

| Key | Holds when |
|---|---|
| `option` alone | the named option of **this** recipe is a `bool` and true, or a `file` and a file is given |
| `option` + `has` | the named `list` option contains `has` |
| `option` + `is` | the named `enum`, `string` or `int` option equals `is` |
| `os` | the slice's target is in the array (`["apple"]`); of use in a recipe of several targets (6.1) |
| `platform` | the slice's platform is in the array (`["ios", "iossim"]`) |
| `host` | the build machine, the one otok runs on, is in the array: `<os>-<arch>` with `<os>` one of `windows`, `macos`, `linux` and `<arch>` one of `x86_64`, `arm64` (`["macos-arm64", "macos-x86_64"]`); a Linux machine whose C library is musl (Alpine) is `linux-<arch>-musl` (`"linux-x86_64-musl"`), and `linux-<arch>` is one of glibc, so that a release binary built for glibc is never fetched where it cannot run. Only in `when` of `[[resource]]` (5) |
| `arch` | the slice's architecture is in the array (`["arm64"]`, Android's `["arm64-v8a"]`). Only in `when` of `[[resource]]` (5) and `[toolchains]` (13), not under `not` |
| `environment` | an environment in the array covers the slice's (common 4.2, as `build-on` does): `["windows/msvc"]`, `["*/ndk"]`. Only in `when` of `[[resource]]` (5) and `[toolchains]` (13), not under `not` |
| `not` | the condition it holds (a table of these keys) does not hold: `{ not = { option = "data-filter" } }` |

`arch` and `environment` decide what a slice needs, an input or a tool, never the edges of a graph, which the resolver
resolves per platform and environment and not per architecture. The resolver resolves the
tool dependency of a toolchain whose `when` tests them wherever it may hold, its `arch` tests holding there, so the lock
may name a tool that no slice of the project builds; the builder decides per slice. Under `not` such a test would turn
"may hold" into "never", so it is not allowed there: list what holds instead.

Conditions are data, not code, on purpose: the resolver, index CI and the catalog read them without
executing the recipe, and the resolver knows which options affect which edges of the graph (needed
to explain conflicts and to guarantee that option resolution terminates). Arbitrary code here is
what makes Conan's graphs hard to lock and impossible to show without running recipes. The language
stays as small as Cargo's `cfg()`: a conjunction and `not = {…}`, which ICU's recipe needs to fetch the prebuilt data
only without a filter; if real recipes need more, `any = [{…}, {…}]` may be added (a combinator over the
same predicates, still data); nothing beyond that.

The option values are the merged ones (8.1). A condition that depends on an option makes
resolution iterate: the merged options decide which dependencies exist, whose `requires-options`
may change merged options in turn (`otok-lock.md` describes the fixed point and its termination).

## 10. `[tool-deps.<library>]`

A library built for the tool slice whose tools, headers or libraries the build uses.
Keys as in 9, except that `when` takes only `option` conditions (the tool slice is fixed). Tool
dependencies never reach the output packages. Its headers and libraries are `deps.tools.<library>` of `build.lua`
(`build-lua-api.md` 4). A toolchain with `default-recipe` is one too (13.1), and so is a program of a dependency
(13.2); each tool dependency is resolved for this recipe alone (`otok-lock.md` 5.7).

## 11. `[[product]]`

What the generated package exposes: a linkable unit built from one or more of the libraries that
`build.lua` returns.

| Key | Type | Meaning |
|---|---|---|
| `name` | product name (common 2) | — (required). Unique in the recipe. The SPM product name on Apple. |
| `libraries` | array of library output names | — (required). Keys of the `libraries` table returned by `build.lua`; on Apple, one XCFramework each (`openssl_ssl.xcframework`, `otok-toml.md` 4). Names: `[A-Za-z0-9_+-]{1,64}`, unique in the recipe only: the target names its output so that the libraries of different recipes do not collide. Empty for a product of headers only (11.1). |
| `headers` | library output name, or `false` | The library whose output carries the public headers (and, on Apple, the module map). Default: the first of `libraries`. `false`: the product has no headers of its own and gets them through `depends` (11.1). |
| `depends` | array of product names | Other products of this recipe that this one needs (Boost: `filesystem` needs `system`). |
| `defines` | array of strings or `{ define, when }` | Preprocessor definitions consumers compile with, so they see the configuration the library was built with (`OPENSSL_NO_DEPRECATED`, `LEVEL=3`). `when` takes option conditions (9.1). On Apple they reach consumers through the module (`otok-toml.md` 4). |
| `link` | table (below) | What consumers must link besides the libraries: system libraries and the C++ standard library. |
| `when` | condition (9.1), options only | The product exists only when the condition holds (Boost: a product per selectable library). |
| `alternatives` | group name (`[A-Za-z_][A-Za-z0-9_]*`) | The product is one of a group of products that exclude each other: a consumer links one of them (11.2). |
| `apple` | table (below) | Apple-only settings. |

`link` keys:

| Key | Type | Meaning |
|---|---|---|
| `libraries` | array of strings or `{ library, when }` | System libraries consumers must link, by the name the linker takes (`z` for `-lz`, `ws2_32` for `ws2_32.lib`). `when` takes `option` and `os` conditions (9.1): `{ library = "ws2_32", when = { os = ["windows"] } }` in a recipe of several targets (6.1). Not the C++ standard library, whose name depends on the toolchain: `runtime`. |
| `runtime` | `"c++"` | The product's libraries need the C++ standard library. The target names it (below). |

The C++ standard library is not one name: libc++ (`c++`) on Apple platforms, libstdc++ (`stdc++`) with GCC and with
clang on Linux, `c++_shared` or `c++_static` with `c++abi` on Android as the project's `stl` chooses (`otok-toml.md`
4.7), and nothing to name with MSVC's STL, which the objects ask for themselves (`/DEFAULTLIB`). A recipe says that its
libraries need it, `runtime = "c++"`, and never names it: `c++`, `stdc++`, `c++_shared`, `c++_static` and `c++abi` in
`libraries` are errors. On Apple platforms the shim links `c++` (`otok-toml.md` 4); elsewhere `otok metadata` reports
the runtime (`metadata.md`), and a consumer linking with a C++ driver gets its own toolchain's.

`apple` keys:

| Key | Type | Meaning |
|---|---|---|
| `module` | module name | Clang module name for the headers (`COpenSSL`). Default: the product name. |
| `frameworks` | array of strings | System frameworks consumers link (`Security`). |
| `wrappers` | array of directory names in `wrappers/` | SPM source targets compiled in the consumer's build. |
| `privacy-manifest` | path in the recipe directory, or `"none"` | `PrivacyInfo.xcprivacy` packaged as a resource. `"none"` states that the product neither tracks nor collects data: otok writes that manifest, which declares no required-reason API (the check of `otok-toml.md` 4.4 still applies). Optional for local and git recipes; `otok publish` requires every product to carry one, its own or that of a product it `depends` on, whose shim it holds (`otok-toml.md` 4). |

`link`, `apple.frameworks` and `apple.privacy-manifest` exist on Apple platforms because a binary target carries neither
linker settings nor resources; the packager emits a shim target that does (`otok-toml.md` 4).

### 11.1 Headers shared by several products

A library such as Boost has one set of headers for many compiled libraries, and most of it needs no
compiled library at all. Its recipe describes the headers as a product of headers only, and the compiled
libraries as products without headers of their own that depend on it:

```toml
[[product]]
name = "Boost"
libraries = []                  # headers only

[[product]]
name = "BoostFilesystem"
libraries = ["filesystem", "atomic"]
headers = false                 # the headers come from "Boost"
depends = ["Boost"]
when = { option = "libraries", has = "filesystem" }
```

- A product of headers only (`libraries = []`) takes the `headers` directory that `build` returned,
  which must then be one directory (`build-lua-api.md` 8); it takes no `headers` key. It may still
  have `link` (Boost: `runtime = "c++"`), `apple.frameworks` and a privacy manifest. On Apple the headers are the same in every
  slice of every platform, and the product has no Clang module: it takes no `defines`, and on Apple no
  `apple.module` or `apple.wrappers` (`otok-toml.md` 4).
- A product with `headers = false` carries no headers; `libraries` must not be empty.
- A product that names a library besides a product of headers only must say `headers = false` when the
  build returns one headers directory: otherwise the library's XCFramework carries the same headers
  again, and packaging for Apple fails on the header paths found twice.

### 11.2 Products that exclude each other

Some libraries come as variants of one interface, of which a program links one: Boost.Stacktrace's backends
(`stacktrace_basic`, `stacktrace_noop`) define the same functions, and so do the variants of Boost.Test
(`unit_test_framework`, `test_exec_monitor`, `prg_exec_monitor`). Their products name one group:

```toml
[[product]]
name = "BoostStacktraceBasic"
libraries = ["stacktrace_basic"]
headers = false
depends = ["Boost"]
alternatives = "stacktrace"

[[product]]
name = "BoostStacktraceNoop"
libraries = ["stacktrace_noop"]
headers = false
depends = ["Boost"]
alternatives = "stacktrace"
```

- Two products of one recipe exclude each other when they are different products of one group, or when the
  products they hold through `depends` (transitively, themselves included) contain two such products.
- The symbols their libraries both define are no overlap (`otok-toml.md` 4.5): the libraries never meet in one
  image. A library both hold (Boost.Test's variants and `exception`) is not compared with itself.
- A group is named by at least two products of the recipe, and no product holds two products of one group through
  `depends`.
- Nothing prevents a consumer from linking two of them; the linker then reports the symbols defined twice, or takes
  one definition for every caller.

## 12. `[apple]`

| Key | Type | Meaning |
|---|---|---|
| `xcode` | constraint on the Xcode version (`dotted`) | Checked before building. |
| `swift-tools-version` | version | Minimum Swift tools version the wrappers need; the generated `Package.swift` uses at least this. |
| `swift-package` | `"."` or a directory inside the source | The recipe delivers this SwiftPM package of the source, patched, instead of building anything (12.1). |

### 12.1 SwiftPM packages

A library written in Swift is built by SwiftPM in the consumer's Xcode; otok does not build it, but it
can deliver it: fetched and checked by its hash like any source, patched by the recipe, and declared in
the generated `Package.swift` (`otok-toml.md` 4.6). This keeps the fixes of an unmaintained package in a
recipe instead of a fork of its repository.

```toml
[recipe]
api = 1
library = "updates"
name = "updates"
version = "2.0.2"
revision = 1
license = "MIT"
license-files = ["LICENSE"]
description = "Checks the App Store for a newer version of the app."
homepage = "https://github.com/rwbutler/Updates"

[source]
git = "https://github.com/rwbutler/Updates"
commit = "388bc6369ca5b83a41b77743d8a62902b6ff7ef3"
tag = "2.0.2"
patches = ["patches/0001-itunes-https.patch"]

[platforms]
supported = ["ios", "iossim", "macosx", "catalyst"]
build-on = ["macos/xcode"]

[apple]
swift-package = "."

[[product]]
name = "Updates"
```

- The recipe has no `build.lua`: nothing is built, and the patched source is cached once for all platforms
  (`cache-key.md` 3.1).
- Each `[[product]]` names a library product of the package's own `Package.swift`, with no other keys.
- Nothing a build needs: no `[[resource]]`, `[vars]`, `[options]`, `[deps]`,
  `[tool-deps]`, `[toolchains]` or `linkage`, and only platforms of the Apple target.
- The package's `Package.swift` must not depend on other packages (`.package(…)`): SwiftPM would fetch
  them from their own locations, at versions the lock does not pin. Packaging fails on such a package.
- `swift-tools-version` is for wrappers and is not allowed beside `swift-package`.

## 13. `[toolchains]`

Programs the build runs besides the platform's compiler. Keys are program names (3.1): one built by a
recipe (`default-recipe`, 13.1), or, without it, one of the names below, found installed. Values are a constraint string
or a table:

```toml
[toolchains]
perl = ">= 5.30"
cmake = ">= 3.25"
rust = { version = ">= 1.80", targets = ["aarch64-apple-ios", "aarch64-apple-ios-sim"] }
python = { version = ">= 3.9", when = { option = "data-filter" } }
ninja = { version = ">= 1.10", default-recipe = "apotocki/ninja" }
flatc = { version = ">= 25", default-recipe = "apotocki/flatbuffers" }
```

A program of a library the recipe links is named in that dependency instead (13.2). A program name is declared once:
in `[toolchains]` or in one dependency's `toolchains`.

| Key (table form) | Meaning |
|---|---|
| `version` | constraint on the tool's version |
| `targets` | `rust` only: required `rustup` targets |
| `when` | condition with `option`, `os`, `arch` and `environment` (9.1; toolchains are checked per target, so not `platform`): without it the build always needs the toolchain; a slice where it does not hold gets no `slice.toolchains.<name>` and has none in its key; one whose `option` or `os` test fails is not looked for at all. `os` lets a recipe that builds one target differently need a tool for that target only (`cmake = { version = ">= 3.20", when = { os = ["windows"] } }`); not with `default-recipe`, whose tool dependency is resolved for the build machine. `arch` and `environment` let a recipe of several environments or architectures need a tool for some of them, also with `default-recipe`: `perl = { version = ">= 5.30", default-recipe = "apotocki/perl", when = { arch = ["arm64"], environment = ["windows/msvc"] } }` (gas-preprocessor of dav1d) |
| `default-recipe` | recipe id: a tool recipe (3.1) that builds the toolchain (13.1). Not with `targets`. Required in a published recipe (13) |

Names otok finds installed in API 1 (without `default-recipe`), with the programs it looks for (in this order) and how
it reads the version:

| Name | Programs | Version command | Output must contain |
|---|---|---|---|
| `autoconf` | `autoconf` | `--version` | |
| `automake` | `automake` | `--version` | |
| `cmake` | `cmake` | `--version` | |
| `gn` | `gn` | `--version` | |
| `go` | `go` | `version` | |
| `libtool` | `glibtool`, `libtool` | `--version` | `GNU libtool` |
| `make` | `gmake`, `make` | `--version` | `GNU Make` |
| `meson` | `meson` | `--version` | |
| `nasm` | `nasm` | `-v` | |
| `ninja` | `ninja` | `--version` | |
| `perl` | `perl` | `--version` | |
| `pkg-config` | `pkg-config`, `pkgconf` | `--version` | |
| `python` | `python3`, `python` (on Windows `python`, `python3`: installers make `python.exe` only) | `--version` | |
| `rust` | `rustc` | `--version` | |
| `zig` | `zig` | `version` | |

A newer otok may add names without changing the API.

- **Finding.** Each program is looked up in the `PATH` the target starts its builds with (on Windows
  the Visual Studio environment, which has its CMake and Ninja; on Apple the Xcode and system
  directories; for Android the NDK's), then in otok's own `PATH`;
  its first occurrence counts. A recipe is checked once per
  target it builds for. The version is the first run of digits and dots with at least one dot
  (`N.N`, `N.N.N`, …) in what the version command prints (standard output and standard error), read
  in the `dotted` scheme (common 3); for `gn`, which has no releases, the first run of digits (its
  commit count, `2257` of `2257 (fde0b7b)`). The first program that runs, prints what the table requires and
  satisfies `version` is chosen; one that does not is passed over. `targets` of `rust` are checked
  with `rustup target list --installed` (`rustup` from the same `PATH`).
- **When.** Before anything is fetched or built, for every recipe the selected configuration builds,
  tool dependencies included, also when their slices are in the cache. Every missing or unsuitable
  toolchain is reported at once, with what was found:
  `toolchain 'meson' (>= 1.5), needed by 'dav1d': not found in PATH`,
  `toolchain 'cmake' (>= 3.25), needed by 'x': found 3.22.1 at '/usr/bin/cmake'`.
- **Use.** The chosen program is `slice.toolchains.<name>` (`build-lua-api.md` 3), and its directory
  is in the `PATH` of the build (`build-lua-api.md` 5). The helpers run the chosen programs and fail
  when the recipe does not declare what they run (`build-lua-api.md` 6.2). The versions found are
  part of the slice key (`cache-key.md` 3), so a new version of a tool rebuilds what it built.

A toolchain without `default-recipe` is found installed, as above. Only a recipe not published may name one:
`otok publish` and `otok index-check` refuse it (`index.md` 9), so that what builds for the
publisher builds for everyone, whatever the publisher has installed. The compiler of the build environment is not a toolchain
(common 4.2): the target finds it (`config.md` 2).

### 13.1 Toolchains built by recipes

A toolchain with `default-recipe` is also a tool dependency (10) of the recipe on the library of the recipe it names
(`apotocki/flatbuffers` builds `flatbuffers`), with the toolchain's `version` (any without it) and `when`.
It is resolved for this recipe alone and locked (`[[tool-package]]`, `otok-lock.md` 5.7) as any tool dependency, so
the project chooses another recipe or version for that library in `[tool-deps.<library>]` (`otok-toml.md` 7). The
recipe chosen is a tool recipe (3.1) whose `programs` list the key; otherwise resolution fails. Several keys may name one recipe (`ctest = { default-recipe = "apotocki/cmake" }` beside
`cmake`): one tool dependency, with all their constraints. A recipe has one tool dependency on a library: a toolchain
whose recipe builds a library the recipe also has in `[tool-deps]`, or as a dependency with `toolchains` (13.2), fails
resolution.

The recipe is built in the tool slice before the recipes that need it, and nothing is looked for. Its
program is `slice.toolchains.<name>`, its directory is in the `PATH` of the build, and the slice key has the tool
slice's key (`tool.<library>`) instead of a version found (`cache-key.md` 3). A tool dependency of a toolchain is not in
`deps.tools` of `build.lua`.

### 13.2 Programs of a dependency

A program whose output must match a library the recipe links (`flatc` and the flatbuffers runtime, `protoc` and
protobuf) is named in that dependency (9):

```toml
[deps.flatbuffers]
version = ">= 23"
default-recipe = "apotocki/flatbuffers"
toolchains = ["flatc"]
```

The dependency is then also a tool dependency of the recipe on the same library, with the recipe and the version the
dependency got in the graph (`= 23.5.26`), built for the build machine in the variant of its system (that variant's
latest revision) (`otok-lock.md` 5.7). Each name is `slice.toolchains.<name>` where the dependency's `when` holds, as
13.1 says otherwise; the recipe's `[tool] programs` must list it. A recipe that is a library and a tool at once
builds its programs where `slice.is_tool` holds (`build-lua-api.md` 3).

## 14. `[publish]`

| Key | Type | Meaning |
|---|---|---|
| `check` | array of tables `{ options = { … } }` (written `[[publish.check]]`) | Option sets `otok publish` builds in addition to the defaults; no `file` option (8.3). |
| `<target>` (`apple`, `android`, `windows`, `linux`) | table | Keys of the target table of the project `otok publish` builds with (`otok-toml.md` 4), over the target's defaults below: `deployment` for `apple`, `cflags`, `configurations`, `linkage`, … `platforms`, `architectures`, `build-on` and `output` are not allowed: publication builds every supported platform and architecture in the environments this machine has. |

`otok publish` builds the recipe as the only dependency of a project of its own, whose
dependencies come from the index and from the recipes published with it. Its target table holds every supported
platform, architectures as `[platforms] architectures` restricts them, and as `build-on` one environment of the recipe
that the publishing machine has (common 4.2): a project for each of them, since a machine builds a target in one; with
none, publication fails before building (`--no-build` publishes without a build). A tool recipe is built in the tool
slice of its target, the environment as `[otok] tool-build-on`. The build is the publisher's own check: the entry records
nothing of it. The defaults of the target tables:

| Target | Defaults |
|---|---|
| `apple` | `deployment = { ios = "15.0", macos = "12.0", tvos = "15.0", watchos = "26.0", visionos = "1.0" }`: watchOS 26, since Apple Watch `arm64` exists only from there and publication builds every architecture; a recipe whose `[platforms] architectures` keeps watchOS to `arm64_32` may deploy lower |
| `android` | `deployment = { android = "24" }` |
| `windows`, `linux` | none |

A recipe that needs newer systems says so: `[publish.apple] deployment = { ios = "16.0" }` (tables are merged
with the defaults key by key, other values replace them).

## 15. The recipe directory

```text
openssl/
├── recipe.toml
├── build.lua
├── README.md                 # optional; rendered on the catalog page
├── PrivacyInfo.xcprivacy
├── patches/
│   └── 0001-ios-no-fork.patch
└── wrappers/
    └── OpenSSLSwift/
        └── TLSContext.swift
```

What a recipe directory holds is text: `recipe.toml`, `build.lua`, patches, wrapper sources
(11, `apple.wrappers`), a privacy manifest, a hand-written module map, build files for a library without a
usable build system (`CMakeLists.txt`, `Makefile`), helper scripts run through `otok.run`, a README.

Limits, checked by `otok publish` and index CI (warnings for local and git recipes):

- every file is UTF-8 text without NUL bytes;
- at most 1 MiB in total and at most 256 files;
- no symbolic links; file modes are not significant;
- path components of `[A-Za-z0-9._+-]`, not starting with `-`, and no two paths that differ only in
  case, so a recipe checks out identically on every host (`recipe-hash.md` 3).

Binary data belongs to the library, never to the recipe: it comes with the source or as a
`[[resource]]` with a URL and a hash. A library that has to be patched with binary data before
building is forked, the data is committed to the fork, and a text recipe builds the fork. This keeps
every recipe readable on the catalog page (common 1.6) and closes the gap of a prebuilt
library shipped inside a recipe and copied into `slice.prefix`.

All of these files, and only these, are covered by `recipe-sha256` (`recipe-hash.md`).

## 16. Validation summary

Before resolution: syntax, unknown keys, types; `api` known; names; version and constraints;
`revision ≥ 1`; `linkage` non-empty without repeats; one `[source]` form; hashes and SHAs well-formed; option schemas and their defaults;
`additive` only on `bool`/`list`; no `default` for `file`; `when` refers to existing options of the right type, and tests only options
on `[tool-deps]`, product `when` and `defines`, only options and `os` on `[toolchains]` (`os` not with `default-recipe`) and
`link.libraries`, only options and `host` on `[[resource]]`, `host` nowhere else; `link.libraries` names no C++ standard library
(`runtime = "c++"` instead, 11); `requires-options` and `[[publish.check]]` set no `file` option; `[vars]` cover
every supported slice with one type per variable; `build-on` non-empty, without repeats, environments of the targets of
`supported` and each such target with one, `*` only for `ndk`; `variant` given when `supported` spans several targets;
products: unique names, `depends` and `headers`
refer to existing products and libraries, no dependency cycles, a product of headers only without
`headers` or `defines`, `headers = false` only with libraries, an `alternatives` group named by two products or more and
held twice by no product (11.2); at least one product unless `[tool]` is there; `[tool] programs`
names without repeats; `default-recipe` of a toolchain without `targets` and not beside a tool dependency on the same library; referenced files (patches,
wrappers, privacy manifest, license files of the recipe directory) exist; `license-files` paths are
unique; directory limits (15).

Names of platforms, architectures, environments and targets are checked against those this otok knows in a recipe by
`path` and when publishing. In a recipe read from an index or a git repository, which a newer otok may have written,
an unknown one is a name that never matches (README 1.5, 4.1): `supported` and `archs`
keep the known ones, a `when` or `not` testing it is false, `build-on` cannot build there. An unknown `api` is an error
in a recipe by `path` or from a git repository, saying that it needs a newer otok; an index entry of an unknown `api`
is left aside (`index.md` 3.2).

After fetching: the `license-files` of the source exist in it; patches apply.

After building a slice: every library named by a product whose `when` holds was returned by
`build.lua`; the programs it returns are those of `[tool] programs`, files in `slice.prefix`.

A recipe that delivers its source (12.1): no `build.lua`; no `[[resource]]`, `[vars]`, `[options]`,
`[deps]`, `[tool-deps]`, `[toolchains]`, `[tool]` or `linkage`; products with a name only; platforms
of the target that takes the source. When packaging: the directory holds a `Package.swift` without `.package(…)`.

When publishing: `supported` of one target; variants of the id published for the version share no cell (6.1,
`index.md`).
