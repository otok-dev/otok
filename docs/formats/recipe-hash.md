# Tree hash (`recipe-sha256`)

Status: draft.

One algorithm hashes a directory tree. It gives `recipe-sha256` (a recipe directory) and the hash of a
local source directory inside a project (`otok-lock.md` 3); the two differ only in how symbolic links
are treated (1, step 2). It is simple enough to check with standard tools.

## 1. Algorithm

1. Collect every regular file and symbolic link under the root directory, recursively, without
   following links. Skip a `.git` entry at the root (a recipe at the root of its repository).
   Directories themselves, empty or not, contribute nothing.
2. Symbolic links: in a **recipe** tree they are errors (recipes are plain text, `recipe-toml.md`
   15). In a **source** tree (`source = { path }` inside a project, `otok-lock.md` 3) they are
   recorded by their target, written with `/` as separator; a target that is absolute, leaves the
   tree (`../` past the root), or contains a newline is an error. Other non-regular files (devices,
   sockets, pipes) are errors in both.
3. For each entry, its path relative to the root, with `/` as separator. A path containing a newline,
   a backslash, or a component `.` or `..` is an error.
4. Build the listing: one line per entry, sorted by path as bytes (UTF-8):
   - a file: `<sha256 of the file contents, 64 lowercase hex digits>  <path>\n` (two spaces);
   - a link: `symlink:<sha256 of the target text>  <path>\n`. The prefix keeps a link distinct from a
     file whose contents are the target text; hashing the target keeps the line unambiguous.
5. The tree hash is the SHA-256 of the listing, as 64 lowercase hex digits.

File contents are hashed as stored, byte for byte: no line-ending conversion. File modes and times do
not count.

Without links, the listing is exactly what `shasum -a 256` (or `sha256sum`) prints, so on macOS or
Linux:

```sh
cd recipe-dir && find . -type f ! -path './.git/*' | sed 's|^\./||' | LC_ALL=C sort \
    | while read -r f; do shasum -a 256 "$f"; done | shasum -a 256
```

With links (a source tree):

```sh
cd source-dir && find . \( -type f -o -type l \) ! -path './.git/*' | sed 's|^\./||' | LC_ALL=C sort \
    | while read -r f; do
        if [ -L "$f" ]; then
            printf 'symlink:%s  %s\n' "$(printf '%s' "$(readlink "$f")" | shasum -a 256 | cut -c1-64)" "$f"
        else
            shasum -a 256 "$f"
        fi
      done | shasum -a 256
```

## 2. Where the tree comes from

- **Index and git recipes.** otok fetches the commit with its own git invocation (`core.autocrlf`
  off, no filters), so the files are the bytes of the git objects, and hashes the recipe directory at
  that commit. Index CI computes the same hash for the entry (`index.md` 9).
- **Local paths.** The files as they are on disk. A recipe directory or vendored source inside a
  project is hashed from the working copy; on Windows a checkout with `core.autocrlf = true` changes
  text files and thus the hash. Projects that keep recipes or sources in their own repository set
  `* -text` (or `eol=lf`) for them in `.gitattributes`; otok reports a lock mismatch rather than
  guessing.

## 3. Recipe paths

For recipe directories, `recipe-toml.md` 15 additionally restricts paths to portable names, so the
same recipe checks out identically on every host: components of `[A-Za-z0-9._+-]`, not starting with
`-`, and no two paths that differ only in case.
