import { readFile, writeFile, mkdir, cp, readdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Marked } from 'marked';
import GithubSlugger from 'github-slugger';
import sanitizeHtml from 'sanitize-html';

const here = path.dirname(fileURLToPath(import.meta.url));
const repository = 'https://github.com/dagnazty/awokxdag';
export const escape = (value) => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const plain = (text) => text.replace(/<[^>]*>/g, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1').replace(/[*`]/g, '').replace(/\s+/g, ' ').trim();
export function outputPath(source) {
  if (source === 'README.md') return 'index.html';
  if (source === 'CHANGELOG.md') return 'changelog.html';
  return source.replace(/\.md$/i, '.html');
}
export function renderMarkdown(markdown, source, available) {
  const slugger = new GithubSlugger();
  const headings = [];
  const destination = outputPath(source);
  const resolveLink = (href, image = false) => {
    if (/^(?:[a-z][a-z0-9+.-]*:|\/\/|#)/i.test(href)) return href;
    const [file, suffix = ''] = href.split(/(?=[?#])/s, 2);
    const resolved = path.posix.normalize(path.posix.join(path.posix.dirname(source), file));
    if (available.has(resolved) && !image) return path.posix.relative(path.posix.dirname(destination), outputPath(resolved)) + suffix;
    return `${repository}/${image ? 'raw' : 'blob'}/main/${resolved}${suffix}`;
  };
  const marked = new Marked({gfm: true, renderer: {
    heading({tokens, depth}) {
      const text = this.parser.parseInline(tokens);
      const id = slugger.slug(plain(text));
      if (depth === 2) headings.push({id, text: plain(text)});
      return `<h${depth} id="${escape(id)}">${text}<a class="heading-link" href="#${escape(id)}" aria-label="Link to ${escape(plain(text))}">#</a></h${depth}>\n`;
    },
    link({href, title, tokens}) {
      return `<a href="${escape(resolveLink(href))}"${title ? ` title="${escape(title)}"` : ''}>${this.parser.parseInline(tokens)}</a>`;
    },
    image({href, title, text}) {
      return `<img src="${escape(resolveLink(href, true))}" alt="${escape(text)}"${title ? ` title="${escape(title)}"` : ''} loading="lazy">`;
    },
    table(token) { return `<div class="table-scroll" tabindex="0" role="region" aria-label="Documentation table">${this.constructor.prototype.table.call(this, token)}</div>`; }
  }});
  const html = sanitizeHtml(marked.parse(markdown), {
    allowedTags: sanitizeHtml.defaults.allowedTags.concat(['img', 'input']),
    allowedAttributes: {...sanitizeHtml.defaults.allowedAttributes, '*':['id','class','aria-label','role','tabindex'], a:['href','title','aria-label','class'], img:['src','alt','title','loading'], input:['type','checked','disabled']},
    allowedSchemes: ['http','https','mailto'],
    transformTags: {input: (_tag, attrs) => ({tagName:'input', attribs:{type:'checkbox', disabled:'', ...(Object.hasOwn(attrs,'checked') ? {checked:''} : {})}})}
  });
  return {html, headings};
}

export async function build(root = path.resolve(here, '..'), out = path.join(here, 'dist')) {
  const sources = new Map();
  for (const file of ['README.md', 'CHANGELOG.md']) sources.set(file, await readFile(path.join(root, file), 'utf8'));
  async function loadDocs(folder) {
    let entries;
    try { entries = await readdir(path.join(root, folder), {withFileTypes:true}); } catch (error) { if (error.code === 'ENOENT') return; throw error; }
    for (const entry of entries) {
      const file = `${folder}/${entry.name}`;
      if (entry.isDirectory()) await loadDocs(file);
      else if (entry.name.endsWith('.md')) sources.set(file, await readFile(path.join(root,file),'utf8'));
    }
  }
  await loadDocs('docs');
  const available = new Set(sources.keys());
  const readme = sources.get('README.md');
  const name = plain(readme.match(/^# (.+)$/m)?.[1] ?? 'Firmware');
  const description = plain(readme.match(/^# .+\r?\n\s*\n([\s\S]+?)(?:\r?\n){2}/)?.[1] ?? 'Firmware documentation');
  const version = readme.match(/\*\*Version:\*\*\s*(\S+)/)?.[1];
  const changelog = sources.get('CHANGELOG.md');
  const release = changelog.match(/^## \[([^\]]+)\] - (\d{4}-\d{2}-\d{2})/m);
  if (!version || !release) throw new Error('Expected README **Version:** and a dated ## [version] - YYYY-MM-DD changelog entry.');
  const template = await readFile(path.join(here,'template.html'), 'utf8');
  await mkdir(out, {recursive:true});
  await cp(path.join(here,'style.css'), path.join(out,'style.css'));
  await cp(path.join(here,'public'), out, {recursive:true});
  for (const [source, markdown] of sources) {
    const home = source === 'README.md';
    const changes = source === 'CHANGELOG.md';
    const title = home ? 'Firmware guide' : changes ? 'Changelog' : plain(markdown.match(/^# (.+)$/m)?.[1] ?? source);
    // Only the document title moves into the page header; all document body content is preserved.
    const body = markdown.replace(/^# .+\r?\n/, '');
    const {html, headings} = renderMarkdown(body, source, available);
    const prefix = '../'.repeat(outputPath(source).split('/').length - 1);
    const toc = headings.map(h => `<a href="#${escape(h.id)}">${escape(h.text)}</a>`).join('');
    const values = {
      NAME:escape(name), TITLE:escape(title), DESCRIPTION:escape(description), VERSION:escape(version), PREFIX:prefix,
      PAGE_TITLE:escape(home ? `${name} — Firmware guide` : `${title} — ${name}`),
      DOCUMENT_LABEL:escape(home ? 'README' : changes ? 'CHANGELOG' : 'DOCS'),
      REPOSITORY:repository, HOME_CURRENT:home ? 'aria-current="page"' : '', CHANGELOG_CURRENT:changes ? 'aria-current="page"' : '',
      INTRO:home ? escape(description) : changes ? 'Release notes, improvements, and fixes.' : 'Technical documentation',
      EYEBROW:home ? 'FIRMWARE / DOCUMENTATION' : changes ? 'FIRMWARE / RELEASE HISTORY' : 'FIRMWARE / TECHNICAL NOTES',
      HERO_TITLE:home ? escape(name) : escape(title), BODY:html, TOC:toc,
      RELEASE:escape(release[1]), DATE:escape(release[2]), SOURCE_URL:`${repository}/blob/main/${source}`,
      RELEASE_ANCHOR:new GithubSlugger().slug(`${release[1]} - ${release[2]}`)
    };
    const page = template.replace(/\{\{([A-Z_]+)\}\}/g, (_match, key) => {if (!(key in values)) throw new Error(`Unknown template key ${key}`);return values[key];});
    const output = path.join(out, outputPath(source));
    await mkdir(path.dirname(output),{recursive:true});
    await writeFile(output,page);
  }
  await writeFile(path.join(out,'.nojekyll'),'');
  console.log(`Built ${sources.size} pages from README.md, CHANGELOG.md, and docs/ → ${out}`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await build();
