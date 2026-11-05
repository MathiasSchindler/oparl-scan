import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { stat } from 'node:fs/promises';
import { request } from 'node:http';

const [base, binary, index] = process.argv.slice(2);
assert.ok(base && binary && index,
  'Usage: node tests/search-deployment.mjs HTTP_URL LOCAL_SEARCH_BINARY LOCAL_INDEX');
const origin = new URL(base).origin;
assert.equal(new URL(base).protocol, 'http:', 'This check targets the HTTP proxy configuration.');
const get = (path, headers = {}, method = 'GET') => new Promise((resolve, reject) => {
  const req = request(new URL(path, base), { headers, method, timeout: 10000 }, response => {
    const chunks = [];
    let bytes = 0;
    response.on('data', chunk => {
      bytes += chunk.length;
      if (bytes > 8 * 1024 * 1024) response.destroy(new Error('Response exceeds 8 MiB'));
      else chunks.push(chunk);
    });
    response.on('error', reject);
    response.on('end', () => resolve({
      status: response.statusCode, headers: response.headers,
      body: Buffer.concat(chunks).toString('utf8'),
    }));
  });
  req.on('timeout', () => req.destroy(new Error('Deployment request timed out')));
  req.on('error', reject);
  req.end();
});
const status = await get('/api/status');
assert.equal(status.status, 200);
const info = JSON.parse(status.body);
assert.equal(info.indexBytes, (await stat(index)).size);
assert.ok(info.records > 0 && info.terms > 0 && info.pages > 0);

for (const path of ['/', '/health', '/app.js']) {
  const response = await get(path);
  assert.equal(response.status, 200, path);
  assert.equal(response.headers['x-content-type-options'], 'nosniff');
  assert.match(response.headers['content-security-policy'], /script-src 'self'/);
  assert.equal(response.headers['access-control-allow-origin'], undefined);
}
let paperId;
for (const parameters of [
  { q: 'Schule OR wohn*', year: '2024', limit: '50' },
  { q: 'Schule OR wohn*', year: '2024', limit: '20', offset: '50', sort: 'date-asc' },
  { q: 'Haushalt OR Verkehr', year: '2026', kind: 'Paper', limit: '20' },
]) {
  const args = ['--index', index, '--json'];
  for (const [key, value] of Object.entries(parameters))
    if (key !== 'q') args.push(`--${key}`, value);
  args.push(parameters.q);
  const local = spawnSync(binary, args,
    { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
  assert.equal(local.status, 0, `${local.error ?? ''}\n${local.stderr}`);
  const response = await get('/api/search?' + new URLSearchParams(parameters));
  assert.equal(response.status, 200);
  assert.equal(response.body, local.stdout, 'Public API must match the local CLI exactly.');
  const result = JSON.parse(response.body);
  assert.ok(result.total > 0 && result.hits.length > 0);
  paperId ??= result.hits.find(hit => hit.relatedPaper)?.relatedPaper;
}
assert.ok(paperId, 'Corpus must contain a hit with a related Paper.');
const parameters = new URLSearchParams({ id: paperId });
const page = await get('/paper?' + parameters);
assert.equal(page.status, 200);
assert.match(page.body, /id="case-content"/);
const paper = await get('/api/paper?' + parameters);
assert.equal(paper.status, 200);
const context = JSON.parse(paper.body);
assert.equal(context.id, paperId);
assert.equal(new Set(context.objects.map(object => object.id)).size, context.objects.length);
assert.equal(context.objects.find(object => object.id === paperId)?.kind, 'Paper');
assert.ok(context.objects.some(object => object.kind === 'File'));
const object = await get('/api/object?' + parameters);
assert.equal(object.status, 200);
assert.equal(JSON.parse(object.body).id, paperId);
const head = await get('/api/paper?' + parameters, {}, 'HEAD');
assert.equal(head.status, 200);
assert.equal(head.body, '');
for (const headers of [
  { Host: 'untrusted.invalid:8080' },
  { Origin: 'https://untrusted.invalid' },
  { Origin: 'http://127.0.0.1:18080' },
  { 'Sec-Fetch-Site': 'cross-site' },
]) assert.equal((await get('/api/status', headers)).status, 403);
assert.equal((await get('/api/status', { Origin: origin, 'Sec-Fetch-Site': 'same-origin' })).status, 200);
assert.equal((await get('/', { 'Sec-Fetch-Site': 'cross-site' })).status, 200);
assert.equal((await get('/api/status', {}, 'POST')).status, 405);
assert.equal((await get('/api/status', { 'Content-Length': '0' })).status, 400);
assert.equal((await get('/api/paper?id=missing')).status, 404);
console.log(`Deployment verified: ${info.records} records, ${info.indexBytes} bytes; search matches CLI; pages, JSON and proxy guards passed.`);
