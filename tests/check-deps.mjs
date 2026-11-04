import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { open } from 'node:fs/promises';

for (const path of process.argv.slice(2)) {
  const file = await open(path);
  const magic = Buffer.alloc(4);
  try {
    await file.read(magic, 0, 4, 0);
  } finally {
    await file.close();
  }
  const run = (tool, args) => execFileSync(tool, args, { encoding: 'utf8', timeout: 10000 });
  if (magic.toString('hex') === 'cffaedfe') {
    const text = run('llvm-objdump', ['--macho', '--dylibs-used', path]);
    assert.equal(text.trim(), `${path}:`, 'Mach-O must not import any dylib');
  } else if (magic.toString('hex') === '7f454c46') {
    const text = run('llvm-readobj', ['--needed-libs', '--program-headers', path]);
    assert.match(text, /NeededLibraries \[\s*\]/, 'ELF must not need any shared library');
    assert.doesNotMatch(text, /PT_INTERP/, 'ELF must not need a dynamic loader');
  } else if (magic.subarray(0, 2).toString() === 'MZ') {
    const text = run('llvm-readobj', ['--coff-imports', path]);
    const imports = [...text.matchAll(/^\s+Name: (.+)$/gm)].map(match => match[1].toLowerCase());
    assert.ok(imports.length > 0, 'PE imports native operating system APIs');
    for (const name of imports)
      assert.ok(['kernel32.dll', 'ws2_32.dll', 'bcrypt.dll'].includes(name), `unexpected runtime import: ${name}`);
  } else {
    throw new Error(`Unknown executable format: ${path}`);
  }
  console.log(`No libc/CRT dependency: ${path}`);
}
