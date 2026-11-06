import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { stat } from 'node:fs/promises';
import { createServer, connect } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { Script } from 'node:vm';
import { testSearchMcp } from './search-mcp.mjs';

export async function testSearchWeb(binary, index, fileId) {
  if (process.platform === 'win32') {
    console.log('Web tests skipped: pinned newos has no Windows TCP listener backend.');
    return;
  }
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const base = `http://127.0.0.1:${port}`;
  const child = spawn(binary, ['--index', index, '--serve', '--port', String(port)],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '', launchError, stdout = '', idle;
  const held = [];
  child.stderr.setEncoding('utf8').on('data', data => { log += data; });
  child.stdout.setEncoding('utf8').on('data', data => { stdout += data; });
  child.on('error', error => { launchError = error; });
  const get = (path, options = {}) => fetch(base + path, { signal: AbortSignal.timeout(8000), ...options });
  const cli = args => {
    const result = spawnSync(binary, ['--index', index, '--json', ...args],
      { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
    assert.equal(result.status, 0, `${result.error ?? ''}\n${result.stderr}`);
    return result.stdout;
  };
  const raw = request => new Promise((resolve, reject) => {
    const socket = connect(port, '127.0.0.1');
    let response = '';
    socket.setEncoding('utf8');
    socket.setTimeout(8000, () => socket.destroy(new Error('raw HTTP request timed out')));
    socket.on('data', data => { response += data; });
    socket.once('error', reject);
    socket.once('end', () => resolve(response));
    socket.once('connect', () => socket.write(request));
  });
  try {
    const deadline = Date.now() + 8000;
    for (;;) {
      if (launchError) throw launchError;
      assert.equal(child.exitCode, null, log);
      try {
        const response = await get('/api/status');
        assert.equal(response.status, 200);
        const status = await response.json();
        assert.ok(status.records > 1100 && status.terms > 0);
        assert.equal(status.incomplete, true);
        assert.equal(status.indexBytes, (await stat(index)).size, 'health reports the loaded index file size in bytes');
        assert.ok(status.indexMemoryBytes > status.indexBytes);
        assert.ok(status.pages > 0 && status.warningPages > 0 && status.ocrPages > 0);
        assert.ok(status.ocrPages <= status.warningPages && status.warningPages <= status.pages);
        break;
      } catch (error) {
        if (error.cause?.code !== 'ECONNREFUSED' || Date.now() >= deadline) throw error;
        await delay(25);
      }
    }
    const page = await get('/');
    assert.equal(page.status, 200);
    assert.match(page.headers.get('content-type'), /text\/html/);
    assert.match(page.headers.get('content-security-policy'), /script-src 'self'/);
    assert.equal(page.headers.get('access-control-allow-origin'), null);
    const html = await page.text();
    assert.match(html, /<html lang="de">/);
    assert.match(html, /href="\/health"/);
    assert.doesNotMatch(html, /id="coverage"|Index ist unvollstaendig/);
    const health = await get('/health');
    assert.equal(health.status, 200);
    assert.match(await health.text(), /id="health-stats"/);
    const headHealth = await get('/health', { method: 'HEAD' });
    assert.equal(headHealth.status, 200);
    assert.equal(await headHealth.text(), '');
    const paperId = fileId.replace('/files/1', '/papers/1');
    const origin = paperId.slice(0, paperId.lastIndexOf('/') + 1).replace('/papers/', '/');
    const objectId = path => origin + path;
    const caseParameters = new URLSearchParams({ id: paperId });
    const casePage = await get('/paper?' + caseParameters);
    assert.equal(casePage.status, 200);
    assert.match(casePage.headers.get('content-type'), /text\/html/);
    assert.match(await casePage.text(), /id="case-consultations"/);
    const caseResponse = await get('/api/paper?' + caseParameters);
    assert.equal(caseResponse.status, 200);
    const context = await caseResponse.json();
    assert.equal(context.id, paperId);
    assert.equal(context.incomplete, true);
    const caseObjects = new Map(context.objects.map(object => [object.id, object]));
    assert.equal(caseObjects.size, context.objects.length, 'cycles and repeated relations must not duplicate objects');
    assert.equal(caseObjects.get(fileId).metadata.name, 'Original PDF', 'canonical indexed metadata wins over stale embedded metadata');
    assert.equal(caseObjects.get(objectId('consultations/1')).metadata.role, 'Entscheidung');
    for (const path of ['papers/2', 'papers/3', 'agendaItems/1', 'agendaItems/2', 'meetings/1', 'organizations/1', 'organizations/2'])
      assert.equal(caseObjects.get(objectId(path))?.indexed, true, path);
    assert.equal(caseObjects.has(objectId('agendaItems/unrelated')), false, 'do not expand the entire meeting agenda into this case');
    for (const path of ['papers/missing', 'papers/deleted'])
      assert.deepEqual([caseObjects.get(objectId(path)).indexed, caseObjects.get(objectId(path)).metadata], [false, null]);
    const embedded = caseObjects.get(objectId('consultations/missing'));
    assert.equal(embedded.indexed, false);
    assert.equal(embedded.metadata.role, 'Nur eingebettet', 'retain inline details when a standalone resource is unavailable');
    assert.equal(caseObjects.get(objectId('files/103')).metadata.deleted, true, 'deleted inline files remain marked deleted');
    for (const route of ['/paper', '/api/paper']) {
      const response = await get(route + '?' + caseParameters, { method: 'HEAD' });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), '');
    }
    const script = await get('/app.js');
    assert.match(script.headers.get('content-type'), /text\/javascript/);
    new Script(await script.text());

    idle = connect(port, '127.0.0.1');
    await once(idle, 'connect');
    const cases = [
      ['Schulweg', {}],
      ['NOT nonexistenttoken', { kind: 'Person', limit: '100' }],
      [Array(64).fill('Schulweg').join(' OR '), { kind: 'Page' }],
      ['GRO\u0308SSE', { kind: 'Page' }],
      ['Schulweg OR NOT Haushalt', { kind: 'Paper', year: '2024' }],
      ['"sicherer schulweg"', { kind: 'Page', year: '2023', paperType: 'Antrag', organization: 'alpha' }],
      ['Schulweg', { kind: 'Page', year: '2023', paperType: 'Antwort' }],
      ['Schulweg', { kind: 'Page', year: '2024', paperType: 'Antwort', limit: '1' }],
      ['NOT nonexistenttoken', { kind: 'Person', sort: 'date-desc', offset: '7', limit: '50' }],
      ['NOT nonexistenttoken', { kind: 'Person', sort: 'date-asc', offset: '1095' }],
      ['NOT nonexistenttoken', { kind: 'Person', offset: '4294967295' }],
      ['Schulweg', { kind: 'Page', year: '2024', sort: 'date-desc' }],
      ['Schulweg', { group: 'paper', sort: 'date-desc', limit: '1', offset: '1' }],
      ['nonexistenttoken', {}],
    ];
    const flags = { kind: '--kind', year: '--year', paperType: '--paper-type',
      organization: '--organization', limit: '--limit', offset: '--offset', sort: '--sort', group: '--group' };
    for (const [q, filters] of cases) {
      const response = await get('/api/search?' + new URLSearchParams({ q, ...filters }));
      assert.equal(response.status, 200);
      assert.equal(response.headers.get('cache-control'), 'no-store');
      assert.equal(await response.text(), cli([...Object.entries(filters).flatMap(([key, value]) => [flags[key], value]), q]),
        'web JSON must be byte-identical to CLI, including filters, scores/order, snippets, context and totals');
    }
    idle.destroy(); idle = null;
    for (const args of [
      { id: fileId, page: '2' },
      { id: fileId },
    ]) {
      const response = await get('/api/object?' + new URLSearchParams(args));
      assert.equal(response.status, 200);
      assert.equal(await response.text(), cli(['--show', fileId, ...(args.page ? ['--page', args.page] : [])]));
    }
    const head = await get('/api/search?q=Schulweg', { method: 'HEAD' });
    assert.equal(head.status, 200);
    assert.equal(await head.text(), '');
    for (const [path, status] of [
      ['/api/search', 400], ['/api/search?q=', 400], ['/api/search?q=Schulweg&limit=0', 400],
      ['/api/search?q=Schulweg&year=1999', 400], ['/api/search?q=AND', 400],
      ['/api/search?q=Schulweg&q=Haushalt', 400], ['/api/search?q=%00', 400],
      ['/api/search?q=%FF', 400], ['/api/search?q=%', 400], ['/api/search?q=%A', 400],
      ['/api/search?q=Schulweg&paperType=', 400], ['/api/search?q=Schulweg&unknown=1', 400],
      ['/api/search?q=Schulweg&page=1', 400], ['/api/search?q=Schulweg&', 400],
      ['/api/search?q=Schulweg&offset=-1', 400], ['/api/search?q=Schulweg&offset=4294967296', 400],
      ['/api/search?q=Schulweg&offset=', 400], ['/api/search?q=Schulweg&offset=1&offset=2', 400],
      ['/api/search?q=Schulweg&sort=newest', 400], ['/api/search?q=Schulweg&sort=', 400],
      ['/api/search?q=Schulweg&sort=relevance&sort=date-asc', 400],
      ['/api/search?q=Schulweg&group=page', 400],
      ['/api/search?q=Schulweg&group=paper&group=record', 400],
      ['/api/object?id=missing&group=paper', 400],
      ['/api/object', 400], ['/api/object?id=missing', 404], ['/api/object?id=missing&page=0', 400],
      ['/api/object?id=missing&q=foo', 400], ['/api/object?id=missing&sort=relevance', 400],
      ['/api/status?q=foo', 400],
      ['/paper', 400], ['/paper?id=missing', 404], ['/api/paper', 400],
      ['/api/paper?id=', 400], ['/api/paper?id=missing', 404],
      ['/api/paper?' + new URLSearchParams({ id: fileId }), 404],
      ['/api/paper?' + caseParameters + '&page=1', 400], ['/paper?' + caseParameters + '&q=Schulweg', 400],
      ['/api/paper?' + caseParameters + '&id=other', 400],
      ['/missing', 404], ['/%2e%2e/README.md', 404],
    ]) {
      const response = await get(path);
      assert.equal(response.status, status, path);
      assert.ok((await response.json()).error, path);
    }
    const post = await get('/api/search?q=Schulweg', { method: 'POST' });
    assert.equal(post.status, 405);
    assert.ok((await post.json()).error);
    const allowed = await get('/api/search?q=Schulweg', { headers: { Origin: base } });
    assert.equal(allowed.status, 200);
    await allowed.arrayBuffer();
    const host = `Host: 127.0.0.1:${port}\r\n`;
    for (const requestHeaders of [
      'Host: attacker.example\r\n',
      `${host}Origin: https://attacker.example\r\n`,
      `${host}Sec-Fetch-Site: same-site\r\n`,
    ]) assert.match(await raw(`GET /api/search?q=Schulweg HTTP/1.1\r\n${requestHeaders}\r\n`), /^HTTP\/1.1 403 /);
    for (const request of [
      'GET / HTTP/1.1\r\n\r\n',
      `GET / HTTP/1.1\r\n${host}${host}\r\n`,
      `GET / HTTP/1.1\r\n${host}Content-Length: 0\r\n\r\n`,
      `GET / HTTP/1.1\r\n${host}\r\nGET / HTTP/1.1\r\n${host}\r\n`,
      `GET / HTTP/1.1\r\n${host}X-Test: a\0b\r\n\r\n`,
    ]) assert.match(await raw(request), /^HTTP\/1.1 400 /);
    const oversizedPrefix = `GET / HTTP/1.1\r\n${host}X-Long: `;
    assert.match(await raw(oversizedPrefix + 'x'.repeat(65536 - oversizedPrefix.length)),
      /^HTTP\/1.1 431 /);
    await testSearchMcp(base, raw, cli, fileId, port);
    for (let i = 0; i < 8; i++) {
      const socket = connect(port, '127.0.0.1');
      held.push(socket);
      await once(socket, 'connect');
    }
    await delay(25);
    assert.match(await raw(''), /^HTTP\/1.1 503 /);
    for (const socket of held) socket.destroy();
    const timedOut = raw(`GET / HTTP/1.1\r\n${host}X-Partial: `);
    const bodyTimedOut = raw(`POST /mcp HTTP/1.1\r\n${host}Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: 100\r\n\r\n{`);
    assert.equal((await get('/api/status')).status, 200, 'partial requests must not block other clients');
    assert.match(await timedOut, /^HTTP\/1.1 408 /);
    assert.match(await bodyTimedOut, /^HTTP\/1.1 408 /);
    assert.equal((await get('/api/status')).status, 200, 'server must recover from rejected requests');
    assert.equal(child.exitCode, null, log);
    assert.equal(stdout, '', 'HTTP responses must not leak to process stdout');
    for (const args of [
      ['--port', '8080', 'Schulweg'], ['--serve', '--year', '2024'], ['--serve', '--json'],
      ['--serve', 'Schulweg'], ['--serve', '--batch'], ['--serve', '--port', '0'],
      ['--serve', '--sort', 'relevance'], ['--serve', '--offset', '0'],
    ]) {
      const invalid = spawnSync(binary, ['--index', index, ...args],
        { encoding: 'utf8', timeout: 5000, maxBuffer: 1024 * 1024 });
      assert.equal(invalid.status, 1, invalid.error ?? invalid.stderr);
    }
    console.log('Web tests passed: CLI-equivalent JSON, embedded UI, content retrieval, persistent filter changes, input errors, origin/Host checks, HEAD, connection limits and header deadlines.');
  } finally {
    if (idle) idle.destroy();
    for (const socket of held) socket.destroy();
    if (child.pid && child.exitCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
      timer.unref();
      await exited;
      clearTimeout(timer);
    }
  }
}
