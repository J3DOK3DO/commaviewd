#!/usr/bin/env node
/** Register an independently published runtime; optionally promote a beta's public default. */
import { pathToFileURL } from 'node:url';

export function releaseParts(value, prefix = '') {
  const text = prefix ? value?.startsWith(prefix) && value.slice(prefix.length) : value;
  const match = /^(\d+)\.(\d+)\.(\d+)-(alpha(?:\.(\d+))?|beta)$/.exec(text || '');
  if (!match) throw new Error(`Invalid release version: ${value}`);
  const parts = match.slice(1, 4).map(Number);
  const build = match[4] === 'beta' ? 99 : Number(match[5] || 0);
  if (parts.some((n) => !Number.isSafeInteger(n)) || parts[0] > 20 || parts[1] > 99 || parts[2] > 999 ||
      (match[5] && (build < 1 || build > 98))) throw new Error(`Invalid release version: ${value}`);
  return [...parts, build];
}

export function compareRuntimeTags(left, right) {
  return compareParts(releaseParts(left, 'v'), releaseParts(right, 'v'));
}
function compareParts(left, right) {
  for (let i = 0; i < left.length; i++) if (left[i] !== right[i]) return left[i] - right[i];
  return 0;
}
function inRange(version, min, max) {
  const actual = releaseParts(version);
  return compareParts(actual, releaseParts(min)) >= 0 && compareParts(actual, releaseParts(max)) <= 0;
}

export function parseArgs(argv) {
  const out = {};
  const flags = ['dry-run', 'validate-only', 'promote-current', 'allow-runtime-downgrade'];
  const values = ['runtime-tag', 'min-app-version', 'max-app-version', 'sha256', 'publish-url'];
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i].replace(/^--/, '');
    if (!argv[i].startsWith('--') || (!flags.includes(key) && !values.includes(key))) throw new Error(`Unknown argument: ${argv[i]}`);
    if (Object.hasOwn(out, key)) throw new Error(`Duplicate argument: --${key}`);
    if (flags.includes(key)) out[key] = 'true';
    else {
      const value = argv[++i];
      if (!value || value.startsWith('--')) throw new Error(`Missing value for --${key}`);
      out[key] = value;
    }
  }
  return out;
}

export function publicationFromArgs(args) {
  const runtimeTag = args['runtime-tag'];
  releaseParts(runtimeTag, 'v');
  const minAppVersion = args['min-app-version'];
  const maxAppVersion = args['max-app-version'];
  if (!inRange(minAppVersion, minAppVersion, maxAppVersion)) throw new Error('App compatibility bounds are reversed');
  const sha256 = args.sha256?.toLowerCase();
  if (!/^[a-f0-9]{64}$/.test(sha256 || '')) throw new Error('Give --sha256 (64 hex characters)');
  if (args['promote-current'] === 'true' && !runtimeTag.endsWith('-beta')) throw new Error('Only a beta runtime may become the public current release');
  if (args['allow-runtime-downgrade'] === 'true' && args['promote-current'] !== 'true') throw new Error('--allow-runtime-downgrade requires --promote-current');
  return { runtimeTag, minAppVersion, maxAppVersion, sha256 };
}

/** audience|direct bypasses Cloudflare without changing the OIDC audience checked by the service. */
export function publishOrigins(value) {
  const https = (item) => {
    const url = new URL(item);
    if (url.protocol !== 'https:' || url.username || url.password || url.pathname !== '/' || url.search || url.hash) throw new Error(`Invalid publish URL: ${item}`);
    return url.origin;
  };
  return (value ?? '').split(',').map((item) => item.trim()).filter(Boolean).map((item) => {
    const parts = item.split('|').map((part) => part.trim());
    if (parts.length > 2 || parts.some((part) => !part)) throw new Error(`Invalid publish URL: ${item}`);
    return { audience: https(parts[0]), url: https(parts[1] || parts[0]) };
  });
}

async function idToken(fetchImpl, accessToken, account, audience) {
  const response = await fetchImpl(`https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/${account}:generateIdToken`, {
    method: 'POST', headers: { Authorization: `Bearer ${accessToken}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ audience, includeEmail: true }), signal: AbortSignal.timeout(15_000),
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok || !body.token) throw new Error(`Google refused an ID token for ${audience} (${response.status})`);
  return body.token;
}

/** Preflight every public default before any write; alpha registration never touches the manifest. */
export async function publishToAccountServices(origins, accessToken, account, body, options = {}) {
  const { fetchImpl = fetch, log = console.log, promoteCurrent = false, allowRuntimeDowngrade = false } = options;
  if (promoteCurrent) {
    if (!body.runtimeTag.endsWith('-beta')) throw new Error('Only a beta runtime may become the public current release');
    for (const origin of origins) {
      const response = await fetchImpl(`${origin.url}/api/current-release`, { headers: { Accept: 'application/json' }, signal: AbortSignal.timeout(15_000) });
      if (!response.ok) throw new Error(`GET ${origin.audience}/api/current-release failed with ${response.status}`);
      const current = await response.json();
      if (!current.appVersion || !inRange(current.appVersion, body.minAppVersion, body.maxAppVersion)) throw new Error(`Runtime is incompatible with the public app at ${origin.audience}`);
      if (current.runtimeTag && compareRuntimeTags(body.runtimeTag, current.runtimeTag) < 0 && !allowRuntimeDowngrade) {
        throw new Error(`Refusing to promote older runtime tag ${body.runtimeTag} over current ${current.runtimeTag}; use --allow-runtime-downgrade only for an intentional rollback`);
      }
    }
  }
  // Register everywhere first. Retrying a partial publication repeats immutable metadata safely.
  const authorized = [];
  for (const origin of origins) {
    const token = await idToken(fetchImpl, accessToken, account, origin.audience);
    const headers = { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' };
    const response = await fetchImpl(`${origin.url}/api/runtime-releases`, {
      method: 'POST', headers, body: JSON.stringify(body), signal: AbortSignal.timeout(15_000),
    });
    if (!response.ok) throw new Error(`Registering runtime at ${origin.audience} failed with ${response.status}`);
    authorized.push({ origin, headers });
    log(`Registered runtime ${body.runtimeTag} at ${origin.audience}`);
  }
  if (promoteCurrent) {
    const { runtimeTag, minAppVersion, maxAppVersion } = body;
    for (const { origin, headers } of authorized) {
      const response = await fetchImpl(`${origin.url}/api/current-release`, {
        method: 'PATCH', headers, body: JSON.stringify({ runtimeTag, minAppVersion, maxAppVersion }), signal: AbortSignal.timeout(15_000),
      });
      if (!response.ok) throw new Error(`Promoting runtime at ${origin.audience} failed with ${response.status}`);
      log(`Promoted public runtime ${runtimeTag} at ${origin.audience}`);
    }
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const body = publicationFromArgs(args);
  if (args['validate-only'] === 'true') return;
  const origins = publishOrigins(args['publish-url']);
  if (!origins.length) throw new Error('Give --publish-url');
  if (args['dry-run'] === 'true') {
    for (const origin of origins) console.log(`Would register at ${origin.audience}/api/runtime-releases: ${JSON.stringify(body)}`);
    if (args['promote-current'] === 'true') console.log('Would also promote the compatible beta public default after preflight');
    return;
  }
  const accessToken = process.env.GOOGLE_ACCESS_TOKEN?.trim();
  const account = process.env.RELEASE_ACCOUNT?.trim();
  if (!accessToken || !account) throw new Error('GOOGLE_ACCESS_TOKEN and RELEASE_ACCOUNT are needed (the workflow signs in keylessly)');
  await publishToAccountServices(origins, accessToken, account, body, {
    promoteCurrent: args['promote-current'] === 'true', allowRuntimeDowngrade: args["allow-runtime-downgrade"] === "true",
  });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error));
  console.error('Usage: publish-current-release.mjs --runtime-tag vX.Y.Z-alpha.N|vX.Y.Z-beta --min-app-version X.Y.Z-alpha.N --max-app-version X.Y.Z-beta --sha256 HEX --publish-url HTTPS[,HTTPS] [--promote-current] [--dry-run]');
  process.exitCode = 1;
});
