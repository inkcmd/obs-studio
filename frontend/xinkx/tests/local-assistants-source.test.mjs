import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const source = read('LocalAssistants.cpp');
const header = read('LocalAssistants.hpp');
const workspace = read('StudioWorkspace.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');

assert.match(build, /xinkx\/LocalAssistants\.cpp/);
assert.match(build, /winhttp ole32 sapi/);
assert.match(workspace, /installAssistantWorkspace\(mainWindow\)/);
assert.match(header, /class LocalLLM final : public QObject/);
assert.match(header, /class SpeechEngine final : public QObject/);
assert.match(source, /http:\/\/127\.0\.0\.1:11434\/api\/chat/);
assert.match(source, /request\.setTransferTimeout\(120000\)/);
assert.match(source, /QStringLiteral\("stream"\), false/);
assert.match(source, /memory\.size\(\) > 12/);
assert.match(source, /!allowLive && liveOutputs\(window\)/);
assert.match(source, /automation->execute\(parsed\.object\(\)\)/);
assert.match(source, /QPushButton\(QStringLiteral\("Выполнить проверенную команду OBS"\)/);
assert.match(source, /QCheckBox\(QStringLiteral\("Озвучивать платные события/);
assert.match(header, /QThreadPool workers/); // header owns a bounded background pool
assert.match(source, /CoCreateInstance\(CLSID_SpVoice/);
assert.match(source, /pendingSpeeches\.fetch_add\(1\) >= 5/);
assert.doesNotMatch(source, /https:\/\//);

console.log('XINKX phase 4: fixed local Ollama, manual tools, and native SAPI TTS contracts OK');
