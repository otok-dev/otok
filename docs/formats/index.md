# Index, format 1

Status: draft. Common rules are in [README.md](README.md). This document specifies the files of an
index.

The index is a public git repository of immutable version entries, plus a few mutable files
(libraries, publishers, recipe status, advisories). otok reads a generated HTTPS view of it (section
8), or the repository itself (8.1); the git repository stays the authoritative publication log.

## 1. Layout

```text
otok-index/
├── index.toml                          # index-wide settings (2)
├── recipes/
│   └── apotocki/
│       └── openssl/
│           ├── recipe-status.toml      # mutable: deprecation, transfer (5)
│           ├── apple/                  # a variant (3)
│           │   ├── 3.5.4-r1.toml       # immutable version entries (3)
│           │   └── 3.5.4-r2.toml
│           └── windows-msvc/
│               └── 3.5.4-r1.toml
├── libraries/
│   └── openssl.toml                    # version scheme (4)
├── publishers/
│   └── apotocki.toml                   # identity, trusted publishing (6)
├── advisories/
│   └── 2026-0001.toml                  # (7)
├── availability.toml                   # generated: inputs that went missing (11)
└── apple/
    ├── required-reason-apis.toml       # (10.1)
    ├── privacy-sdks.toml               # snapshot of Apple's commonly used SDK list
    └── private-libraries.toml          # (10.2)
```

All files are TOML (common 1). File and directory names are lowercase.

Index CI checks every file strictly, as common 1.2 says (9). otok, reading an index that may have been written for a
newer otok, is lenient where that cannot change what it builds (common 1.5): it skips
the version entries it cannot read (3.2), takes unknown platforms, architectures and environments as names that
never match, reads `index.toml`, `recipe-status.toml`, publisher files, `availability.toml` and the Apple data files
without the check of unknown keys, and reads advisories so that they act at least as much as they mean (7.2).

## 2. `index.toml`

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | Index format version; `1`. |
| `name` | string | Display name (`"Otok public index"`). |
| `catalog` | URL | The catalog site generated from this index. |
| `view` | URL | Base URL of the HTTPS view (8). |
| `repository` | URL | The git repository of the index: where `otok publish` proposes entries when `index` names the view. |
| `established-entries-per-day` | integer | The new entries an established publisher (9.1) may have in 24 hours for index CI to merge them (9.2); at least 1, `20` when missing. |
| `review-entries-per-day` | integer | The new entries any other publisher may have in 24 hours for index CI to pass them to a maintainer (9.2); at least 1, `5` when missing. |

```toml
format = 1
name = "Otok public index"
catalog = "https://otok-dev.github.io/index"
view = "https://otok-dev.github.io/index"
repository = "https://github.com/otok-dev/index"
established-entries-per-day = 50
review-entries-per-day = 10
```

A `format` otok does not know makes the whole index unreadable ("the index has format 2; it needs a newer otok"):
the index format is never meant to change, what changes is the format of its entries (3.2).

A private index uses the same format.

## 3. Version entries

`recipes/<publisher>/<name>/<variant>/<version>-r<revision>.toml`. Never edited or deleted after it is merged;
withdrawal is an advisory (common 1.6). Index CI accepts an entry only after fetching its
`recipe-source` itself and finding the same `recipe-sha256` and copied fields there.

A recipe id has a variant per target and way of building (`recipe-toml.md` 6.1), each with its own revisions:
`apple/1.92.0-r7.toml` and `windows-msvc/1.92.0-r1.toml` are two recipes of one version. The variants of one id
entered for one version share no cell, platform and environment (common 4.2: neither covers the other's
environment for a platform both support); index CI refuses an entry that would. An entry's `[platforms] supported`
is of one target.

An entry is everything resolution needs, so the resolver works from index data alone and fetches
recipe trees only for the versions it selects (as Cargo's index carries dependencies and features):
the publication pins plus a **projection of the recipe's `recipe.toml`**. Index CI recomputes the
projection from the recipe tree at the pinned commit and rejects an entry that differs.

```toml
format = 1
recipe = "apotocki/openssl"
variant = "apple"
library = "openssl"
version = "3.5.4"
revision = 1
channel = "stable"
recipe-source = { git = "https://github.com/apotocki/openssl-iosx", commit = "9f3c1e2a5b7d4c6e8f0a1b2c3d4e5f60718293a4", path = "otok" }
recipe-sha256 = "6d2f0c9e8b7a1d3e5f4c2b0a9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d"
source = { url = "https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz", sha256 = "c087cd0ba9b5f1e2d3c4b5a69788796a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e" }
api = 1
license = "Apache-2.0"
description = "OpenSSL (libssl, libcrypto), built from source."
keywords = ["tls", "crypto"]
published = 2026-10-23T15:00:00Z           # written by index CI as it merges the entry

[platforms]
supported = ["ios", "iossim", "macosx", "catalyst", "tvos", "tvossim", "watchos", "watchossim", "xros", "xrossim"]
build-on = ["macos/xcode"]

[options.no-deprecated]
type = "bool"
default = false

[options.features]
type = "list"
values = ["no-legacy", "no-quic", "no-engine"]
default = []

[options.zlib]
type = "bool"
default = false
additive = true

[deps.zlib]
version = "~> 1.3"
default-recipe = "apotocki/zlib"
when = { option = "zlib" }

[apple]
xcode = ">= 16"
```

| Key | From | Meaning |
|---|---|---|
| `format` | — | Entry format; `1`. |
| `recipe` | publisher + `[recipe] name` | Recipe id; must match the file path. |
| `variant` | `[recipe] variant`, or the target of `supported` | The variant (`recipe-toml.md` 3); must match the file path. |
| `library`, `version`, `revision`, `channel`, `api`, `license`, `description`, `keywords`, `linkage`, `c-standard`, `cxx-standard` | `[recipe]` | As in `recipe-toml.md` 3. `version` and `revision` must match the file name. |
| `recipe-source` | publication | Repository, full commit SHA (with its tag, when the publication names one) and directory of the recipe tree. |
| `recipe-sha256` | publication | Hash of that tree (`recipe-hash.md`). |
| `source`, `[[resource]]` | `[source]`, `[[resource]]` | Pins only: `url` + `sha256` (+ `mirrors`) or `git` + `commit` (+ `tag`, `submodules`), and each resource's `name`. Patches, `verify-against`, `unpack` and `dest` stay in the recipe. |
| `published` | index CI | UTC time of the merge, written into the entry by the workflow that merges it (9.2), or by `otok publish` into a checkout of the index, whose pushes the git host already limits. Never in a pull request: an author could write any time there, and the limit of entries per day counts by it (9.2). Absent from an entry merged otherwise, which has no date. |

An entry says nothing of a build: `otok publish` builds the recipe as the publisher's own check, and whatever a
publisher wrote of it would be taken on trust. Marks of builds, if an index has a CI
that builds published recipes, are records of their own, written by that CI.
| `[tool]`, `[platforms]`, `[options.*]`, `[deps.*]`, `[tool-deps.*]`, `[toolchains]`, `[<target>]` (`[apple]`, …) | the same tables | Copied, except option `doc` strings (the catalog reads them from the recipe). |

Not copied: `[vars]`, `[[product]]`, `[publish]`, `[metadata]`, `name` (it is in `recipe`), `homepage`,
`documentation`, `license-files` — they matter for building and for the catalog, both of which read the
recipe tree.

### 3.1 Projection

The projection of a `recipe.toml` is the entry without `recipe-source`, `recipe-sha256` and `published`,
made from the recipe by these rules, so that otok, `otok publish` and index CI compute
the same one:

- `format = 1`, `recipe` = the publisher, `/`, and `name` of `[recipe]`, and `variant` = `[recipe] variant` or the
  target of `[platforms] supported`;
- the keys of the table above from `[recipe]`, moved to the top level, those absent from the recipe
  left absent (no defaults are filled in);
- `source`: the keys `url`, `sha256`, `mirrors`, `git`, `commit`, `tag`, `submodules` of `[source]`;
  `[[resource]]`: `name` and the same keys of each resource, in the recipe's order;
- the tables of the last row copied whole, with `doc` removed from every option; a table of a target otok does not
  know is copied as well (3.2), so that an older otok projects a newer one's recipe as that otok did.

Two projections are equal when they hold the same keys with the same values (TOML types included), in
any order and layout. otok reads an entry as a recipe without products: the resolver needs nothing
else. After fetching the recipe tree of a version it selected, otok checks that the tree hashes to
`recipe-sha256` and that the projection of its `recipe.toml` equals the entry, and refuses the version
otherwise: an index that misstates a recipe's dependencies or options is caught before anything is built.

Two versions of one library that are equal in its scheme, or whose file names differ only in case,
cannot both be published.

### 3.2 Entries for a newer otok

An entry otok cannot read is not there for it (as in Cargo's index, whose entries carry `v`): an unknown `format` or
`api`, a library whose `scheme` (4) it does not know, or anything else its reader refuses. Resolution chooses among the
other versions, so `update` takes one this otok reads; the version is an error only when the lock or a constraint of the
project leaves nothing else, and the message says that it needs a newer otok (it cannot say which, common 1.4).
`otok outdated` and `otok info` show such versions as needing a newer otok. Index CI never merges an entry its otok
cannot read, so for the otok of index CI no entry is skipped.

Platforms, architectures, environments and targets an entry names that this otok does not know are not errors but
names that never match (common 4.1): `supported` keeps the platforms it knows, a `when` or `not` testing the name
is false, `build-on` cannot build in that environment. An entry of a variant whose platforms are all unknown is of
no use to this otok, and is left aside like an unreadable one.

## 4. Library files

`libraries/<library>.toml`, created with the first recipe of the library:

| Key | Type | Meaning |
|---|---|---|
| `scheme` | `"dotted"`, `"semver"`, `"explicit"` | Version scheme (common 3.1). Default `dotted`. |
| `order` | array of versions | `explicit` only: all known versions, oldest first. Publishing an unknown version adds it (at the end, or where the publisher states). |

Set by the first publisher; changing `scheme` later needs an index maintainer, because it reorders
every recipe's versions. A `scheme` otok does not know (common 1.5) makes the
entries of the library unreadable (3.2), not the index. The recipes that build a library are not listed here: the view
derives them (8).

## 5. Recipe status

`recipes/<publisher>/<name>/recipe-status.toml`, optional and mutable by the publisher:

| Key | Type | Meaning |
|---|---|---|
| `deprecated` | `{ reason = "…", in-favor-of = "<recipe id>" }` | The recipe is no longer maintained; `otok install` and `otok outdated` show the replacement. |
| `transferred-from` | recipe id | Set by an index maintainer when a recipe moves between publishers; the history stays under the old id. |

A deprecated recipe still resolves and builds; `install`, `update` and `fetch` warn when they choose
it, naming the reason and the replacement, and `audit`, `outdated` and `info` show it.

## 6. Publisher files

`publishers/<publisher>.toml`, added by the author's first pull request publishing under the name and merged by an
index maintainer (9.1). `otok publish` adds it to its pull request when the index has none, for the account of its
token (`github-id` from GitHub's API, `kind = "user"`), so that the first publication registers the name too; an
organization's file names the organization, not its author, and is added by an index maintainer.

| Key | Type | Meaning |
|---|---|---|
| `github-id` | integer | The numeric GitHub account id. Logins can be renamed and, after deletion, registered again by someone else; the numeric id cannot, so ownership checks use it. |
| `kind` | `"user"` or `"organization"` | For an organization, its members may publish (checked at PR time). |
| `established` | boolean | Set by an index maintainer: index CI merges the publisher's pull requests itself, within `established-entries-per-day` (9.1, 9.2). `false` when missing: every pull request waits for a maintainer. |
| `trusted-publishing` | array of `{ repository = "owner/repo", workflow = ".github/workflows/<file>", environment = "…" }` | GitHub Actions workflows allowed to publish for this publisher with an OIDC token; `environment` optional. Read but not used yet: how a publisher's workflow reaches the index without a long-lived token is not decided yet. |
| `suspended` | string | Set by an index maintainer: the reason the publisher may not publish any more. Index CI refuses its pull requests (9); its recipes are withdrawn by advisories. |

### 6.1 Why GitHub only

In format 1 a publisher of the public index is a GitHub account. This restricts
only the publisher's identity: recipe repositories (`recipe-source`) and library sources may be on any
git host (GitLab, Codeberg, a self-hosted server), and a private index chooses its own rules.

The reasons: the index is a GitHub repository and a publication is a pull request, whose author
GitHub has already identified, so otok needs no account system or server of its own;
trusted publishing uses GitHub Actions OIDC tokens; and one identity provider gives one name space.

The format leaves room for more providers: a publisher file could carry e.g. `gitlab-id` next to
`github-id`, and trusted publishing could accept GitLab CI OIDC tokens (as PyPI does). What has to be
decided first is the name space: `foo` on GitHub and `foo` on GitLab are different people, so either
publisher names get a provider prefix or names are first come, first served across providers. Settled
when there is real demand.

## 7. Advisories

`advisories/<id>.toml`, `id` = `YYYY-NNNN`. Mutable (status, details, references) but never deleted.

```toml
id = "2026-0001"
recipes = ["apotocki/openssl"]
versions = ["3.5.4-r1"]
severity = "withdrawn"
status = "confirmed"
summary = "Recipe revision 1 built without no-legacy despite the option."
published = 2026-11-02T10:00:00Z
updated = 2026-11-02T14:00:00Z
```

| Key | Type | Meaning |
|---|---|---|
| `id` | string | Matches the file name. |
| `recipes` or `library` | array of recipe ids, or a library name | What it concerns: specific recipes (a recipe bug) or every recipe of a library (an upstream vulnerability). Exactly one of the two. |
| `variants` | array of variant names | With `recipes`: only these variants (`["windows-msvc"]`), whose revisions are their own (3); every variant without it. |
| `versions` | array | Affected versions: `"<version>-r<revision>"` for specific revisions, `"<version>"` for all its revisions, or a constraint (`"< 3.5.5"`, common 3.2) for every revision of the versions it matches. Versions are compared in the library's scheme (4). |
| `severity` | `"withdrawn"`, `"security"`, `"broken"` | 7.1 |
| `status` | `"under-review"`, `"confirmed"`, `"rejected"` | A report can be published immediately as `under-review` and resolved later. |
| `summary` | string, one line | |
| `details` | string (Markdown) | Optional. |
| `aliases` | array of strings | `CVE-…`, `GHSA-…`. |
| `references` | array of URLs | |
| `published`, `updated` | datetime | |

### 7.1 Effect

| Severity, status | `install`, `update` | `audit` |
|---|---|---|
| `withdrawn` or `security`, `confirmed` or `under-review` | stop, unless `--allow-advisory=<id>`; `update` avoids the versions | non-zero exit |
| `broken`, `confirmed` or `under-review` | warning; `update` avoids the versions | warning |
| any, `rejected` | nothing | nothing |

`under-review` acts like `confirmed`: a false alarm costs a day of delay, a real problem left
unflagged costs more.

An entry of `versions` is first read as `<version>-r<revision>` when it ends in `-r` and digits and the
part before is a version, then as a version, then as a constraint; a version that its library's scheme
does not accept is not affected.

The resolver never refuses a version for an advisory; it **avoids** it: a version with an active
advisory (any severity, not `rejected`) is tried after every version without one, but before nothing
else, and a version kept from `otok.lock` is tried first all the same. So `update` (and a first
resolution) moves to another allowed version when there is one, while `install` and `fetch` keep the
locked version and stop on it, as the table says. The check runs after resolution, on the chosen
versions. `--allow-advisory=<id>[,<id>…]` of `install`, `update` and `fetch` lets those advisories pass;
the versions still warn.

### 7.2 Advisories for a newer otok

A newer otok may give advisories new keys and values. otok reads them so that an advisory acts at least as much as it
means:

- A key may only narrow what an advisory concerns (as `variants` does), so an unknown key is left aside and the
  advisory applies more widely.
- An unknown `severity` acts as `security`, and an unknown `status` as `confirmed`, with a warning naming them and
  saying that a newer otok knows them. Severities added later are therefore strict ones; a mild one is `broken`.
- An advisory otok cannot read at all (a value of a known key of the wrong type, `versions` it cannot parse) is a
  warning of `install`, `update` and `fetch`, and an error of `audit`.

## 8. HTTPS view

Generated from the repository by `otok index-view` on every change, next to the catalog.
`config.md` names a view as `index = "view+https://<base>"`; its files are under `<base>/index/`.
otok reads only the files a command needs, each at most once per command and revalidated with
`ETag`/`If-None-Match` (`cache.md` 11), so an unchanged index costs one small request per used recipe.

The files are JSON (UTF-8). The files of the repository are carried as their TOML text, unchanged, in
members named `toml`: otok reads them with the parsers of sections 3 to 7 and 11, so the view and the
repository cannot mean different things, and TOML's dates need no JSON form. Every file has
`"view-format": 1`, the version of this section.

| File | Members |
|---|---|
| `index/config.json` | `view-format`; `commit`: the 40 hex digits of the repository's commit the view was generated from; `toml`: `index.toml`. |
| `index/recipes.json` | `view-format`; `recipes`: for each recipe, sorted by id, `{ id, library, description, keywords, version, targets }` of its newest stable version (the newest when all are pre-releases), `targets` the sorted targets of its variants of that version. What `otok search` reads. |
| `index/recipes/<publisher>/<name>.json` | `view-format`; `id`; `entries`: `{ file, toml }` of each version entry (3) of every variant, newest version first (the library's order), then by variant, then newest revision first; `status`: `recipe-status.toml` (5), absent when the recipe has none. |
| `index/libraries/<library>.json` | `view-format`; `library`; `toml`: the library file (4), absent when there is none; `recipes`: the sorted ids of the recipes that build it. |
| `index/advisories.json` | `view-format`; `advisories`: `{ file, toml }` of each advisory (7), sorted by file name. |
| `index/availability.json` | `view-format`; `toml`: `availability.toml` (11), absent when the index has none. |
| `index/apple/<name>.json` | `view-format`; `toml`: `apple/<name>.toml` (10), for each of the three files. |

`file` is the path in the recipe's directory of the repository (`apple/3.5.4-r2.toml`), which otok checks as it checks the files
of a checkout. A recipe or library without its file in the view (`404`) does not exist; the other files
always exist.

`<base>/index/` keeps `view-format` 1. A view of another format is written into a directory of its own,
`<base>/index-<view-format>/`, which `otok index-view` writes beside the older ones for as long as otoks reading them
are in use; a view file whose `view-format` otok does not know is an error saying that it
needs a newer otok.

`index/libraries/openssl.json`, as `otok index-view` writes it:

```json
{
  "view-format": 1,
  "library": "openssl",
  "toml": "scheme = \"dotted\"\n",
  "recipes": [
    "apotocki/openssl"
  ]
}
```

`otok index-view [DIR] --output <dir>` generates the view of the checkout in `DIR` (default: the
current directory) into `<dir>/index/`. It reads every file strictly, as index CI checks it, so an index with
an invalid file has no view. The output is the same bytes for the same repository: members in the
order of the table, two spaces of indentation, a final newline. A file is written only when its
contents change, by renaming a complete file into place, and files of `<dir>/index/` it no longer
generates are removed: web servers derive `ETag` from the modification time and size, so a file
rewritten with the same bytes would be fetched again by every client.

The view is a cache of the git repository, never a source of truth: entries carry `recipe-sha256` and
source hashes, so a tampered view can withhold data but not change what is built. `index-mirror`
(`config.md`) replaces it with a copy of the view at another URL, with a directory holding a view
(one with `index/config.json`, such as the output of `otok index-view`), or with a checkout of the
repository (8.1): mirrors and air-gapped setups.

### 8.1 The repository instead of the view

otok also reads the git repository itself: the index `config.md` names (`index`) when it is a git
repository, which is how a private index without a view works (it is read with the user's git
credentials), and a directory with a checkout of it (`index-mirror`). For a repository URL, otok asks
the server for the commit of `HEAD` once per command, fetches the tree of that commit into the cache
as it fetches git inputs, and reads the files of sections 3 to 5 there (`cache.md` 10); offline it
reads the last commit it fetched. A directory is read as it is, and nothing is fetched.

## 9. Index CI

### 9.1 Authors, publishers and maintainers

- An **author** is the GitHub account that opens a pull request; anyone with an account may. `otok publish` opens it
  from the account of its token; the workflow names it by its numeric id (`--author-id`).
- A **publisher** is a name of `publishers/` (6), held by one account or by an organization whose public members
  publish for it. An author's first pull request publishing under a new name adds its publisher file, and an index
  maintainer merges it or closes it: merging it allows the name, and nothing else registers one. Every pull request
  of a publisher waits for a maintainer.
- An **established publisher** is a publisher whose file has `established = true`, set by an index maintainer: index
  CI merges its pull requests itself, within the limit of 9.2.
- An **index maintainer** is a GitHub account with the `write`, `maintain` or `admin` permission on the index's
  repository (`--repository <owner>/<name>`), as GitHub's API gives it (the `permission` of the account as a
  collaborator: `write` or `admin`). Maintainers merge the pull requests waiting for them; no limit and no age of the
  account apply to their own, which index CI merges itself.

How much an index reviews is thus its maintainers' choice, publisher by publisher: a publisher is reviewed until a
maintainer makes it established, and goes back to review when the maintainer removes the key.

### 9.2 Checks and verdicts

Every pull request is checked by `otok index-check`, run by the index's workflow on the merge of the
pull request with the head of the default branch (`--base` the default branch's commit, `--author-id`
the numeric GitHub id of the pull request's author, `--repository` the index's repository on GitHub,
`<owner>/<name>`, `--pull-request` its number). It executes nothing of the pull request: its files
are data, and otok comes from the index's own release, never from the pull request. It fails the
pull request (`refused`) unless:

- every changed file parses and validates (this document, common rules), and no file is changed
  outside the layout (1) but by a maintainer (below);
- no existing version entry is modified or deleted, no advisory is deleted, no library file, publisher
  file or recipe status is deleted, and `availability.toml` is not changed (11: its workflow writes it);
- every new entry is under a publisher registered before the pull request (or in it, below), whose
  `github-id` is the author's, or which is an organization the author is a public member of (6),
  and which is not `suspended`;
- for every new entry: the recipe tree at `recipe-source` hashes to `recipe-sha256`; the projection
  of its `recipe.toml` equals the entry (3); the tree respects the recipe limits (`recipe-toml.md` 15)
  and what publication requires; every toolchain of `[toolchains]` has a `default-recipe`
  (`recipe-toml.md` 13) naming a recipe of the index, the pull request included, with an entry of a tool recipe
  whose `[tool] programs` names it (`recipe-toml.md` 3.1, 13.1); `recipe-source`, `source`,
  resources and mirrors are `https`; `source` and resources download and match their hashes; the entry has no
  `published`, which the workflow writes as it merges it (3, below); archives with `verify-against` are compared with
  their git tree, and the differences are listed in the report without failing it (`recipe-toml.md` 4);
- the version of every new entry is valid in its library's scheme (4) and differs, in that scheme,
  from the versions of the variant's other entries with the same revision (`1.2` and `1.2.0` are one
  version under `semver`);
- the `[platforms] supported` of every new entry is of one target, and no other variant of the recipe has an entry of
  the same version sharing a cell with it (3);
- a new or changed library file is valid (4);
- a new publisher file names the author (`github-id`), unless the author is an index maintainer, under a name that is valid (common 2), free, and not confusable
  with a registered one (compared after lowercasing, removing `-` and reading `0`, `1`, `5` as `o`, `l`, `s`), and the
  author has no other open pull request adding a publisher file;
- the author's GitHub account is at least 30 days old, unless the author is an index maintainer or the pull request
  only adds what index CI merges for established publishers (below);
- for every publisher with new entries, unless the author is an index maintainer: its entries `published` in the
  last 24 hours at `--base`, the entries its other open pull requests add, and the new ones are together at most
  `established-entries-per-day` for an established publisher, `review-entries-per-day` for another (2, in the
  `index.toml` of `--base`).

A pull request that passes is merged by the index's workflow (verdict `auto`) when it only adds version
entries of its author's registered publishers, their library files, and advisories naming only
recipes of those publishers, and the author is an index maintainer or every one of those publishers is established;
a changed library file may only add versions to an `explicit` `order`, keeping
the order of those it had. Any other change passes with the verdict `maintainer` and waits for an
index maintainer: entries of a publisher that is not established, registering a publisher, changing a publisher file,
`recipe-status.toml`
(deprecation, transfer), a library's `scheme` or `order` otherwise, a library-wide advisory, the Apple
data files, `index.toml`, the workflows and any other file.

An index maintainer approves such a pull request by labelling it `approved`. The workflow checks it again with
`--approved-by`, the numeric id of the account that added the label: when that account is an index maintainer, the
verdict `maintainer` becomes `auto` and the workflow merges it, at the head the maintainer saw (the head of the
pull request when the label was added; a pull request changed since is not merged). An approval lets nothing refused
pass. The workflow removes `approved` from a pull request that changes, so an approval is of what was approved.

The workflow merges every pull request itself, approved ones too: `otok index-check --stamp` writes `published`, the
time of the check, into the new entries of the checked checkout (3), and the workflow commits the result as a merge
commit of the default branch's head and the pull request's head, pushed to the default branch. GitHub then shows the
pull request as merged. A push refused because the default branch moved fails the merge, and the check runs again on
the pull request's next change or by hand. Maintainers do not merge with GitHub's button: an entry merged that way has
no `published`, so no date and no place in the limit of entries per day. An index protects its default branch so
that only the workflow (and its administrators) can push to it.

The open pull requests are the index repository's (`--repository`), read through GitHub's API, the one checked
(`--pull-request`) left out; their entries count whatever their check said, refused ones too, until they are merged
or closed. Without `--repository` there are none, and nobody is a maintainer.

When the limit refuses a pull request, its reason says when it would pass: the time the publisher's entries of the
last 24 hours have dropped to the limit less this pull request's and its other open ones'; or that its other open
pull requests, named, must be merged or closed first; or, when the pull request alone adds more than the limit,
that it must be split. Publishing again after that time updates the pull request with a new commit on
the index's head, and index CI checks it again; nothing checks a pull request again without a change of it.

The workflow runs on `pull_request_target`, so its own definition comes from the default branch and a
pull request cannot change what checks it; it checks the pull request out as data (without
credentials), runs `otok index-check`, and merges an `auto` one at the commit it checked. `--summary
<file>` writes the verdict it reads:

```json
{ "format": 1, "verdict": "auto", "reasons": [], "notes": ["…"] }
```

`verdict` is `auto`, `maintainer` or `refused`; `reasons` say why it is not `auto`; `notes` hold what
the report lists without deciding (the `verify-against` differences).

`--comment <file>` writes the report as the Markdown of a comment for the pull request, and the
workflow posts it in the pull request, replacing its comment of an earlier check, whatever the
verdict: the author learns there whether the pull request is merged, waits for a maintainer or is
refused, and why. The comment ends with the summary in an HTML comment, `<!-- otok-index-check
{…} -->`, the JSON on one line with every `>` written `>`, which `otok publish` reads when it
updates the pull request.

The workflow also labels the pull request with its verdict, `ready-for-review` for `maintainer` and `refused` for
`refused`, removing the other (and both from one merged): the maintainers' queue is the list of open pull requests
labelled `ready-for-review`, every one of which has passed the checks; they leave it labelled `approved`.

### 9.3 Terms of publication

A publisher signs nothing and writes nothing about licenses into a recipe. The index repository's `LICENSE`
(CC0 1.0 for its data) and `CONTRIBUTING.md` state the terms, which a pull request accepts (GitHub's terms make
contributions to a repository subject to its license); `otok`'s `tools/index-ci/repository/` holds the public
index's. They say:

- the files of the index a pull request adds or changes (entries, library and publisher files, recipe status,
  advisories) are in the public domain under CC0;
- a recipe tree is not copied into the index and keeps the license its publisher gives it; the publisher allows
  the index, its view and its catalog to fetch, keep, show, compare and redistribute unchanged the files of the
  trees their entries name, for as long as the entries are in the index;
- a publisher publishes only trees they may allow that for;
- merged entries are permanent (3; withdrawal is an advisory, 7).

`recipe.toml`'s `license` is the license of the library built, not of the recipe (`recipe-toml.md` 3), which is the
recipe repository's own.

## 10. Apple data files

### 10.1 `apple/required-reason-apis.toml`

The symbols of Apple's required-reason APIs, by the category a privacy manifest declares
them under (`NSPrivacyAccessedAPIType`). otok matches them against the undefined symbols of the static
libraries it packages for Apple (`otok-toml.md` 4.4). otok carries a built-in copy of this file, used
when the index has none (a private index, offline work).

```toml
format = 1

[[api]]
category = "NSPrivacyAccessedAPICategoryFileTimestamp"
symbols = ["stat", "fstat", "lstat", "fstatat", "getattrlist", "fgetattrlist", "getattrlistat", "getattrlistbulk",
           "NSFileCreationDate", "NSFileModificationDate", "NSURLCreationDateKey", "NSURLContentModificationDateKey"]

[[api]]
category = "NSPrivacyAccessedAPICategoryUserDefaults"
classes = ["NSUserDefaults"]
```

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | `1`. |
| `api` | array of tables | One per category; a category appears once. |
| `api.category` | string | The `NSPrivacyAccessedAPIType` value. |
| `api.symbols` | array of strings | C functions and global variables, by their C names: `stat`, not the Mach-O `_stat` or `_stat$INODE64`. |
| `api.classes` | array of strings | Objective-C classes (`NSUserDefaults` matches `_OBJC_CLASS_$_NSUserDefaults`). |

One symbol may be in several categories (`getattrlist` reads file timestamps and disk space); a use of it
is declared when any of them is. Selectors (`systemUptime`, `activeInputModes`) are not symbols and are
not listed.

### 10.2 `apple/private-libraries.toml`

The system libraries of Apple's SDKs that are private on iOS (no public headers): App Store review rejects
an app whose executable has a symbol named like one of their exports (ITMS-90338), whoever defines it — a
static ICU built without renaming defines the exports of `libicucore`. otok does not list the symbols: it
reads them from the SDK's text stubs of the libraries (`.tbd`), so the list stays right for every SDK. It
matches them against the symbols that the static libraries it packages define (`otok-toml.md` 4.5). otok
carries a built-in copy of this file, used when the index has none.

```toml
format = 1

[[library]]
path = "usr/lib/libicucore.tbd"
name = "ICU (libicucore)"
```

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | `1`. |
| `library` | array of tables | One per library. |
| `library.path` | string | The text stub, relative to the SDK root. An SDK without it is skipped. |
| `library.name` | string | How messages name the library. |

Public system libraries (`libz`, `libsqlite3`, `libxml2`) are not listed: apps link static copies of them
without trouble.

## 11. Availability of inputs

A published version builds only while its inputs can be fetched: the recipe tree (`recipe-source`), the
source and the resources. Hashes stop substitution, not loss. The index's scheduled
workflow runs `otok index-probe` weekly and commits what it finds to `availability.toml`:

```toml
# Generated by otok index-probe; do not edit.
checked = 2026-11-02T03:00:00Z

[[unavailable]]
recipe = "apotocki/openssl"
version = "3.5.4-r1"
since = 2026-10-26T03:00:00Z
inputs = ["source https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz"]
```

| Key | Meaning |
|---|---|
| `checked` | When the probe ran. |
| `unavailable` | The versions with an input that could not be fetched, by recipe and version, oldest first. |
| `unavailable.since` | The first probe that missed it; kept while it stays missing. |
| `unavailable.inputs` | What is missing: `recipe`, `source` or `resource <name>`, and its location. |

An archive is available when `url` or one of its `mirrors` answers a `HEAD` request (or a one-byte
`GET`) with success; a git input when its repository gives its commit to a shallow fetch. Contents are
not downloaded: the hashes are checked by every build. A version that becomes available again leaves
the file.

The file changes nothing for resolution: `otok info` shows it ("inputs unavailable since …"), and so
does the catalog. A user who has the inputs (the cache, `otok vendor`, `asset-mirror`) still
builds. Pull requests may not change it (9).
