import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { mkdtemp, readdir, readFile, realpath, rm, writeFile } from 'node:fs/promises';
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
  size: pdf.length + (mode.startsWith('size-') && id === 1 ? 7 : 0),
  accessUrl: `${origin}/download/${id}`,
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
    return paper(3, '25/SVV/0403-001', {
      mainFile: uri('files/2'),
      ...(mode === 'year-missing-resource' ? { relatedPaper: [uri('papers/5')] } : {}),
    });
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
  if (url.pathname === '/oparl/organizations/1') {
    const organization = { id: uri('organizations/1'), type: `${schema}Organization`, body: uri('bodies/1') };
    if (mode.startsWith('year-org-')) {
      organization.type = mode === 'year-org-gr' ? 'gr' : 'at';
      if (mode !== 'year-org-missing-type')
        organization.Type = `${schema}${mode === 'year-org-invalid-type' ? 'File' : 'Organization'}`;
    }
    return organization;
  }
  if (url.pathname === '/oparl/files/2') return file(2);
  return undefined;
}

const server = createServer((req, res) => {
  requests.push(req.url);
  const url = new URL(req.url, origin);
  let response;
  if ((mode === 'missing-system' && url.pathname === '/oparl/system') ||
      (mode === 'missing-bodies' && url.pathname === '/oparl/bodies') ||
      (mode === 'missing-collection' && url.pathname === '/oparl/bodies/1/paper') ||
      (mode === 'missing-page' && url.pathname === '/oparl/bodies/1/paper' &&
       url.searchParams.get('page') === '2') ||
      (mode === 'year-missing-index' && url.pathname === '/oparl/bodies/1/paper' &&
       url.searchParams.get('page') === '2') ||
      (mode === 'year-missing-resource' && url.pathname === '/oparl/papers/5')) {
    res.writeHead(404);
    res.end('missing metadata');
    return;
  }
  if (mode === 'missing-then-fatal' && url.pathname === '/oparl/bodies/3/organization') {
    res.writeHead(401);
    res.end('metadata access denied');
    return;
  }
  if (mode === 'bad-json' && url.pathname === '/oparl/system') {
    res.end('{"data":[');
    return;
  }
  if (mode === 'bad-chunk' && url.pathname === '/oparl/system') {
    req.socket.end('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nZ\r\nbad\r\n');
    return;
  }
  if (mode === 'noauth-metadata' && url.pathname === '/oparl/system') {
    res.writeHead(302, { Location: './noauth' });
    res.end();
    return;
  }
  if (mode === 'retry' && url.pathname === '/oparl/system' && failures++ < 2) {
    res.writeHead(503, { 'Retry-After': '0' });
    res.end('temporary failure');
    return;
  }
  if (mode === 'closed-status' && url.pathname === '/oparl/system') {
    req.socket.end();
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
    if (url.pathname === '/content/1' &&
        ['noauth', 'unauthorized', 'forbidden', 'missing-document', 'missing-then-fatal', 'foreign-redirect'].includes(mode)) {
      if (mode === 'noauth' || mode === 'foreign-redirect')
        res.writeHead(302, { Location: mode === 'noauth' ? './noauth' : 'https://example.test/private' });
      else
        res.writeHead(mode === 'unauthorized' ? 401 : mode === 'forbidden' ? 403 : 404);
      res.end('not a document');
    } else if (mode === 'size-unframed' && url.pathname === '/content/1') {
      req.socket.end(Buffer.concat([
        Buffer.from('HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\nConnection: close\r\n\r\n'),
        pdf,
      ]));
    } else if (mode === 'truncated') {
      res.writeHead(200, { 'Content-Length': pdf.length + 20 });
      res.write(pdf);
      res.socket.end();
    } else {
      res.writeHead(200, {
        'Content-Type': 'application/pdf',
        ...(mode === 'size-mismatch' ? { 'Content-Length': pdf.length } : {}),
      });
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
  const send = () => {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.write(text.slice(0, 11));
    res.end(text.slice(11));
  };
  if (mode === 'slow-status' && url.pathname === '/oparl/bodies/1/paper' &&
      url.searchParams.get('page') === '2') {
    const timer = setTimeout(send, 150);
    res.once('close', () => clearTimeout(timer));
  } else {
    send();
  }
});

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
origin = `http://127.0.0.1:${server.address().port}`;

async function run(directory, args = [], expected = 0, env = {}) {
  const child = spawn(binary, [
    '--system', `${origin}/oparl/system`, '--output', directory,
    '--delay-ms', '0', '--timeout-ms', '1000', ...args,
  ], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
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
  const organizationMetadata = `${hash(`${origin}/oparl/organizations/1`)}.json`;
  for (const subtype of ['at', 'gr']) {
    mode = `year-org-${subtype}`;
    const directory = join(root, mode);
    await run(directory, ['--year', '2025']);
    const organization = JSON.parse(await readFile(join(directory, 'metadata', organizationMetadata)));
    assert.equal(organization.type, subtype, 'original subtype is preserved');
    assert.equal(organization.Type, `${schema}Organization`, 'original uppercase Type is preserved');
    start = requests.length;
    await run(directory, ['--year', '2025']);
    assert.equal(requests.length, start, 'legacy Organization metadata is reused from cache');
  }
  for (const variant of ['year-org-invalid-type', 'year-org-missing-type']) {
    mode = variant;
    const directory = join(root, mode);
    const stderr = await run(directory, ['--year', '2025'], 1);
    assert.match(stderr, /unexpected OParl response shape/);
    assert.equal(JSON.parse(await readFile(join(directory, 'archive.json'))).complete, false);
    assert.ok(!(await readdir(join(directory, 'metadata'))).includes(organizationMetadata),
      'invalid legacy types must not be published as valid metadata');
  }
  mode = 'year-many';
  const many = join(root, 'many');
  await run(many, ['--year', '2025']);
  assert.equal(JSON.parse(await readFile(join(many, 'archive.json'))).selectedPapers, 129);
  assert.equal((await readdir(join(many, 'documents'))).length, 1, 'identity-set growth retains deduplication');

  for (const [variant, missingPath, documents] of [
    ['missing-system', '/oparl/system', 0],
    ['missing-bodies', '/oparl/bodies?size=100', 0],
    ['missing-collection', '/oparl/bodies/1/paper?size=100', 2],
    ['missing-page', '/oparl/bodies/1/paper?page=2&size=100', 2],
    ['year-missing-index', '/oparl/bodies/1/paper?page=2&size=100', 7],
    ['year-missing-resource', '/oparl/papers/5', 7],
  ]) {
    mode = variant;
    const directory = join(root, variant);
    const args = variant.startsWith('year') ? ['--year', '2025'] : [];
    start = requests.length;
    const diagnostics = await run(directory, [...args, '--color', 'always'], 3);
    assert.match(diagnostics, /HTTP \x1b\[0m404/);
    assert.match(diagnostics, /\x1b\[1;33mINCOMPLETE \(HTTP 404\)\x1b\[0m/);
    let state = JSON.parse(await readFile(join(directory, 'archive.json')));
    assert.equal(state.complete, false);
    assert.equal(state.missingResources, 1);
    assert.equal((await readdir(join(directory, 'documents'))).length, documents,
      'independent documents are still downloaded after a missing metadata response');
    const missingReport = join(directory, 'missing-resources.jsonl');
    const records = [{
      url: `${origin}${missingPath}`, kind: 'metadata', status: 404,
    }];
    assert.deepEqual((await readFile(missingReport, 'utf8')).trim().split('\n').map(line => JSON.parse(line)), records);
    assert.equal(requests.slice(start).filter(path => path === missingPath).length, 1,
      'missing metadata is not retried within the run, including duplicate graph links');
    const metadataDirectory = variant === 'year-missing-index' ? 'index' : 'metadata';
    assert.ok(!(await readdir(join(directory, metadataDirectory))).includes(`${hash(`${origin}${missingPath}`)}.json`),
      'HTTP error bodies are never published as metadata');
    if (variant === 'missing-collection' || variant === 'missing-page')
      assert.ok(requests.slice(start).includes('/oparl/bodies/3/file?size=100'),
        'a missing collection page does not prevent processing other collections or bodies');

    start = requests.length;
    await run(directory, args, 3);
    assert.deepEqual(requests.slice(start), [missingPath],
      'resume retries the missing URL while reusing all completed work');
    assert.deepEqual((await readFile(missingReport, 'utf8')).trim().split('\n').map(line => JSON.parse(line)), records,
      'the report describes this run rather than accumulating duplicates across runs');

    mode = variant.startsWith('year') ? 'year' : 'normal';
    await run(directory, args);
    state = JSON.parse(await readFile(join(directory, 'archive.json')));
    assert.equal(state.complete, true, 'a previously missing URL can recover without refresh');
    assert.equal(state.missingResources, 0);
    assert.equal(await readFile(missingReport, 'utf8'), '');
  }

  mode = 'missing-document';
  const missingDocument = join(root, mode);
  start = requests.length;
  assert.match(await run(missingDocument, [], 3), /HTTP 404/);
  let missingState = JSON.parse(await readFile(join(missingDocument, 'archive.json')));
  assert.equal(missingState.complete, false);
  assert.equal(missingState.missingResources, 1);
  assert.equal(missingState.unavailableDocuments, 0);
  assert.equal(missingState.newDocuments, 1);
  assert.equal(requests.slice(start).filter(path => path === '/content/1').length, 1);
  assert.equal((await readdir(join(missingDocument, 'documents'))).length, 1);
  assert.deepEqual(JSON.parse((await readFile(join(missingDocument, 'missing-resources.jsonl'), 'utf8')).trim()), {
    url: file(1).accessUrl, kind: 'document', status: 404,
  });
  await run(missingDocument, ['--max-pages', '1', '--skip-unavailable-files'], 3);
  missingState = JSON.parse(await readFile(join(missingDocument, 'archive.json')));
  assert.equal(missingState.limited, true, 'missing resources take exit-code precedence over limits');
  assert.equal(missingState.missingResources, 1);
  mode = 'normal';
  start = requests.length;
  await run(missingDocument);
  assert.deepEqual(requests.slice(start), ['/download/1', '/content/1']);
  assert.equal(JSON.parse(await readFile(join(missingDocument, 'archive.json'))).complete, true);
  assert.equal(await readFile(join(missingDocument, 'missing-resources.jsonl'), 'utf8'), '');

  mode = 'missing-then-fatal';
  const missingThenFatal = join(root, mode);
  const fatalDiagnostics = await run(missingThenFatal, ['--skip-unavailable-files'], 1);
  assert.match(fatalDiagnostics, /HTTP 404/);
  assert.match(fatalDiagnostics, /HTTP 401/);
  const fatalState = JSON.parse(await readFile(join(missingThenFatal, 'archive.json')));
  assert.equal(fatalState.complete, false);
  assert.equal(fatalState.missingResources, 1, 'a later fatal error does not erase the recorded 404');
  assert.ok(!(await readdir(missingThenFatal)).includes('missing-resources.jsonl'));
  assert.equal(JSON.parse((await readFile(join(missingThenFatal, 'missing-resources.jsonl.part'), 'utf8')).trim()).status,
    404, 'an interrupted traversal retains its partial missing-resource report');
  mode = 'normal';
  await run(missingThenFatal);
  assert.equal(await readFile(join(missingThenFatal, 'missing-resources.jsonl'), 'utf8'), '');

  for (const failure of ['bad-json', 'bad-chunk', 'missing-next', 'cycle', 'foreign-host', 'truncated']) {
    mode = failure;
    const directory = join(root, failure);
    await run(directory, [], 1);
    assert.equal(JSON.parse(await readFile(join(directory, 'archive.json'))).complete, false);
    if (failure === 'truncated')
      assert.equal((await readdir(join(directory, 'documents'))).length, 0);
  }
  mode = 'noauth';
  const unavailable = join(root, 'unavailable');
  assert.match(await run(unavailable, [], 1), /access denied/);
  start = requests.length;
  const unavailableDiagnostics = await run(unavailable, ['--skip-unavailable-files', '--color', 'always'], 3);
  assert.match(unavailableDiagnostics, /\x1b\[1;33moparl-scan: warning: \x1b\[0munavailable document recorded/);
  assert.match(unavailableDiagnostics, /\x1b\[1;33mINCOMPLETE \(unavailable documents\)\x1b\[0m/);
  const unavailableSummary = JSON.parse(await readFile(join(unavailable, 'archive.json')));
  assert.equal(unavailableSummary.complete, false);
  assert.equal(unavailableSummary.skipUnavailableFiles, true);
  assert.equal(unavailableSummary.unavailableDocuments, 1);
  assert.equal(unavailableSummary.newDocuments, 1, 'other public files are still downloaded');
  assert.equal((await readdir(join(unavailable, 'documents'))).length, 1, 'no denial page is stored as a document');
  const report = join(unavailable, 'unavailable-files.jsonl');
  const records = (await readFile(report, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
  assert.deepEqual(records, [{
    id: file(1).id, url: file(1).accessUrl, fileName: file(1).fileName, reason: 'access-denied',
  }]);
  assert.equal(requests.slice(start).filter(path => path === '/content/1').length, 1,
    'duplicate references to a denied File are tried and reported only once per run');
  assert.ok(!requests.some(path => path.includes('noauth')), 'authentication redirects are never followed');
  start = requests.length;
  await run(unavailable, ['--skip-unavailable-files'], 3);
  assert.equal(requests.slice(start).filter(path => path === '/content/1').length, 1,
    'a missing file is retried on resume rather than treated as cached');
  assert.deepEqual((await readFile(report, 'utf8')).trim().split('\n').map(line => JSON.parse(line)), records);
  mode = 'normal';
  await run(unavailable, ['--skip-unavailable-files']);
  assert.equal((await readdir(join(unavailable, 'documents'))).length, 2);
  assert.equal(await readFile(report, 'utf8'), '', 'resolved access failures disappear from the new report');
  assert.equal(JSON.parse(await readFile(join(unavailable, 'archive.json'))).complete, true);

  for (const variant of ['unauthorized', 'forbidden', 'foreign-redirect', 'noauth-metadata']) {
    mode = variant;
    const directory = join(root, variant);
    const denied = variant === 'unauthorized' || variant === 'forbidden';
    await run(directory, ['--skip-unavailable-files'], denied ? 3 : 1);
    const state = JSON.parse(await readFile(join(directory, 'archive.json')));
    assert.equal(state.complete, false);
    assert.equal(state.unavailableDocuments, denied ? 1 : 0,
      'only document access denials may be continued');
    if (denied) {
      assert.equal((await readdir(join(directory, 'documents'))).length, 1);
      assert.equal(JSON.parse((await readFile(join(directory, 'unavailable-files.jsonl'), 'utf8')).trim()).id,
        file(1).id);
    }
  }

  for (const variant of ['size-mismatch', 'size-chunked']) {
    mode = variant;
    const directory = join(root, variant);
    start = requests.length;
    const stderr = await run(directory, [], 1);
    assert.match(stderr, new RegExp(`expected bytes=${pdf.length + 7}, received bytes=${pdf.length}`));
    assert.equal(requests.slice(start).filter(path => path === '/content/1').length, 1,
      'a complete but mismatched size is not a transient transfer failure');
    const mismatchDiagnostics = await run(directory, ['--accept-size-mismatch', '--color', 'always']);
    assert.match(mismatchDiagnostics, /\x1b\[1;33moparl-scan: warning: \x1b\[0maccepting document size mismatch/);
    const state = JSON.parse(await readFile(join(directory, 'archive.json')));
    assert.equal(state.complete, true, 'all files acquired under the explicit acceptance policy');
    assert.equal(state.acceptSizeMismatch, true);
    assert.equal(state.sizeMismatchedDocuments, 1);
    const mismatchReport = join(directory, 'size-mismatches.jsonl');
    assert.deepEqual(JSON.parse((await readFile(mismatchReport, 'utf8')).trim()), {
      id: file(1).id, url: file(1).accessUrl, fileName: file(1).fileName,
      expectedSize: pdf.length + 7, actualSize: pdf.length,
    });
    const names = await readdir(join(directory, 'documents'));
    assert.equal(names.length, 2);
    for (const name of names)
      assert.deepEqual(await readFile(join(directory, 'documents', name)), pdf);
    if (variant === 'size-mismatch') {
      const cached = join(directory, 'documents', names.find(name => name.startsWith(hash(file(1).id))));
      await writeFile(cached, 'corrupted');
      start = requests.length;
      await run(directory, ['--accept-size-mismatch']);
      assert.deepEqual(await readFile(cached), pdf, 'wrong-size caches must be revalidated by a fresh download');
      assert.equal(requests.slice(start).filter(path => path === '/content/1').length, 1);
      assert.ok(!requests.slice(start).some(path => path === '/content/2'), 'unaffected cached documents are reused');
      assert.equal((await readFile(mismatchReport, 'utf8')).trim().split('\n').length, 1);
    }
  }
  for (const variant of ['size-unframed', 'truncated']) {
    mode = variant;
    const directory = join(root, `${variant}-accept-size`);
    await run(directory, ['--accept-size-mismatch'], 1);
    assert.equal(JSON.parse(await readFile(join(directory, 'archive.json'))).complete, false);
    assert.equal((await readdir(join(directory, 'documents'))).length, 0,
      'accepting advisory metadata sizes never accepts truncated or unverified-length transfers');
  }

  mode = 'closed-status';
  start = requests.length;
  const closedDiagnostics = await run(join(root, 'closed-status'), ['--metadata-only'], 1);
  assert.match(closedDiagnostics, /cannot read HTTP status/);
  assert.match(closedDiagnostics, /cause=server closed connection/);
  assert.equal(requests.length - start, 3, 'a connection closed before its HTTP status is retried finitely');

  mode = 'slow-status';
  const slow = join(root, 'slow-status');
  start = requests.length;
  const timeoutDiagnostics = await run(slow, ['--metadata-only', '--timeout-ms', '50'], 1);
  assert.match(timeoutDiagnostics, /cannot read HTTP status/);
  assert.match(timeoutDiagnostics, /status read elapsed ms=\d+, read timeout ms=50, cause=read timeout/);
  const slowPage = '/oparl/bodies/1/paper?page=2&size=100';
  assert.equal(requests.slice(start).filter(path => path === slowPage).length, 3);
  assert.equal(JSON.parse(await readFile(join(slow, 'archive.json'))).complete, false);
  assert.ok(!(await readdir(join(slow, 'metadata'))).includes(`${hash(`${origin}${slowPage}`)}.json`),
    'a timed-out metadata response must not be published');
  const completedRequests = requests.slice(start).filter(path => path !== slowPage);
  start = requests.length;
  await run(slow, ['--metadata-only', '--timeout-ms', '500']);
  assert.equal(requests.slice(start).filter(path => path === slowPage).length, 1,
    'a larger timeout allows the slow page to finish without retries');
  assert.ok(!requests.slice(start).some(path => completedRequests.includes(path)),
    'resuming with a larger timeout reuses metadata completed before the failure');
  assert.equal(JSON.parse(await readFile(join(slow, 'archive.json'))).complete, true);

  mode = 'retry';
  failures = 0;
  const retryDiagnostics = await run(join(root, 'retry'), ['--metadata-only', '--color', 'always']);
  assert.equal(failures, 3, 'transient failure is retried twice before success');
  assert.match(retryDiagnostics, /\x1b\[1;31moparl-scan: error: HTTP \x1b\[0m503/);
  assert.match(retryDiagnostics, /\x1b\[1;33moparl-scan: warning: \x1b\[0mretrying/);
  assert.ok(retryDiagnostics.split('\n').filter(line => line.startsWith('metadata: '))
    .every(line => !line.includes('\x1b')), 'ordinary progress output stays unstyled');
  const colorEnv = { CLICOLOR_FORCE: '0', CLICOLOR: '1', NO_COLOR: undefined, TERM: 'xterm-256color' };
  const plainError = await run(join(root, 'color-auto'), ['--invalid', '1'], 1, colorEnv);
  assert.match(plainError, /oparl-scan: error: unknown option/);
  assert.ok(!plainError.includes('\x1b'), 'automatic colors stay off in redirected logs');
  const forcedError = await run(join(root, 'color-always'), ['--color', 'always', '--invalid', '1'], 1, colorEnv);
  assert.match(forcedError, /\x1b\[1;31moparl-scan: error: \x1b\[0munknown option/);
  const disabledError = await run(join(root, 'color-never'), ['--color', 'never', '--invalid', '1'], 1,
    { ...colorEnv, CLICOLOR_FORCE: '1' });
  assert.ok(!disabledError.includes('\x1b'), 'explicit never overrides inherited color forcing');
  assert.match(await run(join(root, 'invalid-color'), ['--color', 'rainbow'], 1, colorEnv), /invalid color mode/);
  console.log('Integration tests passed: pagination, bodies, collections, HTTP, resume, limits, refresh, errors, retries, reference-year selection, later linked proceedings/documents, default 404 continuation and reporting, access-denial reporting, size-mismatch policy, diagnostic colors, slow responses, transport diagnostics and recovery');
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
  await rm(root, { recursive: true, force: true });
}
