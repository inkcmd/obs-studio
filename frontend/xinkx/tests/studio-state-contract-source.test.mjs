import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';

const file = (name) => readFileSync(new URL(`../../../${name}`, import.meta.url), 'utf8');
const stateHeader = file('frontend/xinkx/StudioState.hpp');
const state = file('frontend/xinkx/StudioState.cpp');
const registry = file('frontend/xinkx/EngineRegistry.cpp');
const build = file('frontend/cmake/feature-xinkx-studio.cmake');

test('StudioState is an in-process read-only QObject with a versioned snapshot', () => {
  assert.match(stateHeader, /class StudioState final : public QObject/);
  assert.match(stateHeader, /Q_INVOKABLE QJsonObject snapshot\(\) const/);
  assert.match(state, /setObjectName\(QStringLiteral\("xinkxStudioState"\)\)/);
  assert.match(state, /QThread::currentThread\(\) != thread\(\)/);
  assert.match(state, /QStringLiteral\("schemaVersion"\), 1/);
  assert.match(build, /xinkx\/StudioState\.cpp/);
  assert.match(build, /xinkx\/StudioState\.hpp/);
});

test('the one snapshot reads existing engines without owning or restarting them', () => {
  for (const key of ['engines', 'events', 'alerts', 'stream', 'chat', 'moderation', 'shapes', 'ai', 'speech'])
    assert.ok(state.includes(`QStringLiteral("${key}")`), `missing section ${key}`);
  assert.match(state, /registry->inventory\(\)/);
  assert.match(state, /outputs->activeCount\(\)/);
  assert.match(state, /core->metrics\(\)/);
  assert.match(state, /engine->paidAlerts\(\)/);
  assert.doesNotMatch(state, /\.start\(|\.stop\(|->start\(|->stop\(|obs_source_release\(/);
  assert.doesNotMatch(state, /\.accessToken|\.streamKey|\.refreshToken|\.clientSecret/);
});

test('Overview uses a single StudioState snapshot for badges and engine catalog', () => {
  assert.match(registry, /new StudioState\(window\)/);
  assert.match(registry, /const QJsonObject snapshot = safeState->snapshot\(\)/);
  for (const key of ['events', 'alerts', 'stream', 'chat', 'moderation', 'shapes', 'ai', 'speech', 'engines'])
    assert.ok(registry.includes(`snapshot.value(QStringLiteral("${key}"))`), `Overview misses ${key}`);
  assert.match(registry, /entry.insert\(QStringLiteral\("commandIds"\), commandIds\)/);
  assert.match(registry, /row->setToolTip\(3, commands.join/);
});
