# otok

otok is a dependency manager for native libraries: it builds each library from source on your machine, with
your options and for your platforms, and hands the result to your build — XCFrameworks and Swift packages on
Apple platforms, libraries and CMake packages elsewhere.

otok is being prepared for its first release; documentation will follow.

otok takes no pull requests: please report problems and proposals as issues ([CONTRIBUTING.md](CONTRIBUTING.md)).

## License

otok is licensed under the GNU General Public License, version 3 only ([LICENSE](LICENSE)), with the Otok Output
Exception ([LICENSE-EXCEPTION](LICENSE-EXCEPTION)): otok's own material in what it generates may be used under
terms of your choice, while the libraries it builds, the recipes it runs and other third-party material keep their
own terms. `otok licenses` lists the third-party software built into otok.
