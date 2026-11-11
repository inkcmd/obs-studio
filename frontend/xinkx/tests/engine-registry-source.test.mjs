import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const header = read('EngineRegistry.hpp');
const registry = read('EngineRegistry.cpp');
const workspace = read('StudioWorkspace.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');
const upstream = read('tools/sync-obs-upstream.ps1');

assert.match(build, /xinkx\/EngineRegistry\.cpp/);
assert.match(header, /Q_INVOKABLE QJsonArray inventory\(\) const/);
for (const module of ['events', 'chat', 'alerts', 'donations', 'automation', 'overlay', 'stream', 'ai', 'speech']) {
  assert.ok(registry.includes(`QStringLiteral("${module}")`), `missing engine: ${module}`);
}
assert.match(registry, /tabs->insertTab\(0, page, QStringLiteral\("Обзор"\)\)/);
assert.match(registry, /obs_get_latest_input_type_id\("xinkx_shape"\)/);
assert.match(registry, /outputs->activeCount\(\)/);
assert.match(workspace, /installEngineHub\(mainWindow\)/);
assert.match(upstream, /\[switch\]\$Integrate/);
assert.match(upstream, /status --porcelain/);
assert.match(upstream, /merge --no-ff --no-commit/);
assert.doesNotMatch(upstream, /force-with-lease|force push|reset --hard/);
assert.doesNotMatch(upstream, /git -C \$repository push/);

console.log('XINKX phase 5: unified engine overview, capabilities and isolated upstream merge OK');
