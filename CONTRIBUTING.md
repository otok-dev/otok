# Contributing to otok

Otok is open source, but it does not take contributions of code: **pull requests are not merged**. Every
change to otok is written by its maintainer. This keeps otok's own code in one copyright holder's hands,
which its licensing depends on (its third-party components keep their own holders and licenses).

## Reporting bugs and proposing changes

Open an issue. What helps most:

- the version of otok (`otok --version`) and the system it runs on;
- what you did, what you expected and what happened, with the messages otok printed (`-v` or `-vv` for
  more; `otok log <library>` for the log of a failed build);
- the smallest project that shows it: its `otok.toml`, and the recipe when the problem is in one;
- for a proposal, what you need and why, rather than how to build it.

Code in an issue or a pull request is read as an illustration of the problem only. A fix is written anew
from the description; contributed code is not copied into otok, and a pull request is closed with a pointer
to this file. Thank you for reporting: it is the most useful contribution there is.

## Recipes

Recipes are not part of otok. You publish them in an index of otok, from a repository of your own and under
a license you choose, on the terms of that index (`otok publish`; the public index states its terms in its
`CONTRIBUTING.md`). Nothing is signed for that.

## License

Otok is licensed under the GNU General Public License, version 3 only, with the Otok Output Exception
([LICENSE](LICENSE), [LICENSE-EXCEPTION](LICENSE-EXCEPTION)): otok's own material in what it generates may
be used under terms of your choice, while the libraries it builds, the recipes it runs and other third-party
material keep their own terms.
