# `build.lua` — recipe build script, API 1

Status: draft. Recipe metadata is in [recipe-toml.md](recipe-toml.md); common rules in
[README.md](README.md).

`build.lua` builds one slice of one library: it runs the library's own build system with the
toolchain otok prepared and returns where the libraries and headers are. Everything else —
iterating slices, SDKs, deployment flags, merging architectures, XCFrameworks, module maps,
`Package.swift`, caching — is done by otok. `build.lua` never packages: it returns libraries and headers,
and the target makes the output for consumers of them.

API 1 is Lua 5.5 with the library of section 6 and nothing else.

## 1. Examples

A configure-and-make library (OpenSSL):

```lua
function build(slice, options, deps)
    local configure = {slice.toolchains.perl, "./Configure", slice.vars.openssl_target,
        "no-shared", "no-tests", "--prefix=" .. slice.prefix}
    if options["no-deprecated"] then table.insert(configure, "no-deprecated") end
    for _, flag in ipairs(options.features) do table.insert(configure, flag) end

    otok.run(configure)
    otok.make{"build_libs"}
    otok.make{"install_dev"}

    return {
        libraries = { ssl = slice.prefix .. "/lib/libssl.a", crypto = slice.prefix .. "/lib/libcrypto.a" },
        headers = slice.prefix .. "/include",
    }
end
```

A CMake library (LZ4, whose CMake project lives in a subdirectory):

```lua
function build(slice, options, deps)
    otok.cmake{
        source = slice.source .. "/build/cmake",
        defines = { BUILD_SHARED_LIBS = false, LZ4_BUILD_CLI = false },
    }
    return {
        libraries = { lz4 = slice.prefix .. "/lib/liblz4.a" },
        headers = slice.prefix .. "/include",
    }
end
```

## 2. Execution model

- **One Lua state per slice.** For every slice otok creates a fresh state, loads `build.lua` (its
  top level should only define functions), and calls `build(slice, options, deps)`. Slices of one
  library may build concurrently; they share nothing through Lua.
- **Entry points.**
  - `build(slice, options, deps)` — required; called once per target slice.
  - `build_tools(slice, options, deps)` — optional; called once for the tool slice before any target
    slice, for libraries that build their own tools first (ICU's `--with-cross-build`). Its `slice.prefix` is kept
    and offered to target slices as `slice.tools.prefix`. Its return value is ignored.
- **Fresh inputs.** `slice.source` is a fresh, patched copy of the source (and resources with
  `dest`) for this slice only, so in-tree builds of different slices never collide.
- **Failure.** `error(...)`, a Lua runtime error, or a failing `otok.run` aborts the slice. otok
  reports the message with the `build.lua` line, the failed command and the end of the slice's log,
  and keeps the whole log, or the work directory when `keep-build-dirs` is set (`config.md`, `cache.md` 7.1).
- **No network.** Downloads happen before any build (common 1.6); processes started by the
  build have no network access where the host can enforce it (7.1). Where it cannot, the helpers still
  forbid the downloads of the build systems: `otok.cmake` those of CMake's `FetchContent`, `otok.meson` meson's wraps (6.2).

## 3. `slice`

A read-only table. Paths are absolute, with `/` as separator on every host.

| Field | Type | Meaning |
|---|---|---|
| `name` | string | `"iossim-arm64"` |
| `os` | string | The target (common 4): `"apple"`, `"android"`, `"windows"` or `"linux"`. |
| `platform` | string | `"iossim"` |
| `arch` | string | `"arm64"` |
| `environment` | string | The build environment (common 4.2) of the slice, its host without architecture: `"macos/xcode"`, `"linux/clang-cl"`, `"windows/ndk"` (also when the recipe says `"*/ndk"`). |
| `host` | string | The build machine, `<system>-<arch>`: `"linux-x86_64"`, `"windows-arm64"` (as `when = { host }` of `recipe-toml.md` 9.1). |
| `compiler` | string | The compiler of the environment: `"xcode"`, `"ndk"`, `"msvc"`, `"clang-cl"`, `"gcc"`, `"clang"`. |
| `triple` | string | Clang target triple with deployment version: `"arm64-apple-ios15.0-simulator"`. |
| `configuration` | string | `"release"` or `"debug"`. |
| `linkage` | string | `"static"` or `"dynamic"`: what the project asks for (`otok-toml.md` 5), one of the recipe's `[recipe] linkage`; `"static"` in tool slices. |
| `c_standard` | integer or nil | The C standard of the target (`otok-toml.md` 5.1), `11`: the project's, or the lowest the recipes allow; nil when there is none and the recipe chooses. Its flag is in `toolchain.cflags` (none for C89 and C99 with MSVC), and the helpers pass it on (6.2). |
| `cxx_standard` | integer or nil | The C++ standard of the target (`otok-toml.md` 5.1), `23`: the project's, or the lowest the recipes allow; nil when there is none and the recipe chooses. Its flag is in `toolchain.cxxflags`, and the helpers pass it on (6.2). |
| `is_tool` | boolean | `true` in the slices of a tool package (built for the build machine, `recipe-toml.md` 13) and in `build_tools`: a recipe that is a library and a tool at once builds its programs there only. |
| `source` | path | The fresh source copy; the default working directory. Writable. |
| `build` | path | An empty directory for out-of-source builds. Writable. |
| `prefix` | path | The empty output directory. Everything `build` returns must be inside it. Writable. |
| `recipe` | path | The recipe directory (extra files shipped with the recipe, e.g. a CMakeLists.txt for a library without a build system). Read-only. |
| `resources` | table: name → path | `[[resource]]` entries (`recipe-toml.md` 5) whose `when` holds. Read-only. |
| `vars` | table: name → value | `[vars]` resolved for this slice (`recipe-toml.md` 7). |
| `jobs` | integer | Parallelism of build tools without jobserver support: `jobs` of `config.md`. The most such a tool may run: the helpers give it as many as the slice may have when the command starts (6.2), and `otok.b2` runs the compilers through otok's launcher, which keeps them within the slice's jobserver. A recipe that passes it itself may start more processes than the slice was given. |
| `toolchain` | table | 3.1 |
| `toolchains` | table: program name → path | The programs the recipe declares in `[toolchains]` and in the `toolchains` of its dependencies (`recipe-toml.md` 13), as otok chose them: `slice.toolchains.perl`, `slice.toolchains.make` (which may be `gmake`), or the program a tool recipe built (`recipe-toml.md` 13.1, 13.2), in the cache: `slice.toolchains.flatc`. Undeclared toolchains, and those whose `when` does not hold, are absent. |
| `tools` | table or nil | `{ prefix = … }` of the `build_tools` slice, when the recipe defines `build_tools`. |
| `rust_target`, `zig_target` | string | Target names of those ecosystems (`"aarch64-apple-ios-sim"`). |
| `goos`, `goarch` | string | Go target (`"ios"`, `"arm64"`). |
| `apple` | table or nil | 3.2; present when `os == "apple"`. |
| `android` | table or nil | 3.3; present when `os == "android"`. |

### 3.1 `slice.toolchain`

| Field | Type | Meaning |
|---|---|---|
| `cc`, `cxx` | path | Compilers for the slice (Xcode's clang on Apple). |
| `ar`, `ranlib`, `strip`, `nm` | path | Binary tools. |
| `install_name_tool` | path or empty | Where dynamic libraries need `@rpath/` install names (dynamic `apple` slices, 8): the tool that sets them; empty elsewhere. |
| `rpath` | string | The `RUNPATH` entry by which a dynamic library finds the other libraries of its slice, in the directory it is in: `$ORIGIN` in dynamic slices of targets whose libraries are ELF shared objects (`linux`), empty elsewhere (macOS has `@rpath/` install names, Windows looks beside the DLL, Android's loader finds an app's libraries by name). The helpers set it (6.2); a recipe that links with its own makefiles sets it itself. |
| `cflags`, `cxxflags`, `ldflags` | array of strings | Target, sysroot, deployment and reproducibility flags (`-ffile-prefix-map`), then the configuration's (`-O2 -DNDEBUG` / `-O0 -g`; MSVC `/O2 /DNDEBUG /MD` / `/Od /Z7 /MDd`), then in `cxxflags` the C++ standard's (`-std=c++23`, MSVC `/std:c++latest`; `otok-toml.md` 5.1), then the project's (`otok-toml.md` 4.3). |
| `env` | table | The environment `otok.run` starts processes with (section 5): `CC`, `CXX`, `AR`, `RANLIB`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `PATH`, … |

### 3.2 `slice.apple`

| Field | Type | Meaning |
|---|---|---|
| `sdk` | string | `"iphonesimulator"`, `"macosx"`, … |
| `sdk_path` | path | The SDK root (`xcrun --show-sdk-path`). |
| `min_version` | string | Deployment version for this slice (`"15.0"`). |
| `version_min_flag` | string | `"-mios-simulator-version-min=15.0"`, for build systems that do not take a triple. |

### 3.3 `slice.android`

| Field | Type | Meaning |
|---|---|---|
| `abi` | string | The ABI, `slice.arch` (`"arm64-v8a"`). |
| `api` | string | The API level of the slice (`"24"`), `deployment.android` of the project. |
| `ndk` | path | The NDK's root, for build systems that need it (`ANDROID_NDK`). |

The triple names the API level as the NDK's compilers do (`aarch64-linux-android24`,
`armv7a-linux-androideabi24`); `slice.toolchain.cflags` start with `--target=<triple>`. The compilers
are the NDK's `clang` and `clang++`, the binary tools its `llvm-ar`, `llvm-ranlib`, `llvm-strip`, `llvm-nm`.

### 3.4 `slice.windows`

| Field | Type | Meaning |
|---|---|---|
| `arch` | string | The architecture, `slice.arch` (`"x86_64"`, `"arm64"`). |
| `sdk` | path or empty | The directory of `xwin splat` (`windows-sdk` of `config.md` 2) in `linux/clang-cl`, for build systems that need its parts (gn's `win_sdk`); empty on Windows, where Visual Studio's environment has them. |
| `crt_version` | string | The CRT's version (`"14.44.35220"`). |
| `sdk_version` | string | The Windows SDK's version (`"10.0.26100"`). |

The triple is `x86_64-pc-windows-msvc` or `aarch64-pc-windows-msvc`, and the output the same in
every environment of the target.

In `windows/msvc` the compilers are `cl.exe`, the archiver `lib.exe` and the linker `link.exe` of the Visual Studio
environment (5), with the CRT and the SDK it names.

In `windows/clang-cl` and `linux/clang-cl` the compiler is clang-cl, for C and C++ alike, the archiver llvm-lib (on
Windows, those of Visual Studio's LLVM or of LLVM's installer, with Visual Studio's CRT and SDK); the flags start with `--target=<triple>`,
`-fuse-ld=lld-link` (for the links clang-cl runs itself: configure's, meson's checks; its default on Linux is GNU ld),
`-fms-compatibility-version=19.<minor>` of the CRT (the STL checks it) and `/Brepro`; C++ adds `-Wno-register`, as MSVC
takes `register` in C++17. As vcvars does, the environment has `INCLUDE` (the CRT's
and the SDK's headers) and `LIB` (their libraries for the slice's architecture), separated by `;`, so that tools
running clang-cl, lld-link or llvm-rc themselves find them. The CMake toolchain file sets `CMAKE_SYSTEM_NAME Windows`,
lld-link, llvm-lib, llvm-rc and llvm-mt, and `CMAKE_ASM_COMPILER_TARGET` (assembly `.S`, compiled by clang-cl); no
MASM: llvm-ml gave Boost.Context's `lea rcx, label` an absolute `ADDR32` relocation, which MSVC's linker refuses in a
program. meson's cross file names the compilers with their `--target`, `c_ld` lld-link
and `windres` llvm-rc.

## 4. `options` and `deps`

`options` is a read-only table of the merged option values (`recipe-toml.md` 8): every declared
option is present. Names with `-` are read as `options["no-deprecated"]`. A `file` option is the path of a
copy of the project's file in the work directory, or `false` when the project gives none
(`recipe-toml.md` 8.3).

`deps` is a read-only table with one entry per dependency active for this slice (its `when` holds):

| Field | Type | Meaning |
|---|---|---|
| `prefix` | path | The dependency's output directory for the same slice. |
| `include` | path | Its headers directory. |
| `libraries` | table: name → path | The files of its libraries to link against for this slice (8). |
| `linkage` | string | How it was built: `"static"` or `"dynamic"`. |
| `lib_dirs` | array of paths | Directories containing them. |
| `version` | string | Resolved version. |
| `options` | table | Its merged options; a `file` option is `true` when the project gave a file, else `false`. |

`deps.tools` holds tool dependencies the same way, with `bin` (directory of its executables) in
addition; it is built for the tool slice. `tools` is therefore reserved and cannot be a library name.

## 5. Processes

`otok.run` and the helpers start programs directly, never through a shell.

- **Environment.** Not the user's environment: `slice.toolchain.env`, which contains the toolchain
  variables, `PATH` made of system directories, the directories of declared toolchains
  (`recipe-toml.md` 13) and `deps.tools.*.bin`, `HOME` (and `USERPROFILE`) and `TMPDIR` (and `TEMP`, `TMP`) pointing into the work directory,
  `LANG=C`, `SOURCE_DATE_EPOCH` and, on Apple, `ZERO_AR_DATE=1` for reproducible archives.
  `MAKEFLAGS` carries the jobserver. Python gets none of the user's modules: `PYTHONNOUSERSITE=1`, and no
  `PYTHONPATH` or `PYTHONHOME`, which an environment captured from the user's (Visual Studio's on Windows) would
  bring. The targets that build for the machine's own system start from nothing of the user's too: in `linux` the
  `PATH` is the directories of its compilers, then `/usr/bin:/bin:/usr/sbin:/sbin`; in `windows/msvc` and
  `windows/clang-cl` the environment `vcvarsall.bat` makes from Windows' own variables, unless otok runs in a Visual
  Studio developer prompt, whose environment is the user's choice of Visual Studio and is taken as it is. `PKG_CONFIG_LIBDIR` is empty in every
  target, so pkg-config finds the dependencies (through `PKG_CONFIG_PATH`, 6.2) and nothing installed on the machine.
  Calls may add or override variables (`env = { FOO = "1" }`) or remove them (`env = { FOO = false }`).
- **Program lookup.** A program name without `/` is looked up in that `PATH`; a name with `/` is a
  path relative to the working directory.
- **Output** goes to the slice's log; the user sees a summary and, on failure, the end of the log.
- **Lifetime.** On Windows every process otok starts is in a job object that ends with otok, however otok ends, and
  with it everything the build started; a program that crashes there ends at once,
  without the dialog of Windows Error Reporting. A daemon a build starts (`mspdbsrv`) ends with otok too.

## 6. The `otok` library

The whole API. All functions raise a Lua error on failure unless stated otherwise.

### 6.1 Running programs

| Function | Meaning |
|---|---|
| `otok.run(argv, opts)` | Runs `argv` (array of strings). `opts`: `cwd` (default `slice.source`), `env` (table, section 5), `check` (default `true`; with `false` a non-zero exit returns the exit code instead of failing), `capture` (default `false`; with `true` returns standard output as a string, at most 1 MiB, and the exit code as a second value). Without `capture` it returns the exit code; a process ended by a signal counts as `128 + signal`. |

Every function that takes a table of options rejects keys it does not know, so a misspelled option
fails instead of being ignored. The command of each process is written to the log (`$ cmake …`).

### 6.2 Build-system helpers

Each helper runs the build system with the slice toolchain, dependencies made findable and output
installed into `slice.prefix`. Extra arguments go into `args`; every helper accepts `cwd` and `env`
as `otok.run` does.

| Function | Meaning |
|---|---|
| `otok.make{targets…, args, jobs}` | Runs `make` with the given targets. Parallelism comes from the jobserver; `jobs` forces a fixed number. |
| `otok.cmake{source, build, defines, args, generator, targets, install}` | Configures (`source` default `slice.source`, `build` default `slice.build`, generator default `Ninja`) with the slice's toolchain file, `CMAKE_INSTALL_PREFIX = slice.prefix`, `CMAKE_BUILD_TYPE` from the configuration, `BUILD_SHARED_LIBS` from `slice.linkage`, `CMAKE_PREFIX_PATH` and `CMAKE_FIND_ROOT_PATH` set to dependency prefixes, and, after `defines` and `args`, so a recipe cannot turn it off, `FETCHCONTENT_FULLY_DISCONNECTED=ON`: `FetchContent` downloads nothing, and content the project fetches comes from a resource of the recipe through `FETCHCONTENT_SOURCE_DIR_<NAME>` or the configure step fails; then builds `targets` (default all) and installs (`install`, default `true`). `defines` values: strings, numbers, booleans (`ON`/`OFF`). |
| `otok.autotools{configure, args, autoreconf, targets}` | Runs `configure` (default `./configure`), on a Windows build machine through the `sh.exe` beside the `make` toolchain when it has one (MSYS2's, with the script and the prefix written `/c/…` as MSYS2 names them, and MSYS2's directory in `PATH` after the toolchain's and before Windows' own) with `--host` from the triple, `--build` naming the build machine for a platform whose programs it does not run (Apple: the devices and simulators, as `aarch64-apple-darwin27.0.0`, so configure runs no test program of the slice), `--prefix=slice.prefix`, `--disable-shared --enable-static` (`--enable-shared --disable-static` for `slice.linkage == "dynamic"`), `CPPFLAGS`/`LDFLAGS`/`PKG_CONFIG_PATH` for dependencies; with `autoreconf = true` regenerates it first instead of running the one the source ships; then `make` with `targets` (default `{"install"}`). |
| `otok.meson{source, build, options, args}` | Configures (`source` default `slice.source`, `build` default `slice.build`) with the slice's machine file (below), `--prefix=slice.prefix`, `--libdir=lib`, `--buildtype=plain`, `--default-library` from `slice.linkage` (`static`/`shared`), `--wrap-mode=nodownload`, `pkg_config_path` and `cmake_prefix_path` set to the dependency prefixes, and `options` as `-D<name>=<value>`; then builds with `ninja` and installs. `options` values: strings, numbers, booleans (`true`/`false`), arrays of strings (joined with commas). |
| `otok.b2{b2, args, targets}` | Runs Boost's `b2` (the program `b2`, required: usually built by `build_tools` with `bootstrap.sh` and run from `slice.tools.prefix`) in `cwd` (default `slice.source`, Boost's root) with a generated `user-config.jam` for the slice toolchain (below); then the `targets` (default `{"install"}`). |
| `otok.cmake_toolchain()` | Path of the generated CMake toolchain file for the slice. |
| `otok.meson_cross_file()` | Path of the slice's generated meson machine file: a cross file, or a native file in slices that are not cross-compiled. |

Toolchains: the helpers run the programs otok chose for the recipe's `[toolchains]` (`recipe-toml.md` 13,
`slice.toolchains`), and each fails before running anything when the recipe does not declare what it
runs, so every tool that shapes the output is in the slice key: `otok.cmake` needs `cmake` and the tool of its generator
(`ninja` for `Ninja`, `make` for `Unix Makefiles`), `otok.meson` `meson` and `ninja`, `otok.make` `make`,
`otok.autotools` `make` and, with `autoreconf = true`, `autoconf`, `automake` and `libtool`. The message
names the helper and the toolchain: `otok.cmake: 'ninja' is not in [toolchains] of the recipe`.
Other generators of `otok.cmake` need only `cmake`. The platform's own build tools (MSVC's, Xcode's) and
`otok.b2`, whose `b2` the recipe builds, need no declaration.

Parallelism: the jobserver (`MAKEFLAGS` in the environment) is given to a tool that understands it
(GNU make 4.4 and later, but not the make of MSYS2 or Cygwin, which takes a fifo and not the named semaphore of
otok on Windows; ninja 1.13 and later; otok checks `--version` once per program and run),
without `-j`. On macOS and Linux, an older GNU make that `otok.make` or `otok.autotools` runs itself gets it too, as
a make before 4.4 takes it from its parent: the jobserver's fifo as descriptors 3 and 4, and `MAKEFLAGS`
`-j<n> --jobserver-auth=3,4` (make 4.2 and 4.3) or `-j --jobserver-fds=3,4` (before 4.2, such as the make 3.81 of
macOS); one that `cmake --build` runs does not, since CMake need not pass descriptors on. The jobserver is the slice's own, which otok fills while all its tokens are in use. Any
other tool runs without `MAKEFLAGS`, and `make` gets `-j <n>`, `cmake --build` `--parallel <n>`: the jobs the slice
may have when the command starts, at most `slice.jobs`, which it holds until the command ends. `otok.cmake` checks the tool of its generator (`Ninja`:
ninja, `Unix Makefiles`: make); other generators always get `--parallel`. `jobs` of `otok.make`
forces `-j <jobs>` without `MAKEFLAGS`. `otok.run` passes the environment as it is.
`cmake --build` and `cmake --install` get `--config` with the build type, for multi-configuration
generators. `defines` are passed sorted by name, so the command line is the same on every run.

Flags in CMake builds: the generated toolchain file sets `CMAKE_<LANG>_FLAGS_INIT` and the linker
flags `CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS_INIT` to `slice.toolchain.cflags`/`cxxflags`/`ldflags`
and sets every `CMAKE_<LANG>_FLAGS_<CONFIG>` to empty, so CMake adds no configuration flags of its own
after otok's and the project's (`otok-toml.md` 4.3). `otok.cmake` runs without `CFLAGS`, `CXXFLAGS`
and `LDFLAGS` in the environment, which CMake would add a second time. For dynamic `apple` slices the
toolchain file sets `CMAKE_INSTALL_NAME_DIR` to `@rpath`, so dynamic libraries get
`@rpath/` install names (8). With MSVC it sets `CMAKE_MSVC_RUNTIME_LIBRARY` to the slice's
runtime and empties `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` and `CMAKE_MSVC_RUNTIME_CHECKS`, so CMake 4
policies add no `/Zi` or `/RTC1` of their own. The toolchain files of the slices built on a machine of the target's own
system (`linux`; `windows` on Windows) keep CMake's searches
out of what was installed on the machine beside the system: they turn off the package
registries (`CMAKE_FIND_USE_PACKAGE_REGISTRY`, `CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY`), on Linux ignore the
prefixes of other package managers (`CMAKE_IGNORE_PREFIX_PATH`: `/usr/local`, `/opt/local`,
`/home/linuxbrew/.linuxbrew`), and on Windows CMake's system prefixes, the directories of Program Files
(`CMAKE_FIND_USE_CMAKE_SYSTEM_PATH` off). The system's own libraries stay findable (`/usr`, the SDK).

The standards (`slice.cxx_standard`, `slice.c_standard`, `otok-toml.md` 5.1), when the target has them, are
passed as each build system takes them, so none adds a standard of its own after otok's flag: `otok.cmake`
defines `CMAKE_CXX_STANDARD=<n>`, `CMAKE_CXX_STANDARD_REQUIRED=ON` and `CMAKE_CXX_EXTENSIONS=OFF`, and the
same `CMAKE_C_*` (`90` for C89) (a project's `CMakeLists.txt` that sets them itself still wins in its own
targets); `otok.meson` sets the options `cpp_std=c++<n>` and `c_std=c<n>` (with MSVC, whose meson knows them so,
`c++latest` and `clatest` for 23 and later, and no `c_std` for C89 and C99), and fails when the recipe's
`options` give another; `otok.b2` adds the property `cxxstd=<n>`, and fails when the recipe's `args` have
another `cxxstd=`. A recipe that chooses a standard itself takes `slice.cxx_standard` or `slice.c_standard`
when it is set. `otok.autotools`, `otok.make` and `otok.run` have the flags in `CXXFLAGS` and `CFLAGS` only.

Search paths of dynamic libraries: where `slice.toolchain.rpath` is set (`$ORIGIN`), the helpers give
the libraries they build that `RUNPATH` with their build system's own means, which also escape its
`$` as it needs: `otok.cmake` `CMAKE_INSTALL_RPATH`, `otok.meson` `-Wl,-rpath,$ORIGIN` among the link
arguments of the machine file (meson keeps them at install, removing only its build directories),
`otok.b2` the property `linkflags=-Wl,-rpath,'$ORIGIN'` (not `dll-path`, which b2 normalizes as a path;
it links through the shell and turns backslashes into slashes, so single quotes keep the `$`),
`otok.autotools` `-Wl,-rpath,\$$ORIGIN` in `LDFLAGS` (the shell of `configure` and make each take one
level). A library of a dynamic slice then finds the libraries of its
slice in its own directory wherever the consumer puts them together; `otok.make` and `otok.run` leave
it to the recipe, and the check of 8 tells when it is missing.

`otok.b2` writes `slice.build/otok-user-config.jam` with `using <toolset> : <version> : <slice.toolchain.cxx> : …`
(the version is `otok`, for `msvc` the toolset's own, `14.5` for MSVC 14.51, from which `msvc.jam` derives
its settings):
the toolset the target names for the slice (`darwin` for Apple slices, `clang` for Android slices, `gcc` or
`clang` for `linux` by the compiler, `msvc` in `windows/msvc`), `slice.toolchain.cflags` as `<cflags>` and
`<asmflags>` (Boost.Context's assembly needs the target flags too), `cxxflags` as `<cxxflags>`,
`ldflags` as `<linkflags>`, the archiver and `ranlib` for `gcc` and `clang`, and an empty `<striper>`
for `darwin`, so libraries keep their symbols until the app is linked. It runs
`b2 --user-config=<file> --ignore-site-config -j<slice.jobs> --build-dir=<slice.build>/b2
--prefix=<slice.prefix> --stagedir=<slice.prefix> toolset=<toolset>-<version> variant=<release|debug>
link=<static|shared> runtime-link=shared threading=multi <target properties> <args> <targets>`. `b2`
has no jobserver, so its compiler command in `user-config.jam` is otok's launcher,
`otok jobserver-exec --slot <slice.build>/otok-job.lock --auth <jobserver> -- <compiler>`: it runs the
compiler once it holds the slot of the slice (the slice's own token: each slice always runs one) or a
token of the slice's jobserver, so every slice may start `jobs` compilers and still no more than `jobs` run on
the machine (not for `msvc`, whose `msvc.jam` looks for `link.exe` beside the compiler's command: there `-j` is the
jobs the slice may hold when b2 starts); `~/user-config.jam` and `site-config.jam` are not read; the target properties are
what the target knows of the slice (`target-os=iphone`, `architecture=arm`, `address-model=64`,
`abi=aapcs`, `binary-format=mach-o` for an iOS slice; `target-os=android`, `architecture=arm`, `address-model=64`,
`abi=aapcs`, `binary-format=elf` for `android-arm64-v8a`; for `linux` and `windows/msvc` slices, which `b2` detects
otherwise, the `abi` (`ms` with MSVC, `aapcs` on arm64, else `sysv`) and with MSVC `architecture=x86` (or `arm`) and
`address-model=64`, as `msvc.jam` takes 32-bit x86 otherwise).
`stage` puts the libraries into `slice.prefix/lib` without `install` copying the headers one by one:
a recipe of a large library copies its headers with `otok.copy`. A target that cannot run `b2` for a
slice makes `otok.b2` fail.

Meson builds: the builder writes the slice's machine file, `meson.ini` in the work directory, with
`[binaries]` (`c` and `cpp` from `slice.toolchain.cc`/`cxx`, also as `objc` and `objcpp` for Apple
slices; `ar`, `strip`; `pkg-config` when the recipe declares it, `slice.toolchains["pkg-config"]`) and
`[built-in options]` (`c_args`, `cpp_args`, for Apple slices `objc_args` and `objcpp_args`, from
`slice.toolchain.cflags`/`cxxflags`; `c_link_args`, `cpp_link_args` from `ldflags`, so meson takes no
flags from the environment). For a cross-compiled slice, every Apple slice, it is a cross file: the
target describes the host machine (`[host_machine]` `system = 'darwin'`, `kernel = 'xnu'`, `subsystem`
`ios`, `ios-simulator`, `macos`, `tvos`, `watchos-simulator`, `visionos`, … as meson names them,
`ios` for Catalyst; `cpu_family = 'aarch64'` and `cpu = 'arm64'`, `cpu = 'arm64_32'` for arm64_32,
`x86_64` for the Intel simulators; `endian = 'little'`) and `[properties]` has `needs_exe_wrapper = true`,
so meson never runs what it builds. Android slices are cross-compiled too: `system = 'android'`, `kernel = 'linux'`,
`cpu_family` `aarch64`, `arm`, `x86_64` or `x86`. Otherwise it is a native file (`--native-file`). With
`--buildtype=plain` meson adds no optimization, debug or `NDEBUG` flags of its own after otok's and the
project's (`otok-toml.md` 4.3); with MSVC `-Db_vscrt=none` as well, since otok's flags choose the
runtime. `otok.meson` runs `meson setup` without `CC`, `CXX`, `AR`, `CFLAGS`, `CXXFLAGS` and `LDFLAGS`
as the slice gives them: meson would add the flags a second time, and in a cross build apply them to
the build machine. It builds with `ninja -C <build>` (the jobserver as above, else `-j <slice.jobs>`)
and installs with `meson install -C <build> --no-rebuild`. `meson install` names a Mach-O dynamic
library after the directory it installs it in; for a dynamic slice with `slice.toolchain.install_name_tool`,
`otok.meson` sets the install names of the libraries in `slice.prefix/lib`, and their references to each
other, back to `@rpath/<file>` (8). Projects that download subprojects
(wraps) fail: their sources are resources of the recipe, placed with `dest` into `subprojects/`.

`otok.cargo{}` and `otok.go{}` (vendored, offline ecosystem builds) are added to API 1
before it is published; their contract depends on how ecosystem lock files become resources.

### 6.3 Files

Paths are absolute or relative to `slice.source`. Reading is allowed in `slice.source`, `slice.build`,
`slice.prefix`, `slice.recipe`, `slice.resources`, the copies of `file` options (4), dependency prefixes, the generated CMake toolchain
file (`otok.cmake_toolchain()`) and meson machine file (`otok.meson_cross_file()`); writing only in
`slice.source`, `slice.build` and `slice.prefix`. Anything else is an error, symbolic links included
(a link is resolved before the check).

| Function | Meaning |
|---|---|
| `otok.exists(path)` | `true` if the path exists. |
| `otok.read(path)` | File contents as a string. |
| `otok.write(path, text)` | Creates or replaces a file, creating parent directories; a file that already has the text is left as it is, its modification time too (`otok-dev-toml.md` 4). |
| `otok.copy(from, to)` | Copies a file or a directory tree; a file that already has the contents is left as it is, its modification time too. |
| `otok.move(from, to)` | Moves a file or directory. |
| `otok.remove(path)` | Removes a file or a directory tree; not `slice.source`, `slice.build` or `slice.prefix` themselves. |
| `otok.mkdir(path)` | Creates a directory and its parents. |
| `otok.glob(pattern, base)` | Array of the absolute paths of files and directories under `base` (default `slice.source`) whose path relative to `base` matches `pattern`, sorted. `*` and `?` match within one path component, `**` across components (`**/` also matches nothing). |
| `otok.join(…)` | Joins path components with `/`. |

### 6.4 Messages

| Function | Meaning |
|---|---|
| `otok.log(…)` | Writes to the slice's log. `print` does the same, and Lua's `warn` writes `warning: …` there. |
| `otok.warn(text)` | A warning shown to the user after the build. |

`otok.api` is the integer `1`.

## 7. Standard library

Available: the basic functions except `dofile`, `loadfile` and binary chunks in `load` (only text
mode `"t"`); `string`, `table`, `math`, `utf8`, `coroutine`. Not available: `io`, `os`, `package`
(and `require`), `debug`. `print` writes to the log.

A recipe therefore reaches the outside world only through the `otok` library, whose file access is
confined (6.3) and whose processes run in the sandbox (7.1).

### 7.1 Security model

The restrictions on Lua are not the security boundary: a recipe can run any program with `otok.run`,
including scripts from the source or tools it has just built. The boundary is the sandbox around the
processes otok starts for a build: no network, writes only in the slice's work
directories, reads only in the work directories, the otok cache, Xcode and the SDKs, system
directories and declared toolchains — not the rest of the user's home directory, so a build can
neither exfiltrate a secret nor embed one into the library it produces.

`build.lua` itself runs inside the otok process, which is not sandboxed (it needs the network and the
cache). The restrictions on Lua make sure Lua can do no more than a sandboxed process: every effect on
the outside world goes through the confined file functions (6.3) or through `otok.run`, whose process
is always sandboxed. They also keep a build deterministic (no environment variables, clock or
arbitrary files) and contain mistakes such as removing the wrong directory.

What no sandbox can do is detect malicious code compiled into the library itself: it protects the
machine during the build, not the app that links the result. That is the job of the supply-chain
measures (pinned hashes, git sources, reviewable diffs, advisories).

## 8. Return value

`build` returns a table:

| Field | Type | Meaning |
|---|---|---|
| `libraries` | table: name → path or `{ link, runtime, symbols }` | The libraries of this slice. Names are the library output names `[[product]] libraries` refers to (`recipe-toml.md` 11). A path, or `link`, is the file linked against: a static archive, a shared object or `.dylib`, an import library. `runtime` is the DLL of a Windows import library, `symbols` its `.pdb` (optional). |
| `headers` | path, or table: library name → path | Public headers: one directory for all libraries, or one per library. A recipe with a product of headers only (`recipe-toml.md` 11.1) returns one directory. |
| `module_map` | path, or table: library name → path | Optional hand-written module map replacing the generated one (Apple). It is placed in `Headers/<module>/module.modulemap` of the XCFramework (`otok-toml.md` 4), so its paths start from there: `header "../openssl/ssl.h"`. |
| `data` | table: file name → path | Optional files the consumer ships beside the libraries and loads at run time (ICU's `icudt79l.dat`). otok packages them into `data/` of the library's output (`otok-toml.md` 4, 4.1) as they are; the consumer decides how they reach the product. Names are `[A-Za-z0-9._-]+`, not starting with `.`. Every slice of a target returns the same names with the same contents. |

| `programs` | table: program name → path or `{ script, interpreter }` | A tool recipe's programs (`recipe-toml.md` 3.1): none but those of `[tool] programs`, every one of them in the slices of a tool package (a library that is a tool too builds them there only, 13.2), each a file (on Windows the `.exe`). A script that is no program of the system (Meson, Python) is `{ script = <path>, interpreter = "python" }`: `interpreter` names a toolchain of the recipe, which otok puts before the script wherever a build starts it. A recipe without `[tool]` returns none. |

`libraries` and `headers` are required, except in a tool recipe without products, which returns `programs` only; other
fields are an error. Every path must exist and lie
inside `slice.prefix` (relative paths are taken from `slice.source`, as in 6.3). otok checks that every
library is for the slice's architecture, that every library a product needs is present
(`recipe-toml.md` 16), and that every slice returns the same library names.

otok tells the kind of a library from its file: a static archive (`!<arch>`), an ELF shared object,
a Mach-O dynamic library, a Windows import library (an archive with `runtime`, a PE DLL). With
`slice.linkage == "static"` every library must be a static archive; with `"dynamic"` static archives
may remain (a library that is static by nature, like `SDL2main`), and a DLL needs its import library
as `link`. A shared object comes with the symlinks pointing to it in its directory (`libz.so` →
`libz.so.1` → `libz.so.1.3.1`). No ELF or Mach-O dynamic library may name the work directory or
otok's cache in what is fixed at link time (its install name or `SONAME`, an `RPATH`/`RUNPATH`, a
`NEEDED` entry or loaded library), and a Mach-O install name starts with `@rpath/`: the consumer's executable chooses where libraries are. Where `slice.toolchain.rpath` is set, an ELF shared object that needs another library of its slice (a `NEEDED` name of a file in the directory of a returned library) must have it in its `RPATH` or `RUNPATH`, since the consumer's own `RUNPATH` does not reach the dependencies of a library.

What an ELF or Mach-O dynamic library or program of the slice loads (a `NEEDED` entry, a loaded library) must be the
slice's own, a dependency's or the system's, so a library installed on the build machine
cannot reach the output unnoticed: a name the target counts as the system's (`linux`: the libraries of the C
and C++ runtimes, `libc.so.6`, `libm.so.6`, `libstdc++.so.6`, `libgcc_s.so.1`, `ld-linux-*`, …; `apple`: everything in
`/usr/lib/` and `/System/`); a name without a directory or starting with `@rpath/`, `@loader_path/` or
`@executable_path/` whose file name is a file of the slice's prefix or of a directory of a library of a dependency,
direct or not; or a library of the `link` of the recipe's products (`recipe-toml.md` 11): `z` allows `libz.so…`.
Anything else, such as `libpng16.so.16` from the machine's `/usr/lib` or `/opt/homebrew/lib/libpng16.16.dylib`, fails
the build. Universal Mach-O files and PE images are not read; `android` and `windows` do not check (their
toolchains see the NDK or the CRT and SDK only), nor static `apple` slices, which load nothing.

The pkg-config files of the slice, `lib/pkgconfig/*.pc` and `share/pkgconfig/*.pc`, are made to find it where it is
kept, since the build wrote the prefix it was built in, a directory that is gone: the
variable `prefix` that names that prefix becomes `${pcfiledir}/../..`, and the prefix elsewhere in the file `${prefix}`
(`${pcfiledir}/../..` in a file without that variable). Dependents find them through the `PKG_CONFIG_PATH` of `otok.autotools`
and the `pkg_config_path` of `otok.meson` (6.2), with any pkg-config.

## 9. Compatibility

Within API 1 nothing in this document changes meaning once API 1 is published. New functions or
fields need API 2; recipes with `api = 1` keep running exactly as specified here (common 1.4).
