// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
// Copied into an index's catalog by `otok index-catalog`, this file is otok's Output: it may be used under terms
// of your choice (LICENSE-EXCEPTION, https://github.com/otok-dev/otok).
//
// The catalog site of an otok index (docs/PLAN.md decision 49): search on the front page (MiniSearch over
// search.json) and the diffs of recipe files on the pages of versions (jsdiff over files/<sha256>). Every text
// of a recipe reaches the page through textContent, never as markup.
(function () {
  'use strict';

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }

  // the front page: the versions published last, the newest of all or those of the target chosen
  const recent = document.getElementById('recent');
  const chosen = document.getElementById('target');
  if (recent && chosen) {
    const rows = Array.prototype.slice.call(recent.rows);
    rows.forEach(function (row) { row.dataset.newest = row.hidden ? '' : '1'; });
    const show = function () {
      const only = chosen.value;
      rows.forEach(function (row) { row.hidden = only ? row.dataset.target !== only : !row.dataset.newest; });
    };
    chosen.addEventListener('change', show);
    show();  // a choice the browser kept when going back
  }

  const box = document.getElementById('search');
  if (box && window.MiniSearch) {
    const root = box.dataset.root;
    const results = document.getElementById('results');
    const idle = document.getElementById('idle');
    // the recipes of one target only (docs/PLAN.md decision 82)
    const target = document.getElementById('target');
    let index = null;

    const run = function () {
      const query = box.value.trim();
      results.replaceChildren();
      idle.hidden = query !== '';
      if (!query || !index) return;
      const only = target ? target.value : '';
      const found = index.search(query, only ? { filter: function (r) { return (r.targets || []).indexOf(only) >= 0; } } : undefined).slice(0, 50);
      if (!found.length) {
        results.append(el('p', 'meta', 'Nothing found.'));
        return;
      }
      const list = el('ul', 'list');
      for (const r of found) {
        const item = el('li');
        // the newest version of each target, of the chosen one only when filtered
        const variants = (r.variants || []).filter(function (v) { return !only || v.target === only; });
        const name = el('a', 'name', r.id);
        name.href = root + (variants.length === 1 ? variants[0].href : 'recipes/' + r.id + '/');
        const library = el('a', null, r.library);
        library.href = root + 'libraries/' + r.library + '/';
        const meta = el('span', 'meta', 'builds ');
        meta.append(library);
        const versions = el('span', 'version');
        variants.forEach(function (v, i) {
          if (i) versions.append(' · ');
          const link = el('a', null, v.target + ' ' + v.version);
          link.href = root + v.href;
          versions.append(link);
        });
        item.append(name, ' ', meta, ' ', versions, el('p', null, r.description));
        list.append(item);
      }
      results.append(list);
    };

    // `?q=` searches at once: a search can be linked to
    const asked = new URLSearchParams(window.location.search).get('q');
    if (asked) box.value = asked;
    box.hidden = false;
    box.addEventListener('input', run);
    if (target) {
      target.hidden = false;
      target.addEventListener('change', run);
    }
    fetch(root + 'search.json')
      .then(function (r) { return r.json(); })
      .then(function (items) {
        index = new MiniSearch({
          fields: ['id', 'library', 'description', 'keywords', 'platforms', 'targets'],
          storeFields: ['id', 'library', 'description', 'targets', 'variants'],
          // arrays as words for the index only: extractField would store `targets` as a string too, which the results
          // and the filter read as an array
          stringifyField: function (value) {
            return Array.isArray(value) ? value.join(' ') : String(value);
          },
          // typos forgiven in long words only: one edit makes "lzm" "lz4"
          searchOptions: {
            prefix: true,
            fuzzy: function (term) { return term.length >= 5 ? 0.2 : false; },
            boost: { library: 3, id: 2 }
          }
        });
        index.addAll(items);
        run();
      })
      .catch(function (err) {
        results.replaceChildren(el('p', 'notice bad', 'Cannot load the search index: ' + err.message));
      });
  }

  // the page of a version
  const diff = document.getElementById('diff');
  if (diff && window.Diff) {
    const root = diff.dataset.root;
    const current = diff.dataset.current;
    const texts = new Map();
    const text = function (sha) {
      if (!texts.has(sha)) {
        texts.set(sha, fetch(root + 'files/' + sha + '.txt').then(function (r) {
          if (!r.ok) throw new Error('HTTP status ' + r.status);
          return r.text();
        }));
      }
      return texts.get(sha);
    };

    const render = function (base, self, out) {
      out.replaceChildren(el('p', 'meta', 'Loading the files…'));
      const paths = Array.from(new Set(Object.keys(base.files).concat(Object.keys(self.files)))).sort();
      Promise.all(paths.map(function (path) {
        const from = base.files[path];
        const to = self.files[path];
        if (from === to) return null;
        return Promise.all([from ? text(from) : '', to ? text(to) : '']).then(function (pair) {
          return {
            path: path,
            change: !from ? 'added' : !to ? 'removed' : 'changed',
            patch: Diff.structuredPatch(path, path, pair[0], pair[1], base.label, self.label, { context: 3 })
          };
        });
      })).then(function (list) {
        out.replaceChildren();
        const changed = list.filter(Boolean);
        if (!changed.length) out.append(el('p', 'meta', 'The recipe files are the same as in ' + base.label + '.'));
        for (const c of changed) {
          const file = el('details', 'file');
          file.open = true;
          const summary = el('summary');
          summary.append(el('code', null, c.path), ' ', el('span', 'meta', c.change));
          const pre = el('pre', 'diff');
          for (const h of c.patch.hunks) {
            // a line a block (style.css), so its background spans the width
            pre.append(el('span', 'hunk', '@@ -' + h.oldStart + ',' + h.oldLines + ' +' + h.newStart + ',' + h.newLines + ' @@'));
            for (const line of h.lines) {
              const kind = line[0] === '+' ? 'add' : line[0] === '-' ? 'del' : line[0] === '\\' ? 'note' : null;
              pre.append(el('span', kind, line));
            }
          }
          file.append(summary, pre);
          out.append(file);
        }
        const same = paths.length - changed.length;
        if (same && changed.length) out.append(el('p', 'meta', same + (same === 1 ? ' other file is' : ' other files are') + ' the same.'));
      }).catch(function (err) {
        out.replaceChildren(el('p', 'notice bad', 'Cannot load the recipe files: ' + err.message));
      });
    };

    fetch(diff.dataset.entries)
      .then(function (r) { return r.json(); })
      .then(function (data) {
        const self = data.entries.find(function (e) { return e.dir === current; });
        const others = data.entries.filter(function (e) { return e.dir !== current && e.files; });
        if (!self || !self.files || !others.length) return;
        const select = el('select');
        for (const e of others) {
          const option = el('option', null, e.label);
          option.value = e.dir;
          select.append(option);
        }
        const preferred = others.find(function (e) { return e.dir === diff.dataset.base; });
        select.value = (preferred || others[0]).dir;
        const label = el('label', null, 'Recipe files compared with ');
        label.append(select);
        const out = el('div');
        diff.replaceChildren(label, out);
        const show = function () {
          render(others.find(function (e) { return e.dir === select.value; }), self, out);
        };
        select.addEventListener('change', show);
        show();
      })
      .catch(function (err) {
        diff.append(el('p', 'notice bad', 'Cannot load the versions of the recipe: ' + err.message));
      });
  }
})();
