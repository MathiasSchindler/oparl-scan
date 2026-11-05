import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { statSync } from 'node:fs';

const [binary, index = 'build/search/index.ops'] = process.argv.slice(2);
assert.ok(binary, 'Usage: node tests/benchmark-search.mjs SEARCH_BINARY [INDEX]');
const repetitions = 300;
const ordinary = ['--year', '2024', '--paper-type', 'Antrag'];
const cases = [
  ...['Schule', 'Haushalt', 'Verkehr', 'Wohnungen', 'Potsdam', 'und']
    .map(query => ({ category: 'word', query, filters: ordinary })),
  { category: 'word', query: 'Schule', filters: ['--year', '2024', '--organization', 'Fraktion'] },
  { category: 'phrase', query: '"Landeshauptstadt Potsdam"', filters: ordinary },
  { category: 'prefix', query: 'wohn*', filters: ordinary },
  { category: 'boolean-prefix', query: 'Schule OR wohn*', filters: ['--year', '2024'] },
];
const results = cases.map(test => {
  const args = ['--index', index, '--limit', '20', ...test.filters, '--benchmark', String(repetitions), test.query];
  const measured = process.platform === 'darwin';
  const child = spawnSync(measured ? '/usr/bin/time' : binary,
    measured ? ['-l', binary, ...args] : args,
    { encoding: 'utf8', timeout: 120000, maxBuffer: 1024 * 1024 });
  assert.equal(child.status, 0, `${test.query}: ${child.error ?? child.signal ?? ''}\n${child.stderr}`);
  const result = JSON.parse(child.stdout);
  assert.equal(result.queries, repetitions);
  assert.equal(result.limit, 20);
  assert.ok(result.matches > 0, `${test.query}: benchmark must exercise actual retrieval`);
  const rss = child.stderr.match(/(\d+)\s+maximum resident set size/);
  return { ...test, ...result, peakResidentBytes: rss ? Number(rss[1]) : null };
});
const report = {
  platform: `${process.platform}/${process.arch}`,
  index, indexBytes: statSync(index).size,
  repetitions, topK: 20,
  warmWordTargetUs: 10000,
  warmWordTargetMet: results.filter(result => result.category === 'word').every(result => result.p95Us < 10000),
  searchRamBudgetBytes: 4 * 1024 ** 3,
  physicalDiskColdControlled: false,
  timingExcludes: 'result rendering and transport; index loading and first query reported separately',
  results,
};
console.log(JSON.stringify(report, null, 2));
assert.ok(report.warmWordTargetMet, 'warm top-20 filtered word search p95 must be below 10 ms');
for (const result of results) if (result.peakResidentBytes !== null)
  assert.ok(result.peakResidentBytes < report.searchRamBudgetBytes, 'search RSS must stay below 4 GiB');
