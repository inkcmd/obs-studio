import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const header = read('EngineRegistry.hpp');
const registry = read('EngineRegistry.cpp');
const state = read('StudioState.cpp');

test('managed engines have one explicit lifecycle state and transition notification', () => {
  assert.match(header, /enum class RuntimePhase \{ Registered, Starting, Running, Stopping, Stopped, Failed \}/);
  assert.match(header, /QHash<QString, RuntimePhase> extensionPhases/);
  assert.match(header, /void lifecycleChanged\(const QString &id, const QString &phase\)/);
  for (const phase of ['Registered', 'Starting', 'Running', 'Stopping', 'Stopped', 'Failed'])
    assert.match(registry, new RegExp(`RuntimePhase::${phase}`));
  assert.match(registry, /emit lifecycleChanged\(id, phaseName\(phase\)\)/);
  assert.doesNotMatch(header, /runningExtensions/);
});

test('start/stop affect owned EngineExtensions only, in reverse shutdown order', () => {
  assert.match(registry, /extensions\.crbegin\(\)/);
  assert.match(registry, /if \(!isRunning\(id\)\) continue/);
  assert.match(registry, /transition\(id, RuntimePhase::Stopping\);[\s\S]*?\(\*it\)->stop\(\)/);
  assert.match(registry, /transition\(id, RuntimePhase::Stopped\)/);
  assert.match(registry, /transition\(id, RuntimePhase::Starting\)/);
  assert.match(registry, /extension->start\(&startError\)/);
  assert.match(registry, /if \(!started \|\| !tracked \|\| shuttingDown\)/);
  assert.match(registry, /transition\(id, shuttingDown \? RuntimePhase::Stopped : RuntimePhase::Failed\)/);
  assert.match(registry, /if \(shuttingDown\) return;/);
  // The existing modules still have only an observer registration path.
  assert.doesNotMatch(registry, /(?:core|gateway|outputs|filter)->start\(/);
  assert.doesNotMatch(registry, /(?:core|gateway|outputs|filter)->stop\(/);
});

test('event delivery and command dispatch require Running state', () => {
  assert.match(registry, /!shuttingDown && extension && isRunning\(extension->moduleId\(\)\)/);
  assert.match(registry, /if \(!isRunning\(extension->moduleId\(\)\)\)/);
  assert.match(registry, /if \(shuttingDown\) return reject\(/);
  assert.match(registry, /if \(QThread::currentThread\(\) != thread\(\)\) return result/);
});

test('legacy, on-demand and capability modules are visibly not managed instances', () => {
  assert.match(header, /enum class ModuleMode \{ Observed, OnDemand, Capability \}/);
  assert.match(registry, /EngineRegistry::ModuleMode::OnDemand/);
  assert.match(registry, /EngineRegistry::ModuleMode::Capability/);
  for (const mode of ['observed', 'managed', 'on-demand', 'capability'])
    assert.ok(registry.includes(`QStringLiteral("${mode}")`));
  for (const key of ['mode', 'lifecycle', 'health', 'available'])
    assert.ok(registry.includes(`QStringLiteral("${key}")`));
  assert.match(registry, /if \(alive && isRunning\(module.id\)\)/);
  assert.match(registry, /QObject::destroyed/);
  assert.match(registry, /status = QStringLiteral\("Наблюдается"\)/);
  assert.match(state, /registry->inventory\(\)/);
  assert.match(state, /QStringLiteral\("schemaVersion"\), 1/);
});
