# Cache directory, layout 1

Status: draft.

Everything otok downloads, unpacks and builds lives in `cache-dir` (`config.md`; macOS
`~/Library/Caches/otok`). It is shared by every project and every otok process on the machine, and it
can be deleted at any time: otok fetches and builds again what is missing. Nothing in it is an input
of its own; every entry is derived from hashed inputs (common 1.6), which is what makes
sharing safe.

## 1. Layout

```text
<cache-dir>/
├── CACHEDIR.TAG                      # marks the directory as a cache for backup tools (2)
├── layout                            # "1\n": the version of this document
├── downloads/sha256/<sha256>         # downloaded files, by content (3)
├── git/<url key>/                    # bare repositories of git inputs (4)
├── index/<url key>                   # the commit of a git index last read (10)
├── view/<url key>/<path>             # files of an index view and their ETag (11)
├── sources/
│   ├── archive-<sha256>/             # an unpacked archive (5)
│   └── git-<commit>[-submodules]/    # the tree of a commit, with its submodules when asked (5)
├── builds/<library>/<version>-r<revision>/
│   ├── slices/<slice key>/           # cache-key.md; prefix/, result.toml, key.txt, build.log (7.1)
│   ├── sources/<sources key>/        # cache-key.md 3.1; source/ (patched) and key.txt
│   └── packages/<package key>/       # cache-key.md 4; the packaged output and key.txt
├── dev/<project key>/<library>-<slice>/  # the kept work directories of dev slices (otok-dev-toml.md 4)
├── failed/<library>-<slice>-<time>/  # what failed builds left: their logs, or work directories (7.1)
├── tmp/<process>/                    # temporary files of one otok process (7)
├── cache.lock                        # shared by running otok processes, exclusive for `otok clean` (8)
├── gc-stamp                          # its time: the last garbage collection (9)
├── layout.lock                       # serializes setting up the marker files (2)
└── <entry>.lock                      # next to each entry while it is created (6)
```

An **entry** is one of: a file in `downloads/sha256/`, a directory in `git/`, `sources/` or
`builds/…/{slices,sources,packages}/`. Entries are created by one process (6), complete when they appear
under their final name, and never modified afterwards (except bare repositories, which only gain
commits). Copies are made from them; they are never written into.

## 2. Marker files

- `CACHEDIR.TAG` follows the [Cache Directory Tagging Specification](https://bford.info/cachedir/):
  its first line is `Signature: 8a477f597d28d172789f06886806bc55`. Backup tools (Time Machine with
  exclusions, `tar --exclude-caches`, restic) skip the directory.
- `layout` holds the layout version. An otok that finds a higher version than it knows does not
  write to the cache and says which otok version created it; a lower version is migrated or, when
  that is not worth it, the old entries are left for `otok clean`. A missing `layout` in a non-empty
  directory is treated as version 1 only when the directory contains nothing but layout-1 names;
  otherwise otok refuses to use a directory that may not be its own.

## 3. Downloads

`downloads/sha256/<sha256>`: the file whose SHA-256 is `<sha256>` (64 lowercase hex digits), as
fetched: an archive of `[source]`, a `[[resource]]` file. The name carries no extension; archive
types are recognized by content (`recipe-toml.md` 4).

A download goes to `tmp/`, is hashed while it arrives, and is renamed into place only when the hash
matches the lock or recipe. A file present under its name is therefore known to be correct and is not
hashed again. The same addressing serves a machine-level mirror (`asset-mirror`, `config.md`).

### 3.1 Locations

A missing archive is downloaded from the first of these locations that gives a file with its hash
(`otok-toml.md` 10, `config.md` 2.1):

1. the project's locations (`[fetch]` of `otok.toml`): each `asset-mirror` template with `{sha256}`
   replaced, then `url` and each of `mirrors` (`recipe-toml.md` 4) rewritten by the project's
   `url-rewrite`, those a rule matches;
2. the machine's locations (`config.toml`), in the same way;
3. `url` and then each of `mirrors` as the recipe has them.

`mirrors-only` of the project, for all inputs or for the input's library, drops 3. `mirrors-only` of
the machine drops 1 and 3.

A network or HTTP error, or a file with another hash, moves on to the next location. When none is
left, the error names every location tried and what went wrong with each. Downloads of different
archives run at once; the locations of one archive are tried one after another.

## 4. Git repositories

`git/<url key>/`: a bare repository (`git init --bare`) for one repository URL. The URL key is the
first 32 hex digits of the SHA-256 of the URL, normalized: scheme and host lower-cased, a trailing
`/` and `.git` removed (the short ssh form `user@host:path`, which has no scheme, only the latter). Two spellings that normalize differently get two repositories, which costs
space, never correctness: commits are addressed by their full hash.

Commits are fetched into it one at a time; a commit already present is not fetched again:

1. With a tag (`recipe-toml.md` 4): `git fetch --depth 1 <url> refs/tags/<tag>` into
   `refs/otok/tags/<tag>`. Every server serves its tags. When the tag points to another commit, the
   fetch is an error: the tag was moved upstream, or the recipe is wrong. When the tag does not
   exist (deleted upstream), otok goes on with step 2.
2. `git fetch --depth 1 <url> <commit>` into `refs/otok/<commit>`. Servers with protocol v2 (GitHub,
   GitLab, Bitbucket) serve any reachable commit by its hash.
3. Only when both fail: the branches and tags of the server, **with their full history** (it is not
   known which of them contains the commit). This is slow for large repositories, which is why
   recipes should name the tag.

The refs keep the fetched commits from being pruned by git. Submodules (`submodules = true`) are fetched into the repositories of their own URLs
(relative URLs resolved against the superproject's, as git does).

The URL of the repository is tried with these steps as rewritten by the project's `url-rewrite`
(`otok-toml.md` 10), then as rewritten by the machine's (`config.md` 2.1), then as it is;
`mirrors-only` drops locations as for archives (3.1). A failure of all three steps, or a tag that
points to another commit, moves on to the next URL; the error names each. The repository and its key
belong to the URL as the lock has it, wherever its commits were fetched from.

git runs with the user's environment (credentials, SSH agent, proxies) and never prompts
(`GIT_TERMINAL_PROMPT=0`). ssh, for an ssh URL (`recipe-toml.md` 4), asks on the terminal itself when it
needs a passphrase or meets an unknown host key: keys in an agent and known hosts keep it from asking.

Each repository has `info/attributes` with
`* -text -eol -filter -ident -working-tree-encoding -export-ignore -export-subst`. It takes precedence
over the tree's `.gitattributes`, so the exported files (5) are the bytes of the git objects: no
line-ending conversion, no filters (Git LFS), no `export-ignore` or `export-subst` — what
`recipe-hash.md` 2 requires.

## 5. Unpacked sources

Each source is unpacked once and kept; every slice gets a fresh copy of it (`build-lua-api.md` 2,
"fresh inputs"):

| Entry | Contents |
|---|---|
| `sources/archive-<sha256>/` | The archive of `downloads/sha256/<sha256>` unpacked, a single top-level directory stripped (`recipe-toml.md` 4). |
| `sources/git-<commit>/` | The tree of the commit (`git archive`, unpacked with the archive rules of `recipe-toml.md` 4 but without stripping a top-level directory), without `.git`. |
| `sources/git-<commit>-submodules/` | The same with every submodule copied in from its own `sources/git-<commit>-submodules/` entry (recursively), at the commit the superproject records. |
| `sources/git-<commit>-<path hash>/` | The directory `path` of the commit alone, at the root of the entry (`git archive <commit>:<path>`); `<path hash>` is the first 16 hex digits of the SHA-256 of `path` (UTF-8, as written in `recipe-source`). |

A recipe from git (`recipe-source = { git, commit, path }`) is `sources/git-<commit>-<path hash>/`, or
`sources/git-<commit>/` when it has no `path`; its `recipe-sha256` is checked there before use. Only the
recipe's directory is exported, so recipes of a repository with many of them (or with a large library
beside the recipe) published at different commits take the space of their own directories.

Copies for slices are made with `clonefile` on macOS (APFS copy-on-write: no data is copied until a
file is written) and by an ordinary recursive copy on Windows and Linux. File times and the executable
bit are kept in both.

## 6. Creating an entry

Several otok processes may want the same entry at once (two projects, a terminal and Xcode). Each
entry has a lock file next to it, `<entry>.lock` (`downloads/sha256/<sha256>.lock`,
`sources/archive-<sha256>.lock`, …). To create an entry:

1. If the entry exists, use it (no lock needed: entries appear atomically).
2. Take the exclusive lock on `<entry>.lock` (created if missing), waiting for another holder; while
   waiting, otok says which entry it waits for.
3. If the entry exists now, another process created it: release and use it.
4. Create it under `tmp/<process>/`, then rename it to its final name (same file system, so the
   rename is atomic).
5. Release the lock. Lock files stay; they are empty and cost nothing.

Locks are advisory OS locks (`flock` on macOS and Linux, `LockFileEx` on Windows), released by the
operating system when a process dies, so a crashed otok never leaves a stale lock. Readers take no
lock: an entry under its final name is complete and never changes.

### 6.1 Offline

With `--offline` (or `--frozen`) otok uses only what the cache already holds. Nothing is downloaded or
fetched, `file://` URLs and local git repositories included. Entries that can be derived locally are
still created: an archive in `downloads/` is unpacked, and a commit present in a bare repository is
exported. Every missing input is an error naming it.

## 7. Temporary files

`tmp/<process>/` (`<process>` = process id and a random suffix) holds what one otok process is
creating: downloads in progress, archives being unpacked, entries before their rename. While it runs,
the process holds the lock `tmp/<process>.lock`; it removes its directory when it exits. A directory
whose lock can be taken belongs to a process that no longer runs (a crash): the next otok removes it,
once per run, when it starts using the cache; not one whose lock file or directory was written within the last
minute: a process may be creating it (it creates its lock file, locks it, then creates the directory), and the
builds of `otok publish`, which run at once, each open the cache in one process.

### 7.1 Builds

A slice is built in `tmp/<process>/build/<library>-<slice>/` (`-debug` for the `debug` configuration,
`-tool` for a tool package, `-tools` for `build_tools`), which holds `source/` (a fresh copy of the source with resources placed at their
`dest` and patches applied), `build/`, `prefix/`, `home/` and `tmp/` (`HOME` and `TMPDIR` of the
build's processes), `toolchain.cmake` and `build.log`; with the sandbox of Windows also `sandbox.log` (what the
AppContainer was refused) and, for a recipe of a path outside the cache, `recipe/`, its copy (`slice.recipe`; the
container reads only the cache, `docs/PLAN.md` decision 103). On Windows, with `virtual-paths` (`config.md`), the build
sees these through short drive letters of a device map of otok's own: one for `tmp/<process>/build/`, so that the work
directory is `<letter>:\<library>-<slice>\`, and one for the cache root, through which it is given the prefixes of its
dependencies, toolchains and resources; `build.log` begins with a line per letter naming its directory. The entry is
created under its lock (6) while the build runs; when the build succeeds it receives:

| File | Contents |
|---|---|
| `prefix/` | `slice.prefix` as the build left it |
| `result.toml` | What `build` returned (`build-lua-api.md` 8), paths relative to `prefix/`: `headers`, `module-map`, `[libraries]`, `[library-headers]`, `[library-module-maps]`, `[data]` |
| `key.txt` | The canonical description the key hashes (`cache-key.md` 3) |
| `build.log` | The slice's log |

The work directory is removed afterwards. When the build fails, what it left is kept in
`failed/<library>-<slice>-<YYYYmmdd-HHMMSS>/` (with `-debug`, `-tool` or `-tools` as the work directory has them, and
`-2`, `-3`, … after a failure within the same second; `<library>-sources-<time>/` when the patches of a delivered
source fail), and the error names it: the log (`build.log`, `patch.log`) and `key.txt`, the key it was built for;
with `keep-build-dirs` (`config.md`) or `--keep-build-dirs`, the whole work directory, `key.txt` added. `otok log`
shows these logs and those of the slices.

## 8. Cleaning

`otok clean` removes the cache of the machine, or the selected parts of it; it needs no project:

| Flag | Removes |
|---|---|
| (none) | everything but the marker files (2) |
| `--downloads` | `downloads/` |
| `--sources` | `sources/`, `git/`, `index/` and `view/` |
| `--builds` | `builds/`, `dev/` and `failed/` |
| `--unused` | instead of whole directories, the entries unused for `cache-max-age` days, as garbage collection does (9) |
| `--days <n>` | with `--unused`: entries unused for `n` days (`0`: every entry) |
| `--dry-run` | nothing: prints what would be removed and its size |

Flags combine; `--unused` applies to the selected parts. Each directory is first renamed into `tmp/<process>/` and then removed, so an
interrupted clean never leaves a half-removed entry under its final name; the rest goes with the
process's tmp directory (7).

Readers take no lock on entries (6), so removing them under a running otok could break its copies.
Every otok process therefore holds a **shared** lock on `cache.lock` for as long as it uses the cache,
and `otok clean` holds it **exclusively**: it waits until the other processes finish ("waiting for
other otok processes to finish with the cache"), and a process started meanwhile waits for it
("waiting for 'otok clean' to finish"). Setting up the marker files is serialized by `layout.lock`.

## 9. Garbage collection

Entries unused for `cache-max-age` days (`config.md`, default 30) are removed.

- **Last use** is the modification time of the entry itself (the directory, or the file in
  `downloads/`). otok sets it to the current time when it uses an entry: found in the cache, copied
  for a slice, a download already present, a repository fetched into. Only the metadata changes, never
  the contents, and only when the time is more than a day old. Failing to set it (a read-only file
  system) is not an error. A new entry gets the current time before it is renamed into place: unpacking
  gives directories the times stored in the archive or the commit, which would make it look unused.
- **Entries** are those of 1 (downloads, repositories in `git/`, unpacked sources, slices, packages)
  and what failed builds left in `failed/` (7.1), and the work directories of dev slices in `dev/`
  (`otok-dev-toml.md` 4), aged by the time of their last build. Each is removed on its own: whatever is still needed is
  fetched or built again from its hashed inputs. The entry's `.lock` goes with it, and directories of
  `builds/` left empty are removed.
- **When:** at the end of `install`, `update` and `fetch`, at most once a day: when `gc-stamp` is
  missing or more than a day old. The process releases its shared `cache.lock` and tries to take it
  exclusively without waiting (8); when another otok process uses the cache, collection is skipped
  and a later run does it. Nobody waits for garbage collection.
- `otok clean --unused` does the same at any time, waiting for the exclusive lock like every clean.
- Removal renames into `tmp/<process>/` first, as cleaning does. When anything was removed, otok
  says how many entries and how much space ("removed 12 cache entries unused for 30 days (1.4 GB)").

## 10. Index

A git index (`config.md` `index`, `index.md` 8.1) is read from the tree of a commit: its repository is a
repository of 4, keyed by its URL, and the tree an entry of `sources/git-<commit>/` (5), so an index that
did not change since the last command costs one request for its `HEAD` and nothing else.

`index/<url key>` (the key of 4) holds the 40 hex digits of the commit last read, and a newline. It is
not an entry: otok rewrites it (atomically, 6) when `HEAD` has moved, and reads it when offline (6.1),
where a missing file is an error. Its tree, like any entry, may have been removed by garbage collection
since; it is then fetched again, or, offline, it is missing.

## 11. Index view

The files of an index view (`index.md` 8) are kept in `view/<url key>/`, the key of 4 computed from
the base URL of the view (`https://<base>`, or the `index-mirror` read instead). `<path>` is the file's
path under `<base>/index/` (`recipes/apotocki/openssl.json`), and `<path>.etag` holds the `ETag` the
server sent with it, when it sent one. They are not entries: garbage collection leaves them, and
`otok clean --sources` removes them.

A command requests each file it reads at most once, with `If-None-Match` when the `.etag` exists:

- `200`: otok removes the `.etag`, writes the file atomically (6), then writes the new `.etag`; an
  interrupted update leaves a file without an `ETag`, which the next request fetches whole.
- `304`: the cached file is read.
- `404`: the file and its `.etag` are removed; the recipe or library does not exist (`index.md` 8).

Offline (6.1) nothing is requested: the cached files are read as they are, and a file that is not
cached is an error.
