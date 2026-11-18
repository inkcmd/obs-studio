// Native visual behavior needs a Windows OBS smoke test; these are source contracts.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const read = path => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const settings = read('PresentationSettings.cpp');
const studioSettings = read('StudioSettingsPages.cpp');
const workspace = read('PresentationWorkspace.cpp');
const header = read('MarkerEngine.hpp');
const markers = read('MarkerEngine.cpp');
const runtime = read('OverlayRuntime.cpp');
const browser = read('assets/overlay.html');
const studio = read('StudioWorkspace.cpp');

// Legacy presentation.json profiles have no dedicated viewers object.
assert.match(settings, /entry\.contains\(QStringLiteral\("viewers"\)\)/);
for (const key of ['horizontal', 'vertical', 'offsetX', 'offsetY', 'fontSize',
  'textColor', 'backgroundColor', 'backgroundOpacity', 'padding', 'radius']) {
  assert.match(settings, new RegExp(`QStringLiteral\\("${key}"\\)`), key);
  assert.match(workspace, new RegExp(`viewerStyle\\.insert\\(QStringLiteral\\("${key}"\\)`), key);
}
assert.match(workspace, /QRegion\(viewersBounds\(\)\.adjusted/);
assert.match(workspace, /draggingViewers/);
assert.match(studioSettings, /add\(QStringLiteral\("Счётчик зрителей"\), viewerCounterPage\(stack, presentation\)/);
assert.match(studioSettings, /profile\.insert\(QStringLiteral\("viewers"\), options\)/);
assert.match(studioSettings, /chat\.insert\(QStringLiteral\("showViewers"\), visible->isChecked\(\)\)/);
assert.match(browser, /applyViewerAppearance\(profile\.viewers\|\|\{\}\)/);
assert.match(browser, /viewers\.style\.background/);
assert.match(browser, /viewers\.style\.transform/);
assert.match(browser, /viewers\.style\.display=viewers\.childElementCount\?'flex':'none'/);

// Only successful opted-in hotkey markers produce a local visual confirmation.
assert.match(header, /void momentMarked\(const QString &label\)/);
assert.match(markers, /if \(!add\(label, QStringLiteral\("moment"\), QStringLiteral\("local"\), true\)\) return false;[\s\S]*emit momentMarked\(label\)/);
assert.match(workspace, /MarkerEngine::momentMarked, streamer, &StreamerOverlay::showMarkerToast/);
assert.match(workspace, /Метка создана/);
assert.match(workspace, /markerToastUntil/);

// Same EventCore->AlertEngine->OverlayRuntime event feeds browser and local UI.
assert.match(runtime, /broadcast\("effect", event\);\s*emit effectGenerated\(event\)/);
assert.match(runtime, /QByteArray alertCardPng\(const QJsonObject &event\)/);
assert.match(runtime, /event\.insert\(QStringLiteral\("cardPng"\), QString::fromLatin1\(png\.toBase64\(\)\)\)/);
assert.match(runtime, /img-src 'self' data:/);
assert.match(workspace, /OverlayRuntime::effectGenerated, streamer, &StreamerOverlay::addEffect/);
assert.match(workspace, /QImage::fromData\(QByteArray::fromBase64\(cardPng\.toLatin1\(\)\), "PNG"\)/);
assert.match(workspace, /painter\.drawImage\(target, event\.card\)/);
assert.match(workspace, /pendingEvents\.push_back\(std::move\(next\)\)/);
assert.match(workspace, /pendingEvents\.takeFirst\(\)/);
assert.match(browser, /img\.src='data:image\/png;base64,'\+event\.cardPng/);
assert.match(workspace, /event\.value\(QStringLiteral\("name"\)\)/);
assert.match(workspace, /event\.value\(QStringLiteral\("amount"\)\)/);
assert.match(workspace, /event\.value\(QStringLiteral\("message"\)\)/);
for (const label of ['НОВЫЙ ФОЛЛОВЕР', 'НОВАЯ ПОДПИСКА', 'ДОНАТ', 'РЕЙД']) {
  assert.ok(workspace.includes(label), label);
  assert.ok(browser.includes(label), label);
}
assert.ok(studio.indexOf('installEngineServices(mainWindow)') < studio.indexOf('installPresentationWorkspace(mainWindow)'));

// Run the actual embedded overlay script with a small DOM stub: no network/OBS required.
class Element {
  constructor() {
    this.children = [];
    this.style = {setProperty(key, value) {this[key] = value;}};
    this.dataset = {};
    this.className = '';
    this.classList = {
      contains: key => this.className.split(/\s+/).includes(key),
      add: key => {this.className += ` ${key}`;},
      toggle: (key, value) => {if (value && !this.classList.contains(key)) this.classList.add(key);}
    };
  }
  append(...children) {this.children.push(...children);}
  replaceChildren() {this.children = [];}
  get childElementCount() {return this.children.length;}
  setAttribute() {}
  querySelector() {return null;}
  remove() {}
}
const elements = new Map();
const getElement = id => {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id);
};
const listeners = new Map();
let fakeNow = 1200;
const animationFrames = [];
class EventSourceStub {
  addEventListener(event, handler) {listeners.set(event, handler);}
}
const document = {
  body: new Element(), documentElement: new Element(),
  getElementById: getElement,
  createElement: () => new Element(),
  createElementNS: () => new Element()
};
const script = browser.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, 'browser overlay script missing');
vm.runInNewContext(script, {
  document, location: {search: ''}, URLSearchParams, EventSource: EventSourceStub,
  performance: {now: () => fakeNow}, requestAnimationFrame: callback => {
    animationFrames.push(callback); return animationFrames.length;
  },
  setTimeout: () => 1, clearTimeout: () => {}, setInterval: () => 1
});
const emit = (event, payload) => listeners.get(event)({data: JSON.stringify(payload)});
emit('state', {
  presentation: {viewer: {
    chat: {showViewers: true}, effects: {enabled: true, maxConcurrent: 2, scalePercent: 100},
    viewers: {horizontal: 'right', vertical: 'bottom', offsetX: -35, offsetY: -18,
      fontSize: 24, textColor: '#abcdef', backgroundColor: '#123456',
      backgroundOpacity: 50, padding: 12, radius: 8}
  }},
  viewers: {twitch: 17, kick: 2}
});
const viewerElement = getElement('viewers');
assert.equal(viewerElement.style.background, '#1234567f');
assert.equal(viewerElement.style.fontSize, '24px');
assert.equal(viewerElement.style.color, '#abcdef');
assert.equal(viewerElement.style.right, 'calc(3% - -35px)');
assert.equal(viewerElement.style.bottom, 'calc(3% - -18px)');
assert.equal(viewerElement.children.length, 2);
emit('effect', {kind: 'alert', type: 'sub', platform: 'twitch',
  name: 'Tester', amount: '3 месяца', message: 'Привет', durationMs: 5000});
const alert = getElement('surface').children.at(-1)?.children?.[0];
assert.equal(alert.children[0].textContent, 'НОВАЯ ПОДПИСКА');
assert.equal(alert.children[1].textContent, 'Tester');
assert.equal(alert.children[2].textContent, '3 месяца · Привет');
const cardBytes = Buffer.from('shared-alert-renderer-test').toString('base64');
emit('effect', {kind: 'alert', type: 'follow', platform: 'twitch',
  name: 'SameCard', cardPng: cardBytes, durationMs: 5000});
const cardImage = getElement('surface').children.at(-1)?.children?.[0];
assert.equal(cardImage.src, `data:image/png;base64,${cardBytes}`);
assert.match(cardImage.alt, /SameCard/);
assert.equal(Number(getElement('surface').children.at(-1).style.scale), .78);
fakeNow += 240;
for (const frame of animationFrames.splice(0)) frame();
assert.equal(Number(getElement('surface').children.at(-1).style.opacity), 1);
assert.equal(Number(getElement('surface').children.at(-1).style.scale), 1);
fakeNow += 4935;
for (const frame of animationFrames.splice(0)) frame();
assert.equal(Number(getElement('surface').children.at(-1).style.opacity), .5);
assert.equal(Number(getElement('surface').children.at(-1).style.scale), .97);
assert.equal(getElement('surface').children.at(-1)?.style?.left, undefined,
  'Native and browser alerts must follow the same scene placement');
emit('state', {presentation: {viewer: {chat: {showViewers: false}, effects: {enabled: true}}},
  viewers: {twitch: 17}});
assert.equal(viewerElement.style.display, 'none');
console.log('Viewer styles: per-profile, editable; markers: local success toast; alerts: shared event detail');
