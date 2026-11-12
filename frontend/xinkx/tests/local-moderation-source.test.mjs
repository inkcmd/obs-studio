import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const header = read('ModerationEngine.hpp');
const engine = read('ModerationEngine.cpp');
const chat = read('ChatGateway.cpp');
const ui = read('ChatWorkspace.cpp');
const workspace = read('StudioWorkspace.cpp');
const overview = read('EngineRegistry.cpp');
const cmake = read('../cmake/feature-xinkx-studio.cmake');

// A single local filter, inserted before the unified event pipeline and bot.
assert.match(header, /class ModerationEngine final : public QObject/);
assert.match(header, /bool enabled = false;/);
assert.match(header, /bool allowChat\(/);
const start = chat.indexOf('void ChatGateway::dispatch(ChatLine line)');
assert.ok(start > 0);
const filter = chat.indexOf('moderation->allowChat(', start);
const publish = chat.indexOf('events->publish(std::move(event))', start);
assert.ok(filter > start && filter < publish, 'moderate before EventCore and automation');
assert.match(chat.slice(start, publish), /line\.type == QStringLiteral\("chat"\)/);
assert.match(workspace, /new ModerationEngine\(mainWindow\);/);
assert.ok(workspace.indexOf('new ModerationEngine(mainWindow)') < workspace.indexOf('installChatWorkspace(mainWindow)'),
  'moderation must exist before ChatGateway');
assert.match(cmake, /xinkx\/ModerationEngine\.cpp/);
assert.match(cmake, /xinkx\/ModerationEngine\.hpp/);

// Preferences are local and atomic; received chat text must not be persisted.
assert.match(engine, /QStringLiteral\("moderation\.json"\)/);
assert.match(engine, /QSaveFile file\(path\)/);
assert.match(engine, /QJsonArray::fromStringList\(normalized\.blockedPhrases\)/);
const persistence = engine.slice(engine.indexOf('bool ModerationEngine::updateConfiguration'),
  engine.indexOf('void ModerationEngine::notifyCounters'));
for (const secret of ['message', 'accessToken', 'refreshToken', 'streamKey', 'viewer']) {
  assert.ok(!persistence.includes(`QStringLiteral("${secret}")`), `${secret} must not be persisted`);
}
assert.match(engine, /constexpr int kViewerLimit = 1024/);
assert.match(engine, /constexpr qint64 kRepeatWindowMs = 15000/);
assert.match(engine, /config\.blockLinks && isLink\(message\)/);
assert.match(engine, /config\.blockRepeats && state\.previous == text/);
assert.match(engine, /config\.blockFlood/);
assert.match(engine, /if \(!config\.enabled\) return true/);

// No remote moderation API, bans, user identifiers in diagnostics or shell.
for (const forbidden of ['QNetworkAccessManager', 'ShellExecute', 'QProcess', 'banUser', 'deleteChatMessage',
  'writeDiagnostic', 'emit incoming', 'obs_sceneitem']) {
  assert.ok(!engine.includes(forbidden), `unexpected moderation side effect: ${forbidden}`);
}
assert.match(ui, /Локальная модерация чата/);
assert.match(ui, /Донаты и подписки не фильтруются/);
assert.match(ui, /moderation->updateConfiguration\(changed, &error\)/);
assert.match(overview, /Локальный фильтр: %1 скрыто/);

console.log('XINKX local moderation: opt-in, per-viewer bounded filters, EventCore guard and native UI OK');
