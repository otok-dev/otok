# `config.toml` — machine settings

Status: draft. Common rules are in [README.md](README.md).

Settings that belong to the machine, not to a project (DESIGN 5.8). The rule that keeps DESIGN
principle 8 intact: **machine settings never change what is built.** None of them enters the build
cache key (`cache-key.md`); anything that would change the output is accepted only in `otok.toml` or
a recipe.

## 1. Location

| Host | File |
|---|---|
| macOS, Linux | `$XDG_CONFIG_HOME/otok/config.toml`, default `~/.config/otok/config.toml` |
| Windows | `%APPDATA%\otok\config.toml` |

`OTOK_CONFIG` names another file. A missing file means all defaults.

## 2. Keys

| Key | Type | Default | Environment | Meaning |
|---|---|---|---|---|
| `jobs` | integer ≥ 1 | number of logical CPUs | `OTOK_JOBS` | Total build parallelism: the size of the jobserver (DESIGN 6.4). |
| `cache-dir` | path | macOS `~/Library/Caches/otok`, Linux `$XDG_CACHE_HOME/otok`, Windows `%LOCALAPPDATA%\otok\cache` | `OTOK_CACHE_DIR` | Downloads, recipe trees, index files and builds (DESIGN 5.7). |
| `index` | URL, or `""` | `view+https://otok-dev.github.io/index`, the public index (`otok-dev/index`) | `OTOK_INDEX` | The recipe index `recipe = "publisher/name"` reads (`index.md`), which projects call `default` beside the indexes they add (`otok-toml.md` 6.4): its HTTPS view as `view+https://<base>` (`index.md` 8), or the URL of its git repository (`https`, or `file` for one on this machine), read as `index.md` 8.1 says. `""` (also `OTOK_INDEX` set but empty) is none: such recipes are then an error. `otok publish` publishes to it when `--index` is not given (DESIGN 7.4); for a view, to the `repository` of its `index.toml`. |
| `index-mirror` | URL or path | — | `OTOK_INDEX_MIRROR` | Read the index of `index` (`default`) from elsewhere: an `https` URL is a copy of the HTTPS view at that base; a directory holding `index/config.json` is a view on disk (`index.md` 8); any other directory is a checkout of the index repository, read as it is (`index.md` 8.1). |
| `asset-mirror` | URL template, or array of them | — | `OTOK_ASSET_MIRROR` (templates separated by spaces) | Mirrors of archive inputs addressed by their sha256 (2.1). |
| `url-rewrite` | array of tables `{ from, to }` | — | — | Other locations of the inputs under a URL prefix (2.1). |
| `mirrors-only` | boolean, or array of library names | `false` | `OTOK_MIRRORS_ONLY` (a boolean) | Fetch inputs only from this machine's `asset-mirror` and rewritten URLs, never from the locations recipes and projects name (2.1). |
| `proxy` | URL | from `HTTPS_PROXY` / `HTTP_PROXY` | `OTOK_PROXY` | HTTP(S) proxy for all downloads. |
| `cache-max-age` | integer ≥ 0 (days) | `30` | `OTOK_CACHE_MAX_AGE` | Cache entries unused for longer are removed by the automatic garbage collection (`cache.md` 9); `0` turns it off. |
| `sandbox` | boolean | `true` | `OTOK_SANDBOX` | Run build processes in the host's sandbox (DESIGN 6.5): `sandbox-exec` on macOS, `bwrap` (bubblewrap) of `PATH` on Linux, which otok tries once per command; none on Windows (`docs/PLAN.md` decision 73). `false` is for diagnosing a tool the sandbox stops, and every build warns about it; results are the same, so it is not part of any cache key. |
| `keep-build-dirs` | boolean | `false` | `OTOK_KEEP_BUILD_DIRS` | Keep the whole work directories of failed builds for inspection, not only their logs (in `<cache-dir>/failed/`, `cache.md` 7.1); `--keep-build-dirs` asks for it in one run. |
| `visual-studio` | string | the newest installation with the VC tools | `OTOK_VISUAL_STUDIO` | Windows, the environments `windows/msvc` and `windows/clang-cl` (common 4.2): which Visual Studio provides MSVC, or the CRT and SDK for clang-cl — a major version (`"17"`, `"18"`), a product year (`"2022"`, `"2026"`) or an installation path. Ignored when otok runs in a developer prompt, whose environment is used as it is. Like `PATH`, it chooses an installed toolchain; the toolchain's own version is what enters the cache key (`cache-key.md` 2). |
| `xcode` | path | the selected Xcode (`xcode-select -p`) | `OTOK_XCODE` | Apple target: which Xcode builds — `Xcode.app` or its `Contents/Developer` directory. `DEVELOPER_DIR` in the environment takes precedence, as for Apple's own tools. Like `visual-studio`, it chooses an installed toolchain; its build version and the SDK versions enter the cache key (`cache-key.md` 2). |
| `android-ndk` | path | `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`, else the newest `ndk/<version>` of the Android SDK (`ANDROID_HOME`, `ANDROID_SDK_ROOT`, else `~/Library/Android/sdk`, `~/Android/Sdk` or `%LOCALAPPDATA%\Android\Sdk`) | `OTOK_ANDROID_NDK` | Android target: which NDK builds. Like `xcode`, it chooses an installed toolchain; the NDK's revision (`Pkg.Revision` of its `source.properties`) enters the cache key (`cache-key.md` 2). |
| `windows-sdk` | path | none | `OTOK_WINDOWS_SDK` | Windows target in `linux/clang-cl` (common 4.2): the CRT and the Windows SDK, the output directory of `xwin --accept-license splat` (`--include-debug-libs` for `debug` slices, `--arch x86_64,aarch64` for both architectures), which the user makes, accepting Microsoft's license. Their versions (`crt/include/crtversion.h`, the `10.0.<build>` entry of `sdk/include`) and clang-cl's enter the cache key (`docs/PLAN.md` decision 75). |
| `llvm` | path | clang-cl of `PATH` (`clang-cl`, else the newest `clang-cl-<version>`), on Windows else that of the Visual Studio of `visual-studio` (`VC/Tools/Llvm`) | `OTOK_LLVM` | Windows target in `windows/clang-cl` and `linux/clang-cl` (common 4.2): the LLVM installation whose `bin/clang-cl` builds, with lld-link and LLVM's tools beside it (`"C:/Program Files/LLVM"`). Like `visual-studio`, it chooses an installed toolchain; clang-cl's version enters the cache key. |
| `git` | path | `git` of otok's `PATH` | `OTOK_GIT` | The git program otok fetches git sources, resources, recipes and the index with (`"C:/Program Files/Git/cmd/git.exe"`), for a machine whose `PATH` has none. It runs with otok's own environment. |
| `color` | `"auto"`, `"always"`, `"never"` | `"auto"` | `OTOK_COLOR` | Colored terminal output. |

Precedence: command-line flag, then environment variable, then the file, then the default.

The programs of recipes' `[toolchains]` are not machine settings: published recipes build them from their recipes
(`default-recipe`, `recipe-toml.md` 13.1), and a local recipe's other toolchains are found in the target's and otok's
`PATH`. `tool-paths` and `toolchains` of earlier versions are gone (`docs/PLAN.md` decision 85): the file is refused
with them, naming what replaces them.

### 2.1 Where inputs are fetched from

Recipes name where their archives and git repositories are (`recipe-toml.md` 4, 5); a machine may know
better places: a company's artifact store, a proxy of the upstream hosts, a directory of an air-gapped
network. Every input is still checked against its sha256 or commit, so these settings change where the
bytes come from, never what is built, and they enter no cache key.

```toml
asset-mirror = ["https://artifacts.example.com/otok/{sha256}", "file:///Volumes/mirror/{sha256}"]

[[url-rewrite]]
from = "https://archives.boost.io/release/{version}/source/{file}"
to = "https://artifacts.example.com/boost/{version}/{file}"
exact = true

[[url-rewrite]]
from = "https://github.com/{owner}/{repo}"
to = "https://git.example.com/{owner}-{repo}"
```

- `asset-mirror`: URL templates (`https`, `http` or `file`) containing `{sha256}`, which is replaced by
  the 64 hex digits of an archive input. Such a mirror is a plain directory of files named by their
  hash, like otok's own `downloads/sha256/` (`cache.md` 3), which can fill it. Git inputs have no file
  hash and are not looked up there.
- `url-rewrite`: rules `{ from, to, exact }` that give a URL another location (2.2). It applies to
  every URL otok fetches inputs from: archive URLs and their `mirrors`, git repositories of sources,
  resources and recipes (`recipe-source`), and submodules, and to the commits of a git `index` (not to
  the request for its `HEAD`); the index is read elsewhere through `index-mirror`. A git repository
  keeps the cache entry of its original URL (`cache.md` 4).
- `mirrors-only = true`: only the machine's locations are tried, never those of recipes or of a
  project's `[fetch]` (`otok-toml.md` 10), so nothing is fetched from hosts the machine's owner has
  not named. An input that neither an asset mirror nor a rewrite of the machine covers fails, naming
  it. This is for networks where only approved hosts may be reached, and for making sure that a
  mirror really holds everything. An array of library names does the same for the inputs of those
  libraries only (their sources and resources; recipes fetched from git count as inputs of their
  library).

A project may name locations of its own in `[fetch]` (`otok-toml.md` 10), tried before the machine's.
The order is in `cache.md` 3.1 and 4.

### 2.2 URL rewrite rules

`from` is a URL pattern: literal text with placeholders `{name}`, which match one or more characters
other than `/`, and `{name*}`, which match one or more characters including `/`. Names are
`[a-z][a-z0-9_]*`. Without `exact`, the pattern matches the beginning of a URL, and the rest of the URL
is appended to the result; with `exact = true` it must match the whole URL. A placeholder takes as much
as it can while the rest of the pattern still matches. `to` is the new URL, in which each `{name}` of
`from` is replaced by what it matched (written `{name}` there, with or without `*` in `from`), and
`{sha256}` by the hash of an archive input; a rule whose `to` has `{sha256}` does not apply to git URLs.

Rules are tried in the order they are written, and the first that matches applies, once: write the
specific ones before the general ones. A pattern without placeholders is a plain prefix:
`from = "https://github.com/"`, `to = "https://git.example.com/github/"`.

Checked when the file is read: each name appears once in `from`; two placeholders are separated by
literal text (`{a}{b}` could be split in many ways); `to` uses only names of `from`, and `sha256`;
`sha256` is not a name in `from`; braces are paired. URLs cannot contain braces unencoded, so a literal
brace needs no escape.

## 3. Strictness

An unknown key is a **warning** here, not an error (an exception to common 1.2): one machine may run
several otok versions, a newer key must not break an older otok, and no machine setting can change
the output. A wrong type is still an error.
