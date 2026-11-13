import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const read = file => readFileSync(new URL(`../${file}`, import.meta.url), 'utf8');
const extension = read('EngineExtension.hpp');
const registryH = read('EngineRegistry.hpp');
const registry = read('EngineRegistry.cpp');
const editor = read('EngineActionForm.cpp');
const automation = read('EventAutomation.cpp');
const recordingH = read('RecordingEngine.hpp');
const recording = read('RecordingEngine.cpp');
const marker = read('MarkerEngine.cpp');
const basic = read('../widgets/OBSBasic.cpp');
const cmake = read('../cmake/feature-xinkx-studio.cmake');

// V1 extension IDs and single-argument fields stay source-compatible.
for (const token of ['QString argumentKey;', 'QString argumentType;', 'QJsonObject defaults;',
  'virtual QJsonObject execute(', 'virtual void onEvent(']) assert.ok(extension.includes(token), token);
for (const token of ['struct EngineArgument', 'QList<EngineArgument> arguments;',
  'virtual bool start(', 'virtual void stop()']) assert.ok(extension.includes(token), token);
for (const token of ['shutdownExtensions()', 'void shutdownEngineServices(',
  'bool validateAction(', 'signals:', 'catalogChanged()']) assert.ok(registryH.includes(token), token);
for (const token of ['if (!extension->start(&startError))', 'shuttingDown = true;',
  'runningExtensions.clear();', 'extension->eventTypes().contains(event.type)',
  'argument.choices.contains(value.toString())', 'if (!declared) return reject(',
  'if (fromRule && !action.automatable)', 'EngineActionForm(commandsGroup)',
  'tabs->addTab(commandsPage, QStringLiteral("Действия"))']) assert.ok(registry.includes(token), token);
assert.match(basic, /shutdownEngineServices\(this\);[\s\S]*stopRecordingJobs\(this\)/);
for (const token of ['QFormLayout', 'QCheckBox', 'QComboBox', 'QSpinBox', 'QLineEdit',
  'QJsonObject EngineActionForm::command() const']) assert.ok(editor.includes(token), token);
assert.match(automation, /EngineActionForm\(&dialog\)/);
assert.match(automation, /registry->validateAction\(action, true, &actionError\)/);
assert.match(automation, /registry->executeAction\(command, true\)/);
for (const f of ['EngineActionForm.cpp', 'EngineActionForm.hpp'])
  assert.ok(cmake.includes(`xinkx/${f}`), `missing CMake ${f}`);
// Existing Record Engine gets a read-only snapshot; it is not installed again.
assert.match(recordingH, /QJsonArray activeRecordingSessions\(OBSBasic \*main\)/);
assert.match(recording, /QString outputPath;/);
assert.match(recording, /run->created.elapsed\(\)/);
assert.match(marker, /recordings = activeRecordingSessions\(window\)/);
assert.match(marker, /recording_elapsed_ms/);
assert.match(marker, /QStringLiteral\("version"\), 2/);
assert.doesNotMatch(marker, /event\.(name|message|amount|accountId)\b/);
console.log('Engine Platform v2: lifecycle, schema, shared editors and recording-linked markers wired');
