import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('./main.js', import.meta.url), 'utf8');
const classSource = source.slice(source.indexOf('class Observatory {'), source.lastIndexOf('new Observatory();'));

function createHarness() {
  const timers = new Map();
  const sockets = [];
  const requests = [];
  let timerId = 0;
  const harness = {
    timers, sockets, requests,
    fetch: async () => ({ ok: true, json: async () => ({ status: 'ok' }) }),
    ticket: async url => url,
  };
  class FakeWebSocket {
    static OPEN = 1;
    constructor(url) {
      this.url = url;
      this.readyState = 0;
      sockets.push(this);
    }
    open() {
      this.readyState = FakeWebSocket.OPEN;
      this.onopen?.();
    }
    close() {
      this.readyState = 3;
      this.onclose?.();
    }
  }
  const context = {
    window: { location: { hostname: 'localhost', origin: 'http://localhost:3000' } },
    URL, WebSocket: FakeWebSocket,
    console: { log() {} },
    AbortSignal: { timeout: milliseconds => ({ milliseconds }) },
    fetch: (url, options) => {
      requests.push({ url, timeout: options.signal.milliseconds });
      return harness.fetch(url, options);
    },
    withWsTicket: url => harness.ticket(url),
    setTimeout: (callback, delay) => {
      timers.set(++timerId, { callback, delay });
      return timerId;
    },
    clearTimeout: id => timers.delete(id),
  };
  const Observatory = vm.runInNewContext(`${classSource}\nObservatory;`, context);
  harness.observatory = Object.create(Observatory.prototype);
  Object.assign(harness.observatory, {
    settings: { dataSource: 'demo', wsUrl: '' },
    _ws: null, _liveData: null, _connectionGeneration: 0,
    _liveRetryTimer: null, _connectionTimer: null,
    _hud: { updateSourceBadge() {} },
  });
  harness.runRetry = () => {
    const entry = [...timers].find(([, timer]) => timer.delay === 3000);
    assert.ok(entry, 'a bounded reconnect timer must be scheduled');
    timers.delete(entry[0]);
    entry[1].callback();
  };
  return harness;
}

const settle = () => new Promise(resolve => setImmediate(resolve));

test('startup health detection allows time for WebGL initialization', async () => {
  const harness = createHarness();
  harness.observatory._autoDetectLive();
  await settle();
  assert.equal(harness.requests[0].timeout, 15000);
  assert.equal(harness.sockets[0].url, 'ws://localhost:3000/ws/sensing');
});

test('failed discovery retries and connects when the server becomes available', async () => {
  const harness = createHarness();
  harness.fetch = async () => { throw new Error('temporarily unavailable'); };
  harness.observatory._autoDetectLive();
  await settle();
  assert.equal(harness.sockets.length, 0);
  harness.fetch = async () => ({ ok: true, json: async () => ({ status: 'ok' }) });
  harness.runRetry();
  await settle();
  harness.sockets[0].open();
  assert.equal(harness.observatory.settings.dataSource, 'ws');
});

test('an unexpected close clears stale data and reconnects to live mode', async () => {
  const harness = createHarness();
  await harness.observatory._connectWS('ws://localhost:3000/ws/sensing');
  harness.sockets[0].open();
  harness.sockets[0].onmessage({ data: '{"source":"esp32"}' });
  harness.sockets[0].close();
  assert.equal(harness.observatory._liveData, null);
  assert.equal(harness.observatory.settings.dataSource, 'demo');
  harness.runRetry();
  await settle();
  harness.sockets[1].open();
  assert.equal(harness.observatory.settings.dataSource, 'ws');
});

test('a socket that never opens times out and retries', async () => {
  const harness = createHarness();
  await harness.observatory._connectWS('ws://localhost:3000/ws/sensing');
  const entry = [...harness.timers].find(([, timer]) => timer.delay === 15000);
  assert.ok(entry, 'a stalled handshake must have a deadline');
  harness.timers.delete(entry[0]);
  entry[1].callback();
  harness.runRetry();
  await settle();
  assert.equal(harness.sockets.length, 2);
});

test('intentional disconnection cancels automatic recovery', async () => {
  const harness = createHarness();
  await harness.observatory._connectWS('ws://localhost:3000/ws/sensing');
  harness.sockets[0].open();
  harness.sockets[0].close();
  harness.observatory._disconnectWS();
  assert.equal(harness.timers.size, 0);
  assert.equal(harness.observatory._ws, null);
});

test('selecting demo cancels an in-flight health probe', async () => {
  const harness = createHarness();
  let finish;
  harness.fetch = () => new Promise(resolve => { finish = resolve; });
  harness.observatory._autoDetectLive();
  harness.observatory._disconnectWS();
  finish({ ok: true, json: async () => ({ status: 'ok' }) });
  await settle();
  assert.equal(harness.sockets.length, 0);
  assert.equal(harness.observatory.settings.dataSource, 'demo');
});

test('a late ticket response cannot replace a newer connection', async () => {
  const harness = createHarness();
  let finish;
  harness.ticket = () => new Promise(resolve => { finish = resolve; });
  const pending = harness.observatory._connectWS('ws://old/ws/sensing');
  harness.ticket = async url => url;
  await harness.observatory._connectWS('ws://new/ws/sensing');
  finish('ws://old/ws/sensing?ticket=old');
  await pending;
  assert.equal(harness.sockets.length, 1);
  assert.equal(harness.sockets[0].url, 'ws://new/ws/sensing');
});

test('separate vital messages preserve the latest live sensing frame', async () => {
  const harness = createHarness();
  await harness.observatory._connectWS('ws://localhost:3000/ws/sensing');
  const socket = harness.sockets[0];
  socket.open();
  socket.onmessage({ data: JSON.stringify({ type: 'sensing_update', source: 'esp32', nodes: [{ node_id: 3 }], estimated_persons: 1 }) });
  socket.onmessage({ data: JSON.stringify({ type: 'edge_vitals', node_id: 3, numeric_vitals_authorized: true, heart_rate_bpm: 61, breathing_rate_bpm: 14 }) });
  assert.equal(harness.observatory._liveData.type, 'sensing_update');
  assert.equal(harness.observatory._liveData.nodes[0].node_id, 3);
  assert.equal(harness.observatory._liveData.vital_signs.heart_rate_bpm, 61);
});

test('abstained vital messages cannot introduce numeric live readings', async () => {
  const harness = createHarness();
  await harness.observatory._connectWS('ws://localhost:3000/ws/sensing');
  const socket = harness.sockets[0];
  socket.open();
  socket.onmessage({ data: JSON.stringify({ type: 'sensing_update', source: 'esp32', nodes: [{ node_id: 3 }] }) });
  socket.onmessage({ data: JSON.stringify({ type: 'edge_vitals', node_id: 3, numeric_vitals_authorized: false, heart_rate_bpm: 60 }) });
  assert.equal(harness.observatory._liveData.type, 'sensing_update');
  assert.ok(!harness.observatory._liveData.vital_signs?.heart_rate_bpm);
});

test('live diagnostics preserve both configured sensor identities and geometry', () => {
  const harness = createHarness();
  harness.observatory.settings.dataSource = 'ws';
  harness.observatory._telemetry = {
    nodes: { nodes: [{ node_id: 1, status: 'active', position_configured: true, rssi_dbm: -40 }, { node_id: 3, status: 'active', position_configured: true, rssi_dbm: -50 }] },
    mesh: { nodes: { 1: { csi_fps_ema: 30 }, 3: { csi_fps_ema: 28 } } },
    status: { trust: { node_positions: { 1: [0, -0.5, 1.2], 3: [2, -3, 0.1] } } },
  };
  assert.equal(typeof harness.observatory._getLiveDiagnostics, 'function');
  const diagnostics = harness.observatory._getLiveDiagnostics();
  assert.equal(diagnostics.nodes.length, 2);
  assert.equal(diagnostics.nodes[1].id, 3);
  assert.equal(diagnostics.nodes[1].fps, 28);
  assert.equal(JSON.stringify(diagnostics.nodes[1].position), '[2,-3,0.1]');
});

test('zero-confidence pose placeholders are not qualified live human geometry', () => {
  const harness = createHarness();
  harness.observatory.settings.dataSource = 'ws';
  harness.observatory._liveData = { persons: [{ id: 1, keypoints: [{ x: 1, y: 2, z: 3, confidence: 0 }] }] };
  assert.equal(typeof harness.observatory._getLiveDiagnostics, 'function');
  assert.equal(harness.observatory._getLiveDiagnostics().qualifiedPersons.length, 0);
});

test('diagnostics do not publish numeric vitals when the server abstains', () => {
  const harness = createHarness();
  harness.observatory.settings.dataSource = 'ws';
  harness.observatory._telemetry = { vitals: { authority: 'abstained', abstention_reason: 'calibration required', vital_signs: { heart_rate_bpm: 60 } } };
  assert.equal(typeof harness.observatory._getLiveDiagnostics, 'function');
  const diagnostics = harness.observatory._getLiveDiagnostics();
  assert.equal(diagnostics.vitalSigns, null);
  assert.equal(diagnostics.vitalReason, 'calibration required');
});

test('waiting live views do not borrow demo measurements', () => {
  const harness = createHarness();
  harness.observatory.settings.dataSource = 'ws';
  harness.observatory._demoData = { update() { throw new Error('demo generator must not run in a live view'); } };
  assert.equal(typeof harness.observatory._getRenderData, 'function');
  const frame = harness.observatory._getRenderData(0.1);
  assert.equal(frame.source, 'esp32');
  assert.equal(frame.features.mean_rssi, undefined);
  assert.equal(frame.estimated_persons, undefined);
  assert.equal(frame.persons.length, 0);
});

test('configured mesh geometry supports every size from one to six nodes', () => {
  const harness = createHarness();
  assert.equal(typeof harness.observatory._getConfiguredMeshEdges, 'function');
  for (let count = 1; count <= 6; count++) {
    const nodes = Array.from({ length: count }, (_, index) => ({
      id: index + 1, configured: true, position: [index, 0, 1.2],
    }));
    const edges = harness.observatory._getConfiguredMeshEdges({ nodes });
    assert.equal(edges.length, count * (count - 1) / 2);
    assert.ok(edges.every(edge => edge.length === 2 && edge[0].id !== edge[1].id));
  }
});

test('offline sensing status does not count as fresh mesh CSI', () => {
  const harness = createHarness();
  harness.observatory.settings.dataSource = 'ws';
  harness.observatory._consumeLiveMessage({ type: 'sensing_update', source: 'esp32:offline', nodes: [] });
  assert.equal(harness.observatory._getLiveDiagnostics().frameFresh, false);
});

test('stale mesh nodes do not report their historical rate as current CSI', () => {
  const harness = createHarness();
  harness.observatory._telemetry = {
    nodes: { nodes: [{ node_id: 1, csi_status: 'stale', status: 'stale' }] },
    mesh: { nodes: { 1: { csi_fps_ema: 20 } } },
  };
  assert.equal(harness.observatory._getLiveDiagnostics().nodes[0].fps, 0);
});