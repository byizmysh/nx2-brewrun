'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const {createHash} = require('node:crypto');
const [platform, nroPath, bundlePath, scratch, displayVersion] = process.argv.slice(2);
const archive = require(path.join(platform, 'core/archive.cjs'));
const tools = require(path.join(platform, 'engines/iz2s-core/tools/prepare-sd-app.cjs'));
const validator = require(path.join(platform, 'engines/iz2s-core/tools/nro-package.cjs'));
const id = 'nup-library';
const version = /^\d+\.\d+$/.test(displayVersion) ? `${displayVersion}.0` : displayVersion;
const app = {
  id, format: 'izlauncher.nro-app', abi: 1, target: 'switch2', program: 'launcher.nro',
};

async function main() {
  const nro = await fs.readFile(nroPath);
  const metadata = await validator.metadata(nro.length, (at, n) => nro.subarray(at, at + n));
  assert(validator.switch2Program(nro), 'expected a Switch 2 NRO');
  assert.equal(metadata.version, displayVersion);

  const descriptor = {
    format: 'izlauncher.module', formatVersion: 1, apiVersion: 1, id, name: metadata.name,
    version, moduleVersion: version, author: 'IZ2S',
    platforms: ['linux-x64', 'linux-arm64'],
    requires: {platformApi: 1, shell: 'izlauncher', consoleAbi: 1},
    switch: {entry: 'switch/app.json', downloadName: id + '.iza', abi: 1,
      files: ['switch/app.json', 'switch/launcher.nro']},
    dependencies: [], settingsSchema: [], license: 'MIT',
  };

  const files = [
    {name: 'module.json', bytes: Buffer.from(JSON.stringify(descriptor))},
    {name: 'switch/app.json', bytes: Buffer.from(JSON.stringify(app))},
    {name: 'switch/launcher.nro', bytes: nro},
    {name: 'README.md', bytes: await fs.readFile(path.join(__dirname, 'README.md'))},
    {name: 'LICENSE', bytes: await fs.readFile(path.join(__dirname, 'LICENSE'))},
  ];
  const checksums = Object.fromEntries(files.map(file => [file.name, archive.sha(file.bytes)]));
  files.push({name: 'checksums.json', bytes: Buffer.from(JSON.stringify({algorithm: 'sha256', files: checksums}))});
  const bytes = archive.encode(files);
  await fs.writeFile(bundlePath, bytes);

  const imported = await tools.prepare(bundlePath, scratch);
  assert.equal(imported.format, '.nro');
  assert.equal(imported.id, id);
  assert.equal(imported.version, version);
  const installed = await fs.readFile(path.join(imported.folder, 'launcher.nro'));
  assert(installed.equals(nro), 'the installed NRO differs from the standalone NRO');
  for (const file of [nroPath, bundlePath]) {
    const sum = createHash('sha256').update(await fs.readFile(file)).digest('hex');
    console.log(sum + '  ' + path.basename(file));
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
