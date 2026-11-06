import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, readdir, realpath, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const binary = process.argv[2];
const baseline = process.argv[3] === '--baseline';
const tlsOptions = process.env.OPARL_TEST_TLS_CERT ? {
  cert: await readFile(process.env.OPARL_TEST_TLS_CERT),
  key: await readFile(process.env.OPARL_TEST_TLS_KEY),
} : null;
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-scan-concurrency-')));
const schema = 'https://schema.oparl.org/1.1/';
const pdf = Buffer.from('%PDF-1.4\nparallel fixture\n%%EOF\n');
let origins, system, mode = 'normal', events = [], active = [0, 0], maximum = [0, 0], totalMaximum = 0;
let retryCount = 0;
const page = data => ({
  data, pagination: { currentPage: 1, totalPages: data.length ? 1 : 0, totalElements: data.length },
  links: {},
});
const file = id => ({
  id: `${origins[0]}/oparl/files/${id}`, type: `${schema}File`,
  fileName: `${id}.pdf`, size: pdf.length + (mode === 'size' && id === 2 ? 7 : 0),
  accessUrl: `${origins[mode === 'same-origin' || id === 3 ? 0 : 1]}/content/${id}`,
});
function handler(host, req, res) {
  const path = new URL(req.url, origins[host]).pathname;
  const event = { host, path, method: req.method, start: performance.now(), end: null, status: null };
  events.push(event);
  active[host]++;
  maximum[host] = Math.max(maximum[host], active[host]);
  totalMaximum = Math.max(totalMaximum, active[0] + active[1]);
  let closed = false;
  const finish = () => {
    if (closed) return;
    closed = true;
    event.end = performance.now();
    event.status = res.statusCode;
    active[host]--;
  };
  res.once('finish', finish);
  res.once('close', finish);
  const timer = setTimeout(() => {
    if (path === '/oparl/system') {
      res.end(JSON.stringify({
        id: `${origins[0]}/oparl/system`, type: `${schema}System`, body: `${origins[0]}/oparl/bodies`,
      }));
    } else if (path === '/oparl/bodies') {
      res.end(JSON.stringify(page([{
        id: `${origins[0]}/oparl/bodies/1`, type: `${schema}Body`, paper: `${origins[0]}/oparl/papers`,
      }])));
    } else if (path === '/oparl/papers') {
      res.end(JSON.stringify(page([1, 2, 3].map(id => ({
        id: `${origins[0]}/oparl/papers/${id}`, type: `${schema}Paper`, reference: `23/SVV/${id}`,
      })))));
    } else if (/^\/oparl\/papers\/[123]$/.test(path)) {
      const id = Number(path.split('/').at(-1));
      if ((mode === 'fatal-metadata' || mode === 'missing-metadata-fatal-document') && id === 2) {
        res.writeHead(mode === 'fatal-metadata' ? 401 : 404);
        res.end('metadata denied');
      } else if (['metadata-redirect', 'retry-redirect'].includes(mode) && host === 0 && id === 2) {
        res.writeHead(302, { Location: `${origins[1]}${path}` });
        res.end();
      } else {
        res.end(JSON.stringify({
          id: `${origins[0]}${path}`, type: `${schema}Paper`, reference: `23/SVV/${id}`,
          mainFile: file(id === 3 ? 4 : id),
          ...(id === 2 ? { auxiliaryFile: mode === 'queue' ?
            Array.from({ length: 36 }, (_, index) => file(index + 10)) : [file(3), file(1)] } : {}),
        }));
      }
    } else if (path.startsWith('/content/')) {
      if (mode === 'redirect' && host === 1 && path === '/content/1') {
        res.writeHead(302, { Location: `${origins[0]}/content/1` });
        res.end();
      } else if (['retry', 'retry-redirect'].includes(mode) &&
                 host === 1 && path === '/content/1' && retryCount++ === 0) {
        res.writeHead(429, { 'Retry-After': '1' });
        res.end();
      } else if (mode === 'missing' && path === '/content/1') {
        res.writeHead(404);
        res.end('not found');
      } else if (mode === 'denied' && path === '/content/1') {
        res.writeHead(403);
        res.end('denied');
      } else if ((mode === 'truncated' || mode === 'missing-metadata-fatal-document') && path === '/content/1') {
        res.writeHead(200, { 'Content-Length': pdf.length + 10 });
        res.write(pdf);
        res.socket.end();
      } else {
        if (req.headers['if-none-match'] === '"fixture"') {
          res.writeHead(304, { ETag: '"fixture"' });
          res.end();
          return;
        }
        res.writeHead(200, { 'Content-Length': pdf.length, ETag: '"fixture"' });
        res.end(pdf);
      }
    } else {
      res.writeHead(404);
      res.end();
    }
  }, mode === 'retry-redirect' && host === 0 && path === '/oparl/papers/2' ? 180 :
    path.startsWith('/content/') || path === '/oparl/papers/2' ? 120 : 5);
  res.once('close', () => clearTimeout(timer));
}
const servers = [0, 1].map(host => {
  const callback = (req, res) => handler(host, req, res);
  const server = tlsOptions ? createHttpsServer({
    ...tlsOptions,
    // The pinned TLS 1.2 ClientHello omits signature_algorithms; allow it only on this fixture server.
    ...(host === 1 && process.env.OPARL_TEST_TLS12 ?
      { maxVersion: 'TLSv1.2', ciphers: 'AES256-GCM-SHA384:@SECLEVEL=0' } : {}),
  }, callback) : createServer(callback);
  return server;
});
const bootstrap = tlsOptions ? createServer((req, res) => res.end(JSON.stringify({
  id: system, type: `${schema}System`, body: `${origins[0]}/oparl/bodies`,
}))) : null;
async function run(directory, args = [], expected = 0, defaultDelay = false) {
  events = [];
  maximum = [0, 0];
  totalMaximum = 0;
  retryCount = 0;
  const start = performance.now();
  const child = spawn(binary, [
    '--system', system, '--year', '2023', '--output', directory,
    '--timeout-ms', '1000', '--full', ...(defaultDelay ? [] : ['--delay-ms', '80']), '--color', 'never', ...args,
    ...(tlsOptions ? ['--ca-file', process.env.OPARL_TEST_TLS_CERT] : []),
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', chunk => { stderr += chunk; });
  const timer = setTimeout(() => child.kill('SIGKILL'), 20000);
  try {
    const [code, signal] = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', (code, signal) => resolve([code, signal]));
    });
    assert.equal(code, expected, `${mode}, signal=${signal}\n${stderr}`);
  } finally {
    clearTimeout(timer);
  }
  assert.deepEqual(active, [0, 0], 'all started responses have joined before process exit');
  for (const host of [0, 1]) {
    assert.ok(maximum[host] <= 1,
      `${mode}: origin ${host} had ${maximum[host]} concurrent requests: ${JSON.stringify(events)}`);
    const own = events.filter(event => event.host === host);
    for (let index = 1; index < own.length; index++)
      assert.ok(own[index].start - own[index - 1].end >= 65,
        `origin ${host} cooldown was ${own[index].start - own[index - 1].end} ms`);
  }
  assert.ok(totalMaximum <= 2);
  return { elapsed: performance.now() - start, stderr, events: [...events], maximum: totalMaximum };
}
async function documents(directory) {
  const result = {};
  for (const name of (await readdir(join(directory, 'documents'))).sort())
    result[name] = await readFile(join(directory, 'documents', name), 'utf8');
  return result;
}
try {
  await Promise.all(servers.map(server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve))));
  origins = servers.map(server => `${tlsOptions ? 'https' : 'http'}://127.0.0.1:${server.address().port}`);
  if (bootstrap) await new Promise(resolve => bootstrap.listen(0, '127.0.0.1', resolve));
  system = bootstrap ? `http://127.0.0.1:${bootstrap.address().port}/oparl/system` : `${origins[0]}/oparl/system`;
  const parallelDirectory = join(root, 'parallel');
  const parallel = await run(parallelDirectory);
  console.log(`${baseline ? 'Baseline' : 'Parallel'}: ${parallel.elapsed.toFixed(0)} ms, maximum overlap=${parallel.maximum}`);
  if (!baseline) {
    const native = !parallel.stderr.includes('continuing serially');
    assert.equal(parallel.maximum, native ? 2 : 1, 'different origins must actually overlap on native worker backends');
    const metadata = parallel.events.find(event => event.path === '/oparl/papers/2');
    const document = parallel.events.find(event => event.path === '/content/1');
    if (native) assert.ok(metadata.start < document.end && document.start < metadata.end,
      'document downloads must overlap metadata discovery, not just a final document-only phase');
    assert.equal(Object.keys(await documents(parallelDirectory)).length, 4);
    assert.equal(parallel.events.filter(event => event.path === '/content/1').length, 1,
      'queued duplicate file references are downloaded once');
    const serialDirectory = join(root, 'serial');
    const serial = await run(serialDirectory, ['--workers', '1']);
    assert.equal(serial.maximum, 1);
    assert.deepEqual(await documents(serialDirectory), await documents(parallelDirectory));
    const withoutRunTimes = bytes => JSON.parse(bytes, (key, value) =>
      key === 'runId' || key.endsWith('EpochSeconds') ? undefined : value);
    assert.deepEqual(withoutRunTimes(await readFile(join(serialDirectory, 'archive.json'))),
      withoutRunTimes(await readFile(join(parallelDirectory, 'archive.json'))));
    for (const name of ['selection.jsonl', 'missing-resources.jsonl'])
      assert.deepEqual(await readFile(join(serialDirectory, name)), await readFile(join(parallelDirectory, name)));
    console.log(`Serial: ${serial.elapsed.toFixed(0)} ms, maximum overlap=${serial.maximum}`);
    const defaultPause = await run(join(root, 'default-pause'), [], 0, true);
    for (const host of [0, 1]) {
      const own = defaultPause.events.filter(event => event.host === host);
      for (let index = 1; index < own.length; index++)
        assert.ok(own[index].start - own[index - 1].end >= 485,
          'the default cooldown must retain at least 500 ms per destination (15 ms observation tolerance)');
    }

    for (const variant of ['same-origin', 'redirect', 'metadata-redirect', 'retry', 'retry-redirect', 'missing', 'denied', 'size']) {
      mode = variant;
      const directory = join(root, variant);
      const expected = variant === 'missing' || variant === 'denied' ? 3 : 0;
      const result = await run(directory, [], expected);
      const state = JSON.parse(await readFile(join(directory, 'archive.json')));
      assert.equal(state.complete, expected === 0);
      assert.equal(Object.keys(await documents(directory)).length, expected === 3 ? 3 : 4);
      if (variant === 'same-origin') assert.equal(result.maximum, 1);
      if (variant === 'retry' || variant === 'retry-redirect') {
        const failed = result.events.find(event => event.status === 429);
        const next = result.events.find(event => event.host === failed.host && event.start > failed.end);
        assert.ok(next.start - failed.end >= 980, 'Retry-After pauses the destination, not just the failed URL');
        if (variant === 'retry-redirect') {
          const redirected = result.events.find(event => event.host === 1 && event.path === '/oparl/papers/2');
          assert.ok(redirected.start - failed.end >= 980,
            'a different URL redirected to a throttled destination must also wait for Retry-After');
        }
      }
      if (expected === 3) {
        mode = 'normal';
        const resumed = await run(directory);
        assert.deepEqual(resumed.events.filter(event =>
          event.path.startsWith('/content/') && event.method === 'GET').map(event => event.path), ['/content/1']);
        assert.equal(JSON.parse(await readFile(join(directory, 'archive.json'))).complete, true);
      }
      if (variant === 'size') assert.equal(state.sizeMismatchedDocuments, 1);
    }
    mode = 'normal';
    const limited = join(root, 'limited');
    await run(limited, ['--max-files', '1'], 2);
    assert.equal(Object.keys(await documents(limited)).length, 1, 'queued files do not overshoot the limit');
    await run(limited);
    assert.equal(Object.keys(await documents(limited)).length, 4);
    mode = 'queue';
    const queued = join(root, 'queue');
    await run(queued);
    assert.equal(Object.keys(await documents(queued)).length, 39,
      'more than one bounded queue of documents is drained without dropping work');
    const queueUpdate = await run(queued);
    assert.ok(queueUpdate.events.some(event => event.path === '/oparl/papers'),
      'queue updates rescan the paper collection');
    assert.ok(!queueUpdate.events.some(event => event.path.startsWith('/content/') && event.method === 'GET'),
      'queue updates do not redownload unchanged documents');
    mode = 'fatal-metadata';
    const fatal = join(root, 'fatal');
    await run(fatal, [], 1);
    assert.equal(JSON.parse(await readFile(join(fatal, 'archive.json'))).complete, false);
    assert.equal(Object.keys(await documents(fatal)).length, 1,
      'a completed concurrent document is published even when its metadata partner fails');
    mode = 'normal';
    const recovered = await run(fatal);
    assert.ok(!recovered.events.some(event => event.path === '/content/1' && event.method === 'GET'));
    mode = 'truncated';
    const broken = join(root, 'truncated');
    await run(broken, [], 1);
    assert.equal(JSON.parse(await readFile(join(broken, 'archive.json'))).complete, false);
    mode = 'normal';
    await run(broken);
    assert.equal(Object.keys(await documents(broken)).length, 4);
    mode = 'missing-metadata-fatal-document';
    const mixedFailure = join(root, mode);
    await run(mixedFailure, [], 1);
    assert.equal(JSON.parse(await readFile(join(mixedFailure, 'archive.json'))).missingResources, 1);
    assert.equal(JSON.parse((await readFile(join(mixedFailure, 'missing-resources.jsonl.part'), 'utf8')).trim()).url,
      `${origins[0]}/oparl/papers/2`, 'a concurrent fatal document error must not hide a known metadata 404');
    console.log('Concurrency tests passed: overlap, destination limits, cooldowns, redirects, backoff, serial equivalence, reports, limits and recovery');
  }
} finally {
  for (const server of [...servers, ...(bootstrap ? [bootstrap] : [])]) server.closeAllConnections();
  await Promise.all([...servers, ...(bootstrap ? [bootstrap] : [])]
    .map(server => new Promise(resolve => server.close(resolve))));
  await rm(root, { recursive: true, force: true });
}
