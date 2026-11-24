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
│   ├── slices/<slice key>/           # cache-key.md; prefix/, result.toml, key.txt, licenses/, build.log (7.1)
│   ├── sources/<sources key>/        # cache-key.md 3.1; source/ (patched) and key.txt
│   └── packages/<package key>/       # cache-key.md 4; the packaged output and key.txt
├── dev/<project key>/<library>-<slice>/  # the kept work directories of dev slices (otok-dev-toml.md 4)
├── projects/<project key>.lock       # held by the otok working in a project, and its PID in .owner (6.3)
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

### 6.2 Damaged entries

A slice entry (`builds/…/slices/<key>/`) is damaged when its `result.toml` cannot be read, or names a
path that is empty, absolute, leaves `prefix/` (`..`) or is not there (a file for `libraries`,
`runtimes`, `symbols`, `library-module-maps`, `programs` and `module-map`). otok never writes one: the
paths `build` returned are checked before the entry is made, and a path it cannot write relative to
`prefix/` fails the build. One written by an older otok, or changed by hand, is found when the entry is
used: otok warns, takes the entry's lock, and, if the entry is still damaged (another process may have
built it again meanwhile), renames it into `tmp/<process>/` and removes it, then builds the slice as if
it were missing. An entry it cannot rename (a file of it held open) is an error that names it.

### 6.3 Projects

Two otok processes in one project (an IDE's build and a terminal) would both write `otok.lock` and the outputs, one
after the other or into each other. Each command that reads a project holds `projects/<project key>.lock`, the key being
the first 32 hex digits of the SHA-256 of the project directory's canonical path (as `dev/`: absolute, with `.`, `..`,
links, junctions and substituted drives resolved, each component in the case the file system keeps, a Windows drive
letter upper-cased, so every way to name the directory gives one key): `install`, `update` and
`fetch`, which write, exclusively; the others (`tree`, `why`, `metadata`, `audit`, `outdated`) shared. A process that
cannot have it waits, saying so with the PID of the holder, which writes it into `projects/<project key>.owner` when it
takes the lock. The lock is the operating system's (6), so a holder that dies releases it; the processes a build
starts never hold it. Nothing of this is in the project directory.

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
minute: a process may be creating it (it creates its lock file, locks it, then creates the directory). A command that
runs others at once in its process (the builds of `otok publish`) opens the cache once and gives each a session of its
own: `tmp/<process>/session-<n>/` is that one's directory for what it creates, removed when it ends, and the work
directories of builds (7.1) are shared.

### 7.1 Builds

A slice is built in `tmp/<process>/build/<n>/<library>-<slice>/` (`<n>` a number per builder of the process, as the
sessions of 7 build at once; `-debug` for the `debug` configuration,
`-tool` for a tool package, `-tools` for `build_tools`), which holds `source/` (a fresh copy of the source with resources placed at their
`dest` and patches applied), `build/`, `prefix/`, `home/` and `tmp/` (`HOME` and `TMPDIR` of the
build's processes), `toolchain.cmake` and `build.log`; with the sandbox of Windows also `sandbox.log` (what the
AppContainer was refused) and, for a recipe of a path outside the cache, `recipe/`, its copy (`slice.recipe`; the
container reads only the cache, `docs/PLAN.md` decision 103). On Windows, with `virtual-paths` (`config.md`), the build
sees these through short drive letters of a device map of otok's own: one for `tmp/<process>/build/`, so that the work
directory is `<letter>:\<n>\<library>-<slice>\` whatever the sessions building at once, and one for the cache root, through which it is given the prefixes of its
dependencies, toolchains and resources; `build.log` begins with a line per letter naming its directory. The entry is
created under its lock (6) while the build runs; when the build succeeds it receives:

| File | Contents |
|---|---|
| `prefix/` | `slice.prefix` as the build left it |
| `result.toml` | What `build` returned (`build-lua-api.md` 8), paths relative to `prefix/`: `headers`, `module-map`, `[libraries]`, `[library-headers]`, `[library-module-maps]`, `[data]` |
| `key.txt` | The canonical description the key hashes (`cache-key.md` 3) |
| `licenses/` | The recipe's `license-files` (`recipe-toml.md` 3) at their paths, copied from the slice's source or the recipe directory as `otok-toml.md` 4.2 lays them out, so that packaging needs no source (12); entries of an earlier otok may lack it, and packaging then takes the source |
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

- `200`: otok writes the file atomically (6), then the new `.etag` (or removes the old one when the server sent none);
  an interrupted update leaves the file with the `ETag` of the one before, which the server no longer matches, so the
  next request fetches it whole. A file that cannot be written, because another otok process replaces it at the same
  moment, is read as received and written another time; it is not an error.
- `304`: the cached file is read.
- `404`: the file and its `.etag` are removed; the recipe or library does not exist (`index.md` 8).

The commands one otok process runs at once (the projects of `otok publish`) request a file one after another: the first
asks the server, the others find its file and `ETag`.

Offline (6.1) nothing is requested: the cached files are read as they are, and a file that is not
cached is an error.

## 12. Shared build cache

An organization may share slice entries among its machines and its CI through stores of its own, which `build-cache`
of `config.md` names (2.3). There is no public one: built output never leaves the trust boundary of whoever runs the
store (DESIGN 3). A store changes where a slice entry comes from, never what it is, so it enters no key.

### 12.1 Layout of a store

A store is a directory (a `file:` URL, a network share) or an HTTP base URL. Its layout has the shape of Bazel's HTTP
cache, so that stores already in use serve it unchanged (bazel-remote, with `--disable_http_ac_validation` since the
descriptors are not Bazel's; nginx with WebDAV; a generic Artifactory repository; S3 or GCS behind presigned URLs):

```text
<base>/
├── cas/<sha256>        # the archive of a slice entry, named by its own SHA-256 (64 hex digits)
└── ac/<slice key>      # the descriptor of the slice: the full 64 hex digits of its key (cache-key.md 3)
```

Nothing is listed: a slice is looked up by its key alone. Stores prune by their own rules (a lifecycle rule, the LRU of
bazel-remote); a reader of a directory store sets a descriptor's modification time when it is more than a day old, as
garbage collection reads it locally (9), so that it can be pruned by that time.

The **archive** is a `tar` (POSIX pax) compressed with zstd, holding `prefix/`, `result.toml`, `key.txt` and `licenses/`
of the entry (7.1), not `build.log`. It is written so that one entry always gives the same bytes: paths sorted
bytewise, directories before their contents, every time `2000-01-01T00:00:00Z`, owner and group `0` without names,
modes `0755` for directories and executable files and `0644` for the rest, symbolic links kept as links (relative
ones inside the entry only, as 6.2 of a slice entry allows), no other kinds of file, zstd level 10 on one thread.

The **descriptor** is a TOML file:

```toml
format = 1
archive = "9f2c…"                  # the SHA-256 of the archive in cas/
size = 48213377                    # its size in bytes
pushed-by = "ci"                   # the user name of the process that pushed it
machine = "build-07"               # its host name
otok = "0.6.0"                     # its version
time = 2026-11-24T10:15:00Z
```

An unknown key is ignored; a `format` other than `1` makes the entry absent for that reader, with a warning.

### 12.2 Lookup

`install` and `update` compute the keys of every slice of the graphs after the lock is written and the toolchains are
found, before fetching anything. A slice is **needed** when the package entry it belongs to is missing (`cache-key.md`
4), or when a slice to be built depends on it (a dependency, a tool dependency, the `build_tools` entry). Each needed
slice missing from the local cache is asked of the stores, in the order `build-cache` gives them, all slices at once;
only the slices no store holds are built, and only their inputs are fetched (3, 4). So the tools of a graph whose
slices are all found are neither fetched nor built, and neither is a source whose slices are.

A hit becomes an entry of the local cache as 6 creates one: the descriptor read, the archive downloaded into
`tmp/<process>/` and checked against `archive` and `size`, unpacked there (with the rules of unpacking archives,
`recipe-toml.md` 4), its `key.txt` hashed and compared with the key asked for, the entry checked as 6.2 checks a slice
entry and its libraries as the builder checks those it builds (their kinds by the linkage of the slice, and what the target
checks of them: their architectures); then renamed into place, with the
current time (9). An entry failing any of these is dropped with a warning naming the store, and the next store is
asked, or the slice is built. A store that cannot be reached (a network error, an HTTP status other than `200` and
`404`) is a warning once per command and is not asked again in that command.

Stores are not used with `--offline` or `--frozen` (6.1), by `otok fetch`, which fetches inputs for building offline,
and by `otok publish`, whose builds say that a recipe builds on this machine. `--no-build-cache` turns them off for one
run of `install` or `update`.

### 12.3 Push

A store with `push = true` receives the slices this machine builds, each uploaded as soon as it is built while the
others build; the command waits for its uploads before it ends. An upload:

1. makes the archive (12.1) in `tmp/<process>/` and hashes it;
2. writes `cas/<sha256>` when it is absent;
3. writes `ac/<slice key>` when it is absent.

Each write happens only when the file is absent: over HTTP a `PUT` with `If-None-Match: *` (`412` is not an error:
another machine pushed it), in a directory a file written under `<base>/tmp/` and renamed into place without
replacing one. A descriptor already there naming another archive means that the slice's build is not reproducible:
a warning naming the key and both pushers; the first push stays. A failed upload is a warning, never a failure of the
command.

Not pushed, unless the store has `push-local = true`: a slice whose source or recipe is a path of this machine
(`path:` or `local:` in its key, a recipe outside the cache), or one whose key names such a slice. Never pushed: dev
slices (`otok-dev-toml.md` 4) and the slices depending on them, and entries without `licenses/` (made by an earlier
otok).

`otok cache push [<library>…] [--store <url>]` uploads the entries of the project's graphs that the local cache
holds, of the given libraries or all, to the stores with `push = true` or to the one named; it is an error when no
store takes them, and a failed upload is an error of the command. `otok cache status` lists the slices of the
project's graphs and where each would come from: the local cache, a store (which) or a build.

### 12.4 HTTP

`GET <base>/ac/<key>` and `GET <base>/cas/<sha256>`; `200` is a hit, `404` a miss. A push (12.3) asks `GET <base>/ac/<key>`
first, since a server that ignores `If-None-Match` (bazel-remote) would replace a descriptor; when there is none,
`HEAD <base>/cas/<sha256>`, `PUT` of the archive when that is not `200`, then `PUT` of the descriptor, each with
`If-None-Match: *`. A `PUT` succeeds with a `2xx`; `412` means another machine pushed it meanwhile. Servers that ignore the
header (bazel-remote, nginx's WebDAV) let the last of two pushes of one key at the same moment win: harmless in `cas/`,
whose names are the contents' hashes, and in `ac/` one of two valid descriptors stays, whose archive readers check. Requests carry
`Authorization: Bearer <token>` when the store has a token (`config.md` 2.3), and go through `proxy`. Redirects are
followed for `GET` and `HEAD` only, so presigned URLs of object stores work behind a server that redirects; libcurl does
not send the token to another host. The token is never written into a log, a message or a file of otok's.

A network error or another status (`401`, `5xx`) is a warning naming the store and the request, and the store is not
asked again in that command (12.2): its slices are built.
