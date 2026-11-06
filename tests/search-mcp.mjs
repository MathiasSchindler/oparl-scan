import assert from 'node:assert/strict';
import { connect } from 'node:net';

const modern = '2026-07-28';
const legacy = '2025-11-25';
const metadata = version => ({
  'io.modelcontextprotocol/protocolVersion': version,
  'io.modelcontextprotocol/clientCapabilities': {},
  'io.modelcontextprotocol/clientInfo': { name: 'oparl-test', version: '1' },
});

export async function testSearchMcp(base, raw, cli, fileId, port) {
  let sequence = 0;
  const post = async (message, headers = {}) => {
    const response = await fetch(base + '/mcp', {
      method: 'POST', signal: AbortSignal.timeout(8000),
      headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream',
        'MCP-Protocol-Version': legacy, ...headers },
      body: typeof message === 'string' ? message : JSON.stringify(message),
    });
    const text = await response.text();
    return { status: response.status, text, json: text ? JSON.parse(text) : null };
  };
  const rpc = (method, params = {}, current = false, headers = {}) => post({
    jsonrpc: '2.0', id: ++sequence, method,
    params: current ? { ...params, _meta: metadata(modern) } : params,
  }, current ? { 'MCP-Protocol-Version': modern, 'Mcp-Method': method,
    ...(method === 'tools/call' ? { 'Mcp-Name': params.name } : {}), ...headers } : headers);
  const snapshotId = (await (await fetch(base + '/api/status')).json()).snapshotId;
  for (const version of [legacy, '2025-06-18', '2025-03-26', '2099-01-01']) {
    const initialized = await rpc('initialize', { protocolVersion: version,
      capabilities: {}, clientInfo: { name: 'oparl-test', version: '1' } });
    assert.equal(initialized.status, 200);
    assert.equal(initialized.json.result.protocolVersion, version === '2099-01-01' ? legacy : version);
    assert.deepEqual(initialized.json.result.capabilities, { tools: {} });
  }
  const notification = await post({ jsonrpc: '2.0', method: 'notifications/initialized' });
  assert.equal(notification.status, 202);
  assert.equal(notification.text, '');
  const discovered = await rpc('server/discover', {}, true);
  assert.equal(discovered.status, 200);
  assert.equal(discovered.json.result.resultType, 'complete');
  assert.ok(discovered.json.result.supportedVersions.includes(modern));
  assert.equal(discovered.json.result._meta['io.modelcontextprotocol/serverInfo'].name, 'oparl-search');
  for (const current of [false, true]) {
    const list = await rpc('tools/list', {}, current);
    assert.equal(list.status, 200);
    assert.deepEqual(list.json.result.tools.map(tool => tool.name),
      ['search', 'get_object', 'get_paper', 'get_meeting', 'get_agenda', 'get_objects', 'get_search_options', 'get_index_status']);
    for (const tool of list.json.result.tools) {
      assert.equal(tool.annotations.readOnlyHint, true);
      assert.equal(tool.inputSchema.additionalProperties, false);
    }
    const ping = await rpc('ping', {}, current);
    assert.equal(ping.status, 200);
    assert.ok(ping.json.result);
    for (const [name, args, expected] of [
      ['search', { q: 'Schulweg', sort: 'date-desc', limit: 1, offset: 1, snapshotId },
        JSON.parse(cli(['--group', 'paper', '--sort', 'date-desc', '--limit', '1', '--offset', '1',
          '--snapshot-id', snapshotId, 'Schulweg']))],
      ['search', { q: 'Schulweg', group: 'record', year: 2024, kind: 'Page' },
        JSON.parse(cli(['--year', '2024', '--kind', 'Page', 'Schulweg']))],
      ['search', { group: 'record', kind: 'Paper', dateRole: 'paper.date', dateFrom: '2024-01-01',
        dateThrough: '2024-12-31', sort: 'date-asc' },
        JSON.parse(cli(['--kind', 'Paper', '--date-role', 'paper.date', '--date-from', '2024-01-01',
          '--date-through', '2024-12-31', '--sort', 'date-asc']))],
      ['search', { q: '', group: 'document', kind: 'Page', year: 2024 },
        JSON.parse(cli(['--group', 'document', '--kind', 'Page', '--year', '2024']))],
      ['get_object', { id: fileId, page: 2 }, JSON.parse(cli(['--show', fileId, '--page', '2']))],
      ['get_object', { id: fileId }, JSON.parse(cli(['--show', fileId]))],
      ['get_paper', { id: fileId.replace('/files/1', '/papers/1') },
        await (await fetch(base + '/api/paper?' + new URLSearchParams({ id: fileId.replace('/files/1', '/papers/1') }))).json()],
      ['get_index_status', {}, await (await fetch(base + '/api/status')).json()],
    ]) {
      const called = await rpc('tools/call', { name, arguments: args }, current);
      assert.equal(called.status, 200, called.text);
      assert.equal(called.json.result.isError, false);
      assert.deepEqual(called.json.result.structuredContent, expected);
      if (!current) assert.deepEqual(JSON.parse(called.json.result.content[0].text), expected);
      else assert.ok(called.json.result.content[0].text.includes('structuredContent'),
        'modern clients receive the full JSON once, not a second serialized copy');
    }
  }
  const id = 'Zitat " \\ 😀';
  const echoed = await post({ jsonrpc: '2.0', id, method: 'ping' });
  assert.equal(echoed.json.id, id);
  for (const invalid of ['{', '[]', '{"jsonrpc":"2.0","id":null,"method":"ping"}',
    '{"jsonrpc":"2.0","id":1,"method":"ping","method":"tools/list"}']) {
    const reply = await post(invalid);
    assert.equal(reply.status, 400);
    assert.ok(reply.json.error);
  }
  for (const args of [{ q: 'Schulweg', limit: 0 }, { q: 'Schulweg', year: '2024' },
    { q: 'Schulweg', group: 'page' }, { q: 'Schulweg', unknown: true }, { q: 'AND' },
    { q: null, kind: 'Meeting' }, { kind: 'UnknownKind' }, { kind: 'Meeting', dateRole: 'referenceYear' },
    { kind: 'Meeting', dateFrom: '2026-02-30' }, { kind: 'Meeting', dateFrom: '2026-12-02', dateThrough: '2026-11-04' },
    { dateRole: 'meeting.start', sort: 'date-desc' }]) {
    const reply = await rpc('tools/call', { name: 'search', arguments: args }, true);
    assert.equal(reply.json.result.isError, true);
    assert.ok(reply.json.result.content[0].text);
    assert.equal(reply.json.result.structuredContent, undefined);
  }
  for (const [name, args] of [['get_object', { id: 'file:///etc/passwd' }],
    ['get_object', { id: fileId, page: 0 }], ['get_paper', { id: fileId }]]) {
    assert.equal((await rpc('tools/call', { name, arguments: args })).json.result.isError, true);
  }
  assert.equal((await rpc('tools/call', { name: 'not_a_tool' })).json.error.code, -32602);
  const unknown = await rpc('not/a/method', {}, true);
  assert.equal(unknown.status, 404);
  assert.equal(unknown.json.error.code, -32601);
  const unsupported = await post({ jsonrpc: '2.0', id: 8, method: 'ping',
    params: { _meta: metadata('2099-01-01') } },
  { 'MCP-Protocol-Version': '2099-01-01', 'Mcp-Method': 'ping' });
  assert.equal(unsupported.status, 400);
  assert.equal(unsupported.json.error.code, -32022);
  assert.equal(unsupported.json.error.data.requested, '2099-01-01');
  assert.ok(unsupported.json.error.data.supported.includes(modern));
  assert.equal((await rpc('ping', {}, true, { 'Mcp-Method': 'tools/list' })).json.error.code, -32020);
  assert.equal((await rpc('tools/call', { name: 'search', arguments: { q: 'Schulweg' } }, true,
    { 'Mcp-Name': 'get_object' })).json.error.code, -32020);
  const encoded = await rpc('tools/call', { name: 'search', arguments: { q: 'Schulweg' } }, true,
    { 'Mcp-Name': `=?base64?${Buffer.from('search').toString('base64')}?=` });
  assert.equal(encoded.json.result.isError, false);
  const invalidEncoding = await rpc('tools/call', { name: 'search', arguments: { q: 'Schulweg' } }, true,
    { 'Mcp-Name': '=?base64?c2VhcmNo====?=' });
  assert.equal(invalidEncoding.json.error.code, -32020);
  const missingMeta = await post({ jsonrpc: '2.0', id: 9, method: 'ping' }, { 'MCP-Protocol-Version': modern });
  assert.equal(missingMeta.status, 400);
  assert.equal(missingMeta.json.error.code, -32602);
  for (const [headers, expected] of [
    [{ Origin: 'https://attacker.example' }, 403], [{ 'Sec-Fetch-Site': 'cross-site' }, 403],
    [{ 'Content-Type': 'text/plain' }, 415], [{ Accept: 'text/event-stream' }, 406],
    [{ Accept: 'application/json;q=0, text/event-stream' }, 406],
  ]) assert.equal((await post({ jsonrpc: '2.0', id: 10, method: 'ping' }, headers)).status, expected);
  const get = await fetch(base + '/mcp', { signal: AbortSignal.timeout(8000) });
  assert.equal(get.status, 405);
  assert.equal(get.headers.get('allow'), 'POST');
  await get.arrayBuffer();
  const host = `Host: 127.0.0.1:${port}\r\n`;
  const common = host + 'Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n';
  for (const [extra, expected] of [
    ['Content-Length: 1\r\nContent-Length: 1\r\n', 400],
    ['Content-Length: 1\r\nTransfer-Encoding: chunked\r\n', 400],
    ['Content-Length: 1\r\nExpect: 100-continue\r\n', 400],
    ['Content-Length: -1\r\n', 400], ['Content-Length: 65537\r\n', 413],
    ['Content-Length: 18446744073709551616\r\n', 400],
  ]) assert.match(await raw(`POST /mcp HTTP/1.1\r\n${common}${extra}\r\n`), new RegExp(`^HTTP/1.1 ${expected} `));
  const body = JSON.stringify({ jsonrpc: '2.0', id: 'fragmented', method: 'ping' });
  const request = `POST /mcp HTTP/1.1\r\n${common}Content-Length: ${Buffer.byteLength(body)}\r\n\r\n`;
  assert.match(await raw(request + body + `GET / HTTP/1.1\r\n${host}\r\n`), /^HTTP\/1.1 400 /);
  const fragmented = await new Promise((resolve, reject) => {
    const socket = connect(port, '127.0.0.1');
    let response = '', timer;
    socket.setEncoding('utf8');
    socket.setTimeout(8000, () => socket.destroy(new Error('fragmented MCP request timed out')));
    socket.on('data', data => { response += data; });
    socket.once('error', reject);
    socket.once('close', () => clearTimeout(timer));
    socket.once('end', () => resolve(response));
    socket.once('connect', () => {
      socket.write(request + body.slice(0, 7));
      timer = setTimeout(() => socket.write(body.slice(7)), 25);
    });
  });
  assert.match(fragmented, /^HTTP\/1.1 200 /);
  assert.equal(JSON.parse(fragmented.split('\r\n\r\n')[1]).id, 'fragmented');
  const full = body + ' '.repeat(65536 - Buffer.byteLength(body));
  assert.match(await raw(`POST /mcp HTTP/1.1\r\n${common}Content-Length: 65536\r\n\r\n${full}`), /^HTTP\/1.1 200 /);
  assert.equal((await rpc('ping')).status, 200, 'server survives rejected/fragmented/maximum-size bodies');
  console.log('MCP tests passed: modern discovery/headers, legacy initialization, eight read-only tools, JSON-RPC errors, framing, origin guards and body limits.');
}
