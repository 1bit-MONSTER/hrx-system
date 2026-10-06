// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generic WASI entry point for bundled wasm tests.

import {readFileSync, realpathSync, statSync} from 'node:fs';
import {basename, dirname, isAbsolute, resolve} from 'node:path';
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
const guestArguments = process.argv.slice(2);
const inputDirectories = new Map();

function resolveInputPath(candidate) {
  const candidates = [resolve(candidate)];
  if (!isAbsolute(candidate)) {
    const testSourceDirectory = process.env.TEST_SRCDIR;
    const testWorkspace = process.env.TEST_WORKSPACE;
    if (testSourceDirectory && testWorkspace) {
      candidates.push(resolve(testSourceDirectory, testWorkspace, candidate));
    }
  }
  for (const path of candidates) {
    try {
      const realPath = realpathSync(path);
      return {path: realPath, info: statSync(realPath)};
    } catch {
      // An argument may be an ordinary value instead of a file path.
    }
  }
  return null;
}

// Resolve path arguments before crossing the WASI boundary. Bazel's native
// launcher passes runfiles as workspace-relative paths, while WASI has no host
// working-directory capability unless it is explicitly preopened. Mount each
// containing directory at a short guest path instead of exposing the entire
// runfiles workspace or leaking sandbox paths into the guest command line.
for (let i = 0; i < guestArguments.length; ++i) {
  const argument = guestArguments[i];
  const separator = argument.indexOf('=');
  let candidate = separator >= 0 ? argument.slice(separator + 1) : argument;
  const responseFile = candidate.startsWith('@');
  if (responseFile) candidate = candidate.slice(1);
  if (!candidate || candidate === '-') continue;

  const resolved = resolveInputPath(candidate);
  if (resolved === null) continue;
  const hostDirectory = resolved.info.isDirectory() ?
      resolved.path : dirname(resolved.path);
  let guestDirectory = inputDirectories.get(hostDirectory);
  if (guestDirectory === undefined) {
    guestDirectory = `/iree-input-${inputDirectories.size}`;
    inputDirectories.set(hostDirectory, guestDirectory);
    preopens[guestDirectory] = hostDirectory;
  }
  const mountedPath = resolved.info.isDirectory() ?
      guestDirectory : `${guestDirectory}/${basename(resolved.path)}`;
  const guestPath = (responseFile ? '@' : '') + mountedPath;
  guestArguments[i] = separator >= 0 ?
      argument.slice(0, separator + 1) + guestPath : guestPath;
}

const wasi = new WASI({
  version: 'preview1',
  args: [__IREE_WASM_BINARY, ...guestArguments],
  env: process.env,
  preopens,
  returnOnExit: true,
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
  // Initialize both streams before changing their flags: Bazel may merge the
  // descriptors, and Node's lazy pipe handles select nonblocking mode on the
  // shared open-file description. Synchronous WASI writes need blocking output.
  // Regular-file streams have no native handle and already write synchronously.
  for (const stream of [process.stdout, process.stderr]) {
    if (stream._handle) {
      const status = stream._handle.setBlocking(true);
      if (status !== 0) {
        throw new Error(
            'Cannot make WASI descriptor ' + stream.fd +
            ' blocking: libuv status ' + status);
      }
    }
  }
  process.exitCode = wasi.start(instance);
} catch (error) {
  console.error(error);
  process.exitCode = 1;
}
