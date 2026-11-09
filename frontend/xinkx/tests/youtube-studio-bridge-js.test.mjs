// Run: node frontend/xinkx/tests/youtube-studio-bridge-js.test.mjs
// Mock-only tests: no browser session, external request, or video is modified.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {webcrypto} from 'node:crypto';
import vm from 'node:vm';

const source = readFileSync(new URL('../YouTubeStudioBridge.cpp', import.meta.url), 'utf8');
const scripts = [...source.matchAll(/R"JS\(([\s\S]*?)\)JS"/g)].map(match => match[1]);
assert.equal(scripts.length, 6, 'CDP script count');
for (const script of scripts) new Function(`return ${script.replaceAll('%1', '"Active Matter"')}`);

const search = scripts.find(script => script.includes('/gaming/game_title'));
assert.ok(search);
let posted;
const searchResult = await vm.runInNewContext(search.replace('%1', '"Active Matter"'), {
	window: {ytcfg: {get: key => ({
		INNERTUBE_CONTEXT: {client: {clientName: 'WEB_CREATOR', clientVersion: 'test'}},
		INNERTUBE_API_KEY: 'fake-key',
		INNERTUBE_CONTEXT_CLIENT_NAME: 62,
		INNERTUBE_CONTEXT_CLIENT_VERSION: 'test',
		SESSION_INDEX: 0
	})[key]}},
	document: {cookie: 'SAPISID=dummy-local-test-secret'},
	location: {origin: 'https://studio.youtube.com'},
	crypto: webcrypto, TextEncoder, Uint8Array, Date, encodeURIComponent,
	fetch: async (url, options) => {
		posted = {url, options};
		return {ok: true, json: async () => ({gameTitles: [
			{title: 'Active Matter', mid: '/g/11y8746zzr'}
		]})};
	}
});
assert.deepEqual(JSON.parse(JSON.stringify(searchResult.games)), [
	{title: 'Active Matter', mid: '/g/11y8746zzr'}
]);
assert.equal(JSON.parse(posted.options.body).userInput, 'Active Matter');
assert.match(posted.options.headers.Authorization, /^SAPISIDHASH \d+_[0-9a-f]{40}$/);
assert.ok(!JSON.stringify(searchResult).includes('dummy-local-test-secret'));

const save = scripts.find(script => script.includes('Поле игры отсутствует'));
assert.ok(save);
let inputEvents = 0;
let optionClicks = 0;
let saveClicks = 0;
const input = {
	value: '',
	dispatchEvent() {inputEvents++;},
	focus() {}
};
const context = {
	document: {
		querySelector(selector) {
			return selector === 'ytcp-form-gaming' ? {querySelector: () => input} : null;
		},
		querySelectorAll(selector) {
			if (selector.includes('ytcp-form-autocomplete-option'))
				return [{textContent: 'Active Matter', click() {optionClicks++;}}];
			if (selector.includes('ytcp-button'))
				return [{id: 'save-button', textContent: 'Сохранить', click() {saveClicks++;}}];
			return [];
		}
	},
	HTMLInputElement: {prototype: {get value() {}, set value(next) {this._value = next;}}},
	InputEvent: class InputEvent {},
	setTimeout(callback) {callback();}
};
const saved = await vm.runInNewContext(save.replaceAll('%1', '"Active Matter"'), context);
assert.equal(saved.clicked, true);
assert.equal(inputEvents, 1);
assert.equal(optionClicks, 1);
assert.equal(saveClicks, 1);
console.log('YouTube Studio bridge: JS parser, dynamic search/mid, UI selection/save mock OK');
