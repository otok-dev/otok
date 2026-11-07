# Workflows of an otok index

The GitHub Actions workflows of an index repository (`docs/formats/index.md` 9, 11; `docs/PLAN.md`
decision 47). An index maintainer copies them to `.github/workflows/` of the index.

- `check.yml`: checks every pull request with `otok index-check` and merges those with the verdict
  `auto` (new entries of established publishers, or of a maintainer: `index.md` 9.1). The others wait for a maintainer,
  labelled `ready-for-review` (`refused` the refused ones), the label being the maintainers' queue; a maintainer
  approves one by adding the label `approved`, and the workflow checks it again and merges it. Either way it
  comments in the pull request with the verdict and its reasons, replacing its comment of an earlier check. It
  merges with a merge commit of its own, pushed to the default branch, whose new entries carry the time of the merge
  (`published`, `index.md` 3): maintainers never merge with GitHub's button.
- `probe.yml`: weekly, `otok index-probe` writes `availability.toml`.
- `view.yml`: on every push, `otok index-view` generates the HTTPS view (`index.md` 8) and `otok index-catalog`
  the catalog site beside it (DESIGN 8), and GitHub Pages serves them (the repository's Pages source set to
  "GitHub Actions").

`update-view.sh` serves the view from a web server of one's own instead: it fetches the index repository
and regenerates the view and the catalog site (`otok index-catalog`, `docs/PLAN.md` decision 49) into the web
root when the commit moved, one run at a time. Both commands rewrite only the files whose contents changed, so
the `ETag`s servers derive from file times stay valid. The catalog fetches the recipe trees with the machine's
git credentials, not with the token of the index: a tree it cannot fetch is shown as unavailable, so what a
private repository holds stays out of the site.

The web server serves `<dir>/index.html` for the pages' directory URLs and redirects a directory URL without its
final `/` to the one with it, since the pages link relatively (nginx: `try_files $uri $uri/ =404;` with the default
`index index.html`)
and the recipe files, `/files/<sha256>.txt`, as UTF-8 plain text that browsers never render (nginx: `charset utf-8;`
and `X-Content-Type-Options: nosniff` for `/files/`). GitHub Pages does all of it by itself.

`view.yml` keeps the recipe trees otok fetched in an actions cache (`actions/cache`, a key per commit restored from
the newest one), so each push fetches only the trees of the new versions.
It is run by the repository's push webhook, with a timer as a fallback for lost deliveries:

- `run-update.sh`: `update-view.sh` with the server's paths and settings, logged.
- `hooks.json`: a hook of [adnanh/webhook](https://github.com/adnanh/webhook) that runs it for a `push` event
  signed with the webhook's secret (`X-Hub-Signature-256`). The request carries nothing the view takes:
  the script fetches the repository itself, so a forged request costs one `git fetch` at most.
- `otok-view-hook.service`: the systemd service of `webhook`, listening on `127.0.0.1:9000`, the secret
  from an environment file (`OTOK_VIEW_HOOK_SECRET`); the web server proxies `/hooks/` to it.
- The repository's webhook: payload URL `https://<view host>/hooks/otok-view`, content type JSON, the
  secret, push events only.

The paths in these files are examples, for a user `otok` of the server; adjust them.

`repository/` holds the files of the index repository's root: `LICENSE`, CC0 1.0 for the data of the index, and
`CONTRIBUTING.md`, the terms a publisher agrees to by opening a pull request (`docs/formats/index.md` 9.3); copy
them when creating the index, adjusting the name.

The workflows take otok from the index's own release named `otok`, asset `otok-linux-x64`, never from a pull
request. The release holds a Linux x86-64 build of otok that the maintainers have checked; the jobs run
in a container of the system it was built on (`ubuntu:26.04`), since it needs that C library.

The repository's settings: Actions may write (the workflows ask for `contents: write` and
`pull-requests: write` themselves); the labels `approved`, `ready-for-review` and `refused` exist; the default branch
is protected so that only GitHub Actions (and administrators) push to it, which `check.yml` and `probe.yml` do.
