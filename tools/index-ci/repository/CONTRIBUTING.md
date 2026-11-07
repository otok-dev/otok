# Publishing in this index

This repository is an index of otok recipes (`docs/formats/index.md` of otok). Recipes are published into it
with `otok publish`, which opens a pull request adding their entries; its CI checks each pull request, and a
maintainer reviews those of publishers that are not established yet.

## Terms of publication

By opening a pull request that adds or changes files of this repository, you agree to the following. Nothing
has to be signed or written into a recipe: the pull request is the agreement, as GitHub's Terms of Service
(section D.6) make contributions to a repository subject to its license.

1. **The index's data is public domain.** What your pull request adds or changes here (version entries,
   library files, publisher files, recipe status, advisories) is dedicated to the public domain under
   CC0 1.0 Universal ([LICENSE](LICENSE)). An entry holds the description, options and dependencies of the
   recipe as `recipe.toml` states them, so that otok can resolve without fetching the recipe.
2. **The recipe stays yours.** A recipe is not copied into this repository: an entry names its tree (a git
   repository, a commit and a directory), which stays under the license you give it. You allow this index,
   its HTTPS view and its catalog site to fetch the files of every recipe tree your entries name, keep them,
   show them, show the differences between its versions, and redistribute them unchanged, for as long as the
   entries are in this index. Users of otok fetch and run the recipe under its own license.
3. **Publish what you may.** Publish only recipe trees for which you can give the permission of item 2: your
   own, or ones whose license allows their redistribution.
4. **Entries are permanent.** A merged entry is never edited or deleted, and the history of this repository
   keeps it; a version is withdrawn by an advisory, not removed. Publish a new revision to change a recipe.

These terms cover the recipes and the data of this index only. otok itself is licensed separately (the GNU
General Public License, version 3 only, with the Otok Output Exception), and the libraries recipes build are
under their own licenses.

## Who may publish

A publisher is a name under `publishers/`, registered with the first pull request publishing under it and
allowed by a maintainer who merges it. Pull requests of a publisher wait for a maintainer until the publisher is
established; the limits of entries per day are in `index.toml`. See `docs/formats/index.md` 9 of otok.
