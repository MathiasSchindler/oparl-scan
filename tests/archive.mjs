import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn, spawnSync } from 'node:child_process';
import { cp, mkdtemp, readFile, readdir, realpath, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { pdf } from './pdf-fixture.mjs';

const [scanner, indexer, searcher] = process.argv.slice(2).map(resolvePath => resolve(resolvePath));
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-archive-')));
const archive = join(root, 'archive'), index = join(root, 'snapshot.ops');
const schema = 'https://schema.oparl.org/1.1/';
const hash = text => createHash('sha256').update(text).digest('hex');
let base, revision = 1, mode = 'normal';
const requests = [];
const id = path => `${base}/${path}`;
function document() {
  const content = `BT /F1 12 Tf 20 100 Td (Version${revision === 1 ? 'Alpha' : 'Bravo'}) Tj ET`;
  return pdf([null, '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
    `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']);
}
const file = n => ({ id: id(`files/${n}`), type: schema + 'File', size: document().length,
  fileName: `${revision === 1 ? 'original' : 'renamed'}-${n}.pdf`, accessUrl: id(`download/${n}`) });
const agenda = n => ({ id: id(`agendaItems/${n}`), type: schema + 'AgendaItem',
  name: `Oeffentlicher TOP ${n}`, public: true, meeting: id('meetings/1'),
  resolutionFile: n === 1 && revision === 1 ? id('files/1') : file(n), consultation: id('consultations/1') });
const meeting = n => ({
  id: id(`meetings/${n}`), type: schema + 'Meeting', name: `Kalendertermin ${n}`,
  modified: revision === 1 ? '2026-04-01T12:00:00+02:00' : '2020-04-01T12:00:00+02:00',
  start: n === 'old' ? '1999-06-02T13:00:00+02:00' : n === 'past' ? '2026-01-02T15:00:00+01:00' :
    n === 'cancelled' ? '2026-12-01T15:00:00+01:00' : '2026-11-04T15:00:00+01:00',
  meetingState: n === 'cancelled' ? 'abgesagt' : 'eingeladen',
  organization: [id(n === 2 ? 'organizations/2' : 'organizations/1')],
  agendaItem: n === 1 ? [agenda(1), ...(revision > 1 ? [agenda(2)] : []),
    { ...agenda(99), public: false }] : n === 'old' ? [agenda(98)] : [],
});
const page = (data, current = 1, total = 1, next) => ({
  data, pagination: { currentPage: current, totalPages: total, totalElements: data.length },
  links: next ? { next } : {},
});
function response(url) {
  const path = url.pathname;
  if (path === '/oparl/system') return { id: id('system'), type: schema + 'System', body: id('bodies') };
  if (path === '/oparl/bodies') return page([1, 2].map(n => ({
    id: id(`bodies/${n}`), type: schema + 'Body',
    paper: id(`bodies/${n}/papers`), meeting: id(`bodies/${n}/meetings`),
  })));
  if (/\/bodies\/[12]\/papers$/.test(path)) return page([]);
  if (path === '/oparl/bodies/1/meetings') {
    if (mode === 'missing-collection') return null;
    if (url.searchParams.get('page') === '2')
      return page([meeting('cancelled'), ...(mode === 'unknown-date' ?
        [{ ...meeting('unknown'), start: undefined }] : [])], 2, 2);
    return page([mode === 'deleted' ?
      { id: id('meetings/1'), type: schema + 'Meeting', deleted: true } : meeting(1),
    meeting('old'), meeting('past')], 1, 2, id('bodies/1/meetings?page=2&size=100'));
  }
  if (path === '/oparl/bodies/2/meetings') return page([meeting(2)]);
  if (path.startsWith('/oparl/meetings/')) {
    const n = path.split('/').at(-1);
    if (n === 'unknown') return { ...meeting(n), start: undefined };
    return meeting(/^\d+$/.test(n) ? Number(n) : n);
  }
  if (/\/agendaItems\/\d+$/.test(path)) return agenda(Number(path.split('/').at(-1)));
  if (/\/files\/\d+$/.test(path)) return file(Number(path.split('/').at(-1)));
  if (path === '/oparl/consultations/1') return {
    id: id('consultations/1'), type: schema + 'Consultation',
    paper: id('papers/forbidden'), meeting: id('meetings/old'),
    agendaItem: id('agendaItems/98'), organization: [id('organizations/1')],
  };
  if (/\/organizations\/[12]$/.test(path))
    return { id: `${base.slice(0, -6)}${path}`, type: schema + 'Organization', name: 'Koerperschaft' };
  return null;
}
const provider = createServer((req, res) => {
  const url = new URL(req.url, base);
  requests.push({ path: url.pathname, method: req.method, conditional: req.headers['if-none-match'] });
  const data = url.pathname.startsWith('/oparl/download/') ? document() : response(url);
  if (data === null) { res.writeHead(404); res.end(); return; }
  const bytes = Buffer.isBuffer(data) ? data : Buffer.from(JSON.stringify(data));
  const etag = `"${hash(bytes)}"`;
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, { ETag: etag }); res.end(); return; }
  res.writeHead(200, { 'Content-Length': bytes.length, ETag: etag,
    'Content-Type': Buffer.isBuffer(data) ? 'application/pdf' : 'application/json' });
  res.end(req.method === 'HEAD' ? undefined : bytes);
});
await new Promise(accept => provider.listen(0, '127.0.0.1', accept));
base = `http://127.0.0.1:${provider.address().port}/oparl`;

async function scan(output = archive, extra = [], expected = 0) {
  const child = spawn(scanner, ['--system', id('system'), '--output', output, '--year', '2026',
    '--delay-ms', '0', '--timeout-ms', '1000', '--full', ...extra], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stderr = '';
  child.stdout.resume(); child.stderr.on('data', data => { stderr += data; });
  const timer = setTimeout(() => child.kill('SIGKILL'), 15000);
  try {
    const code = await new Promise((accept, reject) => {
      child.once('error', reject); child.once('close', accept);
    });
    assert.equal(code, expected, stderr);
  } finally { clearTimeout(timer); }
  return JSON.parse(await readFile(join(output, 'archive.json'), 'utf8'));
}
function run(binary, args, expected = 0) {
  const result = spawnSync(binary, args, { encoding: 'utf8', timeout: 30000 });
  assert.equal(result.status, expected, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
}
const build = (input = archive, expected = 0) =>
  run(indexer, ['--input', input, '--output', index], expected);
const show = object => JSON.parse(run(searcher, ['--index', index, '--json', '--raw', '--show', object]).stdout);
const cachePath = (kind, key) => join(archive, 'cache', `${kind}-${hash(key)}.json`);
const metadataPath = key => join(archive, 'metadata', `${hash(key)}.json`);
const revisions = (kind, key) => readdir(join(archive, 'history', kind, hash(key)));
try {
  const first = await scan();
  assert.equal(first.complete, true);
  assert.deepEqual([first.referenceYear, first.calendarFrom, first.calendarThrough],
    [2026, '2026-01-01', '2026-12-31']);
  assert.equal(first.selectedMeetings, 4);
  assert.equal(first.types.Meeting.completeCollections, 2, 'Read independent calendars for both bodies');
  assert.equal(first.types.File.fetchedResponses, 2);
  assert.ok(first.runStartedAtEpochSeconds <= first.runFinishedAtEpochSeconds);
  assert.equal(first.lastSuccessfulRunEndEpochSeconds, first.runFinishedAtEpochSeconds);
  assert.ok(!requests.some(r => /\/(papers\/forbidden|meetings\/old|agendaItems\/(98|99)|download\/(98|99))$/.test(r.path)),
    'Neither old meetings, private TOP nor consultation backreferences expand the calendar scope');
  build();
  assert.equal(show(id('meetings/past')).metadata.meetingState, 'eingeladen');
  assert.equal(show(id('meetings/past')).model.dates.decidedAt, null);
  assert.equal(show(id('meetings/cancelled')).metadata.meetingState, 'abgesagt');
  run(searcher, ['--index', index, '--show', id('agendaItems/99')], 1);
  const initialVersion = show(id('files/1')).model.documentVersion;
  assert.equal(initialVersion.sha256, hash(document()));
  assert.ok(initialVersion.fetchedAtEpochSeconds > 0);
  const meetingCache = JSON.parse(await readFile(cachePath('metadata', id('meetings/1')), 'utf8'));
  assert.equal(show(id('meetings/1')).model.dates.observedAt.value,
    new Date(meetingCache.observedAtEpochSeconds * 1000).toISOString().replace('.000Z', 'Z'));
  const initialCache = JSON.parse(await readFile(cachePath('document', id('files/1')), 'utf8'));
  await delay(1100);
  const second = await scan();
  const revalidated = JSON.parse(await readFile(cachePath('document', id('files/1')), 'utf8'));
  assert.equal(revalidated.fetchedAtEpochSeconds, initialCache.fetchedAtEpochSeconds);
  assert.ok(revalidated.observedAtEpochSeconds > initialCache.observedAtEpochSeconds);
  assert.equal(second.types.File.revalidatedResponses, 2);
  assert.equal((await revisions('documents', id('files/1'))).length, 1);
  assert.equal((await revisions('metadata', id('meetings/1'))).length, 1);
  assert.equal((await readdir(join(archive, 'imports'))).length, 2);
  const journal = (await readFile(join(archive, 'observations', second.runId + '.jsonl'), 'utf8'))
    .trim().split('\n').map(JSON.parse);
  assert.ok(journal.some(o => o.kind === 'document' && o.revalidated && o.sha256 === initialVersion.sha256));
  assert.equal(journal.find(o => o.kind === 'document').revisionPath,
    `history/documents/${hash(id('files/1'))}/${initialVersion.sha256}`);
  const originalBytes = document();
  const older = join(root, 'older');
  await cp(archive, older, { recursive: true });
  await delay(1100);
  revision = 2;
  assert.equal(document().length, originalBytes.length, 'Replacement is deliberately the same size');
  requests.length = 0;
  await scan();
  assert.ok(requests.some(r => r.path.endsWith('/agendaItems/2')), 'A known meeting discovers a new TOP on normal update');
  assert.equal((await revisions('documents', id('files/1'))).length, 2);
  assert.deepEqual(await readFile(join(archive, 'history', 'documents', hash(id('files/1')), hash(originalBytes))), originalBytes);
  const replacedCache = JSON.parse(await readFile(cachePath('document', id('files/1')), 'utf8'));
  assert.equal(replacedCache.currentPath, initialCache.currentPath, 'A filename change cannot redirect a 304 to stale bytes');
  build();
  const changedVersion = show(id('files/1')).model.documentVersion;
  assert.equal(show(id('files/1')).metadata.fileName, 'renamed-1.pdf',
    'Fresh embedded File metadata must supersede an older full resource, not hide a revision');
  assert.notEqual(changedVersion.id, initialVersion.id);
  run(searcher, ['--index', index, '--show', initialVersion.id], 1);
  assert.equal(show(id('agendaItems/2')).model.dates.observedAt.evidence.rule, 'verified-archive-observation-v1');
  build(root);
  assert.equal(show(id('meetings/1')).metadata.agendaItem.filter(a => a.public).length, 2,
    'Latest proven observation wins across archives even when modified moves backwards');
  await rm(older, { recursive: true, force: true });
  const sourceObserved = show(id('meetings/1')).model.dates.observedAt.value;
  build();
  assert.equal(show(id('meetings/1')).model.dates.observedAt.value, sourceObserved,
    'Rebuilding an index does not claim another provider observation');
  const current = join(archive, replacedCache.currentPath);
  const correct = await readFile(current), corrupt = Buffer.from(correct);
  corrupt[20] ^= 1;
  await writeFile(current, corrupt);
  assert.match(build(archive, 1).stderr, /differs from archive attestation/);
  await scan();
  assert.deepEqual(await readFile(current), correct, 'Corrupt local bytes force a fresh body, not a conditional reuse');
  const metaPath = metadataPath(id('meetings/1')), metaBytes = await readFile(metaPath);
  const corrupted = Buffer.from(metaBytes); corrupted[corrupted.indexOf('Kalendertermin')] = 'X'.charCodeAt(0);
  await writeFile(metaPath, corrupted);
  assert.match(build(archive, 1).stderr, /metadata differs from archive attestation/);
  await scan();
  requests.length = 0;
  await scan(archive, ['--refresh']);
  assert.ok(requests.every(r => !r.conditional), 'Refresh preserves old revisions but sends no conditional requests');
  assert.equal((await revisions('documents', id('files/1'))).length, 2);
  const valid = JSON.parse(await readFile(join(archive, 'archive.json'), 'utf8'));
  mode = 'missing-collection';
  const failed = await scan(archive, [], 3);
  assert.equal(failed.complete, false);
  assert.equal(failed.types.Meeting.complete, false);
  assert.equal(failed.types.Meeting.incompleteCollections, 1);
  assert.equal(failed.lastSuccessfulRunEndEpochSeconds, valid.lastSuccessfulRunEndEpochSeconds);
  mode = 'normal';
  const limited = await scan(join(root, 'limited'), ['--max-pages', '1'], 2);
  assert.equal(limited.types.Meeting.incompleteCollections, 1);
  const calendarOnly = join(root, 'calendar-only');
  requests.length = 0;
  const narrow = ['--calendar-only', '--calendar-from', '2026-11-04', '--calendar-through', '2026-11-04'];
  const narrowRun = await scan(calendarOnly, narrow);
  assert.equal(narrowRun.referenceYear, 0);
  assert.equal(narrowRun.selectedMeetings, 2);
  assert.ok(!requests.some(r => /\/papers$/.test(r.path)), 'Calendar-only does not imply a Paper import');
  mode = 'deleted';
  await scan(calendarOnly, narrow);
  build(calendarOnly);
  run(searcher, ['--index', index, '--show', id('meetings/1')], 1);
  assert.ok((await readdir(join(calendarOnly, 'history', 'metadata', hash(id('meetings/1'))))).length >= 2);
  mode = 'unknown-date';
  const unknown = await scan(join(root, 'unknown'), ['--metadata-only'], 3);
  assert.equal(unknown.unknownMeetingDates, 1);
  assert.equal(unknown.types.Meeting.complete, false);
  const before = await readFile(join(archive, 'archive.json'));
  await scan(archive, ['--calendar-through', '2026-02-30'], 1);
  assert.deepEqual(await readFile(join(archive, 'archive.json')), before);
  console.log('Archive tests passed: independent bounded calendars, public TOP, updates/status/tombstones, verified revisions, observation times, current paths, corruption and per-type failure/limit accounting.');
} finally {
  provider.closeAllConnections();
  await new Promise(accept => provider.close(accept));
  await rm(root, { recursive: true, force: true });
}
