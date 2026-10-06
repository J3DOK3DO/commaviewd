import test from 'node:test';
import assert from 'node:assert/strict';
import { parseArgs, publicationFromArgs, compareRuntimeTags, publishOrigins, publishToAccountServices } from './publish-current-release.mjs';

const args = { 'runtime-tag': 'v0.0.57-alpha.2', 'min-app-version': '0.0.161-alpha.1', 'max-app-version': '0.0.161-beta', sha256: 'a'.repeat(64) };
const body = publicationFromArgs(args);
const origins = publishOrigins('https://my.commaview.com|https://prod.run.app,https://my-staging.commaview.com|https://staging.run.app');
const manifest = { appVersion: '0.0.161-beta', runtimeTag: 'v0.0.56-beta' };
const beta = { ...body, runtimeTag: 'v0.0.57-beta' };
function mockFetch({ current = () => manifest, registerStatus = () => 200 } = {}) {
  const calls = [];
  const fetchImpl = async (url, options = {}) => {
    const call = { url, method: options.method || 'GET', body: options.body && JSON.parse(options.body) };
    calls.push(call);
    if (url.includes('generateIdToken')) return Response.json({ token: 'test-token' });
    if (call.method === 'GET') return Response.json(current(url));
    return Response.json({ ok: true }, { status: call.method === 'POST' ? registerStatus(url) : 200 });
  };
  return { calls, fetchImpl, log: () => {} };
}

test('numbered alphas, legacy alpha and final beta sort correctly', () => {
  assert.ok(compareRuntimeTags('v0.0.57-alpha.2', 'v0.0.57-alpha.1') > 0);
  assert.ok(compareRuntimeTags('v0.0.57-alpha.1', 'v0.0.57-alpha') > 0);
  assert.ok(compareRuntimeTags('v0.0.57-beta', 'v0.0.57-alpha.98') > 0);
  assert.ok(compareRuntimeTags('v0.0.58-alpha.1', 'v0.0.57-beta') > 0);
});
test('registration requires exact compatibility bounds and checksum', () => {
  assert.deepEqual(publicationFromArgs(args), body);
  for (const override of [
    { 'min-app-version': undefined }, { 'max-app-version': 'latest' }, { 'max-app-version': '0.0.160-beta' },
    { 'runtime-tag': 'v0.0.57-alpha.99' }, { 'runtime-tag': 'v999999999999999999.0.0-beta' },
    { sha256: 'invalid' }, { 'promote-current': 'true' }, { 'allow-runtime-downgrade': 'true' },
  ]) assert.throws(() => publicationFromArgs({ ...args, ...override }));
});
test('unknown duplicate and missing CLI options fail closed', () => {
  for (const argv of [['--unknown'], ['--sha256'], ['--dry-run', '--dry-run']]) assert.throws(() => parseArgs(argv));
  assert.equal(parseArgs(['--validate-only'])['validate-only'], 'true');
});
test('publish origins retain audience and reject credentials or paths', () => {
  assert.equal(origins[0].audience, 'https://my.commaview.com');
  assert.equal(origins[0].url, 'https://prod.run.app');
  for (const url of ['http://example.com', 'https://u:p@example.com', 'https://example.com/path', 'https://x.com||https://y.com']) assert.throws(() => publishOrigins(url));
});
test('alpha registration calls neither the manifest nor APK release endpoints', async () => {
  const mock = mockFetch();
  await publishToAccountServices(origins, 'access', 'release@example.com', body, mock);
  assert.deepEqual(mock.calls.filter((call) => !call.url.includes('generateIdToken')), origins.map((origin) => ({ url: `${origin.url}/api/runtime-releases`, method: 'POST', body })));
});
test('beta preflights all services and registers everywhere before public promotion', async () => {
  const mock = mockFetch();
  await publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...mock, promoteCurrent: true });
  const calls = mock.calls.filter((call) => !call.url.includes('generateIdToken'));
  assert.deepEqual(calls.map((call) => call.method), ['GET', 'GET', 'POST', 'POST', 'PATCH', 'PATCH']);
  assert.deepEqual(calls[4].body, { runtimeTag: beta.runtimeTag, minAppVersion: beta.minAppVersion, maxAppVersion: beta.maxAppVersion });
});
test('an incompatible staging app stops publication before any mutation', async () => {
  const mock = mockFetch({ current: (url) => url.includes('staging') ? { ...manifest, appVersion: '0.0.160-beta' } : manifest });
  await assert.rejects(publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...mock, promoteCurrent: true }), /incompatible/);
  assert.ok(mock.calls.every((call) => call.method === 'GET'));
});
test('downgrade checks include staging and require an intentional override', async () => {
  const mock = mockFetch({ current: (url) => url.includes('staging') ? { ...manifest, runtimeTag: 'v0.0.58-alpha.1' } : manifest });
  await assert.rejects(publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...mock, promoteCurrent: true }), /older runtime tag/);
  assert.ok(mock.calls.every((call) => call.method === 'GET'));
  await publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...mock, promoteCurrent: true, allowRuntimeDowngrade: true });
});
test('a partial registry failure stops public promotion and the same metadata can be retried', async () => {
  const failing = mockFetch({ registerStatus: (url) => url.includes('staging') ? 409 : 200 });
  await assert.rejects(publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...failing, promoteCurrent: true }), /409/);
  assert.ok(failing.calls.every((call) => call.method !== 'PATCH'));
  const retry = mockFetch();
  await publishToAccountServices(origins, 'access', 'release@example.com', beta, { ...retry, promoteCurrent: true });
  assert.deepEqual(retry.calls.find((call) => call.url.endsWith('/api/runtime-releases')).body, beta);
});
