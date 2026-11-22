# `--format json` — what commands print for scripts and agents, format 1

Status: draft. Common rules are in [README.md](README.md).

Commands that report something print it for people by default: columns, sections, sentences, laid out to be read. With
`--format json` they print one JSON document instead, which scripts, IDEs and coding agents read without parsing that
layout, and which says everything the text says. `otok metadata` (`metadata.md`) is JSON always and stays as it is.

This document defines the rules common to every command (1, 2) and the document of each command (3, 4). Commands not
listed here do not take `--format`.

## 1. The option

```text
otok <command> --format text|json …
```

- `text`, the default, is the output for people, which may change in any release.
- `json` prints the document of 2 and of the command's section on standard output, UTF-8, followed by a line end,
  and nothing else there. The exit code is the command's own, as without the option.
- Errors and warnings of the command are in the document (2.1) and are not printed on standard error as well.
  Standard error still carries what is shown while the command works (notes of waiting for the cache, progress), which
  a reader of the document can ignore.
- An error of the command line itself (an unknown option, a missing argument, an unknown value of `--format`) is
  reported as without the option, as text on standard error, with no document: otok has not yet started the command.

## 2. The document

Every document is an object with these keys, beside those of its command:

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | The format of this document; `1`. A change that removes or changes a key raises it; new keys may be added without (unlike the files of README 1.4, which a newer otok must read: a reader of this document runs against the otok it calls). |
| `otok` | string | The version of otok that wrote it. |
| `command` | string | The command: `"search"`, `"info"`. |
| `diagnostics` | array | 2.1: the errors and warnings of the command, in the order they were found; empty when there are none. |

The keys of the command are present when the exit code is 0. When the command fails they are `null`, and
`diagnostics` holds at least one error that says why.

As in `metadata.md` 3: every key listed is always present, `null` stands for "none", object keys are written in sorted
order (the examples are ordered for reading) with two spaces of indentation, and paths are absolute with `/` as the
separator, so that the same state always gives the same bytes. Times are UTC in RFC 3339 (`"2026-11-20T12:30:33Z"`).

### 2.1 `diagnostics`

| Key | Type | Meaning |
|---|---|---|
| `severity` | string | `"error"` or `"warning"`. |
| `message` | string | The message as the text output prints it after `error: ` or `warning: `. |
| `file` | string or null | The file it concerns (README 1.3), when it concerns one, named as the text output names it (not always absolute). |
| `line`, `column` | integer or null | Its place in `file`, from 1; `null` when not known. |

```json
{
  "severity": "error",
  "message": "unknown key 'optins' in deps.openssl",
  "file": "/work/app/otok.toml",
  "line": 12,
  "column": 5
}
```

### 2.2 Common values

- A **recipe id** is a string, `"apotocki/libpng"`.
- `index` is the index of the project a recipe was read from (`otok-toml.md` 6.4): `"default"` or a name of
  `[indexes]`, as in `metadata.md` 3.3; outside a project always `"default"`.
- A **version** is a string as the recipe writes it (`"1.6.47"`), a **revision** an integer.
- An **option value** is a JSON boolean, integer, string or array of strings, as in `metadata.md` 3.3.
- A **condition** (`recipe-toml.md` 9.1) is an object holding exactly the keys the recipe writes: `option`, `has`,
  `is` (an option value), `os`, `platform`, `host`, `arch`, `environment` (arrays of strings), `linkage` (a string),
  and `not` (a condition). Its keys are not always present: a reader tests the presence of each, as the recipe does.
- An **advisory** (`index.md` 7) is `{ id, severity, summary, unknown }`: `severity` `"withdrawn"`, `"security"` or
  `"broken"`, as this otok takes it (`index.md` 7.2); `unknown`, an array of the values of the advisory this otok
  does not know and read in place of others, empty when there are none.

## 3. `otok search`

```text
otok search TEXT... [--offline] --format json
```

It finds the recipes as the text output does: those whose id, library, description or one of whose keywords contains
the text, compared without case. Finding none is not an error.

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "search",
  "diagnostics": [],
  "query": "png",
  "recipes": [
    {
      "id": "apotocki/libpng",
      "index": "default",
      "library": "libpng",
      "description": "libpng, the reference library for PNG images.",
      "keywords": ["png", "image"],
      "version": "1.6.47",
      "targets": ["android", "apple", "linux", "windows"]
    }
  ]
}
```

| Key | Type | Meaning |
|---|---|---|
| `query` | string | The words of `TEXT...` joined by spaces, as they were compared. |
| `recipes` | array | The recipes found, of every index of the project, sorted by id and then index. |

Each recipe:

| Key | Type | Meaning |
|---|---|---|
| `id`, `index`, `library` | | 2.2 |
| `description` | string | Of its newest stable version. |
| `keywords` | array of strings | Of its newest stable version, as the recipe orders them. |
| `version` | string | Its newest stable version (its newest version when it has no stable one). |
| `targets` | array of strings | The targets of the variants of that version, sorted. |

## 4. `otok info`

```text
otok info RECIPE|LIBRARY [--offline] --format json
```

Given a library name, it lists the recipes of every index that build that library (4.1); given a recipe id, it
describes that recipe (4.2). Exactly one of the two keys is not `null`. A library no recipe builds, or a recipe id no
index has, is an error.

### 4.1 The recipes of a library

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "info",
  "diagnostics": [],
  "library": "libpng",
  "recipes": [
    {
      "id": "apotocki/libpng",
      "index": "default",
      "library": "libpng",
      "description": "libpng, the reference library for PNG images.",
      "keywords": ["png", "image"],
      "version": "1.6.47",
      "targets": ["android", "apple", "linux", "windows"]
    }
  ],
  "recipe": null
}
```

| Key | Type | Meaning |
|---|---|---|
| `library` | string or null | The library asked for; `null` when a recipe id was. |
| `recipes` | array or null | Its recipes, as in 3, sorted by id and then index. |
| `recipe` | null | |

### 4.2 A recipe

The recipe `apotocki/libpng` of the index (hashes of 64 digits and commits of 40 shortened to `…`; the variants
`android` and `apple`, and the older entries of `versions`, left out):

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "info",
  "diagnostics": [],
  "library": null,
  "recipes": null,
  "recipe": {
    "id": "apotocki/libpng",
    "index": "default",
    "library": "libpng",
    "description": "libpng, the reference library for PNG images.",
    "license": "libpng-2.0",
    "keywords": ["png", "image"],
    "status": { "deprecated": null, "in-favor-of": null, "transferred-from": null },
    "versions": [
      { "version": "1.6.47", "revision": 3, "variant": "linux", "channel": "stable", "published": "2026-11-20T18:56:07Z", "advisories": [], "unavailable": null },
      { "version": "1.6.47", "revision": 3, "variant": "windows", "channel": "stable", "published": "2026-11-18T22:54:41Z", "advisories": [], "unavailable": null },
      { "version": "1.6.47", "revision": 2, "variant": "linux", "channel": "stable", "published": "2026-11-19T12:43:49Z", "advisories": [], "unavailable": null }
    ],
    "unreadable": [],
    "version": "1.6.47",
    "variants": [
      {
        "variant": "linux",
        "revision": 3,
        "target": "linux",
        "platforms": ["linux"],
        "architectures": null,
        "build-on": ["linux/gcc", "linux/clang"],
        "linkage": ["static", "dynamic"],
        "c-standard": null,
        "cxx-standard": null,
        "programs": [],
        "options": [],
        "dependencies": [
          {
            "library": "zlib",
            "programs": [],
            "version": ">= 1.2",
            "default-recipe": "apotocki/zlib",
            "prerelease": false,
            "requires-options": {},
            "linkage": null,
            "export": false,
            "toolchains": [],
            "from-dependency": false,
            "when": null
          }
        ],
        "tool-dependencies": [
          {
            "library": null,
            "programs": ["cmake"],
            "version": ">= 3.20",
            "default-recipe": "apotocki/cmake",
            "prerelease": false,
            "requires-options": {},
            "linkage": null,
            "export": false,
            "toolchains": [],
            "from-dependency": false,
            "when": null
          },
          {
            "library": null,
            "programs": ["ninja"],
            "version": ">= 1.10",
            "default-recipe": "apotocki/ninja",
            "prerelease": false,
            "requires-options": {},
            "linkage": null,
            "export": false,
            "toolchains": [],
            "from-dependency": false,
            "when": null
          }
        ],
        "recipe-source": { "git": "https://github.com/otok-dev/recipes", "commit": "…", "tag": null, "path": "recipes/libpng/1.6/linux" },
        "recipe-sha256": "…"
      }
    ]
  }
}
```

An option and a dependency of the variant `apple` of `apotocki/curl` 8.23.0:

```json
{
  "name": "tls",
  "type": "enum",
  "values": ["openssl", "mbedtls", "none"],
  "default": "openssl",
  "additive": false,
  "doc": "The TLS library: OpenSSL, whose certificates are verified by the system's Security framework, Mbed TLS (the consumer sets the certificates, CURLOPT_CAINFO_BLOB), or none (no HTTPS)."
}
```

```json
{
  "library": "openssl",
  "programs": [],
  "version": ">= 3.0",
  "default-recipe": "apotocki/openssl",
  "prerelease": false,
  "requires-options": {},
  "linkage": null,
  "export": false,
  "toolchains": [],
  "from-dependency": false,
  "when": { "option": "tls", "is": "openssl" }
}
```

| Key | Type | Meaning |
|---|---|---|
| `library`, `recipes` | null | |
| `recipe` | object | The recipe, from its index entries (`index.md` 3); nothing is fetched from its recipe source. |

`recipe`:

| Key | Type | Meaning |
|---|---|---|
| `id`, `index`, `library` | | 2.2 |
| `description`, `license`, `keywords` | | Of its newest stable version (`recipe-toml.md` 3). |
| `status` | object | `index.md` 5: `deprecated`, the reason, or `null` when it is not deprecated; `in-favor-of`, the recipe id to use instead, or `null`; `transferred-from`, the recipe id it was transferred from, or `null`. |
| `versions` | array | Every entry this otok reads, newest version first, and within one version newest revision first and then by variant (4.2.1). |
| `unreadable` | array | The entries this otok cannot read (`index.md` 3.2): `{ variant, version, revision, reason }`, `reason` the first problem its reader found; the text says that they need a newer otok. |
| `version` | string | The version `variants` describe: its newest stable version (its newest when it has no stable one). |
| `variants` | array | Every variant of `version`, each at its newest revision, sorted by name (4.2.2). |

#### 4.2.1 `versions`

| Key | Type | Meaning |
|---|---|---|
| `version`, `revision`, `variant` | | Of the entry. |
| `channel` | string | `"stable"` or `"prerelease"`. |
| `published` | string or null | When index CI merged it (`index.md` 3); `null` for an entry without that time. |
| `advisories` | array | 2.2: the active advisories concerning it (`index.md` 7.1: any severity, not `rejected`), sorted by id. |
| `unavailable` | object or null | `index.md` 11: `{ since, inputs }` when the inputs of the entry were found unavailable, `inputs` the names of those inputs; else `null`. |

#### 4.2.2 `variants`

What the entry of the variant holds (`index.md` 3, `recipe-toml.md`), every list in the order of the entry:

| Key | Type | Meaning |
|---|---|---|
| `variant`, `revision` | | |
| `target` | string or null | The target of its platforms; `null` when this otok knows none of them. |
| `platforms` | array of strings | `[platforms] supported`, the platforms this otok knows. |
| `architectures` | object or null | `[platforms] architectures`: platform → array of strings; `null` when the recipe restricts none. |
| `build-on` | array of strings | `[platforms] build-on`. |
| `linkage` | array of strings | `[recipe] linkage`, its default `["static"]` filled in. |
| `c-standard`, `cxx-standard` | string or null | The constraints of `[recipe]`; `null`: any. |
| `programs` | array of strings | `[tool] programs`: what it gives as toolchains, and what a project may deliver (`otok-toml.md` 7.1). |
| `options` | array | `{ name, type, values, default, additive, doc }` of each `[options.<name>]`: `type` as `recipe-toml.md` 8 writes it, `values` an array (empty when the option has none), `default` an option value (`null` for `file`), `doc` what the option does (`recipe-toml.md` 8), `null` for an entry made before index entries had it (`index.md` 3). |
| `dependencies`, `tool-dependencies` | array | 4.2.3: `[deps.*]`, and `[tool-deps.*]` with the toolchains of `[toolchains]` that are tool dependencies (`recipe-toml.md` 13.1, 13.2). |
| `recipe-source` | object | `{ git, commit, tag, path }` (`tag`, `path` `null` when the entry has none). |
| `recipe-sha256` | string | |

#### 4.2.3 Dependencies

| Key | Type | Meaning |
|---|---|---|
| `library` | string or null | The library; `null` for a toolchain with `default-recipe`, whose library is that of its recipe, which the resolver learns (`recipe-toml.md` 13.1). |
| `programs` | array of strings | For such a toolchain, its programs; else empty. |
| `version` | string or null | The constraint; `null` with `from-dependency`. |
| `default-recipe` | recipe id or null | `null` with `from-dependency`. |
| `prerelease` | boolean | |
| `requires-options` | object | option → option value. |
| `linkage` | string or null | `"static"`, `"dynamic"`, or `null` when it asks for none. |
| `export` | boolean | |
| `toolchains` | array of strings | `[deps.<library>] toolchains`. |
| `from-dependency` | boolean | A program of a dependency (`recipe-toml.md` 13.2): its recipe and version are those the dependency gets. |
| `when` | condition or null | 2.2; `null`: unconditional. |

## 5. Later

The commands that are to take `--format json` next, each with its section here: `outdated`, `audit`, `why`,
`log --list`, and `install`, `update` and `fetch`, whose document will be a stream of JSON lines, one event per line
(the slices built, the targets skipped and why, the diagnostics), since they run for a long time. `otok tree` gets none:
its graph is in `otok metadata`.
