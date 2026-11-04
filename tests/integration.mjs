import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { mkdtemp, readdir, readFile, realpath, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const binary = process.argv[2];
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-scan-test-')));
const schema = 'https://schema.oparl.org/1.1/';
const pdf = Buffer.from('%PDF-1.4\nfixture document\n%%EOF\n');
let origin;
let mode = 'normal';
let requests = [];
let failures = 0;
const hash = text => createHash('sha256').update(text).digest('hex');
const file = id => ({
  id: `${origin}/oparl/files/${id}`, type: `${schema}File`,
  fileName: id === 1 ? '../../evil\\name.pdf' : 'Protokoll \u{1f333}.pdf',
  size: pdf.length, accessUrl: `${origin}/download/${id}`,
});
const page = (data, current = 1, total = 1, next) => ({
  data, pagination: { currentPage: current, totalPages: total, totalElements: total ? data.length : 0 },
  links: next ? { next } : {},
});
const keys = [
  'organization', 'person', 'meeting', 'membership', 'locationList',
  'legislativeTermList', 'agendaItem', 'paper', 'consultation', 'file',
];

function yearResponse(url) {
  const uri = path => `${origin}/oparl/${path}`;
  const paper = (id, reference, extra = {}) => ({
    id: uri(`papers/${id}`), type: `${schema}Paper`, reference, date: '2026-03-06', ...extra,
  });
  const consultation = uri('consultations/1');
  if (mode === 'year-many') {
    if (url.pathname === '/oparl/bodies/1/paper')
      return page(Array.from({ length: 129 }, (_, index) => paper(1000 + index, `25/SVV/${index}`)));
    if (url.pathname === '/oparl/bodies/3/paper') return page([], 1, 0);
    if (/^\/oparl\/papers\/1\d{3}$/.test(url.pathname))
      return paper(Number(url.pathname.split('/').at(-1)), '25/SVV/fixture', { mainFile: file(1) });
  }
  if (url.pathname === '/oparl/bodies/1/paper') {
    if (url.searchParams.get('page') === '2')
      return page([paper(3, '25/SVV/0403-001')], 2, 2);
    return page([
      paper(1, '24/SVV/1216', { date: '2025-01-02', mainFile: file(99) }),
      paper(2, '25/SVV/1216'),
    ], 1, 2, `${origin}/oparl/bodies/1/paper?page=2&size=100`);
  }
  if (url.pathname === '/oparl/bodies/3/paper')
    return page([paper(4, '25/OBR/0001')]);
  if (url.pathname === '/oparl/papers/2')
    return paper(mode === 'year-wrong-id' ? 999 : 2, '25/SVV/1216', {
      mainFile: file(1), consultation: [{ id: consultation, type: `${schema}Consultation` }],
      subordinatedPaper: [uri('papers/5')], underDirectionOf: [uri('organizations/1')],
    });
  if (url.pathname === '/oparl/papers/3')
    return paper(3, '25/SVV/0403-001', { mainFile: uri('files/2') });
  if (url.pathname === '/oparl/papers/4')
    return paper(4, '25/OBR/0001', { mainFile: file(3) });
  if (url.pathname === '/oparl/papers/5')
    return paper(5, '26/SVV/0001', { mainFile: file(4), superordinatedPaper: [uri('papers/2')] });
  if (url.pathname === '/oparl/consultations/1')
    return {
      id: consultation, type: `${schema}Consultation`, paper: uri('papers/2'),
      [mode === 'year-camel' ? 'agendaItem' : 'agendaitem']: uri('agendaItems/1'),
      meeting: uri('meetings/1'), organization: [uri('organizations/1')],
    };
  if (url.pathname === '/oparl/agendaItems/1')
    return {
      id: uri('agendaItems/1'), type: `${schema}AgendaItem`, meeting: uri('meetings/1'),
      consultation, resolutionFile: file(5),
    };
  if (url.pathname === '/oparl/meetings/1')
    return {
      id: uri('meetings/1'), type: `${schema}Meeting`, start: '2026-01-21T15:00:00+01:00',
      invitation: file(6), verbatimProtocol: file(7), auxiliaryFile: [file(8)],
      agendaItem: [{ id: uri('agendaItems/99'), type: `${schema}AgendaItem`, resolutionFile: file(99) }],
    };
  if (url.pathname === '/oparl/organizations/1')
    return { id: uri('organizations/1'), type: `${schema}Organization`, body: uri('bodies/1') };
  if (url.pathname === '/oparl/files/2') return file(2);
  return undefined;
}

const server = createServer((req, res) => {
  requests.push(req.url);
  const url = new URL(req.url, origin);
  let response;
  if (mode === 'bad-json' && url.pathname === '/oparl/system') {
    res.end('{"data":[');
    return;
  }
  if (mode === 'bad-chunk' && url.pathname === '/oparl/system') {
    req.socket.end('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nZ\r\nbad\r\n');
    return;
  }
  if (mode === 'retry' && url.pathname === '/oparl/system' && failures++ < 2) {
    res.writeHead(503, { 'Retry-After': '0' });
    res.end('temporary failure');
    return;
  }
  if (url.pathname === '/oparl/system') {
    response = { id: `${origin}/oparl/system`, type: `${schema}System`, body: `${origin}/oparl/bodies` };
  } else if (url.pathname === '/oparl/bodies') {
    response = page([1, 3].map(id => ({
      id: `${origin}/oparl/bodies/${id}`, type: `${schema}Body`,
      ...Object.fromEntries(keys.map(key => [key, `${origin}/oparl/bodies/${id}/${key}`])),
    })));
  } else if (mode.startsWith('year') && yearResponse(url) !== undefined) {
    response = yearResponse(url);
  } else if (url.pathname.startsWith('/oparl/bodies/')) {
    const parts = url.pathname.split('/');
    const body = Number(parts[3]);
    const kind = parts[4];
    const current = Number(url.searchParams.get('page') ?? 1);
    if (kind === 'paper' && body === 1) {
      const next = `${origin}/oparl/bodies/1/paper?page=2&size=100`;
      response = current === 1 ?
        page([{ type: `${schema}Paper`, mainFile: file(1) }], 1, 2, next) :
        page([{ type: `${schema}Paper`, name: 'second page' }], 2, 2);
      if (mode === 'missing-next' && current === 1) response.links = {};
      if (mode === 'cycle' && current === 1) response.links.next = `${origin}${req.url}`;
      if (mode === 'foreign-host' && current === 1) response.links.next = 'https://example.test/private';
    } else if (kind === 'file') {
      response = page([file(body === 1 ? 1 : 2)]);
    } else {
      response = page([], 1, 0);
    }
  } else if (url.pathname.startsWith('/download/')) {
    res.writeHead(302, { Location: url.pathname.replace('/download/', '/content/') });
    res.end();
    return;
  } else if (url.pathname.startsWith('/content/')) {
    if (mode === 'truncated') {
      res.writeHead(200, { 'Content-Length': pdf.length + 20 });
      res.write(pdf);
      res.socket.end();
    } else {
      res.writeHead(200, { 'Content-Type': 'application/pdf' });
      res.write(pdf.subarray(0, 9));
      res.end(pdf.subarray(9));
    }
    return;
  } else {
    res.writeHead(404);
    res.end('missing');
    return;
  }
  const text = JSON.stringify(response);
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.write(text.slice(0, 11));
  res.end(text.slice(11));
});

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
origin = `http://127.0.0.1:${server.address().port}`;

async function run(directory, args = [], expected = 0) {
  const child = spawn(binary, [
    '--system', `${origin}/oparl/system`, '--output', directory,
    '--delay-ms', '0', '--timeout-ms', '1000', ...args,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stderr = '';
  child.stdout.resume();
  child.stderr.on('data', chunk => { stderr += chunk; });
  const timeout = setTimeout(() => child.kill('SIGKILL'), 15000);
  try {
    const code = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    });
    assert.equal(code, expected, stderr);
  } finally {
    clearTimeout(timeout);
  }
  return stderr;
}

try {
  const archive = join(root, 'complete');
  await run(archive);
  assert.equal((await readdir(join(archive, 'metadata'))).length, 23);
  assert.equal((await readdir(join(archive, 'documents'))).length, 2);
  assert.equal(requests.filter(path => path.startsWith('/content/')).length, 2);
  for (const name of await readdir(join(archive, 'documents')))
    assert.deepEqual(await readFile(join(archive, 'documents', name)), pdf);
  assert.ok(requests.some(path => path.includes('/bodies/3/file')));
  const paperPage = join(archive, 'metadata', `${hash(`${origin}/oparl/bodies/1/paper?page=2&size=100`)}.json`);
  assert.equal(JSON.parse(await readFile(paperPage)).data[0].name, 'second page');
  const count = requests.length;
  await run(archive);
  assert.equal(requests.length, count, 'resume makes no network requests for cached resources');
  await run(archive, ['--refresh']);
  assert.equal(requests.filter(path => path.startsWith('/content/')).length, 4,
    'refresh downloads each unique document once, including inline duplicates');

  const metadata = join(root, 'metadata-only');
  await run(metadata, ['--metadata-only']);
  assert.ok(!(await readdir(metadata)).includes('documents'));
  await run(metadata);
  assert.equal((await readdir(join(metadata, 'documents'))).length, 2);

  const limited = join(root, 'limited');
  await run(limited, ['--max-pages', '1', '--max-files', '1'], 2);
  assert.equal(JSON.parse(await readFile(join(limited, 'archive.json'))).complete, false);
  assert.equal((await readdir(join(limited, 'documents'))).length, 1);
  await run(limited);
  assert.equal((await readdir(join(limited, 'documents'))).length, 2);

  mode = 'year';
  const year = join(root, '2025');
  let start = requests.length;
  await run(year, ['--year', '2025', '--max-files', '1'], 2);
  assert.equal((await readdir(join(year, 'documents'))).length, 1);
  await run(year, ['--year', '2025']);
  const summary = JSON.parse(await readFile(join(year, 'archive.json')));
  assert.equal(summary.complete, true);
  assert.equal(summary.referenceYear, 2025);
  assert.equal(summary.scannedPapers, 4);
  assert.equal(summary.selectedPapers, 3);
  assert.equal((await readdir(join(year, 'documents'))).length, 8);
  const selection = (await readFile(join(year, 'selection.jsonl'), 'utf8'))
    .trim().split('\n').map(line => JSON.parse(line).reference);
  assert.deepEqual(selection, ['25/SVV/1216', '25/SVV/0403-001', '25/OBR/0001']);
  const yearRequests = requests.slice(start);
  assert.ok(yearRequests.some(path => path === '/oparl/papers/5'), 'follow-up paper from 2026 included');
  assert.ok(yearRequests.some(path => path === '/oparl/agendaItems/1'), 'lowercase agendaitem followed');
  assert.ok(!yearRequests.some(path => path === '/oparl/papers/1'), '2024 reference excluded despite 2025 date');
  assert.ok(!yearRequests.some(path => /\/(?:content|download)\/99/.test(path)), 'unrelated meeting agenda files excluded');
  assert.ok(!yearRequests.some(path => /\/bodies\/[13]\/(meeting|file|consultation)/.test(path)),
    'year mode does not crawl global non-paper collections');
  start = requests.length;
  await run(year, ['--year', '2025']);
  assert.equal(requests.length, start, 'year resume is completely local when cached');
  await run(year, ['--year', '2024'], 1);
  assert.equal(JSON.parse(await readFile(join(year, 'archive.json'))).referenceYear, 2025,
    'another year cannot overwrite the archive identity');
  assert.equal(requests.length, start);
  for (const variant of ['year-camel', 'year-wrong-id']) {
    mode = variant;
    await run(join(root, variant), ['--year', '2025'], variant === 'year-wrong-id' ? 1 : 0);
  }
  mode = 'year-many';
  const many = join(root, 'many');
  await run(many, ['--year', '2025']);
  assert.equal(JSON.parse(await readFile(join(many, 'archive.json'))).selectedPapers, 129);
  assert.equal((await readdir(join(many, 'documents'))).length, 1, 'identity-set growth retains deduplication');

  for (const failure of ['bad-json', 'bad-chunk', 'missing-next', 'cycle', 'foreign-host', 'truncated']) {
    mode = failure;
    const directory = join(root, failure);
    await run(directory, [], 1);
    assert.equal(JSON.parse(await readFile(join(directory, 'archive.json'))).complete, false);
    if (failure === 'truncated')
      assert.equal((await readdir(join(directory, 'documents'))).length, 0);
  }
  mode = 'retry';
  failures = 0;
  await run(join(root, 'retry'), ['--metadata-only']);
  assert.equal(failures, 3, 'transient failure is retried twice before success');
  console.log('Integration tests passed: pagination, bodies, collections, HTTP, resume, limits, refresh, errors, retries, reference-year selection and later linked proceedings/documents');
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
  await rm(root, { recursive: true, force: true });
}
