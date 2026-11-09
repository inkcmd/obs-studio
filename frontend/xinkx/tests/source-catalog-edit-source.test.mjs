import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const code = readFileSync(new URL('../OverlayRuntime.cpp', import.meta.url), 'utf8');

assert.match(code, /Применить к выделенному источнику/);
assert.match(code, /configurationFor\(catalog\.at\(index\), oldReaction\)/);
assert.match(code, /obs_source_update\(source, applied\)/);
assert.match(code, /Ширина области/);
assert.match(code, /Высота области/);
assert.match(code, /catalog\/presets/);
assert.match(code, /messageColor/);
assert.match(code, /panelColor/);

console.log('XINKX source catalog: existing native-source editing and local user presets OK');
