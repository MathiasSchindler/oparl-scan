import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, readdir, realpath, rm, utimes, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const binary = process.argv[2];
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-incremental-')));
const schema = 'https://schema.oparl.org/1.1/';
const old = '2000-01-01T00:00:00+01:00';
const hash = text => createHash('sha256').update(text).digest('hex');
let origin, objects, fileBodies, versions, requests, mode = '';
const id = path => `${origin}/oparl/${path}`;
const recent = () => new Date(Date.now() - 10000).toISOString();
const payload = number => Buffer.from(`%PDF-1.4\nfile ${number}, version ${versions.get(number)}\n%%EOF\n`);
const object = (path, type, fields = {}) => ({ id: id(path), type: `${schema}${type}`, modified: old, ...fields });
function addFile(number, body = 1) {
  versions.set(number, 1);
  fileBodies.set(number, body);
  objects.set(`files/${number}`, object(`files/${number}`, 'File', {
    fileName: `${number}.pdf`, size: payload(number).length, accessUrl: `${origin}/content/${number}`,
  }));
}
function change(path, fields = {}) {
  Object.assign(objects.get(path), { modified: recent() }, fields);
}
function changeFile(number) {
  versions.set(number, versions.get(number) + 1);
  change(`files/${number}`);
}
function reset() {
  mode = ''; objects = new Map(); fileBodies = new Map(); versions = new Map();
  for (const number of [1, 4, 5, 99]) addFile(number);
  addFile(3, 3);
  objects.set('papers/1', object('papers/1', 'Paper', {
    reference: '26/SVV/0001', mainFile: id('files/1'), auxiliaryFile: [id('files/3')],
    relatedPaper: [id('papers/4')], originator: [id('organizations/1')],
  }));
  objects.set('papers/4', object('papers/4', 'Paper', {
    reference: '25/SVV/0004', mainFile: id('files/4'), consultation: id('consultations/4'),
  }));
  objects.set('papers/9', object('papers/9', 'Paper', { reference: '25/SVV/0009', mainFile: id('files/99') }));
  objects.set('meetings/1', object('meetings/1', 'Meeting', {
    start: '2026-06-01T10:00:00+02:00',
    agendaItem: [
      { id: id('agendaitems/1'), type: `${schema}AgendaItem`, public: true },
      { id: id('agendaitems/99'), type: `${schema}AgendaItem`, public: false },
    ],
  }));
  objects.set('meetings/99', object('meetings/99', 'Meeting', { start: '2025-01-01T10:00:00Z' }));
  objects.set('meetings/4', object('meetings/4', 'Meeting', {
    start: '2025-12-01T10:00:00Z', resultsProtocol: id('files/4'),
  }));
  objects.set('agendaitems/1', object('agendaitems/1', 'AgendaItem', {
    public: true, meeting: id('meetings/1'), resolutionFile: id('files/5'), consultation: id('consultations/1'),
  }));
  objects.set('agendaitems/99', object('agendaitems/99', 'AgendaItem', {
    public: false, meeting: id('meetings/1'), resolutionFile: id('files/99'),
  }));
  objects.set('consultations/1', object('consultations/1', 'Consultation', {
    paper: id('papers/9'), organization: id('organizations/1'),
  }));
  objects.set('consultations/4', object('consultations/4', 'Consultation', {
    paper: id('papers/4'), meeting: id('meetings/4'),
  }));
  objects.set('organizations/1', object('organizations/1', 'Organization', { name: 'Test council' }));
}
const collections = { papers: 'paper', files: 'file', meetings: 'meeting',
  agendaitems: 'agendaItem', consultations: 'consultation', organizations: 'organization' };
const server = createServer((req, res) => {
  const url = new URL(req.url, origin);
  requests.push({ path: url.pathname, method: req.method, query: Object.fromEntries(url.searchParams) });
  if (url.pathname.startsWith('/content/')) {
    const number = Number(url.pathname.split('/').at(-1));
    if (mode === 'denied' && number === 1) { res.writeHead(403); res.end(); return; }
    const etag = `"${number}-${versions.get(number)}"`;
    if (req.headers['if-none-match'] === etag) {
      res.writeHead(304, { ETag: etag }); res.end(); return;
    }
    const bytes = payload(number);
    res.writeHead(200, { ETag: etag, 'Content-Length': bytes.length });
    res.end(req.method === 'HEAD' ? undefined : bytes);
    return;
  }
  res.setHeader('Content-Type', 'application/json');
  if (url.pathname === '/oparl/system') {
    res.end(JSON.stringify(object('system', 'System', { body: id('bodies') }))); return;
  }
  let rows;
  const match = url.pathname.match(/^\/oparl\/bodies\/([13])\/([a-z]+)$/);
  if (url.pathname === '/oparl/bodies') {
    rows = [1, 3].map(body => object(`bodies/${body}`, 'Body',
      Object.fromEntries(Object.entries(collections).map(([path, key]) => [key, id(`bodies/${body}/${path}`)]))));
  } else if (match) {
    const [, body, collection] = match;
    rows = [...objects].filter(([key]) => key.startsWith(`${collection}/`) &&
      Number(body) === (collection === 'files' ? fileBodies.get(Number(key.split('/')[1])) : 1))
      .map(([, value]) => value);
    if (url.searchParams.has('modified_since') && mode !== 'ignored-filter') {
      const since = Date.parse(url.searchParams.get('modified_since'));
      const until = Date.parse(url.searchParams.get('modified_until'));
      rows = rows.filter(row =>
        (mode === 'bad-timestamp' && row.id === id('files/1')) ||
        (Date.parse(row.modified) >= since && Date.parse(row.modified) <= until));
    }
  } else {
    const resource = objects.get(url.pathname.replace('/oparl/', ''));
    if (!resource) { res.writeHead(404); res.end(); return; }
    res.end(JSON.stringify(resource)); return;
  }
  const size = Number(url.searchParams.get('size') ?? 2);
  const currentPage = Number(url.searchParams.get('page') ?? 1);
  const totalPages = Math.ceil(rows.length / size);
  res.end(JSON.stringify({
    data: rows.slice((currentPage - 1) * size, currentPage * size),
    pagination: { currentPage, totalPages, totalElements: rows.length },
    links: currentPage < totalPages ? { next: `${origin}${url.pathname}?page=${currentPage + 1}` } : {},
  }));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
origin = `http://127.0.0.1:${server.address().port}`;
const manifest = async directory => JSON.parse(await readFile(join(directory, 'archive.json'), 'utf8'));
const metadata = async (directory, path) =>
  JSON.parse(await readFile(join(directory, 'metadata', `${hash(id(path))}.json`), 'utf8'));
async function document(directory, number) {
  const names = await readdir(join(directory, 'documents'));
  const name = names.find(name => name.startsWith(`${hash(id(`files/${number}`))}-`));
  assert.ok(name, `missing archived file ${number}`);
  return readFile(join(directory, 'documents', name));
}
const requested = path => requests.filter(request => request.path === path);
async function scan(directory, args = [], expected = 0) {
  requests = [];
  const child = spawn(binary, ['--system', id('system'), '--output', directory, '--year', '2026',
    '--page-size', '2', '--delay-ms', '0', '--timeout-ms', '1000', '--color', 'never', ...args],
  { stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', chunk => { stderr += chunk; });
  const timer = setTimeout(() => child.kill('SIGKILL'), 20000);
  try {
    const code = await new Promise((resolve, reject) => {
      child.once('error', reject); child.once('close', resolve);
    });
    assert.equal(code, expected, stderr);
  } finally { clearTimeout(timer); }
  return { ...await manifest(directory), stderr };
}
async function rewind(directory) {
  const state = await manifest(directory);
  state.syncCheckpointEpochSeconds -= 3600;
  await writeFile(join(directory, 'archive.json'), JSON.stringify(state));
  return state.syncCheckpointEpochSeconds;
}
try {
  reset();
  const directory = join(root, 'default');
  const first = await scan(directory);
  assert.equal(first.syncMode, 'full');
  assert.equal(first.syncCheckpointEpochSeconds, first.runStartedAtEpochSeconds);
  assert.equal(first.complete, true);
  assert.equal(first.newDocuments, 4);
  assert.equal(requested('/oparl/papers/9').length, 0, 'calendar does not expand foreign Papers');
  const initialSelection = await readFile(join(directory, 'selection.jsonl'), 'utf8');
  const futureTime = Math.floor(Date.now() / 1000) + 3600;
  await utimes(join(directory, 'metadata', `${hash(id('files/1'))}.json`), futureTime, futureTime);

  const empty = await scan(directory);
  assert.equal(empty.syncMode, 'incremental');
  assert.equal(empty.newDocuments, 0);
  assert.equal(requests.filter(request => request.path.startsWith('/content/')).length, 0);
  assert.equal(requested('/oparl/papers/1').length, 0, 'unchanged graph is not fetched again');
  assert.equal(await readFile(join(directory, 'selection.jsonl'), 'utf8'), initialSelection);
  for (const request of requests.filter(request => /^\/oparl\/bodies\/[13]\//.test(request.path))) {
    assert.ok(request.query.modified_since);
    assert.ok(request.query.modified_until);
  }

  changeFile(1); changeFile(3); changeFile(4);
  objects.get('files/3').modified = new Date(Date.now() - 10000 + 3600000).toISOString().replace('Z', '+01:00');
  objects.get('files/4').modified = new Date(Date.now() - 10000 - 9000000).toISOString().replace('Z', '-02:30');
  const before = await document(directory, 1);
  const filesOnly = await scan(directory, ['--page-size', '1']);
  assert.equal(filesOnly.newDocuments, 3);
  assert.notDeepEqual(await document(directory, 1), before, 'equal-sized PDF changes are replaced');
  assert.deepEqual(await document(directory, 3), payload(3), 'Body 3 File updates need no Paper update');
  assert.deepEqual(await document(directory, 4), payload(4), 'known context outside the reference year updates');
  assert.equal(requested('/oparl/papers/1').length, 0);
  assert.ok(requests.some(request => request.query.page === '2' && request.query.modified_since),
    'pagination restores the filters omitted by links.next');

  for (const number of [2, 6, 7, 8]) { addFile(number); change(`files/${number}`); }
  change('files/99');
  change('meetings/4', { resultsProtocol: id('files/8') });
  change('agendaitems/99');
  change('papers/9');
  objects.set('papers/2', object('papers/2', 'Paper', {
    reference: '26/SVV/0002', modified: recent(), mainFile: id('files/2'),
  }));
  objects.set('agendaitems/2', object('agendaitems/2', 'AgendaItem', {
    public: true, modified: recent(), meeting: id('meetings/1'), resolutionFile: id('files/6'),
  }));
  objects.set('agendaitems/4', object('agendaitems/4', 'AgendaItem', {
    public: true, modified: recent(), meeting: id('meetings/4'), consultation: id('consultations/4'),
    resolutionFile: id('files/7'),
  }));
  const additions = await scan(directory);
  assert.equal(additions.newDocuments, 4);
  assert.deepEqual(await document(directory, 6), payload(6), 'new TOP works even without a modified Meeting');
  assert.deepEqual(await document(directory, 7), payload(7), 'new TOP retains Paper context outside the calendar');
  assert.deepEqual(await document(directory, 8), payload(8), 'changed context Meetings discover new attachments');
  assert.equal(requested('/oparl/meetings/1').length, 0);
  assert.equal(requested('/oparl/papers/9').length, 0);
  assert.equal(requested('/content/99').length, 0, 'unrelated/private files stay outside the archive');
  await assert.rejects(metadata(directory, 'files/99'), { code: 'ENOENT' });
  const selection = (await readFile(join(directory, 'selection.jsonl'), 'utf8')).trim().split('\n').map(JSON.parse);
  assert.deepEqual(selection.map(row => row.id).sort(), [id('papers/1'), id('papers/2')]);
  await scan(directory);
  assert.equal((await readFile(join(directory, 'selection.jsonl'), 'utf8')).trim().split('\n').length, 2);

  let checkpoint = await rewind(directory);
  const limited = await scan(directory, ['--max-pages', '1'], 2);
  assert.equal(limited.syncCheckpointEpochSeconds, checkpoint);
  assert.equal(limited.complete, false);
  assert.equal((await scan(directory)).complete, true);

  checkpoint = await rewind(directory);
  changeFile(1); mode = 'denied';
  const failed = await scan(directory, ['--no-skip-unavailable-files'], 1);
  assert.equal(failed.syncCheckpointEpochSeconds, checkpoint);
  const unavailable = await scan(directory, [], 3);
  assert.equal(unavailable.state, 'incomplete');
  assert.equal(unavailable.unavailableDocuments, 1);
  assert.equal(unavailable.syncCheckpointEpochSeconds, checkpoint);
  mode = '';
  assert.equal((await scan(directory)).complete, true);
  assert.deepEqual(await document(directory, 1), payload(1));

  checkpoint = await rewind(directory);
  mode = 'ignored-filter';
  const ignored = await scan(directory, [], 1);
  assert.match(ignored.stderr, /filter ignored or invalid timestamp/);
  assert.equal(ignored.syncCheckpointEpochSeconds, checkpoint);
  mode = 'bad-timestamp';
  const originalModified = objects.get('files/1').modified;
  for (const invalid of ['2026-02-31T10:00:00Z', '2026-01-01T25:00:00Z', null,
    '2099-01-01T00:00:00Z']) {
    objects.get('files/1').modified = invalid;
    const bad = await scan(directory, [], 1);
    assert.equal(bad.syncCheckpointEpochSeconds, checkpoint);
    assert.match(bad.stderr, /filter ignored or invalid timestamp/);
  }
  objects.get('files/1').modified = originalModified;
  mode = '';
  await scan(directory);

  objects.set('papers/4', object('papers/4', 'Paper', { deleted: true, modified: recent() }));
  objects.set('files/4', object('files/4', 'File', { deleted: true, modified: recent() }));
  await scan(directory);
  assert.equal((await metadata(directory, 'papers/4')).deleted, true);
  assert.equal((await metadata(directory, 'files/4')).deleted, true);
  assert.deepEqual(await document(directory, 4), payload(4), 'tombstones retain historical bytes');
  assert.ok((await readdir(join(directory, 'history', 'metadata', hash(id('files/4'))))).length >= 2);

  reset();
  const legacy = join(root, 'legacy');
  await scan(legacy);
  const legacyState = await manifest(legacy);
  for (const key of Object.keys(legacyState)) if (key.startsWith('sync')) delete legacyState[key];
  legacyState.complete = false;
  await writeFile(join(legacy, 'archive.json'), JSON.stringify(legacyState));
  const localTime = Math.floor(Date.now() / 1000) - 86400;
  for (const name of await readdir(join(legacy, 'metadata')))
    await utimes(join(legacy, 'metadata', name), localTime, localTime);
  changeFile(1);
  const adopted = await scan(legacy, [], 3);
  assert.equal(adopted.syncMode, 'incremental');
  assert.equal(adopted.syncSinceEpochSeconds, localTime - 300);
  assert.equal(adopted.syncBaselineComplete, false);
  assert.equal(adopted.complete, false, 'a successful delta cannot heal old gaps');
  assert.equal(adopted.syncCheckpointEpochSeconds, adopted.runStartedAtEpochSeconds);
  assert.match(adopted.stderr, /oldest local metadata timestamp/);
  assert.equal(await readFile(join(legacy, 'selection.jsonl'), 'utf8'), initialSelection);
  assert.equal((await scan(legacy, [], 3)).syncBaselineComplete, false);
  assert.equal((await scan(legacy, ['--full'])).complete, true);
  assert.ok(requested('/oparl/papers/1').length, '--full walks the known graph');
  const refreshed = await scan(legacy, ['--refresh']);
  assert.equal(refreshed.syncMode, 'full');
  assert.equal(refreshed.newDocuments, 4, '--refresh still forces content transfers');

  reset();
  const metadataOnly = join(root, 'metadata-only');
  assert.equal((await scan(metadataOnly, ['--metadata-only'])).newDocuments, 0);
  const backfill = await scan(metadataOnly);
  assert.equal(backfill.syncMode, 'full');
  assert.equal(backfill.newDocuments, 4, 'switching modes backfills unchanged binary files');

  reset(); mode = 'denied';
  const interrupted = join(root, 'interrupted-initial');
  assert.equal((await scan(interrupted, [], 3)).syncCheckpointEpochSeconds, 0);
  mode = '';
  assert.equal((await scan(interrupted)).syncMode, 'full', 'failed initial archives are not legacy bootstraps');
  console.log('Incremental synchronization tests passed');
} finally {
  await new Promise(resolve => server.close(resolve));
  await rm(root, { recursive: true, force: true });
}
