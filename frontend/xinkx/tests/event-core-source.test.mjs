import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const header = read('EventCore.hpp');
const core = read('EventCore.cpp');
const gateway = read('ChatGateway.cpp');
const donation = read('DonationGateway.cpp');
const donationHeader = read('DonationGateway.hpp');
const donationsUI = read('DonationsWorkspace.cpp');
const overlay = read('OverlayRuntime.cpp');
const alerts = read('AlertEngine.cpp');
const workspace = read('StudioWorkspace.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');

// One serialized ingress path for real chat, follow, subscription, raid,
// YouTube paid chat events, DonationAlerts and configured donation JSON feeds.
assert.match(header, /class EventCore final : public QObject/);
assert.match(header, /bool publish\(StudioEvent event\)/);
assert.match(header, /void delivered\(const xinkx::StudioEvent &event\)/);
assert.match(gateway, /events->publish\(std::move\(event\)\)/);
assert.match(gateway, /connect\(events, &EventCore::delivered, this/);
assert.match(gateway, /emit incoming\(line\)/);
assert.ok(gateway.indexOf('emit incoming(line)') < gateway.indexOf('void ChatGateway::dispatch(ChatLine line)'),
  'legacy chat signal must be emitted from core delivery, not the provider adapter');
assert.match(donationHeader, /void donationDetailed\(const QString &provider, const QString &sourceId, const QString &eventId/);
assert.match(donation, /emit donationDetailed\(it->config.name, id, eventId/);
assert.match(donation, /emit donationDetailed\(QStringLiteral\("DonationAlerts"\), QStringLiteral\("donationalerts"\), eventId/);
assert.match(donationsUI, /&DonationGateway::donationDetailed, events/);
assert.doesNotMatch(donationsUI, /&DonationGateway::donationEvent, runtime/);
assert.match(alerts, /events, &EventCore::delivered, this/);
assert.match(overlay, /alerts, &AlertEngine::alertReady, runtime/);
assert.match(overlay, /alerts, &AlertEngine::chatReady, runtime/);
assert.match(overlay, /runtime->publishChat\(event.platform/);
assert.match(overlay, /runtime->publishAlert\(event.type/);
assert.doesNotMatch(gateway, /overlay->publishChat\(line.platform/);
assert.doesNotMatch(gateway, /overlay->publishAlert\(line.type/);
assert.match(workspace, /new EventCore\(eventsFile, mainWindow\)/);
assert.match(build, /xinkx\/EventCore\.cpp/);

// Stable external IDs are deduplicated at the core level, including across
// restarts; transient UI labels must not change the event's dedup identity.
assert.match(core, /if \(event.externalId.isEmpty\(\)\) return \{\}/);
assert.match(core, /const QStringList parts\{event.accountId, event.sourceId, event.externalId\}/);
assert.match(core, /seen.contains\(key\)/);
assert.match(core, /remember\(event\)/);
assert.match(core, /constexpr int kDedupLimit = 4096/);

// Non-blocking bounded delivery; important alerts have priority over chat
// on overflow. A retained journal is atomic and never replayed as live alerts.
assert.match(core, /constexpr int kQueueLimit = 2048/);
assert.match(core, /constexpr int kDeliveryBatch = 64/);
assert.match(core, /event.type != QStringLiteral\("chat"\)/);
assert.match(core, /QTimer::singleShot\(0, this, &EventCore::drain\)/);
assert.match(core, /emit delivered\(event\)/);
assert.match(core, /QSaveFile file\(path\)/);
assert.match(core, /history.size\(\) > kHistoryLimit/);
assert.match(core, /void EventCore::load\(\)/);
assert.doesNotMatch(core.slice(core.indexOf('void EventCore::load()'), core.indexOf('void EventCore::flush()')), /emit delivered\(/);

// Journal schema is an explicit allowlist. OAuth tokens, JSON response
// bodies, stream keys and arbitrary account credentials are never serialized.
const serialization = core.slice(core.indexOf('QJsonObject serialize('), core.indexOf('StudioEvent deserialize('));
for (const term of ['externalId', 'sourceId', 'platform', 'accountId', 'type', 'subtype', 'paid', 'name', 'message', 'amount', 'receivedAt']) {
  assert.ok(serialization.includes(`QStringLiteral("${term}")`), `event field missing: ${term}`);
}
for (const forbidden of ['accessToken', 'refreshToken', 'streamKey', 'clientSecret', 'headers', 'metadata']) {
  assert.ok(!serialization.includes(forbidden), `secret/opaque payload persisted: ${forbidden}`);
}

console.log('XINKX EventCore: unified real-event routing, dedup, bounded queue, local atomic journal OK');
