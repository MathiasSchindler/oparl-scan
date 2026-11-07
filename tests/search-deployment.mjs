import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFile, stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { request } from 'node:http';

const [base, binary, index] = process.argv.slice(2);
assert.ok(base && binary && index,
  'Usage: node tests/search-deployment.mjs HTTP_URL LOCAL_SEARCH_BINARY LOCAL_INDEX');
const origin = new URL(base).origin;
assert.equal(new URL(base).protocol, 'http:', 'This check targets the HTTP proxy configuration.');
const get = (path, headers = {}, method = 'GET', body) => new Promise((resolve, reject) => {
  // Synchronous CLI comparisons can outlast the proxy's idle keepalive timeout.
  const req = request(new URL(path, base), { headers, method, timeout: 10000, agent: false }, response => {
    const chunks = [];
    let bytes = 0;
    response.on('data', chunk => {
      bytes += chunk.length;
      if (bytes > 8 * 1024 * 1024) response.destroy(new Error('Response exceeds 8 MiB'));
      else chunks.push(chunk);
    });
    response.on('error', reject);
    response.on('end', () => {
      const bytes = Buffer.concat(chunks);
      resolve({ status: response.statusCode, headers: response.headers,
        body: bytes.toString('utf8'), bytes });
    });
  });
  req.on('timeout', () => req.destroy(new Error('Deployment request timed out')));
  req.on('error', reject);
  req.end(body);
});
const status = await get('/api/status');
assert.equal(status.status, 200);
const info = JSON.parse(status.body);
assert.equal(info.indexBytes, (await stat(index)).size);
assert.ok(info.records > 0 && info.terms > 0 && info.pages > 0);
const localStatus = spawnSync(binary, ['--index', index, '--status', '--json'],
  { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
assert.equal(localStatus.status, 0, `${localStatus.error ?? ''}\n${localStatus.stderr}`);
assert.deepEqual(info, JSON.parse(localStatus.stdout), 'Public service must load the complete current index.');
const snapshotId = info.snapshotId;
const preview = await get('/api/preview-status');
assert.equal(preview.status, 200);
assert.deepEqual(JSON.parse(preview.body), { enabled: true });
const renderer = await get('/pdf-crumb.js');
assert.equal(renderer.status, 200);
assert.equal(renderer.body, await readFile(new URL('../vendor/pdf-reader/dist/pdf-crumb.min.js', import.meta.url), 'utf8'));
const pdfResults = JSON.parse((await get('/api/search?' + new URLSearchParams({
  q: 'Cyber*', group: 'paper', sort: 'date-asc', limit: '50',
}))).body);
const pdfHit = pdfResults.hits.find(hit => hit.page && hit.evidence?.citation?.documentVersion?.bytes < 1048576);
assert.ok(pdfHit, 'Corpus must contain a small cited PDF for preview deployment checks.');
const pdfHash = pdfHit.evidence.citation.documentVersion.sha256;
const pdfPath = '/pdf/' + pdfHash + '.pdf';
const pdf = await get(pdfPath);
assert.equal(pdf.status, 200);
assert.equal(pdf.headers['content-type'], 'application/pdf');
assert.deepEqual(pdf.bytes, await readFile(join(dirname(index), 'web', 'pdf', pdfHash + '.pdf')));
const pdfHead = await get(pdfPath, {}, 'HEAD');
assert.equal(pdfHead.status, 200);
assert.equal(Number(pdfHead.headers['content-length']), pdf.bytes.length);
assert.equal(pdfHead.bytes.length, 0);
assert.equal((await get(pdfPath, { 'Sec-Fetch-Site': 'cross-site' })).status, 403);
assert.equal((await get(pdfPath, { Origin: origin, 'Sec-Fetch-Site': 'same-origin' })).status, 200);
assert.equal((await get('/pdf/' + '0'.repeat(64) + '.pdf')).status, 404);

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
  { q: 'Noosha', group: 'paper', sort: 'date-desc', limit: '100' },
  { q: 'Aubel', group: 'paper', sort: 'relevance', limit: '50' },
]) {
  if (parameters.offset) parameters.snapshotId = snapshotId;
  const args = ['--index', index, '--json'];
  const flags = { snapshotId: '--snapshot-id' };
  for (const [key, value] of Object.entries(parameters))
    if (key !== 'q') args.push(flags[key] || `--${key}`, value);
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
const grouped = [];
for (let offset = 0, total = 1; offset < total; offset += 100) {
  const response = await get('/api/search?' + new URLSearchParams({
    q: 'Noosha', group: 'paper', sort: 'date-desc', limit: '100', offset: String(offset),
    ...(offset ? { snapshotId } : {}),
  }));
  assert.equal(response.status, 200);
  const result = JSON.parse(response.body);
  total = result.total;
  assert.ok(total < 10000);
  grouped.push(...result.hits);
}
assert.equal(new Set(grouped.map(hit => hit.relatedPaper || hit.id)).size, grouped.length,
  'chronological Noosha results must contain no repeated Papers across pages');
const rpc = async (method, params = {}, modern = true) => {
  const message = { jsonrpc: '2.0', id: 'public-test', method, params };
  const headers = { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream',
    'MCP-Protocol-Version': modern ? '2026-07-28' : '2025-11-25' };
  if (modern) {
    params._meta = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28',
      'io.modelcontextprotocol/clientCapabilities': {} };
    headers['Mcp-Method'] = method;
    if (method === 'tools/call') headers['Mcp-Name'] = params.name;
  }
  const body = JSON.stringify(message);
  headers['Content-Length'] = String(Buffer.byteLength(body));
  const response = await get('/mcp', headers, 'POST', body);
  assert.equal(response.status, 200, response.body);
  const result = JSON.parse(response.body);
  assert.ok(result.result, response.body);
  return result.result;
};
assert.equal((await rpc('initialize', { protocolVersion: '2025-11-25',
  capabilities: {}, clientInfo: { name: 'public-test', version: '1' } }, false)).protocolVersion, '2025-11-25');
assert.ok((await rpc('server/discover')).supportedVersions.includes('2026-07-28'));
assert.deepEqual((await rpc('tools/list')).tools.map(tool => tool.name),
  ['search', 'get_object', 'get_paper', 'get_meeting', 'get_agenda',
    'get_objects', 'get_search_options', 'get_index_status']);
for (const modern of [false, true]) {
  const result = await rpc('tools/call', { name: 'search',
    arguments: { q: 'Noosha', group: 'paper', sort: 'date-desc', limit: 100 } }, modern);
  assert.equal(result.isError, false);
  assert.deepEqual(result.structuredContent.hits, grouped.slice(0, 100));
  if (!modern) assert.deepEqual(JSON.parse(result.content[0].text), result.structuredContent);
  else assert.ok(result.content[0].text.includes('structuredContent'));
}
const mcpStatus = await rpc('tools/call', { name: 'get_index_status' });
assert.deepEqual(mcpStatus.structuredContent, info);
const options = await get('/api/options?' + new URLSearchParams({ kind: 'Meeting', fields: 'name,start', limit: '1' }));
assert.equal(options.status, 200);
const meetingId = JSON.parse(options.body).objects[0]?.id;
assert.ok(meetingId, 'Corpus must contain a meeting for research-tool deployment checks.');
for (const [name, route, args] of [
  ['get_object', '/api/object', { id: paperId }],
  ['get_paper', '/api/paper', { id: paperId }],
  ['get_meeting', '/api/meeting', { id: meetingId }],
  ['get_agenda', '/api/agenda', { id: meetingId }],
  ['get_objects', '/api/objects', { ids: [paperId, meetingId] }],
  ['get_search_options', '/api/options', { kind: 'Meeting', fields: 'name,start', limit: 1 }],
]) {
  const result = await rpc('tools/call', { name, arguments: args });
  assert.equal(result.isError, false, name);
  const parameters = Object.fromEntries(Object.entries(args).map(([key, value]) =>
    [key, Array.isArray(value) ? JSON.stringify(value) : String(value)]));
  const response = await get(route + '?' + new URLSearchParams(parameters));
  assert.equal(response.status, 200, route);
  assert.deepEqual(result.structuredContent, JSON.parse(response.body), name + ' must match the JSON API');
}
const mcpGet = await get('/mcp');
assert.equal(mcpGet.status, 405);
assert.equal(mcpGet.headers.allow, 'POST');
const badOriginBody = '{"jsonrpc":"2.0","id":1,"method":"ping"}';
assert.equal((await get('/mcp', { Origin: 'https://untrusted.invalid', 'Content-Type': 'application/json',
  Accept: 'application/json, text/event-stream', 'Content-Length': String(Buffer.byteLength(badOriginBody)) },
'POST', badOriginBody)).status, 403);
console.log(`Deployment verified: ${info.records} records, ${info.indexBytes} bytes; ${grouped.length} unique Noosha groups; CLI parity, modern/legacy MCP and proxy guards passed.`);
