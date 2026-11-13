import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const automation = read('EventAutomation.cpp');
const alert = read('AlertEngine.cpp');
const chat = read('ChatGateway.cpp');
const workspace = read('StudioWorkspace.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');

for (const source of ['EventAutomation.cpp', 'AlertEngine.cpp'])
  assert.ok(build.includes(`xinkx/${source}`), `${source} not linked`);
assert.match(workspace, /new AlertEngine\(mainWindow\)/);
assert.match(workspace, /installEventAutomationWorkspace\(mainWindow\)/);
assert.match(automation, /EventCore::delivered, this, &EventAutomation::onEvent/);
assert.match(automation, /QSaveFile file\(path\)/);
assert.match(automation, /registry->automatableActions\(\)\.contains\(command/);
assert.match(automation, /registry->executeAction\(command, true\)/);
assert.match(automation, /nextAllowed\[id\] = now \+ rule\.value/);
assert.doesNotMatch(automation, /\b(system|ShellExecute|QProcess|CreateProcess)\s*\(/);
assert.match(alert, /emit alertReady\(event\)/);
assert.match(alert, /emit chatReady\(event\)/);
assert.match(chat, /line\.subtype = subtype/);
assert.match(chat, /line\.paid = true/);
assert.match(chat, /line\.subtype = type/);

console.log('XINKX phase 2: real events → categorized alerts and allowlisted rules OK');
