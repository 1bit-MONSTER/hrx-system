// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generic WASI entry point for bundled wasm tests.

import {readFileSync, statSync} from 'node:fs';
import {dirname, isAbsolute, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {WASI} from 'node:wasi';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const wasmPath = resolve(scriptDirectory, __IREE_WASM_BINARY);

const preopens = {};
if (process.env.TEST_TMPDIR) {
  preopens[process.env.TEST_TMPDIR] = process.env.TEST_TMPDIR;
}
if (process.env.XML_OUTPUT_FILE) {
  const xmlDirectory = dirname(process.env.XML_OUTPUT_FILE);
  preopens[xmlDirectory] = xmlDirectory;
}
// Grant the guest access only to directories containing existing path
// arguments. Relative paths share the process working directory; absolute
// paths retain their spelling so the guest can consume argv unchanged.
for (const argument of process.argv.slice(2)) {
  const separator = argument.indexOf('=');
  let candidate = separator >= 0 ? argument.slice(separator + 1) : argument;
  if (candidate.startsWith('@')) candidate = candidate.slice(1);
  if (!candidate || candidate === '-') continue;

  const hostPath = resolve(candidate);
  let pathInfo;
  try {
    pathInfo = statSync(hostPath);
  } catch {
    continue;
  }
  if (isAbsolute(candidate)) {
    const hostDirectory = pathInfo.isDirectory() ? hostPath : dirname(hostPath);
    preopens[hostDirectory] = hostDirectory;
  } else {
    preopens['.'] = process.cwd();
  }
}

const wasi = new WASI({
  version: 'preview1',
  args: [__IREE_WASM_BINARY, ...process.argv.slice(2)],
  env: process.env,
  preopens,
});

const imports = wasi.getImportObject();
const context = {
  memory: null,
};
const companionImports = createWasmImports(context);
for (const [moduleName, moduleImports] of Object.entries(companionImports)) {
  if (imports[moduleName]) {
    Object.assign(imports[moduleName], moduleImports);
  } else {
    imports[moduleName] = moduleImports;
  }
}

const wasmBytes = readFileSync(wasmPath);
const {instance} = await WebAssembly.instantiate(wasmBytes, imports);
context.memory = instance.exports.memory;

try {
  wasi.start(instance);
} catch (error) {
  if (!process.exitCode) {
    console.error(error);
    process.exitCode = 1;
  }
}
