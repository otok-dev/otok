# `otok.toml` — project manifest, format 1

Status: draft. Common rules (strictness, names, versions, platforms, paths) are in
[README.md](README.md); this document refers to its sections as "common 1.2" and so on.

`otok.toml` lies in the project root. It is data: otok never executes anything from it
(common 1.6). Everything that changes what is built for the project is in this file or in the files it
includes; machine settings (`config.md`) never change the output.

## 1. Example

```toml
[otok]
format = 1
include = ["otok.generated.toml"]

[apple]
platforms = ["ios", "iossim", "macosx", "catalyst"]
architectures = { iossim = ["arm64"] }
deployment = { ios = "15.0", macos = "12.0" }

[deps.openssl]
recipe = "apotocki/openssl"
version = "~> 3.5"
options = { no-deprecated = true }

[deps.boost]
recipe = "apotocki/boost"
version = "~> 1.92"
options = { libraries = ["json", "program_options"] }
platforms = ["ios", "iossim"]

[deps.libssh2]
recipe = { path = "../otok-recipes/libssh2" }

[tool-deps.protobuf]
recipe = "apotocki/protobuf"
version = "~> 31.0"

[profile.ci]
apple = { platforms = ["iossim"] }

[profile.debug]
apple = { configurations = ["debug"] }

[policy]
deny-licenses = ["GPL-3.0-only", "GPL-3.0-or-later", "AGPL-3.0-only", "AGPL-3.0-or-later"]
```

## 2. Top-level tables

| Table | Required | Section |
|---|---|---|
| `[otok]` | yes | 3 |
| `[apple]` | at least one target table | 4 |
| `[android]` | at least one target table | 4.7 |
| `[windows]` | at least one target table | 4.8 |
| `[linux]` | at least one target table | 4.1 |
| `[deps.<library>]` | no | 6 |
| `[tool-deps.<library>]` | no | 7 |
| `[profile.<name>]` | no | 8 |
| `[policy]` | no | 9 |
| `[fetch]` | no | 10 |
| `[indexes.<name>]` | no | 6.4 |
| `[index-of]` | no | 6.4 |
| `[metadata]` | no | common 1.2; never read |

Any other top-level key is an error.

## 3. `[otok]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `format` | integer | — (required) | Manifest format version; `1` for this document (common 1.4). |
| `include` | array of paths | `[]` | Fragments with more dependencies (section 10). |
| `tool-build-on` | array of environments (common 4.2), at most one per host system | `["macos/xcode", "linux/gcc", "windows/msvc"]` | The environment tool slices are built in on each system (7): an environment of the target of that system's own platform (`macos/xcode` of `apple`, `linux/gcc` or `linux/clang`, `windows/msvc` or `windows/clang-cl`). The systems it leaves out take the default. |
| `tools-output` | path | `"Otok/tools"` | Where the tools with `deliver` go (7.1), relative to the manifest; must stay inside the project. It may be inside the `output` of a target, never that directory or one around it. |

## 4. `[apple]`

The Apple target settings. A project builds for the targets whose tables its manifest has (common 4): `[apple]`,
`[android]` (4.7), `[windows]` (4.8) and `[linux]` (4.1). Every target
table takes the keys below, with the target's platforms, architectures, deployment keys and environments. There
is no shorthand (`[otok] platforms`, a top-level `[deployment]`): there is one way to write it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `platforms` | array of platform names | — (required, non-empty); the platform of a target that has one (`android`, `windows`, `linux`) | Platforms every dependency is built for, unless a dependency narrows them (6). |
| `build-on` | array of environments (common 4.2) | the target's only environment (`["macos/xcode"]`), `["*/ndk"]` for `[android]`; required for `[windows]` and `[linux]` | The environments the project is built in (4.9): a machine builds the target only in one of them. |
| `architectures` | table: platform → array of architectures | the full list of common 4 | Restricts the architectures of a platform, e.g. `{ iossim = ["arm64"] }` to skip Intel simulators. Each array must be a non-empty subset of the platform's architectures. |
| `deployment` | table (below) | — | Minimum OS versions. |
| `output` | path | `"Otok"` | Directory of the generated packages, relative to the manifest; must stay inside the project. The outputs of two targets must not overlap (the same directory, or one inside the other, compared ignoring case). |
| `cflags`, `cxxflags`, `ldflags` | array of strings | `[]` | Compiler and linker flags for every library built for this target (4.3). |
| `configurations` | array of `"release"`, `"debug"` | `["release"]` | The configurations every library is built in (5). `[apple]` takes one. |
| `linkage` | `"static"` or `"dynamic"` | the target's: `"static"` | How libraries are built unless a dependency says otherwise (5). `[apple]` makes dynamic libraries frameworks (4.10). |
| `c-standard` | integer | — (the lowest the recipes allow) | The C standard of every library of the target (5.1). |
| `cxx-standard` | integer | — (the lowest the recipes allow) | The C++ standard of every library of the target (5.1). |

`[apple]` also takes `bundle-id-prefix`, a string, default `"org.otok"`: the start of the bundle identifiers of the
frameworks of dynamic libraries (4.10), letters, digits, `-` and `.`, neither starting nor ending with `.` and without
`..`. It changes the packages, not the builds (`cache-key.md` 4).

`deployment` keys and the platforms they apply to:

| Key | Platforms | Note |
|---|---|---|
| `ios` | `ios`, `iossim`, `catalyst` | |
| `catalyst` | `catalyst` | Optional; overrides `ios` for Mac Catalyst. Uses iOS version numbers, as the `-macabi` target triple does. |
| `macos` | `macosx` | |
| `tvos` | `tvos`, `tvossim` | |
| `watchos` | `watchos`, `watchossim` | |
| `visionos` | `xros`, `xrossim` | |

Values are version strings of digits and dots (`"15.0"`, `"17.4"`), never numbers. A deployment
version is required for every platform in the effective platform set (12) — there is no implicit
default, because Xcode's default changes with every Xcode release and would silently change the
output. Keys for platforms that are not used are allowed (a profile may add the platform).

The output is one SwiftPM package: `<output>/Package.swift` and, per library,
`<output>/Packages/<library>/` with one XCFramework per library output name:

- An XCFramework, its binary target and the archive inside are named `<library>_<output name>`
  (`openssl_ssl.xcframework` holding `libopenssl_ssl.a`): consumers name products, not binary targets,
  and Xcode copies the archives of all static XCFrameworks into one build directory, so names chosen by
  the authors of different recipes must not meet there. Two libraries whose names would still coincide
  (`a_b` + `c` and `a` + `b_c`) are an error.
- The slices of one platform are combined with `lipo`. Headers must be the same in all slices of a
  platform, except for differences between architectures: a header that differs is kept per
  architecture in `Headers/otok-arch/<arch>/<path>`, and `<path>` becomes a generated header that
  includes the copy of the architecture being compiled (`#error` for another one). A header that only
  some slices have fails packaging, which names it.
- The XCFramework of the library a product names in `headers` (`recipe-toml.md` 11) carries the headers
  and a module map in `Headers/<module>/module.modulemap`, in a directory of its own because Xcode merges
  the headers of all XCFrameworks into one directory. The module map is the one `build` returned, or a
  generated `module <module> { header "../…" … export * }` listing every header (the per-architecture copies as
  `textual header`); `<module>` is the product's `apple.module`, or its name. One header path in the
  XCFrameworks of two libraries is an error, since Xcode merges them.
- A product's `defines` whose `when` holds are written into `Headers/<module>/otok_defines.h`, which the
  generated module map lists first: they take effect in the module's headers and are exported to its
  importers (Swift, and Objective-C and C with modules). SwiftPM has no settings a binary target passes
  to its consumers, so C++ without `-fcxx-modules` and a non-modular `#include` do not see them. A
  module map returned by `build` must list `otok_defines.h` itself when the product has defines;
  otherwise packaging fails.
- A product with `link.libraries` whose `when` holds, `link.runtime`, `apple.frameworks` or `apple.privacy-manifest` gets
  a C source target `<product>_otok` (`Packages/<library>/shims/<product>/`): its `linkerSettings`
  (`.linkedLibrary`, `.linkedFramework`; `.linkedLibrary("c++")` for `runtime = "c++"`) reach the link of the app, and the privacy manifest, copied as
  `PrivacyInfo.xcprivacy`, is its resource. A product holds the shims of every product whose libraries it
  holds (its own, those of the products it depends on and of its dependencies), so their link settings
  and manifests come along.
- A product of headers only (`recipe-toml.md` 11.1) always gets its shim target, whose `include/` holds
  the headers, once for all platforms (Boost's 180 MB would be copied into every platform of an
  XCFramework), and a module map `module <product>_otok { header "<product>_otok.h" export * }` that
  lists only the shim's own header: the headers are reached through the header search path SwiftPM
  gives the targets depending on it (`#include <boost/…>` from C, C++ and Objective-C), never built into
  a Clang module, which C++ headers of that size could not be. Swift does not see them. A product holds
  the shims of the products of headers only it depends on, and of those of its dependencies.
- Each wrapper of a product (`apple.wrappers`, `recipe-toml.md` 11) is copied to
  `Packages/<library>/wrappers/<name>/` and declared as a `target` (SwiftPM picks the language from the
  files; `Resources/` is processed as resources) that depends on every target of the products listing
  it, with the recipe's option values as `swiftSettings` and `cSettings` defines (`recipe-toml.md` 8.2).
  The products hold it.
- `Package.swift` declares a `binaryTarget` per library output name and a `library` product per `[[product]]` whose `when` holds. A product's targets are its
  libraries, those of the products it `depends` on, and every library of the recipe's dependencies
  (transitively), with the shims above: a binary target cannot declare dependencies, and SwiftPM links a target once however
  many products hold it. Product names, module names and the names of all targets (binary targets,
  shims, wrappers) must be unique in the package; a clash names both recipes.
- The files `build` returned as `data` (`build-lua-api.md` 8) are in `Packages/<library>/data/`, as they are,
  outside the XCFrameworks and not declared in `Package.swift`: the consumer decides how they reach the
  product, as a resource of the app or packed with its other resources. With `bundle-data` (6) `data/` is
  also the C target `<library>_otok_data` (the library name with every character other than a letter or a digit
  replaced by `_`), held by every product holding the library's libraries: its resources are the data files,
  copied into its bundle in the app, and its header `otok_<library>_data.h` declares
  `char const* otok_<library>_data_directory(void)`, the directory of the bundle's resources holding them
  (`u_setDataDirectory(otok_icu4c_data_directory())` for ICU; in Swift `import icu4c_otok_data`).
- `platforms` of `Package.swift` come from `deployment`; `swift-tools-version` is the highest of 5.9
  and the recipes' `[apple] swift-tools-version`.
- `Acknowledgements.plist` lists the same as `Acknowledgements.md` (4.2) as `PreferenceSpecifiers` of a
  Settings.bundle page (a `PSGroupSpecifier` per library with `Title`, `License` and `FooterText`).

Beside `Package.swift` the output has `<output>/otok-config.cmake`, the same products for builds with CMake 3.28
or later, which links XCFrameworks and takes the headers of the slice it links:

```cmake
find_package(Otok CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/Otok" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
target_link_libraries(app PRIVATE otok::OpenSSL)
```

- `NO_CMAKE_FIND_ROOT_PATH` is needed: when building for iOS and the other devices CMake looks for packages only
  in the SDK (`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE`). `Otok_DIR` naming the directory works too, as does
  `include(Otok/otok-config.cmake)`. Paths are relative to the file, so the output may be moved.
- Each XCFramework is an imported static library `otok::<library>_<output name>` whose `IMPORTED_LOCATION` is the
  XCFramework: CMake picks the slice of `CMAKE_SYSTEM_NAME` and `CMAKE_OSX_SYSROOT` (a simulator SDK) and adds
  its `Headers/` to the include path.
- A dynamic library (4.10) is an imported shared library `otok::<library>_<output name>` on its XCFramework of
  frameworks, with the static `otok::<library>_<output name>_headers` on the XCFramework of its headers. The file
  defines `otok_embed_frameworks(<target>)`: with the Xcode generator it embeds the frameworks in the target's bundle
  (`XCODE_EMBED_FRAMEWORKS`, signed on copy) and adds the run paths of 4.10; with the others it adds the directories
  of the frameworks for macOS to the target's `BUILD_RPATH`, so a program runs from the build tree. An app bundle
  made by the other generators gets the frameworks by the project's means.
- Each product whose `when` holds is an interface target `otok::<product>` holding what its SwiftPM product
  holds: the targets of its libraries; the link settings of the products whose libraries it holds and of the
  products of headers only it holds (`-l<name>` for `link.libraries`, `-lc++` for `link.runtime`, `-framework <name>` for `apple.frameworks`);
  the `include/` of those products of headers only; and, as `INTERFACE_COMPILE_DEFINITIONS`, the `defines` of
  itself, of the products whose modules its libraries carry and of the products of headers only it holds: what
  `otok_defines.h` gives SwiftPM's consumers, and a plain `#include` would not see. A product named as a
  library's target is an error.
- The file refuses CMake older than 3.28, a system other than Apple's, and a platform that the XCFrameworks have
  no slice for, naming it as `platforms` does (`iossim` for the iOS simulator SDK). CMake has no Mac Catalyst: a
  build for `Darwin` takes the macOS slice.
- With the Xcode generator it sets `CMAKE_XCODE_LINK_BUILD_PHASE_MODE` to `KNOWN_LOCATION` unless the project
  has set it, for the targets created after it: the XCFrameworks are then in the link phase, and Xcode picks the
  slice of the SDK it builds with, so a project configured for a device builds for its simulator too. Otherwise
  CMake fixes the slice when it configures.
- `Otok_PRIVACY_MANIFESTS` lists the privacy manifests of the products of static libraries
  (`shims/<product>/PrivacyInfo.xcprivacy`): an app built with CMake puts them into its bundle itself. Those of dynamic
  libraries are in their frameworks.
- As for the other targets (4.11), `Otok_ACKNOWLEDGEMENTS` names `Acknowledgements.md` and `Otok_<library>_DATA_DIR` the
  `data/` of a library with data files, and `otok_copy_runtime_dlls(<target>)` is defined, doing nothing here, so that a
  project of several targets calls it alike.
- What only SwiftPM builds is missing, and the file warns of each when CMake reads it: wrappers, the target of
  `bundle-data` with its `otok_<library>_data_directory()` (the data files stay in `data/`), and the packages
  of recipes delivering a SwiftPM package (4.6).

### 4.1 `[linux]`

The Linux target (it replaces the development target `native` of earlier versions, which built for the build
machine): libraries for programs of the build machine's Linux, with its glibc and GNU C++ runtime.

```toml
[linux]
build-on = ["linux/gcc"]
configurations = ["debug", "release"]
```

It has one platform, `linux`, whose architecture is the build machine's (a sysroot for other architectures is not
supported yet), and no deployment versions. It takes `build-on` (`"linux/gcc"` or `"linux/clang"`: the compiler
is the project's choice), `output`, `cflags`, `cxxflags`, `ldflags`, `configurations` (any of the two) and `linkage`
(default `"static"`) as `[apple]` does. Its builds start from nothing of the user's environment (`build-lua-api.md` 5).
A manifest needs at least one target table.

Its output is one directory per library, `<output>/<library>/`, with:

- `include/`: the headers `build` returned; per-library header directories are merged, and one path
  with different contents in two of them, or in two configurations, is an error;
- `lib/`: the files linked against (static archives, shared objects with their symlinks), each under the file name
  the build gave it, which may be part of its contract (a shared object's name is fixed at link time). Two libraries
  of one recipe with one file name are an error, one file name in two recipes a warning, as it matters only to a
  consumer that links through search paths;
- `bin/`: programs a library ships, when it returns any;
- `debug/lib/` and `debug/bin/`: the same for the `debug` configuration; `lib/` and `bin/` hold
  `release`;
- `data/`: the files `build` returned as `data` (`build-lua-api.md` 8), as they are;
- `licenses/` (4.2).

Shared objects on Linux carry `$ORIGIN` in their `RUNPATH` (`build-lua-api.md` 6.2), so the libraries of
one recipe find each other in its `lib/` wherever it is copied. Libraries of different recipes find each
other the same way once the consumer puts their shared objects into one directory, as an application
does when it ships them; a consumer that runs them from the output directories sets `LD_LIBRARY_PATH`.

Tool dependencies have no output, but for the tools the project delivers (7.1). `otok-config.cmake` gives the libraries and products to a consumer's CMake (4.11).

### 4.2 The output directory

otok owns the `output` directory of a target: `install` and `update` replace what is in it and remove
what the resolution no longer has. To avoid deleting files it did not create, otok writes into the
directory only when it does not exist, is empty, or contains `.otok-output.toml`, the state file otok
writes there; otherwise it is an error and nothing is changed. The state file records the package key
(`cache-key.md` 4) of each library in the directory:

```toml
# Generated by otok; do not edit.
format = 1

[packages]
zlib = "3f5c…"   # 64 hex digits
```

A library whose package key has not changed is not copied again, so the consumer's build sees the same
files with the same times. The files otok writes beside the libraries on every run (`Acknowledgements.md`,
`Package.swift`, `otok-config.cmake`, `otok.props`, …) are written only when their contents change, for the same
reason.

Every library's directory has `licenses/` with the recipe's `license-files` (`recipe-toml.md` 3) at
their paths in the source or the recipe directory. otok writes `<output>/Acknowledgements.md` on every run, for an about screen
or a notice file: a section per library of the output in name order (`## zlib 1.3.2`, the SPDX
expression, the texts of its license files). Tool dependencies are not listed: they do not reach the
product.

### 4.3 Compiler flags

`cflags`, `cxxflags` and `ldflags` pass flags to the builds of dependencies without the recipe
declaring them as options: macros such as `-D_CRT_SECURE_NO_WARNINGS`, a library's own configuration
macros, `-fno-omit-frame-pointer`, sanitizers in a profile. They are
arrays of strings, one argument each:

```toml
[apple]
cflags = ["-fno-omit-frame-pointer"]

[deps.openssl]
cflags = ["-DOPENSSL_SMALL_FOOTPRINT"]

[profile.asan.apple]
cflags = ["-fsanitize=address"]
```

| Where | Applies to |
|---|---|
| a target table (`[apple]`, `[android]`, `[windows]`, `[linux]`) | every library built for that target; not tool dependencies, which run on the build machine |
| `[deps.<library>]` | that library, on every target |
| `[tool-deps.<library>]` | that tool dependency |
| the same tables in a profile | appended to the base flags (8) |

A recipe's `build_tools` entry (`build-lua-api.md` 2) gets no project flags: it builds tools for the
build machine, like a tool dependency without an entry of its own.

A build sees otok's flags first, then the project's, in this order: the slice's (target, SDK,
deployment), the configuration's (`-O2 -DNDEBUG` / `-O0 -g`, `build-lua-api.md` 3.1), the target
table's, the profile's target table's, the library's, the profile's library's. For flags where the last
one wins (`-O`, `-D` of the same macro) the project's therefore win; others add up. Flags that a
project's own build files add to a target come after all of these and cannot be overridden.

Flags that choose the slice are otok's and are errors here: `-target`, `--target=…`, `-arch`,
`-isysroot`, `--sysroot`, `-m<os>-version-min=…`, `-mtargetos=…`, `-ffile-prefix-map=…`,
`-fdebug-prefix-map=…`, `-fmacro-prefix-map=…`. A library built for another platform than its slice
would land in the cache under the slice's key.

The flags are part of the slice key (`cache-key.md` 3), not of `otok.lock`: they do not take part in
resolution. They reach only the builds of dependencies; a `-D` that changes a library's public headers
or ABI is the project's responsibility, since what consumers compile with comes from the recipe's
product `defines` (`recipe-toml.md` 11).

### 4.4 Required-reason APIs

The Apple target lists the undefined symbols of every library it packages (as `nm -u` does, over all
slices of all platforms) into the package entry, `Packages/<library>/undefined-symbols/<output name>.txt`, one
Mach-O symbol per line, sorted. Every install matches them against the required-reason API list
(`index.md` 10.1) and the privacy manifests of the products, so a newer list needs no rebuild:

- A library uses a category when one of its undefined symbols is listed under it: `_stat` and
  `_stat$INODE64` are `stat`, `_OBJC_CLASS_$_NSUserDefaults` is the class `NSUserDefaults`.
- A use is declared when a product listing the library in `libraries` (and whose `when` holds) has an
  `apple.privacy-manifest` whose `NSPrivacyAccessedAPITypes` names one of the symbol's categories; the manifest
  of that product reaches the app with the library (4).
- An undeclared use is a warning of `otok install` naming the library, the categories and the symbols; it
  fails `otok publish`. Reason codes are not checked: no symbol tells the reason.
- Uses through a function pointer, a selector or a dynamic lookup are not seen: the check catches the
  common case, not every case.

Privacy manifests are XML property lists; a binary one fails packaging.

### 4.5 Defined symbols

The Apple target also lists the external symbols every library it packages defines (as `nm -g -U` does,
over all slices of all platforms), in `Packages/<library>/defined-symbols/<output name>.txt`, one Mach-O
symbol per line, sorted, and those of them that may be defined more than once (weak definitions, such as
C++ inline functions and template instances, and common symbols) in `<output name>.weak.txt` beside it.
Private external symbols (hidden visibility) are listed too: an archive's objects are linked into the
app, where they meet.

Every install matches the symbols against the exports of the private system libraries (`index.md` 10.2),
read from the text stubs of the SDK of each platform the project builds for:

- A library collides when it defines a symbol that a listed library exports, by its Mach-O name (`_ubrk_next`).
- A collision is a warning of `otok install` naming the library, the system library and up to ten of the
  symbols with their count; it fails `otok publish`. The cure is in the recipe (renaming, as ICU's default
  build does); the check only says that App Store review would reject the app (ITMS-90338).

The libraries' symbols are also matched against each other: Xcode links the archives of all products the app uses into one image, where a symbol
two archives define either fails the link or, when the linker pulls only one of the archive members holding
it, silently gives every caller that one (a library that carries a copy of another, as its own build of zlib):

- Two static libraries of the output overlap when both define a symbol that neither lists as weak, two output names
  of one recipe too, unless every two products of the recipe holding them exclude each other
  (`recipe-toml.md` 11.2): Boost.Stacktrace's backends define the same functions, and a consumer links one.
- An overlap fails `otok install` (and so `otok publish`), naming both libraries and up to ten of the symbols
  with their count. The cure is in the recipe: build against the dependency instead of a copy, or rename.

### 4.6 SwiftPM packages from recipes

A recipe that delivers a SwiftPM package (`recipe-toml.md` 12.1) is a dependency like any other:
`[deps.<library>]` names it, and the lock pins its recipe and source. In the output:

- `Packages/<library>/<library>/` holds the patched package with its own `Package.swift`. The generated
  `Package.swift` depends on it as `.package(path: "Packages/<library>/<library>")`, so the package's
  identity in SwiftPM is the library's name; a consumer that also depends on the same package from its
  repository gets SwiftPM's error about two packages of one identity.
- Each product gets a Swift target `<product>_otok` in `Packages/<library>/shims/<product>/` that depends on
  `.product(name: "<product>", package: "<library>")`, and the generated `Package.swift` declares the
  library product `<product>` holding it. The consumer imports the package's modules as they are
  (`import Updates`): SwiftPM makes the modules of a dependency visible to the targets depending on it.
- Nothing of 4.4 and 4.5 applies: there are no binaries, and SwiftPM builds the package in the
  consumer's build.

### 4.7 `[android]`

The Android target: native libraries for an app's shared objects, built with the
NDK.

```toml
[android]
architectures = { android = ["arm64-v8a", "x86_64"] }
deployment = { android = "24" }
```

It has one platform, `android` (common 4), whose architectures are the ABIs. `deployment` has the key
`android`: the API level (the app's `minSdk`), required, digits only. It takes `build-on` (default `["*/ndk"]`:
any machine with the NDK; `["macos/ndk", "linux/ndk"]` keeps the project to recipes that name those), `output`,
`cflags`, `cxxflags`, `ldflags`, `configurations` (any of the two) and `linkage` (default `"static"`) as `[linux]`
does. Tool dependencies and `build_tools` are built for the build machine (7).

The C++ runtime: static libraries are compiled against the NDK's libc++ headers, which `c++_static` and
`c++_shared` share, and link none: the consumer's shared object chooses it. Dynamic libraries link the
runtime that `stl` names:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `stl` | `"shared"`, `"static"` | `"shared"` | `"shared"`: dynamic libraries link `c++_shared`, as Android requires of an app with several shared objects using C++; the app ships `libc++_shared.so` of the same NDK. `"static"`: each links `c++_static` into itself, for an app whose C++ is one shared object (with the static libraries built into it), which then ships nothing more; two such objects in one app each have a runtime of their own, and exceptions and type information do not cross between them. |

`stl` is the project's for the whole app: a dependency or a profile does not change it. Any other value is
an error, which names the one meant for `"dynamic"` (`"shared"`), as an error of `linkage` names `"dynamic"`
for `"shared"`.

The output is one directory per library, `<output>/<library>/`, with:

- `include/`: the headers `build` returned, which must be the same in every slice;
- `lib/<abi>/`: the static archives or shared objects of the `release` slices, `debug/lib/<abi>/` those
  of `debug`; `<abi>` as Gradle's `jniLibs` names it (`arm64-v8a`);
- `data/` (as in 4.1) and `licenses/` (4.2).

A shared object has no `RUNPATH`: Android's loader finds the libraries of an app by name. Its `SONAME`
must be its file name, unversioned (`libfoo.so`), the only names Gradle packs; packaging checks it. As
in 4.1, one file name in the `lib/<abi>/` of two recipes is a warning. `otok-config.cmake` gives the libraries to the
app's CMake, which Gradle's `externalNativeBuild` runs, and Gradle packs their shared objects (4.11). Prefab is not
made.

### 4.8 `[windows]`

The Windows target: libraries for programs built with MSVC, in one of its
environments (common 4.2):

- `windows/msvc`: `cl.exe` and `link.exe` of Visual Studio (`visual-studio` of `config.md` 2) on Windows;
- `windows/clang-cl`: clang-cl, lld-link and LLVM's tools on Windows, with Visual Studio's CRT and SDK;
- `linux/clang-cl`: the same across from Linux, against the CRT and the Windows SDK that `xwin splat` lays out on the
  build machine (`windows-sdk` of `config.md` 2), its builds in the sandbox.

```toml
[windows]
build-on = ["windows/msvc"]
architectures = { windows = ["x86_64", "arm64"] }
configurations = ["debug", "release"]
```

It has one platform, `windows`, with the architectures `x86_64` (the default) and `arm64`, and no deployment
versions. It takes `build-on` (required: the environments differ in how libraries build, and recipes name theirs),
`output`, `cflags`, `cxxflags`, `ldflags`, `configurations` (any of the two) and `linkage` (default `"static"`) as
`[linux]` does. Tool dependencies and `build_tools` are built for the build machine (7).

The runtime is MSVC's dynamic CRT, `/MD` (`/MDd` in `debug`); debug information is in the objects (`/Z7`). A
consumer links them with an MSVC whose toolset is the CRT's the libraries were built with or newer (`14.44` libraries
link into a `14.51` program, not the other way round); otok shows the CRT's version. Link-time code generation
(`/GL`) is not used: clang-cl's objects are not MSVC's, and an MSVC's are its version's only.

The output is one directory per library, `<output>/<library>/`, with:

- `include/`: the headers `build` returned, which must be the same in every slice;
- `lib/<arch>/` and `bin/<arch>/`: the static or import libraries, and the DLLs with their `.pdb`, of the
  `release` slices; `debug/lib/<arch>/` and `debug/bin/<arch>/` those of `debug`, under the names the build gave
  them;
- `data/` (as in 4.1) and `licenses/` (4.2).

Packaging checks that every object of a library is for the slice's architecture (the COFF machine), and that a static
library calls none of the helpers of compiler-rt for 128-bit integers (`__udivti3`, `__divti3`, …): clang has
`__int128`, MSVC has not, nor do its runtime libraries, so a program linked by MSVC would not link. As
in 4.1, one file name in the `lib/<arch>/` of two recipes is a warning. `otok-config.cmake`, and
`otok.props` with `otok.targets` for projects of Visual Studio, give the libraries to the consumer's build (4.11).

### 4.9 Build environments

A target table's `build-on` names the environments (common 4.2) the project may be built in; the lock,
the systems it is built on.
Recipes of one id may be variants for different environments (`recipe-toml.md` 6.1), so the environment chooses what
is built, not only where:

- Each environment of each target table is a graph of its own: its libraries, versions and recipe variants. For a
  library, the graph takes the variant of its recipe whose `build-on` covers the environment (common 4.2) and whose
  `supported` holds every platform of the table. When none covers it, the variant of the first environment that
  substitutes for it (common 4.2), in the target's order, that one covers: the library is built there, on the same
  machine, and links with the rest, whose ABI it shares. Skia, which builds with clang-cl
  only, is built in `windows/clang-cl` in a project that builds in `windows/msvc`. A machine without
  that environment does not build the target (the last item), and `otok tree` names it beside the library. No variant in the
  environment or its substitutes is a resolution error naming the library, the target and the environment. `*/ndk`
  is one graph, of variants that build on every host (`["*/ndk"]` in the recipe).
- A library has one version in every graph of the project; its variants differ in revisions only. A version some
  graph has no variant for is not chosen; when none is left, resolution fails naming the graph that holds it back.
- The lock holds the graphs of the systems the project is built on, its `systems` (`otok-lock.md` 2), and `*` environments
  always: a machine adds its system the first time it builds the project, so the lock does not depend on the machine
  resolving it, and a system nobody builds on needs nothing, not even its tools.
- Target tables are independent: a library of two tables is two packages, from variants of its recipe for each target,
  with the dependencies of each. What otok prints names them `<library>@<target>` when the project has several target
  tables (`zlib@android`); `otok tree` shows the graph of each target this machine builds, under `[<target>]
  <environment>`.
- A machine builds a target in the first environment of `build-on` it has (common 4.2). `install` and `update` build
  every target table the machine can build and skip the others, each with a note naming
  the target and why: this otok does not build the target on this machine (`[apple]` off a Mac); the machine has none
  of its environments, named with the machine's system (`this project builds 'windows' in windows/msvc; this machine
  is linux-x86_64`); or a library of its graph is built in a substituting environment the machine does not have. The
  output directory of a skipped table is left as it is. A machine that can build none of the project's target tables
  is an error naming each with its reason, after the lock is written and the inputs are fetched. The lock does not
  change with the machine: it holds the graphs of every system of `systems`, and `--locked` checks all of it. Adding
  an environment is a change of the manifest, which `otok update` resolves.

### 4.10 Dynamic libraries on Apple

With `linkage = "dynamic"` (5) a library of the Apple target is a framework, the only form of a dynamic library
App Store takes in an app:

- Its XCFramework `<library>_<output name>.xcframework` holds a framework `<library>_<output name>.framework` per
  platform: the binary of that name (the slices of the platform combined with `lipo`), `Info.plist` and the privacy
  manifest. On macOS and Mac Catalyst the framework is versioned (`Versions/A/` with `Resources/`, and the links
  `Versions/Current`, `<name>` and `Resources`), on the other platforms flat.
- Its install name is `@rpath/<name>.framework/<name>` (`@rpath/<name>.framework/Versions/A/<name>` on macOS and Mac
  Catalyst). The target sets it as soon as the slice is built, before anything links against it, so dependents load
  it under that name; the references between the libraries of the slice change with it. Dynamic slices link with
  `-Wl,-headerpad_max_install_names`, the room `install_name_tool` needs for the longer names. Each changed binary
  is signed ad hoc, and the framework once made; Xcode signs them again when it embeds them.
- `Info.plist`: `CFBundleExecutable` and `CFBundleName` the name; `CFBundleIdentifier`
  `<bundle-id-prefix>.<library>.<output name>` (4), every character other than a letter, a digit, `-` or `.` replaced
  by `-`; `CFBundlePackageType` `FMWK`; `CFBundleShortVersionString` the library's version up to its third numeric
  component (App Store takes at most three integers: `1.92.0`; `0` for a version starting with no number);
  `CFBundleVersion` the recipe's revision; `CFBundleSupportedPlatforms` the SDK's platform (`iPhoneOS`,
  `iPhoneSimulator`, `MacOSX`, …); `MinimumOSVersion` the deployment version, `LSMinimumSystemVersion` on macOS, and
  neither on Mac Catalyst, whose deployment is an iOS version.
- A library carrying headers (`recipe-toml.md` 11) has them in a second XCFramework,
  `<library>_<output name>_headers.xcframework`, laid out as a static library's (4: `Headers/`, the module map,
  `otok-arch/`) around an archive of one object that defines nothing: a framework's headers are reached as
  `<Framework/path>` only, and the `#include <openssl/ssl.h>` of C and C++ code would not find them. Both
  XCFrameworks are binary targets of the products holding the library.
- A product's `apple.privacy-manifest` is `PrivacyInfo.xcprivacy` of the frameworks of its libraries (in `Resources/`
  on macOS), where App Store looks for it, rather than a resource of the product's shim. Two products with different
  manifests holding one dynamic library are an error.
- The checks of 4.4 and of the private system libraries (4.5) apply as to static libraries; 4.4 reads the manifests
  of a dynamic library's products from its frameworks, where they are, not from the shims. The overlap of symbols
  (4.5) does not: every dynamic library is an image of its own. A product of a dynamic library holding a static library
  of another recipe is warned of (5).
- Xcode embeds the frameworks of the products an app links (Embed & Sign). The app's run path holds
  `@executable_path/Frameworks` (`@executable_path/../Frameworks` on macOS): Xcode's templates set it; a project made
  otherwise (xcodegen for several platforms) may need `LD_RUNPATH_SEARCH_PATHS` for macOS. SwiftPM from the command
  line gives `@loader_path` to an executable only through a target of its own depending on the products.

### 4.11 Consumer files of `[windows]`, `[linux]` and `[android]`

The outputs of these targets tell the consumer's build where the libraries are, as
`Package.swift` and `otok-config.cmake` do on Apple (4). The files do not depend on the machine or the environment the
libraries were built in: a Windows output of `windows/msvc`, `windows/clang-cl` or `linux/clang-cl` has the same ones.

Each library's directory has `otok-package.toml`, written while packaging: which of its files is which library output
name in each architecture and configuration (the files keep the names their builds gave them), and what the target
records of them: `crt`, the toolset of MSVC whose CRT the Windows libraries were built with (the newest of the slices);
`stl`, the C++ runtime of Android's dynamic libraries. It is otok's own file; consumers read the files below.

```toml
# Generated by otok; do not edit. Which file is which library output name (otok-toml.md 4.11).
format = 1

[properties]
crt = "14.44"

[[library]]
name = "lz4"
arch = "x86_64"
configuration = "release"
link = "lib/x86_64/lz4.lib"
runtime = "bin/x86_64/lz4.dll"
symbols = "bin/x86_64/lz4.pdb"
shared = true
```

`<output>/otok-config.cmake`, for CMake 3.28 or later, is found as on Apple:

```cmake
find_package(Otok CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/Otok" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
target_link_libraries(app PRIVATE otok::LZ4)
otok_copy_runtime_dlls(app)
```

- Each library output name is an imported library `otok::<library>_<output name>`, the names of the Apple target:
  `STATIC IMPORTED`, or `SHARED IMPORTED` for a dynamic library, with the file of each configuration built
  (`IMPORTED_LOCATION_RELEASE`, `IMPORTED_LOCATION_DEBUG`); a DLL with its import library (`IMPORTED_IMPLIB_*`), so that
  `$<TARGET_RUNTIME_DLLS>` finds it; a shared object with its `SONAME` (`IMPORTED_SONAME_*`); the `include/` of its
  library. A static library links after it the libraries the products holding it list after it (linkers that read an
  archive once need the order) and the system libraries of those products.
- Each product whose `when` holds is an interface target `otok::<product>` holding what it holds on Apple: its libraries,
  those of the products it `depends` on and of the recipe's dependencies; the products of headers only it holds, with
  their `include/` and system libraries; and, as `INTERFACE_COMPILE_DEFINITIONS`, the `defines` of itself, of the products
  whose headers its libraries carry and of the products of headers only it holds. A system library of `link` is
  `<name>.lib` on Windows, `-l<name>` elsewhere. `link.runtime = "c++"` adds `-lstdc++` on Linux to a static library, for
  a consumer linking with the C driver (a shared object names its runtime itself); nothing with MSVC, whose objects name
  their STL; nothing on Android, whose app chooses the runtime (`ANDROID_STL`). A product named as a library's target
  is an error.
- The architecture the consumer builds for: on Windows the generator platform (`CMAKE_VS_PLATFORM_NAME`, `x64` is
  `x86_64` and `ARM64` `arm64`), or the compiler's (`CMAKE_<LANG>_COMPILER_ARCHITECTURE_ID`) with the other generators;
  on Android the ABI (`CMAKE_ANDROID_ARCH_ABI`, as the NDK's toolchain file and Gradle set it); on Linux
  `CMAKE_SYSTEM_PROCESSOR`.
- The configuration: `Debug` links the debug libraries, the other configurations the release ones. One the project did
  not build is served by the other on Linux and Android. On Windows the CRTs of the two do not mix: a build of one
  configuration (Ninja, `CMAKE_BUILD_TYPE`) is refused when CMake configures it, and a build of several (the Visual
  Studio generator) fails when it links that configuration, on a library named
  `otok-has-no-debug-libraries--add-debug-to-configurations-of-windows-in-otok.toml.lib`.
- The file refuses, when CMake configures, with the cure in the message: CMake older than 3.28; another system, and on
  Windows a compiler without MSVC's ABI; an architecture the output has no libraries for, named as `architectures`
  names it; on Windows `cl` older than the toolset of `crt` (`cl` 19.<n> is the toolset 14.<n>, `14.44` libraries link
  into a `14.51` program, 4.8); on Android, dynamic libraries of `stl = "shared"` (4.7) in an app whose `ANDROID_STL` is
  not `c++_shared`, or of `stl = "static"` in one whose is.
- `otok_copy_runtime_dlls(<target>)` copies the DLLs a target loads beside it once it is built
  (`$<TARGET_RUNTIME_DLLS>`). It does nothing on the other targets, so a project of several targets calls it alike.
- `Otok_<library>_DATA_DIR` is the `data/` of a library with data files; `Otok_ACKNOWLEDGEMENTS` is
  `Acknowledgements.md`. Paths are relative to the file, so the output may be moved.

On Windows the output also has `otok.props` and `otok.targets`, for projects of Visual Studio without CMake: a
`.vcxproj` imports `otok.props` before its settings and `otok.targets` after them, or every project of a solution gets
them from `Directory.Build.props` and `Directory.Build.targets`:

```xml
<Import Project="..\Otok\otok.props" />
<!-- … the project's configurations, Microsoft.Cpp.props … -->
<PropertyGroup>
  <OtokProducts>LZ4;Boost.JSON</OtokProducts>
</PropertyGroup>
<!-- … its items, Microsoft.Cpp.targets … -->
<Import Project="..\Otok\otok.targets" />
```

- Every project gets the include directories of all libraries, and the library directories of its platform (`x64`,
  `ARM64`) and configuration: the debug libraries when it uses the debug CRT (`UseDebugLibraries`), the release ones
  otherwise, so that auto-linking (`#pragma comment(lib, …)`) finds them.
- A project links the products it names in `OtokProducts` (`;`-separated, case does not matter): their libraries and
  system libraries in `AdditionalDependencies`, their `defines` in `PreprocessorDefinitions`. Without it a project links
  nothing by itself, since some products exclude each other (`recipe-toml.md` 11.2).
- After `Build` of an `Application`, the target `OtokCopyDlls` copies beside it (`$(OutDir)`) the DLLs of the products it
  names, or every DLL of its platform and configuration when it names none.
- Before a `.vcxproj` builds, `OtokCheck` stops it with an error naming the cure for a platform or a configuration the
  output has no libraries for, a toolset older than `crt` (`VCToolsVersion`), and a product of `OtokProducts` it does not
  have.
- `OtokDir`, `OtokAcknowledgements` and `OtokDataDir_<library>` (every character of the name other than a letter or a
  digit as `_`) name the output, `Acknowledgements.md` and a library's `data/`.

Gradle's `externalNativeBuild { cmake { … } }` runs the app's CMake for each ABI, which finds the output as above. The
Android Gradle Plugin packs into the app the shared objects of the imported libraries its own link, and
`libc++_shared.so` when it uses `c++_shared` (`arguments += "-DANDROID_STL=c++_shared"`, which dynamic libraries of
`stl = "shared"` need): no `jniLibs` (checked with AGP 9.5.1 and NDK r30).

## 5. Configurations and linkage

`configurations` of a target table lists the build configurations of its libraries: each one is a set of slices
of its own, built with the configuration's flags (`build-lua-api.md` 3.1), and a `debug` slice links the `debug`
slices of its dependencies. On Windows the libraries of a consumer must match its CRT (`/MDd` and
`_ITERATOR_DEBUG_LEVEL=2` in debug builds, `/MD` in release), so a project builds both and its build picks `lib/` or `debug/lib/` (4.1). `[apple]` takes one
configuration: a SwiftPM binary target cannot vary by configuration, and apps link release libraries
in their debug builds. Tool dependencies are always built `release`: they run during the build.

`linkage` chooses static or dynamic libraries: the target table gives the default for its libraries
(the target's own when absent), `[deps.<library>] linkage` overrides it for one library on every
target, and profiles may change both (8). Between the two, a library the project chooses no linkage for
takes the one its dependents' recipes ask for (`recipe-toml.md` 9, `[deps.<library>] linkage`); dependents
asking for different ones are an error naming them, which `[deps.<library>] linkage` settles. In order: the
profile's `[deps]`, `[deps]`, the dependents' recipes, the profile's target table, the target table, the target. The recipe must list the linkage in `[recipe] linkage`
(`recipe-toml.md` 3); otherwise resolution fails. `[apple]` packages dynamic libraries as frameworks (4.10). Tool
dependencies are always static.

Static libraries shared by several products are not duplicated in one image: an archive holds only its
own library's objects (a dependency's symbols stay undefined in it), and SwiftPM links each binary
target once however many products hold it (4). They are duplicated when the consumer links them into
several dynamic images of its own (two of its frameworks or dynamic SwiftPM products, or an app and an
extension each embedding one): every image gets its own copy of the code and of its global state, and
Objective-C classes are reported as implemented in both. Xcode warns of it; otok cannot see how the
consumer's images are made, and the remedy is dynamic libraries for `[apple]` (4.10). A dynamic library built against
a static one holds a copy of it, and a product holds the libraries of its dependencies, so an app linking both has
the static library twice: `otok install` warns of every such pair (4.10).

The configuration and the linkage are part of the slice key (`cache-key.md` 3). There is no `[build]`
table.

### 5.1 The language standards

`cxx-standard` (an integer: `11`, `14`, `17`, `20`, `23` or `26`) chooses the C++ standard the libraries are
compiled with, and `c-standard` (`89`, `99`, `11`, `17` or `23`) the C standard, so that they match the
consumer's code: a project compiled with C++23 gets libraries compiled with
C++23. Each is a key of the target table (and of a profile's target table, which wins), one for every
library of the target: the standard changes what headers declare (`__cplusplus`, `<version>`,
`__STDC_VERSION__`), and libraries built with different ones may not link together, or link with
definitions that differ. `[deps.<library>]` has no such keys. `[tool-deps.<library>] cxx-standard` and
`c-standard` apply to that tool only, a program of its own: the target table's do not reach tool
dependencies and `build_tools`, which run on the build machine and do not reach the product. A tool package without
its own takes the standard of the tool packages it is linked with (a tool, what it links, what links it): the highest
one of them has in `[tool-deps]`, else the lowest every recipe of them allows (`protoc` with protobuf and Abseil, both
`">= 17"`: 17), else none; when no standard suits them all, resolution fails and asks for it in `[tool-deps]`.

Standards are named by their two-digit numbers and ordered by year: C89 is older than C99, and C99 than
C11. Without a key, the target still gets a standard of that language: the lowest of the list above that
every recipe of the target declaring `[recipe] cxx-standard` (or `c-standard`) allows (`install` prints it:
`C++ standard of 'android': 17, the lowest every recipe allows`). When no recipe of the target declares one,
there is none, and each recipe builds with the standard it chooses. When the recipes allow no common one,
resolution fails, naming them. With a standard, given or chosen:

- the recipes must allow it: `[recipe] cxx-standard` and `c-standard` (`recipe-toml.md` 3) are constraints
  such as `">= 17"`, and a standard outside one fails resolution, before anything is fetched;
- the compiler's flag for it comes after otok's flags and before the project's, in
  `slice.toolchain.cxxflags` or `cflags` (`build-lua-api.md` 3.1), so every build system gets it: `-std=c++23`,
  `-std=c11`; MSVC `/std:c++14`, `/std:c++17`, `/std:c++20`, `/std:c++latest` for 23 and later, `/std:c11`,
  `/std:c17`, `/std:clatest` for C23, and no flag for C89 and C99, which MSVC's C without one is;
- the helpers pass it as each build system takes it (`CMAKE_CXX_STANDARD`, `CMAKE_C_STANDARD` (`90` for
  C89), meson's `cpp_std` and `c_std`, b2's `cxxstd`; `build-lua-api.md` 6.2), so none adds another after
  it; `build.lua` reads them as `slice.cxx_standard` and `slice.c_standard`;
- it is part of the slice key (`cache-key.md` 3).

`-std=` in `cxxflags` and `cflags` (and MSVC's `/std:`) is an error: the standard is the key.

## 6. `[deps.<library>]`

A direct dependency. The table key is the library name (common 2); the recipe that builds it is
chosen with `recipe`. Transitive dependencies of the same library resolve to this entry.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `recipe` | string or table (6.1) | — (required) | Which recipe builds the library. |
| `version` | constraint (common 3.2) | see 6.1 | Accepted upstream versions. |
| `prerelease` | boolean | `false` | Allow pre-release versions (common 3.2). |
| `options` | table: option → value | `{}` | Values for options declared by the recipe (6.2). |
| `platforms` | array of platform names | `[apple] platforms` | Narrows the platforms for this library and everything only it depends on. Must be a non-empty subset of the platforms of the base `[apple]` table (without profiles). |
| `source` | table `{ path = … }` | — | Local source override (6.3). |
| `cflags`, `cxxflags`, `ldflags` | array of strings | `[]` | Compiler and linker flags for this library on every target (4.3). |
| `linkage` | `"static"` or `"dynamic"` | the target table's | How this library is built on every target (5). |
| `bundle-data` | boolean | `false` | The library's data files (`build-lua-api.md` 8) also as resources of its products, where the target has bundles: on Apple a target of their own (4). Without it the consumer takes them from `data/` of the output. Not in `[tool-deps]`. |
| `deliver` | boolean or array of program names | `false` | Also build the library's recipe as a tool for this machine, at the version the library got, and copy the programs named (`true`: all of them) into the tools output (7.1). |

Example of the sub-table form, equivalent to an inline table:

```toml
[deps.boost.options]
libraries = ["json", "program_options"]
```

### 6.1 `recipe`

| Form | Meaning | `version` |
|---|---|---|
| `"publisher/name"` | A recipe from the index. | Required. |
| `{ git = "<url>", rev = "<commit>", tag = "<tag>", path = "<dir>" }` | A recipe in a git repository, pinned to a full 40-character commit SHA (`rev`). `path` is the recipe directory inside the repository (default: the root). Branches, tags and short SHAs are errors as `rev`: they move or are ambiguous. `tag` is optional and only helps, as in `recipe-toml.md` 4: the tag of that commit, fetched first; one pointing elsewhere is an error. | Optional; if given, the recipe's own version must satisfy it. |
| `{ path = "<dir>" }` | A recipe in a local directory, for developing a recipe or patching a library. May point outside the project (common 6). | Optional, as for `git`. |

A table form accepts no other keys. The library name in the recipe (`[recipe] library`) must equal
the table key.

### 6.2 `options`

Keys are option names declared by the recipe's `[options.*]` (`recipe-toml.md`); values must match
the declared type. Unknown options and type mismatches are errors reported before anything is
fetched or built. Options not given take the recipe's default, unless a dependent
requires a value (`recipe-toml.md` 8.1).

A `file` option (`recipe-toml.md` 8.3) takes a path relative to the manifest that stays inside the
project, also in a profile or a fragment (11): `options = { data-filter = "icu/filter.json" }`. The file must
exist when the project is built; its contents enter the slice key, its path does not. Without a value the
build gets no file. Only the project gives one: a dependent recipe cannot.

### 6.3 `source`

`source = { path = "<dir>" }` builds the library from a local checkout instead of the recipe's
`[source]`. The recipe, its patches and its options still apply. The path may
point outside the project (common 6):

- **inside the project** (vendored sources, committed with it): `otok.lock` records the path and the
  tree hash of the directory (`otok-lock.md` 3, `recipe-hash.md`); such a source is reproducible;
- **outside the project** (a developer's checkout): `otok.lock` records only that the source is
  local, never the path or a hash, and `otok install --locked` fails while such an override is
  present, so CI never builds from a developer's checkout by accident.

In both cases the build cache key uses the hash of the directory contents at build time
(`cache-key.md`).

### 6.4 Indexes: `[indexes.<name>]` and `[index-of]`

A recipe id (`"publisher/name"`, in `recipe` and in the `default-recipe` of every recipe of the graph) is looked up in
the indexes of the project: the one `config.toml` names, called `default` here (the public index unless the machine
says otherwise, `config.md` 2), and those the manifest adds:

```toml
[indexes.corp]
url = "view+https://otok.corp.example/index"
publishers = ["corp", "corp-tools"]

[index-of]
"apotocki/zlib" = "corp"
```

`[indexes.<name>]`, a name of the library name syntax (common 2) other than `default`:

| Key | Type | Meaning |
|---|---|---|
| `url` | URL | — (required). The index as `index` of `config.md` 2 names one: its HTTPS view `view+https://<base>`, or the URL of its git repository (`https`, or `file` for one on this machine). |
| `publishers` | array of publisher names | The publishers whose recipes are in this index only: their ids are looked up nowhere else. A publisher owned by two indexes is an error. |

`[index-of]` maps recipe ids to the name of the index they are taken from (`default` or one of `[indexes]`), for the
whole graph: the project's own dependencies and the references of every recipe.

An id is looked up:

1. in the index `[index-of]` names for it;
2. else, when an index owns its publisher, in that index;
3. else in every index the manifest has: found in one, it is taken from there; found in two or more, resolution fails
   and names them, until `[index-of]` chooses. There is no priority between indexes, and equal entries in two indexes
   are no exception: a copy of an index is a mirror of the same index (`index-mirror` in `config.toml`), not a second
   one.

The index each package came from is in `otok.lock` (`otok-lock.md` 2), where `otok audit` and `otok outdated` look for
its advisories and newer versions. A locked project reads no index at all, so an id published later in another index
changes nothing until the project is resolved again, and then stops it with that error instead of taking the other
recipe. Indexes are declared in the manifest only, not in fragments (11); the machine gives the address of `default`
and its mirror (`config.md` 2), not other indexes.

## 7. `[tool-deps.<library>]`

Chooses the recipe, versions and options of a library when recipes use it as a tool dependency.
It causes a tool build on its own only with `deliver` (7.1): an entry that no recipe uses and
nothing delivers is reported as unused (a warning). Tool dependencies are resolved separately from `[deps.*]`; the same library may
appear in both with different recipes, versions and options. A toolchain that names a tool recipe is a tool dependency
on the library of its name (`recipe-toml.md` 13.1), so `[tool-deps.cmake]` chooses the CMake recipes use.

Keys: `recipe`, `version`, `prerelease`, `options`, `source`, `cflags`, `cxxflags`, `ldflags`, `c-standard`, `cxx-standard`, with the
meaning of section 6, and `deliver` (7.1).
`platforms` is not allowed: a tool dependency is built for the tool slice only.

The tool slice is built for the build machine by the target of its system: on macOS the
`macosx` platform of `apple`, on Linux `linux`, on Windows `windows`, in the environment `[otok] tool-build-on` names
for that system (3). The tool recipes are those variants (`recipe-toml.md` 3.1). Tool dependencies are resolved for
every system a project's environment (4.9) may run on: one host system for `windows/msvc`, every system for `*/ndk`.

A library whose recipe does not build in that environment is built in another environment of the same system that it
builds in, the first of those the target lists: on Windows `windows/msvc`, then
`windows/clang-cl`; on Linux `linux/gcc`, then `linux/clang`. A tool that builds in `windows/clang-cl` thus runs a
ninja whose recipe builds in `windows/msvc` only. Each library is built in one environment of a system, for every tool
that uses it: the environments of one system share its ABI, so a tool links the libraries it depends on whichever of
them built each.

### 7.1 Tools the project runs

A program the consumer's own build runs (a compiler of the project's scripts, a code generator) comes from a tool recipe
(`recipe-toml.md` 3.1) through `deliver`. A tool that belongs to a library of the project, such as `protoc` of Protocol
Buffers, is delivered from that library's entry, so that the tool and the library come from one recipe and one version:

```toml
[deps.protobuf]
recipe = "apotocki/protobuf"
version = "~> 36.2"
deliver = ["protoc"]
```

A tool without a library of the project is delivered from `[tool-deps]`:

```toml
[tool-deps.annium-compile]
recipe = { git = "https://example.com/recipes.git", rev = "<commit>", path = "annium-compile" }
deliver = true
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `deliver` | boolean or array of program names | `false` | Build the tool for this machine though no recipe uses it, and copy it into the tools output: `true` with every program of the recipe's `[tool] programs`, a list with those named. |

The programs named must be among the recipe's `[tool] programs`, and a recipe without them is not delivered; both fail
resolution. The tools output gets the whole prefix of the tool slice either way (the programs may need files beside
them); only the programs named are in the files that give their paths.

In `[deps.<library>]` the tool is the library's recipe (`recipe`, its variant for the machine's system) at exactly the
version the library got in the target graphs (one version in all of them, 6), as if a recipe took it with
`[deps.<library>] toolchains` (`recipe-toml.md` 13.2). `[tool-deps.<library>]` beside it is not needed; it may choose
the tool's `options`, flags and standards, must name the same `recipe`, and its `version` must allow the library's. The
tool is built from the recipe's source, not from a `source` of `[deps]`. `deliver` is in one of the two tables, never in
both.

Such a library is resolved as a tool dependency of the project in the tool graph of every system of the lock (7), so
`otok.lock` has its `[[tool-package]]`, and `install` and `update` build it in the tool slice of the machine. After the
outputs of the targets they make `[otok] tools-output` (3, default `Otok/tools`) hold exactly the delivered tools:

- `<tools-output>/<library>/`: the prefix of its tool slice as `build` left it, with `bin/` and whatever else the recipe
  installs. Tool slices are release and static (5), so the programs need nothing beside them but the system's libraries.
- `<tools-output>/otok-tools.toml`: the paths of their programs (`[tool] programs`, as `build` returned them), relative
  to the file:

  ```toml
  # Generated by otok; do not edit. …
  format = 1

  [programs."annium-compile"]
  "annium-compile" = "annium-compile/bin/annium-compile.exe"
  ```

- `<tools-output>/otok-tools.props` and `<tools-output>/otok-tools.cmake`: the same paths for the consumer's build, as
  the MSBuild property `OtokTool_<program>` and the CMake variable `Otok_TOOL_<program>`, absolute from where the file
  is. A character of the program's name other than a letter, a digit or `_` is `_` there (`annium-compile`:
  `OtokTool_annium_compile`); two programs of the delivered tools with one such name are an error. A project of Visual
  Studio imports the first (from `Directory.Build.props`, beside `otok.props`), a CMake project `include`s the second:

  ```xml
  <Import Project="$(MSBuildThisFileDirectory)Otok\tools\otok-tools.props" Condition="Exists('$(MSBuildThisFileDirectory)Otok\tools\otok-tools.props')" />
  ```
  ```cmake
  include("${CMAKE_SOURCE_DIR}/Otok/tools/otok-tools.cmake")
  add_custom_command(OUTPUT parser.cpp COMMAND "${Otok_TOOL_bison}" -o parser.cpp "${CMAKE_SOURCE_DIR}/parser.y" ...)
  ```

  They are files of the tools output, not of a target's (`otok.props`, `otok-config.cmake` of 4.11): the tools are the
  machine's and need no target, and the tools output may be anywhere in the project.
- `.otok-output.toml`, the state file of 4.2, with the key of each tool's slice: a tool is copied again only when its
  slice changed, and a tool no longer delivered is removed. Without any delivered tool the directory is removed.

A program that is a script run by another tool (Meson by `python`, `recipe-toml.md` 13) is not delivered: its
interpreter is not. The tools output may be inside a target's `output`, where no library of that output may be (a
library named `tools` in `Otok/` of a target that lays libraries out as `<output>/<library>/`). `otok.dev.toml` builds a
delivered tool from working trees as any tool dependency (`otok-dev-toml.md`). Only the machine's own tools are
delivered, never those of another system.

## 8. `[profile.<name>]`

A named set of overrides selected with `--profile <name>`. Without `--profile` no profile
applies. A profile may contain:

| Key | Contents |
|---|---|
| `apple`, `android`, `windows`, `linux` | `platforms`, `architectures`, `deployment`, `cflags`, `cxxflags`, `ldflags`, `configurations`, `linkage`, `c-standard`, `cxx-standard`; not `build-on`, which chooses the recipes (4.9) |
| `deps.<library>` | `options`, `platforms`, `cflags`, `cxxflags`, `ldflags`, `linkage` |
| `tool-deps.<library>` | `options`, `cflags`, `cxxflags`, `ldflags` |

A profile cannot add or remove dependencies, change a `recipe`, `version`, `prerelease` or `source`,
or change `[policy]` or `[fetch]`: the set of inputs to fetch stays the same in every profile, so one
`otok.lock` serves all of them (`otok-lock.md`). A `deps.<library>` key for a library without a
`[deps.<library>]` entry is an error.

Merging onto the base manifest: tables merge key by key; arrays and scalar values replace;
`options` merge option by option (a profile's value replaces the base value of that option, including
list options). Flags (4.3) are the exception: a profile's flags are appended to the base ones.
Example: with the manifest of section 1, `--profile ci` builds everything for
`iossim` only; `deps.boost` keeps `platforms = ["ios", "iossim"]`, intersected with the effective
set (12).

## 9. `[policy]`

| Key | Type | Meaning |
|---|---|---|
| `deny-licenses` | array of SPDX license ids | Resolution fails if a dependency, direct or transitive (tool dependencies included), is licensed only under denied licenses. |
| `allow-licenses` | array of SPDX license ids | The stricter form: resolution fails if a dependency is not licensed under allowed licenses. |

At most one of the two keys may be present. Entries are license ids, optionally with an exception
(`"GPL-2.0-only WITH Classpath-exception-2.0"`). A dependency's license expression is evaluated as in
common 5: `MIT OR GPL-3.0-only` passes `deny-licenses = ["GPL-3.0-only"]`, because MIT can be chosen.
`otok audit` checks the same policy against `otok.lock`.

## 10. `[fetch]`

Where this project's inputs may be fetched from besides the locations the recipes name: a fixed copy
of an archive whose host went away, a fork of a repository, a mirror the team uses. The keys are those
of `config.md` 2.1, with the same meaning:

```toml
[fetch]
asset-mirror = ["https://files.example.com/otok/{sha256}"]
mirrors-only = ["boost"]

[[fetch.url-rewrite]]
from = "https://archives.boost.io/release/{version}/source/{file}"
to = "https://files.example.com/boost/{file}"
exact = true
```

| Key | Type | Meaning |
|---|---|---|
| `asset-mirror` | URL template, or array of them | Mirrors of archive inputs addressed by their sha256 (`{sha256}`). |
| `url-rewrite` | array of tables `{ from, to, exact }` | Rules giving URLs other locations (`config.md` 2.2); with `exact = true` and a whole URL in `from`, for one archive or repository. |
| `mirrors-only` | boolean, or array of library names | Never fetch from the recipes' own locations: for every input (`true`), or for the inputs of these libraries (their sources and resources; recipes fetched from git count as inputs of their library). |

URLs are `https`, `http` or `file`; a `file` URL names an absolute path. The project's locations are
tried before the machine's and the recipes' (`cache.md` 3.1, 4), with one exception: a machine with
`mirrors-only` in `config.toml` fetches only from the machine's own locations, so a project cannot lead
it to hosts its owner has not approved.

Like the machine settings, `[fetch]` changes where the bytes come from, never what is built: every
input is still checked against the hash or commit in `otok.lock`, and nothing of `[fetch]` enters the
lock or a cache key. The same lock on a machine without these locations fetches from the recipes'.

## 11. Includes

`[otok] include` lists fragment files, for dependency lists generated by other tools:

- Paths are relative to the manifest and must stay inside the project. A missing file is an error.
- A fragment is TOML with only `[deps.*]` and `[tool-deps.*]` tables (and `[metadata]`), with the
  meaning of sections 6 and 7. It has no `[otok]` table: it takes the manifest's format.
- Fragments cannot include other files.
- A library defined in more than one file is an error naming both files. Profiles in the manifest
  may refer to libraries defined in fragments.

## 12. Effective configuration

For a run with profile P (or none), otok computes:

1. The manifest merged with its fragments (11).
2. P's overrides merged onto it (8).
3. The effective platform set `E` = `[apple] platforms`.
4. For each direct dependency: platforms `E ∩ deps.<library>.platforms` (all of `E` when not given;
   a subset of the base platforms by 6). When a profile narrows `E` so that the intersection is
   empty, the library is not built in that profile, and otok says so.
5. Transitive dependencies are built for the union of the platforms of their dependents for which
   the dependency's `when` condition holds (`recipe-toml.md`).
6. Each platform in use must have a deployment version (4) and be supported by the dependency's
   recipe; otherwise resolution fails, naming the library and the platform.

## 13. Validation summary

Reported before resolution, all at once rather than one per run:

- syntax, unknown keys, wrong types (common 1.2);
- `format` missing or unknown;
- names (common 2), platform and architecture names (common 4), constraints (common 3.2);
- `recipe` forms (6.1), git `rev` not a full SHA;
- includes (11), profiles referring to unknown libraries (8);
- `[indexes]` and `[index-of]` (6.4): a name not of the library name syntax or `default`, a missing or malformed `url`, a
  publisher owned by two indexes, an `[index-of]` id that is not a recipe id, names an unknown index, or sends a publisher
  another index owns elsewhere;
- both `deny-licenses` and `allow-licenses`;
- `[fetch]` values (10): `asset-mirror` templates without `{sha256}` or not URLs, `url-rewrite` rules
  without `from` or `to` or with bad patterns (`config.md` 2.2), `mirrors-only` naming a library twice (a library no dependency has is a warning
  after resolution);
- `output` outside the project, or overlapping the `output` of another target;
- `tools-output` outside the project, or the `output` of a target or around one (7.1); `deliver` neither a boolean nor a
  non-empty list of distinct program names, in both `[deps.<library>]` and `[tool-deps.<library>]`, or with a `[tool-deps]`
  entry of another recipe beside it in `[deps]` (7.1);
- flags that choose the slice (4.3);
- `configurations` empty, with a value twice, or with more than one value for `[apple]`; `linkage`
  other than `"static"` for `[apple]`;
- `build-on` missing where the target needs it, empty, with a value twice, or naming an environment the target
  does not have or `*` with another compiler than `ndk` (4.9); `tool-build-on` with two environments of one system or
  one of another target (3).

Reported before building: a target table this machine cannot build, a note, and an error when it can build none of
them (4.9). After building: a delivered tool whose program is a script, and a `tools-output` where a library of an output is
(7.1).

Reported during resolution: options against recipe schemas, version and platform availability,
deployment versions, option conflicts between dependents, license policy, a `linkage` the recipe cannot
build, a recipe id found in two indexes that `[index-of]` does not choose between (6.4), a delivered tool whose recipe
builds no programs or not one the project names (7.1).
