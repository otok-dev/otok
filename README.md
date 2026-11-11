# otok

Native libraries built from source into XCFrameworks for Xcode and SwiftPM — and for Android, Windows and Linux.

otok builds each library your app depends on from source on your machine, with your options and for your platforms,
and hands the result to your build: a local Swift package of XCFrameworks for Xcode, a CMake package for Gradle and
CMake, property sheets for Visual Studio. One file, `otok.toml`, says what your project needs; `otok.lock` pins every
input by its hash.

```toml
[otok]
format = 1

[apple]
platforms = ["ios", "iossim", "macosx"]
deployment = { ios = "16.0", macos = "13.0" }

[deps.openssl]
recipe = "apotocki/openssl"
version = "~> 4.1"
options = { no-deprecated = true }
```

```sh
otok install
```

**Documentation: [otok-dev.github.io](https://otok-dev.github.io/)** — getting started for Apple, Android and
Windows, coming from CocoaPods, writing and publishing recipes. The libraries otok can build are in the
[catalog](https://otok-dev.github.io/index/).

## Install

```sh
brew install otok-dev/tap/otok                                           # macOS
```

```powershell
scoop bucket add otok https://github.com/otok-dev/scoop-bucket; scoop install otok   # Windows
```

```sh
curl -fsSL https://otok-dev.github.io/install.sh | sh                   # Linux: the static binary, any distribution
```

Building otok from this source is described in [Install otok](https://otok-dev.github.io/start/install/#build-from-source).

## Contributing

otok takes no pull requests: please report problems and proposals as issues ([CONTRIBUTING.md](CONTRIBUTING.md)).
Recipes are published to the [index](https://github.com/otok-dev/index) by pull requests, which `otok publish` makes.
If otok saves you time, [sponsoring](https://github.com/sponsors/apotocki) keeps it going.

## License

otok is licensed under the GNU General Public License, version 3 only ([LICENSE](LICENSE)), with the Otok Output
Exception ([LICENSE-EXCEPTION](LICENSE-EXCEPTION)): otok's own material in what it generates may be used under
terms of your choice, while the libraries it builds, the recipes it runs and other third-party material keep their
own terms. `otok licenses` lists the third-party software built into otok. The format specifications in
[docs/formats](docs/formats/) are licensed under CC BY 4.0.
