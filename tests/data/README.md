# Test fixtures

Manifests, recipes, a file-based index and expected outputs used by `otok-tests`. Tests reach this
directory through the `OTOK_TEST_DATA_DIR` macro, set by both builds. Sources of test libraries are
vendored here so that the tests need no network.

`projects/` holds whole projects that tests copy to a temporary directory and install with the real
target of the machine (milestone A, `docs/PLAN.md` decisions 19, 82): their `otok.toml` builds for `linux` with GCC, which
`host_target_test` turns into `windows` with MSVC on Windows and the tests of other targets into theirs; their recipes
name the platforms and environments of every target they build for. `tiny` (two tiny C libraries vendored in
`vendor/`, built by the default test run; on the Mac also for every Apple platform, with tiny-a's product
exercising `defines`, `link`, `apple.frameworks`, a privacy manifest and a Swift wrapper, and a header
that differs between architectures; tiny-b has a license file in its recipe directory), `dynamic`
(two libraries built dynamic, the one linked against the other, in the debug and release
configurations, run by consumers of both) and `zlib-lz4` (zlib from its release archive, lz4 from git, built only with `OTOK_TEST_NETWORK=1`).

`consumer/otok-package.toml` is the example of `docs/formats/otok-toml.md` 4.11, the file naming the files of a library's
directory in the outputs of `windows`, `linux` and `android`.
