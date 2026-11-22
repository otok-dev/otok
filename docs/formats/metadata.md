# `otok metadata` — the project as JSON, format 1

Status: draft. Common rules are in [README.md](README.md).

`otok metadata` prints what otok knows about a project without building anything: the resolved graph,
the targets as the selected profile has them, and where each library lands in the output. It is for
IDEs and scripts, as `cargo metadata` is.

## 1. The command

```text
otok metadata [--profile NAME] [--offline]
```

- It resolves as `otok tree` does: from `otok.toml` and, when it exists, `otok.lock`, in memory. It
  never writes `otok.lock`, fetches no sources and builds nothing; recipes that are neither local nor
  in the cache are fetched unless `--offline` is given.
- A missing or out-of-date lock is not an error: `project.lock.state` says so, and the graph is the one
  `otok install` would lock.
- The document goes to standard output, UTF-8, followed by a line end; warnings and errors go to
  standard error. The exit code is 0 when the document was printed.

## 2. Example

The project of `tests/data/projects/tiny` with an Apple target and a profile, before its first
`otok install` (64-digit hashes shortened to `…`):

```json
{
  "format": 1,
  "otok": "…",
  "project": {
    "dir": "/work/app",
    "manifest": "/work/app/otok.toml",
    "lock": { "path": "/work/app/otok.lock", "state": "missing" },
    "profile": null,
    "profiles": ["simulator"]
  },
  "targets": [
    {
      "name": "apple",
      "output": "/work/app/Otok",
      "platforms": ["ios", "iossim"],
      "architectures": { "ios": ["arm64"], "iossim": ["arm64", "x86_64"] },
      "deployment": { "ios": "16.0" },
      "configurations": ["release"],
      "linkage": "static",
      "build-on": ["macos/xcode"],
      "toolchain": null
    }
  ],
  "packages": [
    {
      "library": "tiny-a",
      "tool": false,
      "target": "apple",
      "environment": "macos/xcode",
      "variant": null,
      "index": null,
      "version": "1.0",
      "revision": 1,
      "recipe": "path",
      "recipe-source": { "path": "recipes/tiny-a" },
      "recipe-sha256": "…",
      "source": { "path": "vendor/tiny-a", "sha256": "…" },
      "resources": [],
      "license": "MIT",
      "description": "A tiny C library for otok's tests.",
      "homepage": null,
      "dependencies": [],
      "tool-dependencies": [],
      "required-by": [
        { "requester": "otok.toml", "constraint": null },
        { "requester": "tiny-b 1.0", "constraint": ">= 1.0" }
      ],
      "platforms": ["ios", "iossim"],
      "options": { "fast": false, "value": 7 },
      "outputs": [
        {
          "target": "apple",
          "dir": "/work/app/Otok/Packages/tiny-a",
          "linkage": "static",
          "linkage-fallback": null,
          "hidden": false,
          "package-key": null,
          "products": [{ "name": "TinyA", "libraries": ["tiny_a"], "depends": [], "link": ["z"], "runtime": "c++" }]
        }
      ]
    },
    {
      "library": "tiny-b",
      "tool": false,
      "target": "apple",
      "environment": "macos/xcode",
      "variant": null,
      "index": null,
      "version": "1.0",
      "revision": 1,
      "recipe": "path",
      "recipe-source": { "path": "recipes/tiny-b" },
      "recipe-sha256": "…",
      "source": { "path": "vendor/tiny-b", "sha256": "…" },
      "resources": [],
      "license": "MIT",
      "description": "A tiny C library for otok's tests, depending on tiny-a.",
      "homepage": null,
      "dependencies": ["tiny-a"],
      "tool-dependencies": [],
      "required-by": [{ "requester": "otok.toml", "constraint": null }],
      "platforms": ["ios", "iossim"],
      "options": {},
      "outputs": [
        {
          "target": "apple",
          "dir": "/work/app/Otok/Packages/tiny-b",
          "linkage": "static",
          "linkage-fallback": null,
          "hidden": false,
          "package-key": null,
          "products": [{ "name": "TinyB", "libraries": ["tiny_b"], "depends": [], "link": [], "runtime": null }]
        }
      ]
    }
  ]
}
```

## 3. Structure

Paths are absolute, with `/` as the separator on every host. Every key listed here is always present;
`null` stands for "none". Object keys are written in sorted order (the example is ordered for reading),
with two spaces of indentation, so the same project always gives the same bytes.

| Key | Type | Meaning |
|---|---|---|
| `format` | integer | The format of this document; `1`. A change that removes or changes a key raises it; new keys may be added without. |
| `otok` | string | The version of otok that wrote it. |
| `project` | object | 3.1 |
| `targets` | array | 3.2: the target tables of `otok.toml` that this otok knows, in file order. |
| `packages` | array | 3.3: every package of the graphs this machine builds (`otok-lock.md` 2): of each target table the graph of its environment, chosen as `otok install` chooses it (`otok-toml.md` 4.9), or its first when the machine has none; the target packages (`otok-lock.md` `package`), sorted by name and then target, and then the tool packages (`tool-package`) of the tool graph of the machine's system, sorted by name. |

### 3.1 `project`

| Key | Type | Meaning |
|---|---|---|
| `dir`, `manifest` | path | The project directory and its `otok.toml`. |
| `lock` | object | `path` of `otok.lock` and its `state`: `"missing"`, `"current"` (it holds exactly what this resolution locks), or `"stale"` (`otok install` would change it). |
| `profile` | string or null | The profile selected with `--profile`. |
| `profiles` | array of strings | Every profile `otok.toml` defines, in file order. |

### 3.2 `targets`

The values in effect for the selected profile (`otok-toml.md` 5, 8):

| Key | Type | Meaning |
|---|---|---|
| `name` | string | `"apple"`, `"android"`, `"windows"`, `"linux"`. |
| `output` | path | The output directory (`otok-toml.md` 4.2). Its consumer files are there under fixed names: `otok-config.cmake`, `otok-provider.cmake` and `cmake.d/` on every target, `Package.swift` on Apple, `otok.props` and `otok.targets` on Windows (`otok-toml.md` 4, 4.11, 4.12); `otok metadata` does not list them. |
| `platforms` | array of strings | The platforms built. |
| `architectures` | object: platform → array of strings | The architectures of each platform built. |
| `deployment` | object: key → version | The deployment versions the project sets. |
| `configurations` | array of strings | `"release"`, `"debug"`. |
| `linkage` | string | The target's linkage, before `[deps.*] linkage`. |
| `build-on` | array of strings | The environments of the target table (`otok-toml.md` 4.9), as written. |
| `toolchain` | object or null | What this machine builds the target with, for scripts that build the consumer with the same: on Android `ndk`, the path of the NDK otok uses (`config.md` `android-ndk`, `ANDROID_NDK_HOME`, …; `otok-toml.md` 4.7), and `revision`, its `Pkg.Revision` (`"30.0.16248370"`). `null` on the other targets, and when the NDK is not found (which is no error here). |

### 3.3 `packages`

What `otok.lock` records for the package (`otok-lock.md` 2), as the current resolution would lock it,
and what the resolution adds:

| Key | Type | Meaning |
|---|---|---|
| `library` | string | The library name. |
| `tool` | boolean | A tool package: built for the tool slice (`otok-toml.md` 7); it has no outputs (a tool the project delivers is in `[otok] tools-output`, not an output of a target, `otok-toml.md` 7.1). |
| `target` | string | The target of its graph; for a tool package the target that builds the tools of the machine (`otok-toml.md` 7). |
| `environment` | string | The environment of its graph, as the project writes it (`*/ndk`), or of `[otok] tool-build-on` for a tool package. |
| `variant` | string or null | The variant of its recipe (`recipe-toml.md` 3); `null` for a git or path recipe. |
| `index` | string or null | The index of the project its recipe was taken from (`otok-toml.md` 6.4): `"default"` or a name of `[indexes]`; `null` for a git or path recipe. |
| `version`, `revision` | string, integer | |
| `recipe`, `recipe-source`, `recipe-sha256`, `source`, `resources`, `license`, `dependencies`, `tool-dependencies` | | As in `otok-lock.md` 2: inline tables become objects, `"local"` stays a string, `recipe-sha256` of a local recipe is `null`, and the arrays are always present. A git `recipe-source` has its `path`. |
| `description`, `homepage` | string or null | From the recipe. |
| `required-by` | array of `{ requester, constraint }` | Who asked for the library and with which constraint (`null`: any version), as `otok why` shows them. |
| `platforms` | array of strings | The platforms it is built for in the selected configuration; empty when not built. |
| `options` | object | The merged options (`recipe-toml.md` 8): booleans, integers, strings and arrays of strings as JSON values. |
| `outputs` | array | Where it lands in the output of its target (3.3.1); empty for a tool package or one not built. |

#### 3.3.1 `outputs`

| Key | Type | Meaning |
|---|---|---|
| `target` | string | The target. |
| `dir` | path | Where the target puts the library in its output: `<output>/<library>` for `android`, `windows` and `linux`, `<output>/Packages/<library>` for Apple (`otok-toml.md` 4, 4.1). |
| `linkage` | string | How the library is built there (`otok-toml.md` 5). |
| `linkage-fallback` | string or null | Why `linkage` is not the one the target table prefers (`otok-toml.md` 5), as `otok install` notes it (`"its recipe builds no dynamic library for 'apple'"`); `null` when it is that one or chosen for the library. |
| `hidden` | boolean | Whether the library exists for the consumer only inside dynamic libraries (`otok-toml.md` 5): static, not in `[deps]`, its code held by no product of a library existing for the consumer. Its headers and licenses are in the output, nothing else of it: no libraries, products or CMake package. Decided from the resolution, as `otok install` will write the output. |
| `package-key` | string or null | The package key the output's state file records for it (`otok-toml.md` 4.2, `cache-key.md` 4): what `otok install` put there last; `null` when it is not there. Whether it is up to date needs the toolchain, so `otok metadata` does not say. |
| `products` | array of `{ name, libraries, depends, link, runtime }` | The recipe's products whose `when` holds on the target (`recipe-toml.md` 11): their library output names, the products they depend on, the system libraries of their `link` whose `when` holds there, and their `link.runtime` (`"c++"`: the C++ standard library, which the consumer's toolchain names) or null. |
