import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm, readdir } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { build, renderMarkdown } from './build.mjs';

test('Markdown supports tables, safe HTML, stable unique anchors, and document links', () => {
  const {html, headings} = renderMarkdown('## Setup\n\n## Setup\n\n[Notes](CHANGELOG.md#100---2026-01-01)\n\n[Link mode](docs/link-mode.md)\n\n| Pin | Use |\n| --- | --- |\n| 3 | Touch |\n\n<script>alert(1)</script>\n\n[bad](javascript:alert%281%29)\n\n- [x] Done', 'README.md', new Set(['README.md','CHANGELOG.md','docs/link-mode.md']));
  assert.deepEqual(headings.map(h => h.id), ['setup','setup-1']);
  assert.match(html, /href="changelog.html#100---2026-01-01"/);
  assert.match(html, /href="docs\/link-mode.html"/);
  assert.match(html, /<table>/);
  assert.match(html, /class="table-scroll"/);
  assert.doesNotMatch(html, /<script|javascript:/);
  assert.match(html, /disabled/);
});

test('Changing only source Markdown updates pages, release summary and navigation', async () => {
  const root = await mkdtemp(path.join(os.tmpdir(),'awok-site-test-'));
  try {
    const out = path.join(root,'output');
    await mkdir(path.join(root,'docs'));
    await writeFile(path.join(root,'README.md'),'# AWOKxDAG\n\n**Firmware guide** for ESP32-C5.\n\n- **Version:** 1.0.0\n\n## Features\n\nOriginal content.\n\n[Changes](CHANGELOG.md)\n\n[Details](docs/link-mode.md)');
    await writeFile(path.join(root,'CHANGELOG.md'),'# Changelog\n\n## [Unreleased]\n\nFuture feature.\n\n## [1.0.0] - 2026-01-01\n\nInitial release.');
    await writeFile(path.join(root,'docs/link-mode.md'),'# Link mode\n\n## Pairing\n\n[Guide](../README.md#features)');
    await build(root,out);
    let html = await readFile(path.join(out,'index.html'),'utf8');
    assert.match(html,/release-version">v1.0.0/);
    assert.match(html,/README v1.0.0/);
    assert.match(await readFile(path.join(out,'changelog.html'),'utf8'),/CHANGELOG v1.0.0/);
    assert.match(await readFile(path.join(out,'docs/link-mode.html'),'utf8'),/DOCS v1.0.0/);
    assert.doesNotMatch(html,/release-version">vUnreleased/);
    assert.match(html,/href="https:\/\/espterminator.com\/"/);
    assert.match(await readFile(path.join(out,'docs/link-mode.html'),'utf8'),/href="..\/index.html#features"/);
    await writeFile(path.join(root,'README.md'),'# AWOKxDAG\n\nUpdated description.\n\n- **Version:** 2.0.0\n\n## New section\n\nNew source content.');
    await writeFile(path.join(root,'CHANGELOG.md'),'# Changelog\n\n## [Unreleased]\n\nFuture feature.\n\n## [2.0.0] - 2026-02-01\n\nNew release.');
    await build(root,out);
    html = await readFile(path.join(out,'index.html'),'utf8');
    assert.match(html,/release-version">v2.0.0/);
    assert.match(html,/New source content/);
    assert.match(html,/href="#new-section"/);
    assert.doesNotMatch(html,/Original content/);
    // Check every generated local HTML link and fragment, including subpath pages.
    async function check(folder) {
      for (const entry of await readdir(folder,{withFileTypes:true})) {
        const file = path.join(folder,entry.name);
        if (entry.isDirectory()) await check(file);
        else if (file.endsWith('.html')) {
          const page = await readFile(file,'utf8');
          for (const [,href] of page.matchAll(/href="([^"]+)"/g)) {
            if (/^(https?:|mailto:)/.test(href)) continue;
            const [target,fragment] = href.split('#');
            const linked = target ? path.resolve(path.dirname(file),target) : file;
            const content = await readFile(linked,'utf8');
            if (fragment) assert.ok(content.includes(`id="${fragment}"`),`Missing ${href} in ${file}`);
          }
        }
      }
    }
    // Keep the fixture doc reference current after changing the guide heading.
    await writeFile(path.join(root,'docs/link-mode.md'),'# Link mode\n\n## Pairing\n\n[Guide](../README.md#new-section)');
    await build(root,out);
    await check(out);
  } finally { await rm(root,{recursive:true,force:true}); }
});
