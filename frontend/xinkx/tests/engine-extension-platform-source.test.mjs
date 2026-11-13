import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const read = f => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
const registry = read('EngineRegistry.cpp');
const header = read('EngineRegistry.hpp');
const extension = read('EngineExtension.hpp');
const marker = read('MarkerEngine.cpp');
const markerHeader = read('MarkerEngine.hpp');
const automation = read('EventAutomation.cpp');
const workspace = read('StudioWorkspace.cpp');
const cmake = read('../cmake/feature-xinkx-studio.cmake');

// Existing modules are retained, not re-implemented, by the new extension API.
for (const id of ['events', 'chat', 'alerts', 'donations', 'moderation', 'automation', 'overlay', 'stream', 'recording', 'shapes', 'ai', 'speech'])
  assert.ok(registry.includes(`QStringLiteral("${id}")`), `removed existing module ${id}`);
for (const token of ['EngineExtension : public QObject', 'virtual void onEvent(const StudioEvent &)', 'virtual QJsonObject execute(', 'bool automatable = false'])
  assert.ok(extension.includes(token), token);
for (const token of ['registerExtension(EngineExtension *extension', 'actionCatalog() const', 'automatableActions() const', 'executeAction(const QJsonObject &command, bool fromRule = false)'])
  assert.ok(header.includes(token), token);
for (const token of ['EventCore::delivered, this, &EngineRegistry::deliverToExtensions', 'if (fromRule && !action.automatable)', 'QThread::currentThread() != thread()', 'QJsonDocument(command).toJson(QJsonDocument::Compact).size() > 4096', 'Команда уже зарегистрирована'])
  assert.ok(registry.includes(token), token);
for (const token of ['"scene.activate"', '"effect.trigger"', '"source.visible"', '"timer.start"'])
  assert.ok(registry.includes(`QStringLiteral(${token})`), `legacy command lost ${token}`);
assert.match(registry, /new StudioCommandsExtension\(window, registry\)/);
assert.match(registry, /new MarkerEngine\(registry\)/);
assert.match(automation, /registry->automatableActions\(\)/);
assert.match(automation, /registry->executeAction\(command, true\)/);
assert.doesNotMatch(automation, /const QStringList allowedCommands/);
assert.match(workspace, /installEngineServices\(mainWindow\)/);
assert.ok(workspace.indexOf('installEngineServices(mainWindow)') < workspace.indexOf('installEventAutomationWorkspace(mainWindow)'));
assert.match(workspace, /installMarkerWorkspace\(mainWindow\)/);
for (const f of ['EngineExtension.hpp', 'MarkerEngine.cpp', 'MarkerEngine.hpp'])
  assert.ok(cmake.includes(`xinkx/${f}`), `not compiled ${f}`);
assert.match(markerHeader, /class MarkerEngine final : public EngineExtension/);
assert.match(markerHeader, /QStringList eventTypes\(\) const override/);
assert.match(registry, /extension->eventTypes\(\)\.contains\(event.type\)/);
assert.match(marker, /if \(!autoTypes.contains\(event.type\)\) return;/);
assert.match(marker, /QSaveFile file\(path\)/);
assert.match(marker, /while \(entries.size\(\) > 1000\) entries.removeAt\(0\)/);
assert.match(marker, /tabs->addTab\(page, QStringLiteral\("Маркеры"\)\)/);
assert.match(marker, /QStringLiteral\("CSV \(\*\.csv\)"\)/);
assert.doesNotMatch(marker, /event\.(name|message|amount|accountId)\b/);
console.log('Engine Extension SDK: catalog, safe command routing, live event delivery and new markers engine wired');
