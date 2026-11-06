import assert, { AssertionError } from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, realpath, rm, stat, writeFile } from 'node:fs/promises';
import { createServer as httpServer } from 'node:http';
import { createServer as tcpServer } from 'node:net';
import { arch, cpus, platform, release, tmpdir, totalmem } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { context, createCitizenFixture, hash, id, schema, truth } from './fixtures/citizen-questions.mjs';

const protocol = '2026-07-28';

async function run(binary, args, { measure = false, allowed = [0], timeoutMs = 30000 } = {}) {
  const timed = measure && process.platform === 'darwin';
  const started = performance.now();
  const child = spawn(timed ? '/usr/bin/time' : binary,
    timed ? ['-l', '/bin/sh', '-c', 'printf "%s\\n" "$$" >&3; exec "$@"', 'sh', binary, ...args] : args,
    { stdio: timed ? ['ignore', 'pipe', 'pipe', 'pipe'] : ['ignore', 'pipe', 'pipe'] });
  let stdout = '', stderr = '', timedOut = false, measuredPid, pidText = '';
  if (timed) child.stdio[3].setEncoding('utf8').on('data', data => {
    pidText += data;
    if (/^\d+\n$/.test(pidText)) measuredPid = Number(pidText.trim());
  });
  child.stdout.setEncoding('utf8').on('data', data => { stdout += data; });
  child.stderr.setEncoding('utf8').on('data', data => { stderr += data; });
  const timer = setTimeout(() => {
    timedOut = true;
    if (measuredPid) {
      try { process.kill(measuredPid, 'SIGKILL'); }
      catch (error) { if (error.code !== 'ESRCH') console.error(error); }
    } else child.kill('SIGKILL');
  }, timeoutMs);
  try {
    const code = await new Promise((accept, reject) => {
      child.once('error', reject);
      child.once('close', accept);
    });
    if (timedOut || !allowed.includes(code))
      throw new Error(`${binary}: ${timedOut ? `${timeoutMs / 1000}-second deadline exceeded` : `exit ${code}`}\n${stderr}`);
    const rss = stderr.match(/(\d+)\s+maximum resident set size/);
    if (timed && !rss) throw new Error('macOS time did not report maximum resident set size');
    return { code, stdout, stderr, wallMs: performance.now() - started,
      peakResidentBytes: rss ? Number(rss[1]) : null };
  } finally {
    clearTimeout(timer);
  }
}

async function listen(server) {
  await new Promise((accept, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', accept);
  });
  return server.address().port;
}

async function withSearch(binary, index, use) {
  const reservation = tcpServer(), port = await listen(reservation);
  await new Promise((accept, reject) => reservation.close(error => error ? reject(error) : accept()));
  const child = spawn(binary, ['--index', index, '--serve', '--port', String(port)],
    { stdio: ['ignore', 'ignore', 'pipe'] });
  let log = '', launchError, closed = false;
  child.stderr.setEncoding('utf8').on('data', data => { log += data; });
  child.on('error', error => { launchError = error; });
  const stopped = new Promise(accept => child.once('close', () => { closed = true; accept(); }));
  const base = `http://127.0.0.1:${port}`;
  try {
    const deadline = Date.now() + 15000;
    for (;;) {
      if (launchError) throw launchError;
      if (closed) throw new Error(`Local search server exited before readiness:\n${log}`);
      try {
        const response = await fetch(base + '/api/status', { signal: AbortSignal.timeout(3000) });
        if (!response.ok) throw new Error(`Readiness HTTP ${response.status}`);
        await response.json();
        break;
      } catch (error) {
        if (error.cause?.code !== 'ECONNREFUSED' || Date.now() >= deadline) throw error;
        await delay(25);
      }
    }
    return await use(base);
  } finally {
    if (child.pid && !closed) {
      child.kill('SIGTERM');
      await Promise.race([stopped, delay(3000)]);
      if (!closed) {
        child.kill('SIGKILL');
        await stopped;
        throw new Error(`Local search server needed SIGKILL:\n${log}`);
      }
    }
  }
}

function client(base, exchanges) {
  let sequence = 0;
  const rpc = async (method, params = {}) => {
    const message = { jsonrpc: '2.0', id: ++sequence, method,
      params: { ...params, _meta: { 'io.modelcontextprotocol/protocolVersion': protocol,
        'io.modelcontextprotocol/clientCapabilities': {} } } };
    const started = performance.now();
    const response = await fetch(base + '/mcp', {
      method: 'POST', signal: AbortSignal.timeout(10000),
      headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream',
        'MCP-Protocol-Version': protocol, 'Mcp-Method': method,
        ...(method === 'tools/call' ? { 'Mcp-Name': params.name } : {}) },
      body: JSON.stringify(message),
    });
    const bytes = Buffer.from(await response.arrayBuffer());
    const json = JSON.parse(bytes.toString('utf8'));
    exchanges.push({ sequence, method, ...params, status: response.status,
      responseBodyBytes: bytes.length, responseSha256: hash(bytes),
      elapsedMs: performance.now() - started, isError: json.result?.isError === true });
    if (!response.ok || json.error || !json.result || json.id !== sequence)
      throw new Error(`MCP transport/protocol failure: HTTP ${response.status}\n${bytes.toString('utf8')}`);
    return { result: json.result, bytes: bytes.length };
  };
  return {
    rpc,
    async tool(name, args = {}) {
      const { result, bytes } = await rpc('tools/call', { name, arguments: args });
      if (result.isError || !result.structuredContent)
        throw new Error(`MCP tool ${name} failed: ${JSON.stringify(result.content)}`);
      return { data: result.structuredContent, bytes };
    },
  };
}

function traffic(exchanges) {
  return { calls: exchanges.length,
    responseBodyBytes: exchanges.reduce((sum, e) => sum + e.responseBodyBytes, 0),
    largestResponseBodyBytes: Math.max(0, ...exchanges.map(e => e.responseBodyBytes)) };
}

async function digest(path) {
  const sha = createHash('sha256');
  for await (const chunk of createReadStream(path)) sha.update(chunk);
  return sha.digest('hex');
}

async function benchmarks(binary, index, queries) {
  const measurements = [];
  for (const q of queries) {
    const result = await run(binary,
      ['--index', index, '--group', 'paper', '--sort', 'date-desc', '--benchmark', '100', q], { measure: true });
    const data = JSON.parse(result.stdout);
    if (!data.matches) throw new Error(`Benchmark query does not exercise retrieval: ${q}`);
    measurements.push({ q, ...data, peakResidentBytes: result.peakResidentBytes });
  }
  return measurements;
}

async function calendarProbe(scanner, indexer, searcher, root) {
  let base;
  const requests = [];
  const meeting = () => ({ id: base + '/meetings/independent', type: schema + 'Meeting',
    name: 'Stadtverordnetenversammlung', start: '2026-12-02T15:00:00+01:00',
    meetingState: 'eingeladen', agendaItem: [] });
  const paper = () => ({ id: base + '/papers/one', type: schema + 'Paper',
    reference: '26/SVV/0001', name: 'Unabhaengige Vorlage', date: '2026-10-01' });
  const bodyObject = () => ({ id: base + '/bodies/1', type: schema + 'Body', name: 'Testkommune',
    paper: base + '/bodies/1/paper', meeting: base + '/bodies/1/meeting' });
  const page = data => ({ data, pagination: { currentPage: 1, totalPages: 1, totalElements: data.length }, links: {} });
  const provider = httpServer((req, res) => {
    const path = new URL(req.url, base).pathname;
    requests.push(path);
    let data;
    if (path === '/oparl/system') data = { id: base + '/system', type: schema + 'System', body: base + '/bodies' };
    else if (path === '/oparl/bodies') data = page([bodyObject()]);
    else if (path === '/oparl/bodies/1') data = bodyObject();
    else if (path === '/oparl/bodies/1/paper') data = page([paper()]);
    else if (path === '/oparl/bodies/1/meeting') data = page([meeting()]);
    else if (path === '/oparl/papers/one') data = paper();
    else if (path === '/oparl/meetings/independent') data = meeting();
    else { res.writeHead(404); res.end('Unknown fixture resource'); return; }
    const body = JSON.stringify(data);
    res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
    res.end(body);
  });
  const port = await listen(provider);
  base = `http://127.0.0.1:${port}/oparl`;
  try {
    const archive = join(root, 'calendar-archive'), index = join(root, 'calendar.ops');
    await run(scanner, ['--system', base + '/system', '--output', archive, '--year', '2026',
      '--metadata-only', '--delay-ms', '0', '--timeout-ms', '1000']);
    const built = await run(indexer, ['--input', archive, '--output', index], { allowed: [0, 3] });
    const manifest = JSON.parse(await readFile(join(archive, 'archive.json'), 'utf8'));
    const coverage = (await readFile(index + '.coverage.jsonl', 'utf8')).trim().split('\n').filter(Boolean).map(JSON.parse);
    if (!manifest.metadataOnly || coverage.some(entry => entry.error))
      throw new Error('Calendar probe must be a metadata-only archive without document errors');
    const lookup = await run(searcher, ['--index', index, '--json', '--show', meeting().id], { allowed: [0, 1] });
    if (lookup.code === 1 && !lookup.stderr.includes('resource/page is not in this index'))
      throw new Error(`Unexpected calendar lookup error:\n${lookup.stderr}`);
    return { providerRequests: requests, metadataOnly: manifest.metadataOnly, indexerExit: built.code,
      found: lookup.code === 0,
      start: lookup.code === 0 ? JSON.parse(lookup.stdout).metadata.start : null,
      expectedStart: meeting().start };
  } finally {
    provider.closeAllConnections();
    await new Promise((accept, reject) => provider.close(error => error ? reject(error) : accept()));
  }
}

async function main() {
  const [indexerArg, searcherArg, scannerArg, ...args] = process.argv.slice(2);
  if (!indexerArg || !searcherArg || !scannerArg)
    throw new Error('Usage: node tests/search-acceptance.mjs INDEXER SEARCHER SCANNER [--report FILE] [--corpus-index FILE] [--corpus-archive DIR] [--citizen-report FILE]');
  const options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!['--report', '--corpus-index', '--corpus-archive', '--citizen-report'].includes(args[i]) ||
        !args[i + 1] || Object.hasOwn(options, args[i])) throw new Error('Invalid/duplicate acceptance options');
    options[args[i]] = args[i] === '--corpus-archive' ? args[i + 1] : resolve(args[i + 1]);
  }
  const binaries = await Promise.all([indexerArg, searcherArg, scannerArg].map(path => realpath(path)));
  const [indexer, searcher, scanner] = binaries;
  const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-acceptance-')));
  const report = {
    reportVersion: 1, createdAt: new Date().toISOString(), protocolVersion: protocol,
    scope: 'local synthetic semantic acceptance; no production writes',
    context, truth, hardware: { platform: platform(), architecture: arch(), release: release(),
      cpu: cpus()[0]?.model, logicalCpus: cpus().length, physicalMemoryBytes: totalmem(), node: process.version },
    binaries: [], testSources: [], checks: [], exchanges: [],
    timing: { repetitions: 100, coldDiskControlled: false,
      warmTargetP95Us: 10000, searchMemoryBudgetBytes: 4 * 1024 ** 3,
      responseBytes: 'HTTP response body bytes, including JSON-RPC and both tool content representations; excludes headers.',
      benchmark: 'Native warm retrieval excludes rendering/HTTP; RSS includes index loading and query workspace.' },
  };
  const check = async (name, definition, observe, verify) => {
    const actual = await observe();
    try {
      verify(actual);
      report.checks.push({ name, status: 'pass', definition, actual });
      console.log(`PASS ${name}`);
    } catch (error) {
      if (!(error instanceof AssertionError)) throw error;
      report.checks.push({ name, status: 'fail', definition, actual, message: error.message,
        expected: error.expected, received: error.actual });
      console.error(`FAIL ${name}: ${error.message}`);
    }
  };
  try {
    for (const binary of binaries) report.binaries.push({ path: binary, sha256: await digest(binary) });
    for (const name of ['./search-acceptance.mjs', './fixtures/citizen-questions.mjs', './pdf-fixture.mjs'])
      report.testSources.push({ path: name, sha256: await digest(new URL(name, import.meta.url)) });
    const fixture = await createCitizenFixture(root), index = join(root, 'fixture.ops');
    const built = await run(indexer, ['--input', fixture.archive, '--output', index], { measure: true });
    report.fixture = { digest: fixture.digest, objects: fixture.objects.length, documents: fixture.documents.length,
      indexBytes: (await stat(index)).size, buildWallMs: built.wallMs,
      buildPeakResidentBytes: built.peakResidentBytes, buildDiagnostics: built.stderr.split('\n').filter(l => l.startsWith('Index published:')) };
    await withSearch(searcher, index, async base => {
      const api = client(base, report.exchanges), search = async args => (await api.tool('search', { limit: 100, ...args })).data;
      const papers = data => data.hits.map(h => h.relatedPaper ?? (h.kind === 'Paper' ? h.id : null)).filter(Boolean).sort();
      report.fixture.status = (await api.tool('get_index_status')).data;
      for (const [n, q] of truth.attribution.queries.entries())
        await check(`paper-attribution-${n + 1}`, truth.attribution,
          async () => papers(await search({ q, group: 'paper', sort: 'date-desc' })),
          actual => assert.deepEqual(actual, truth.attribution.papers,
            'A meeting packet must not turn its school page into another Paper source'));
      await check('meeting-page-remains-readable', { unit: 'page', questions: ['B3'] },
        async () => (await api.tool('get_object', truth.attribution.retainedPage)).data.text,
        actual => assert.match(actual, /Schulwegsicherungskonzept/,
          'Do not fix attribution by discarding the shared source text'));
      await check('direct-shared-document', truth.shared,
        async () => papers(await search({ q: truth.shared.query, group: 'paper' })),
        actual => assert.deepEqual(actual, truth.shared.papers, 'Both direct owners must survive'));
      await check('same-owner-filter', { unit: 'paper', dateRole: 'referenceYear', questions: ['B1', 'B4'] },
        async () => (await search({ q: truth.shared.query, year: 2026, paperType: 'Antwort' })).total,
        actual => assert.equal(actual, 0, 'Year and type must not be satisfied by different owners'));
      await check('reference-year-not-document-year', { unit: 'paper', dateRole: 'referenceYear', questions: ['D2'] },
        async () => papers(await search({ q: truth.shared.query, year: 2025, group: 'paper' })),
        actual => assert.deepEqual(actual, [id('papers/joint-b')]));
      await check('meeting-start-exposed', truth.meetings,
        async () => (await search({ q: truth.meetings.query, kind: 'Meeting', sort: 'date-desc' }))
          .hits.filter(h => h.id !== truth.meetings.unknownStart).map(h => ({ id: h.id, date: h.date })),
        actual => {
          assert.equal(actual.length, 4);
          for (const hit of actual) assert.equal(hit.date, fixture.starts[hit.id.split('/').at(-1)],
            'Search dates must expose the actual Meeting.start including its timezone');
        });
      await check('meeting-start-order', truth.meetings,
        async () => (await search({ q: truth.meetings.query, kind: 'Meeting', sort: 'date-desc' })).hits.map(h => h.id),
        actual => assert.deepEqual(actual, truth.meetings.descending,
          'Order by instants across same-day times and the DST fold; unknown dates last'));
      await check('reference-year-contract', { unit: 'toolSchema', dateRole: 'referenceYear', questions: ['A1', 'D2'] },
        async () => {
          const { result } = await api.rpc('tools/list');
          const properties = result.tools.find(t => t.name === 'search').inputSchema.properties;
          return properties.referenceYear?.description ?? properties.year?.description ?? '';
        },
        actual => assert.match(actual, /reference|referenz|drucksachen/i,
          'MCP clients must be told that year is not a document/meeting calendar year'));
      await check('paper-response-budget', truth.compact,
        async () => (await api.tool('get_paper', { id: id('papers/school') })).bytes,
        actual => assert.ok(actual < truth.compact.maxBytesExclusive, `Standard response is ${actual} bytes, budget is <20000`));
      await check('paper-context-excludes-unrelated-agenda', { unit: 'agendaItem', questions: ['A2', 'E5'] },
        async () => {
          const { data } = await api.tool('get_paper', { id: id('papers/school') });
          return (JSON.stringify(data).match(/agendaItems\/unrelated-/g) ?? []).length;
        }, actual => assert.equal(actual, 0, 'Unrelated agenda items must not hide inside embedded Meeting metadata'));
      await check('conflicting-date-evidence', truth.dates, async () => {
        const raw = [], documentDates = [];
        for (const name of ['request', 'answer']) {
          raw.push((await api.tool('get_object', { id: id(`papers/${name}`) })).data.metadata.date);
          const text = (await api.tool('get_object', { id: id(`files/${name}`), page: 1 })).data.text;
          const found = text.match(/Datum (\d{2})\.(\d{2})\.(\d{4})/);
          if (!found) throw new Error('Date evidence could not be read from fixture PDF');
          documentDates.push({ text: `${found[1]}.${found[2]}.${found[3]}`,
            instant: Date.UTC(Number(found[3]), Number(found[2]) - 1, Number(found[1])) });
        }
        return { raw, documentDates: documentDates.map(d => d.text),
          documentDeltaDays: (documentDates[1].instant - documentDates[0].instant) / 86400000 };
      }, actual => {
        assert.deepEqual(actual.raw, [truth.dates.rawPaperDate, truth.dates.rawPaperDate]);
        assert.deepEqual(actual.documentDates, [truth.dates.requestDocumentDate, truth.dates.answerDocumentDate]);
        assert.equal(actual.documentDeltaDays, truth.dates.documentDeltaDays);
      });
      await check('mislabelled-role-evidence', truth.roles, async () => {
        const evidence = [];
        for (const name of ['extension', 'answer'])
          evidence.push({ rawType: (await api.tool('get_object', { id: id(`papers/${name}`) })).data.metadata.paperType,
            text: (await api.tool('get_object', { id: id(`files/${name}`), page: 1 })).data.text });
        return evidence;
      }, actual => {
        assert.equal(actual[0].rawType, truth.roles.extension.rawType);
        assert.match(actual[0].text, /Fristverlaengerungsbitte, keine Sachantwort/);
        assert.equal(actual[1].rawType, truth.roles.answer.rawType);
        assert.match(actual[1].text, /Antwort der Verwaltung/);
      });
      await check('unknown-is-not-negative', { unit: 'relationship', questions: ['A1', 'E2'],
        unknown: { meetingOccurred: null, missingPaperContent: null } }, async () => {
        const meeting = (await api.tool('get_object', { id: id('meetings/early') })).data.metadata;
        const paper = (await api.tool('get_paper', { id: id('papers/request') })).data;
        const missing = paper.objects.find(o => o.id === id('papers/not-indexed'));
        return { meetingState: meeting.meetingState, missing };
      }, actual => {
        assert.equal(actual.meetingState, 'eingeladen', 'Elapsed scheduled time is not proof of completion');
        assert.equal(actual.missing?.indexed, false);
        assert.equal(actual.missing.metadata, null, 'Missing content is unknown, not an empty known object');
      });
    });
    report.calendar = await calendarProbe(scanner, indexer, searcher, root);
    await check('independent-calendar-import', { unit: 'meeting', dateRole: 'meeting.start', questions: ['A1', 'A3'] },
      async () => report.calendar, actual => {
        assert.equal(actual.found, true, 'Annual archival must include a calendar meeting without any Paper relation');
        assert.equal(actual.start, actual.expectedStart);
      });
    report.fixture.benchmarks = await benchmarks(searcher, index, ['Schulwegsicher*', 'Radverkehr']);
    let corpus = options['--corpus-index'];
    if (options['--corpus-archive']) {
      const rebuilt = join(root, 'corpus.ops');
      const built = await run(indexer, ['--input', options['--corpus-archive'], '--output', rebuilt],
        { measure: true, allowed: [0, 3], timeoutMs: 600000 });
      const coverage = (await readFile(rebuilt + '.coverage.jsonl', 'utf8')).trim().split('\n').filter(Boolean).map(JSON.parse);
      report.corpusRebuild = { archive: resolve(options['--corpus-archive']), inputArgument: options['--corpus-archive'],
        exitCode: built.code, wallMs: built.wallMs, peakResidentBytes: built.peakResidentBytes,
        indexBytes: (await stat(rebuilt)).size, sha256: await digest(rebuilt),
        coverageRecords: coverage.length, documentErrors: coverage.filter(entry => entry.error).length,
        diagnostics: built.stderr.split('\n').filter(line => line.startsWith('Index published:')),
        outputIsTemporary: true };
      if (corpus) report.corpusRebuild.sameBytesAsExistingIndex = report.corpusRebuild.sha256 === await digest(corpus);
      else corpus = rebuilt;
    }
    if (corpus) {
      const exchanges = [];
      report.corpus = { index: corpus, sha256: await digest(corpus), indexBytes: (await stat(corpus)).size,
        buildWallMs: report.corpusRebuild &&
          (corpus === join(root, 'corpus.ops') || report.corpusRebuild.sameBytesAsExistingIndex) ?
          report.corpusRebuild.wallMs : null,
        buildNote: report.corpusRebuild ? 'Full rebuild measured separately into temporary output; original index untouched.' :
          'Existing read-only index; no full archive rebuild requested.' };
      await withSearch(searcher, corpus, async base => {
        const api = client(base, exchanges);
        report.corpus.status = (await api.tool('get_index_status')).data;
        report.corpus.samples = [];
        for (const q of ['Noosha', '"26/SVV/0736"']) {
          const { data, bytes } = await api.tool('search', { q, sort: 'date-desc', limit: 3 });
          report.corpus.samples.push({ q, total: data.total, responseBodyBytes: bytes,
            hits: data.hits.map(h => ({ id: h.id, page: h.page, relatedPaper: h.relatedPaper, reference: h.relatedReference })) });
        }
      });
      report.corpus.traffic = traffic(exchanges);
      report.corpus.exchanges = exchanges;
      report.corpus.benchmarks = await benchmarks(searcher, corpus, ['Noosha', 'Schule OR wohn*']);
      if (await digest(corpus) !== report.corpus.sha256)
        throw new Error('Corpus index changed during measurement; discard this baseline');
      report.corpus.fileHashUnchangedAfterRun = true;
    }
    if (options['--citizen-report']) {
      const source = JSON.parse(await readFile(options['--citizen-report'], 'utf8'));
      if (!source.question_status_counts || !source.run || !source.as_of)
        throw new Error('Citizen report lacks question counts, run measurements or as_of');
      report.citizenRun = { source: options['--citizen-report'], sha256: await digest(options['--citizen-report']),
        asOf: source.as_of, index: source.index, questionStatusCounts: source.question_status_counts, run: source.run,
        warning: 'Historical exploratory 25-question run, not rerun here and not comparable to synthetic fixture traffic.' };
    }
    report.traffic = traffic(report.exchanges);
    report.summary = { passed: report.checks.filter(c => c.status === 'pass').length,
      failed: report.checks.filter(c => c.status === 'fail').length };
    const measured = [report.fixture, ...(report.corpus ? [report.corpus] : [])].flatMap(scope => scope.benchmarks);
    report.performance = { warmTargetMet: measured.every(b => b.p95Us < report.timing.warmTargetP95Us),
      memoryBudgetMet: measured.every(b => b.peakResidentBytes !== null) ?
        measured.every(b => b.peakResidentBytes < report.timing.searchMemoryBudgetBytes) : null };
    console.log(`Acceptance: ${report.summary.passed} passed, ${report.summary.failed} failed; ` +
      `${report.traffic.calls} MCP calls, ${report.traffic.responseBodyBytes} response body bytes.`);
    process.exitCode = report.summary.failed || !report.performance.warmTargetMet ||
      report.performance.memoryBudgetMet === false ? 1 : 0;
  } catch (error) {
    report.fatal = { message: error.message, stack: error.stack };
    throw error;
  } finally {
    try {
      await rm(root, { recursive: true });
      report.temporaryCleanupComplete = true;
    } catch (error) {
      report.temporaryCleanupComplete = false;
      report.fatal = { message: error.message, stack: error.stack };
      throw error;
    } finally {
      if (options['--report']) {
        await mkdir(dirname(options['--report']), { recursive: true });
        await writeFile(options['--report'], JSON.stringify(report, null, 2) + '\n');
        console.log(`Report: ${options['--report']}`);
      }
    }
  }
}

await main().catch(error => {
  console.error(error.stack);
  process.exitCode = 2;
});
