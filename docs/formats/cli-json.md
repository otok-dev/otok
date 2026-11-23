# `--format json` — what commands print for scripts and agents, format 1

Status: draft. Common rules are in [README.md](README.md).

Commands that report something print it for people by default: columns, sections, sentences, laid out to be read. With
`--format json` they print one JSON document instead, which scripts, IDEs and coding agents read without parsing that
layout, and which says everything the text says. `otok metadata` (`metadata.md`) is JSON always and stays as it is.

This document defines the rules common to every command (1, 2) and the document of each command (3 to 10). Commands not
listed here do not take `--format`.

## 1. The option

```text
otok <command> --format text|json …
```

- `text`, the default, is the output for people, which may change in any release.
- `json` prints the document of 2 and of the command's section on standard output, UTF-8, followed by a line end,
  and nothing else there; `install`, `update` and `fetch` print a stream of events instead, one per line (9). The exit
  code is the command's own, as without the option.
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
| `command` | string | The command: `"search"`, `"info"`, `"outdated"`, `"audit"`, `"why"`, `"log"`. |
| `diagnostics` | array | 2.1: the errors and warnings of the command, in the order they were found; empty when there are none. |

The keys of the command are present when it did its work, whatever its exit code (`otok audit` exits with 1 for what it
found, 6). When the command could not do it they are `null`, and `diagnostics` holds at least one error that says why.

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

## 5. `otok outdated`

```text
otok outdated [--profile NAME] [--offline] --format json
```

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "outdated",
  "diagnostics": [],
  "packages": [
    {
      "library": "libpng",
      "tool": false,
      "id": "libpng",
      "target": "linux",
      "environment": "linux/gcc",
      "recipe": "apotocki/libpng",
      "index": "default",
      "current": { "version": "1.6.47", "revision": 1 },
      "compatible": { "version": "1.6.47", "revision": 2 },
      "latest": { "version": "1.6.50", "revision": 1 },
      "behind": true,
      "deprecation": null,
      "unreadable": []
    }
  ]
}
```

`packages` holds every package of the graphs this machine builds, as the text lists them: the target packages, then the
tool packages, in the order of the resolution.

| Key | Type | Meaning |
|---|---|---|
| `library` | string | The library name. |
| `tool` | boolean | A tool package (`otok-toml.md` 7). |
| `id` | string | Its id in `otok.lock` (`otok-lock.md` 2): `"zlib"`, `"zlib@android"`, `"cmake@4.4.5"`. |
| `target`, `environment` | string | The target and the environment of its graph, as in `metadata.md` 3.3. |
| `recipe` | string | The recipe id, `"git"` or `"path"`, as in `metadata.md` 3.3. |
| `index` | string or null | As in `metadata.md` 3.3; `null` for a git or path recipe. |
| `current` | `{ version, revision }` or null | What `otok.lock` holds; `null` when it has no entry for the package. |
| `compatible` | `{ version, revision }` or null | What `otok update` would choose within the constraints of `otok.toml`; `null` when the package would leave the graph. |
| `latest` | `{ version, revision }` or null | The newest stable version of the recipe in its index, whatever the constraints, at the newest revision of the package's variant (`recipe-toml.md` 6.1), or of the first variant of that version by name when the package's has none; `null` for a git or path recipe and when the index cannot be read. |
| `behind` | boolean | Whether `compatible` or `latest` differs from `current`. |
| `deprecation` | string or null | `"deprecated: <reason>; use '<id>' instead"` when the recipe is deprecated (`index.md` 5). |
| `unreadable` | array | The entries of the recipe this otok cannot read, as in 4.2. |

## 6. `otok audit`

```text
otok audit [--offline] --format json
```

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "audit",
  "diagnostics": [],
  "packages": 12,
  "findings": [
    {
      "kind": "advisory",
      "severity": "error",
      "message": "libpng 1.6.47 (r1) from 'apotocki/libpng' has advisory 2026-0003 (security): Overflow in png_read_row.",
      "package": {
        "library": "libpng",
        "tool": false,
        "target": "linux",
        "recipe": "apotocki/libpng",
        "index": "default",
        "variant": "linux",
        "version": "1.6.47",
        "revision": 1
      },
      "license": null,
      "advisory": { "id": "2026-0003", "severity": "security", "summary": "Overflow in png_read_row.", "unknown": [] },
      "deprecation": null,
      "index": null
    }
  ]
}
```

What audit finds is in `findings`, not in `diagnostics`, which holds only the problems that kept it from checking
something (an index that cannot be read). The exit code is 1 when a finding is an error, as without `--format`.

| Key | Type | Meaning |
|---|---|---|
| `packages` | integer | The number of packages of `otok.lock` checked, tool packages included. |
| `findings` | array | In the order the text reports them: the licenses, then each package's advisories (sorted by id) and deprecation, then the advisories that cannot be read. Empty: nothing to report. |

A finding:

| Key | Type | Meaning |
|---|---|---|
| `kind` | string | `"license"`: `[policy]` refuses the package's license (`otok-toml.md` 9); `"advisory"` (`index.md` 7); `"deprecated"` (`index.md` 5); `"unreadable-advisory"`: an advisory of an index this otok cannot read (`index.md` 7.2), which may concern any package. |
| `severity` | string | `"error"` (a refused license, a `withdrawn` or `security` advisory, an advisory that cannot be read) or `"warning"`. |
| `message` | string | What the text prints after `error: ` or `warning: `. |
| `package` | object or null | The package of `otok.lock`: `library`, `tool`, `target` (`null` for a tool package), `recipe`, `index`, `variant` (as in `metadata.md` 3.3), `version`, `revision`. `null` for `"unreadable-advisory"`. |
| `license` | string or null | `"license"`: the package's license expression. |
| `advisory` | advisory or null | `"advisory"`: 2.2. |
| `deprecation` | string or null | `"deprecated"`: as `deprecation` of 5. |
| `index` | string or null | `"unreadable-advisory"`: the index, `"default"` or a name of `[indexes]`. |

## 7. `otok why`

```text
otok why LIBRARY [--profile NAME] [--offline] --format json
```

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "why",
  "diagnostics": [],
  "library": "zlib",
  "profile": null,
  "packages": [
    {
      "library": "zlib",
      "tool": false,
      "id": "zlib",
      "target": "linux",
      "environment": "linux/gcc",
      "recipe": "apotocki/zlib",
      "index": "default",
      "version": "1.3.2",
      "revision": 3,
      "required-by": [
        { "requester": "otok.toml", "constraint": null },
        { "requester": "apotocki/libpng 1.6.47", "constraint": ">= 1.2" }
      ],
      "platforms": ["linux"],
      "options": {
        "minizip": { "value": false, "requests": [] },
        "shared": { "value": true, "requests": [{ "requester": "otok.toml", "value": true }] }
      }
    }
  ]
}
```

| Key | Type | Meaning |
|---|---|---|
| `library` | string | The library asked about. |
| `profile` | string or null | The profile selected with `--profile`. |
| `packages` | array | Every package of the library in the graphs this machine builds, as in 5: one per target and environment, and a tool package beside them when it is also a tool. |

A package:

| Key | Type | Meaning |
|---|---|---|
| `library`, `tool`, `id`, `target`, `environment`, `recipe`, `index` | | As in 5. |
| `version`, `revision` | string, integer | The version the resolution chose, with its revision. |
| `required-by` | array of `{ requester, constraint }` | As in `metadata.md` 3.3: who asked for the library and with which constraint, `null` for any version. |
| `platforms` | array of strings | The platforms it is built for in the selected profile; empty when not built. |
| `options` | object: name → `{ value, requests }` | Every option of its recipe: the merged value (2.2) and who asked for which value (`{ requester, value }`), in the order the resolver met them; empty `requests`: the default. |

A library that is not in the graphs is an error, and the keys of the command are `null`.

## 8. `otok log --list`

```text
otok log LIBRARY [SLICE] --list --format json
```

The log itself is text, which `--format json` does not wrap: without `--list` it is an error of the command line.

```json
{
  "format": 1,
  "otok": "0.6.0",
  "command": "log",
  "diagnostics": [],
  "library": "zlib",
  "slice": null,
  "cache": "/home/me/.cache/otok",
  "logs": [
    {
      "file": "/home/me/.cache/otok/failed/zlib-linux-x86_64-20261122-123033/build.log",
      "version": "1.3.2",
      "slice": "linux-x86_64",
      "configuration": "release",
      "entry": "build",
      "failed": true,
      "time": "2026-11-22T12:30:33Z"
    }
  ]
}
```

| Key | Type | Meaning |
|---|---|---|
| `library` | string | The library asked about. |
| `slice` | string or null | The slice asked about, `null` when none was. |
| `cache` | path | The cache the logs were found in (`cache.md`). |
| `logs` | array | The logs kept in the cache, the newest first. None is an error, as without `--format`. |

A log:

| Key | Type | Meaning |
|---|---|---|
| `file` | path | The log file. |
| `version` | string | The version of the library built. |
| `slice`, `configuration` | string or null | The slice and its configuration (`"release"`, `"debug"`); `null` for the patching of a source. |
| `entry` | string | `"build"`, `"build_tools"` (the `build_tools` entries of the recipe, `recipe-toml.md`), or `"source"` (the source patched). |
| `failed` | boolean | Whether the build failed (the log kept in `failed/`, `cache.md`). |
| `time` | string | When the log was last written. |

## 9. `otok install`, `update` and `fetch`

```text
otok install|update|fetch … --format json
```

These run for minutes, so they do not print one document at the end: they print a stream of JSON lines (JSON Lines,
`https://jsonlines.org`), one event per line as it happens, each a JSON object on one line with its `event`. The rules
of 1 and 2 hold for each event; in place of the keys of 2, the stream begins with `start` and ends with `end`:

```text
{"command":"install","event":"start","format":1,"otok":"0.6.0"}
{"changes":["added zlib 1.3.2 (r3)"],"event":"lock","state":"created"}
{"event":"inputs","resources":0,"sources":1}
{"build-tools":false,"configuration":"release","dev":null,"event":"slice","hint":null,"id":"zlib","library":"zlib","log":null,"message":null,"revision":3,"seconds":null,"slice":"linux-x86_64","state":"building","tool":false,"version":"1.3.2"}
{"build-tools":false,"configuration":"release","dev":null,"event":"slice","hint":null,"id":"zlib","library":"zlib","log":null,"message":null,"revision":3,"seconds":12.4,"slice":"linux-x86_64","state":"built","tool":false,"version":"1.3.2"}
{"cached":0,"count":1,"event":"slices"}
{"copied":14,"dir":"Otok","event":"output","removed":0,"target":"linux","unchanged":0}
{"event":"end","exit":0}
```

A reader takes each line as it comes and ignores the events and keys it does not know. A line of standard output that
otok has not made into an event of its own is given as a `text` event rather than left out, so the stream is always
valid. Standard error carries the same progress for people as without the option (the slices under way, `note:` lines
of waiting and fetching); errors and warnings are `diagnostic` events instead.

| `event` | Keys | When |
|---|---|---|
| `start` | `format`, `otok`, `command` (`"install"`, `"update"`, `"fetch"`) | First. |
| `lock` | `state`: `"created"`, `"updated"`, `"rewritten"` (the same packages written anew) or `"up-to-date"`; `changes`: the changes, as the text prints them (`"added zlib 1.3.2 (r3)"`, `"zlib: 1.3.1 (r1) -> 1.3.2 (r3)"`) | After resolving, once `otok.lock` is checked or written. Not with an error before. |
| `standard` | `target`, `language` (`"C"`, `"C++"`), `standard` (`"17"`) | A C or C++ standard the project does not set, chosen as the lowest every recipe allows (`otok-toml.md` 5.1). |
| `inputs` | `sources`, `resources`: how many are in the cache | Once the inputs are fetched (or found in the cache). |
| `skipped` | `target`, `reason` | A target table this machine does not build (`otok-toml.md` 4.9). |
| `linkage-fallback` | `package` (its id in `otok.lock`), `linkage`, `reason` | A library built with another linkage than its target table prefers (`otok-toml.md` 5). |
| `slice` | `state`: `"building"`, `"built"`, `"cached"` or `"failed"`; `library`, `id` (as in 5), `version`, `revision`, `tool`, `build-tools` (the `build_tools` entry of its recipe), `slice` (`"linux-x86_64"`, `"headers"`), `configuration`, `seconds` (how long it built, to a tenth, for `"built"` and `"failed"`; else `null`), `dev` (the working trees of `otok.dev.toml` it was built from, as the text names them; else `null`); for `"failed"`, else `null`: `message` (the error, as the text prints it), `log` (the log kept in `failed/`, `cache.md` 7.1; `null` when none could be kept or the failure came before the build had one), `hint` (what the user can do about a cause otok recognizes, as the text prints it after `hint: `; `null` when none is known) | Each slice, as it starts building, is built, is found in the cache, or fails. A slice that fails is also an error `diagnostic`, which comes after it. |
| `slices` | `count`, `cached` | Once every slice is there. |
| `output` | `target`, `dir` (the output as `otok.toml` writes it), `copied`, `unchanged`, `removed` (files) | Each output written. |
| `tools` | `dir`, `copied`, `unchanged`, `removed` | The tools output written (`otok-toml.md` 7.1). |
| `garbage` | `entries`, `bytes` | The cache entries removed by the collection at the end (`cache.md` 9). |
| `diagnostic` | as 2.1 | An error or a warning. Those of the slices and the outputs come as they are found; those otok gathers while it checks the project and the lock (and the failures of slices) come together before `end`. |
| `text` | `text` | A line of standard output not made into an event (see above). |
| `end` | `exit` | Last: the exit code. |

## 10. `otok publish`

```text
otok publish … --format json
```

`otok publish` builds for minutes too, and prints JSON lines as 9 does: `start` (`command` `"publish"`), the events below
as they happen, then `end` with the exit code. Errors and warnings are `diagnostic` events; the progress for people
stays on standard error.

With `--dry-run --no-build` it is a check: each recipe is fetched back from its remote, read, given its entry and
compared with the index as a publication does before it builds, and nothing is built or published. A recipe that
cannot be published is an error `diagnostic` whose `file` is its `recipe.toml` (absolute), and the others are
checked all the same; the check ends with `publication` `"checked"`, its exit code 1 when a recipe was refused. A
publication refuses before anything, every refused recipe said the same way.

```text
{"command":"publish","event":"start","format":1,"otok":"0.6.0"}
{"checkout":false,"event":"index","index":"https://github.com/otok-dev/index","publisher":"apotocki"}
{"entry":"recipes/apotocki/zlib/linux/1.3.2-r3.toml","event":"recipe","library":"zlib","published":false,"recipe":"apotocki/zlib","revision":3,"source":{"commit":"5c7d…","git":"https://github.com/otok-dev/recipes.git","path":"recipes/zlib/1.3"},"variant":"linux","version":"1.3.2"}
{"adding":1,"event":"limit","exceeded":false,"key":"review-entries-per-day","limit":5,"passes-at":null,"pending":0,"pending-requests":[],"recent":0}
{"build":1,"environment":"linux/gcc","event":"build","library":"zlib","options":null,"platforms":["linux"],"recipe":"apotocki/zlib","revision":3,"seconds":null,"state":"queued","target":"linux","variant":"linux","version":"1.3.2"}
{"build":1,"environment":"linux/gcc","event":"build","library":"zlib","options":null,"platforms":["linux"],"recipe":"apotocki/zlib","revision":3,"seconds":null,"state":"building","target":"linux","variant":"linux","version":"1.3.2"}
{"build":1,"build-tools":false,"configuration":"release","dev":null,"event":"slice","hint":null,"id":"zlib","library":"zlib","log":null,"message":null,"revision":3,"seconds":null,"slice":"linux-x86_64","state":"building","tool":false,"version":"1.3.2"}
{"build":1,"build-tools":false,"configuration":"release","dev":null,"event":"slice","hint":null,"id":"zlib","library":"zlib","log":null,"message":null,"revision":3,"seconds":12.4,"slice":"linux-x86_64","state":"built","tool":false,"version":"1.3.2"}
{"build":1,"environment":"linux/gcc","event":"build","library":"zlib","options":null,"platforms":["linux"],"recipe":"apotocki/zlib","revision":3,"seconds":14.1,"state":"built","target":"linux","variant":"linux","version":"1.3.2"}
{"entries":["recipes/apotocki/zlib/linux/1.3.2-r3.toml"],"event":"publication","published":[],"pull-request":{"branch":"publish/apotocki/zlib-linux-1.3.2-r3","last-check":null,"number":412,"reused":false,"url":"https://github.com/otok-dev/index/pull/412"},"registered":null,"state":"pull-request"}
{"event":"end","exit":0}
```

| `event` | Keys | When |
|---|---|---|
| `start` | as in 9, `command` `"publish"` | First. |
| `index` | `index` (the repository on GitHub, or the checkout of the index as an absolute path), `checkout` (whether it is a checkout), `publisher` | Once the index and the publisher are known. |
| `recipe` | `recipe` (the recipe id it is published under, `"<publisher>/<name>"`), `library`, `variant`, `version`, `revision`, `entry` (the path of its entry in the index, `index.md` 3), `source` (`git`, `commit`, `path`: where users fetch it; `path` `null` for a recipe at the root of its repository), `published` (whether the index has this entry already, the same: nothing to add) | Each recipe, once it is fetched back from its remote and checked, before anything is built. |
| `limit` | `key` (of `index.toml`, `index.md` 2; counted as 9.2 says), `limit`, `recent` (the publisher's entries published in the last 24 hours), `pending` (its entries in other open pull requests), `pending-requests` (their numbers), `adding`, `exceeded`, `passes-at` (when the limit lets these entries in by waiting, a time; `null` when it does now or waiting does not help) | Before anything is built, when the index counts the entries of this publisher: a pull request with entries to add, by one who cannot push to the index. When it is exceeded, an error follows (with `--dry-run` a warning). |
| `build` | `build` (its number, from 1), `state`: `"queued"`, `"building"`, `"built"` or `"failed"`; `recipe`, `library`, `variant`, `version`, `revision`, `target`, `platforms` (those it builds), `environment` (`"linux/gcc"`), `options` (the option set of a `[[publish.check]]`, an object of option values; `null` for the defaults), `seconds` (how long it took, for `"built"` and `"failed"`; else `null`) | Each build of a recipe: one per environment of the recipe this machine has and per option set (`DESIGN` 7.4). All are `"queued"` first, then they build at once (one after another with `-vv`). |
| `slice`, `skipped`, `linkage-fallback`, `standard`, `diagnostic` | as in 9, with `build`, the number of the build they belong to | What the install of a build reports (its other events are of the temporary project it builds in and are left out). A failed slice is a `slice` event with `state` `"failed"`, its log and hint, then the build's error `diagnostic`. |
| `publication` | `state` (below), `entries` (the entries it adds, or would add with `--dry-run`), `published` (the entries the index had already), `pull-request` (`number`, `url`, `branch`, `reused`: whether an open pull request of the branch was updated, `last-check`: what index CI said of it before this update, `{ verdict, reasons }` as `index-check` gives them, else `null`; `null` but for `"pull-request"`), `registered` (the publisher this pull request registers, `{ publisher, account }` with the GitHub account; else `null`) | At the end, when nothing failed; for a check, also when recipes were refused. |
| `diagnostic` | as 2.1 | An error or a warning of the publication itself; without `build`. A recipe that cannot be published is one with its `recipe.toml` as `file`; a failed build ends the publication with one. |
| `text` | `text` | As in 9. |
| `end` | `exit` | Last: the exit code. |

`state` of `publication`:

- `"checked"`: checked, nothing built or published (`--dry-run --no-build`);
- `"dry-run"`: built, nothing published (`--dry-run`);
- `"published-already"`: every recipe's entry is in the index already;
- `"pull-request"`: the entries are proposed in a pull request on GitHub, which index CI checks and merges;
- `"committed"`: committed to a checkout of the index, not pushed (`--no-push`);
- `"pushed"`: committed to a checkout of the index and pushed.

A script follows a publication by its `pull-request`: index CI writes its verdict into the pull request (`index.md` 9).
