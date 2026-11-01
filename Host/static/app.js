/* LED Strip Host - web UI logic (vanilla JS, no dependencies).
 *
 * Talks only to the local Python backend (/api/...), which in turn talks to the
 * device over the binary TCP protocol. See Host/README.md for the API list.
 */
'use strict';

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...children) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children) if (c !== null && c !== undefined) e.append(c);
  return e;
};

const PARAM_FLOAT = 0, PARAM_INT = 1, PARAM_BOOL = 2, PARAM_ENUM = 3;
const MODE_MAIN = 0, MODE_CALIB = 1, MODE_FW = 2;
const WIFI_STATES = ['off', 'soft-AP', 'station connected', 'station connecting'];

const state = {
  activeTab: 'main',
  activeSub: 'fw',
  connected: false,
  device: null,
  info: null,
  hostSettings: { poll_rate_hz: 15, default_device_ip: '', auto_connect: true },
  algorithms: [],
  calib: null,            // response of /api/calib
  selectedAlg: null,      // algorithm id, or 'all'
  calibDirty: false,
  liveFail: 0,
  liveTimer: null,
  lastBeat: null,
  fwPolling: false,
  fwRunning: false,
  diagTimer: null,
  settingsOrig: {},
  cliHistory: [],
  cliIndex: 0,
};

/* ------------------------------------------------------------------ helpers */

async function api(path, { method = 'GET', body, raw = false, headers = {} } = {}) {
  const opts = { method, headers: { ...headers } };
  if (body !== undefined) {
    if (body instanceof Blob || body instanceof ArrayBuffer) {
      opts.body = body;
      opts.headers['Content-Type'] = 'application/octet-stream';
    } else {
      opts.body = JSON.stringify(body);
      opts.headers['Content-Type'] = 'application/json';
    }
  }
  let resp;
  try {
    resp = await fetch(path, opts);
  } catch (e) {
    throw new Error('Host server not reachable (is "python -m ledhost" running?)');
  }
  if (!resp.ok) {
    let msg = `HTTP ${resp.status}`;
    try { const j = await resp.json(); if (j.error) msg = j.error; } catch (_) { /* not JSON */ }
    throw new Error(msg);
  }
  return raw ? resp : resp.json();
}

function toast(msg, kind = 'info', ms = 4000) {
  const t = el('div', { class: `toast ${kind}`, text: msg });
  $('toasts').append(t);
  setTimeout(() => t.remove(), kind === 'error' ? Math.max(ms, 6000) : ms);
}

/** Run an async action from a button, disabling it meanwhile and toasting errors. */
async function guard(btn, fn) {
  if (btn) btn.disabled = true;
  try { return await fn(); }
  catch (e) { toast(e.message, 'error'); }
  finally { if (btn) { btn.disabled = false; refreshCalibEnabled(); } }
}

function debounce(fn, ms) {
  let t = null;
  const d = (...args) => { clearTimeout(t); t = setTimeout(() => { t = null; fn(...args); }, ms); };
  d.pending = () => t !== null;
  return d;
}

/** Two-click confirmation for a button (no window.confirm). */
function armConfirm(btn, label = 'Click again to confirm') {
  if (btn.dataset.armed === '1') {
    btn.dataset.armed = '';
    btn.textContent = btn.dataset.label;
    clearTimeout(btn._armTimer);
    return true;
  }
  btn.dataset.armed = '1';
  btn.dataset.label = btn.textContent;
  btn.textContent = label;
  btn._armTimer = setTimeout(() => { btn.dataset.armed = ''; btn.textContent = btn.dataset.label; }, 3000);
  return false;
}

async function downloadFrom(url) {
  const resp = await api(url, { raw: true });
  const blob = await resp.blob();
  const cd = resp.headers.get('Content-Disposition') || '';
  const m = /filename="?([^"]+)"?/.exec(cd);
  const a = el('a', { href: URL.createObjectURL(blob), download: m ? m[1] : 'download.bin' });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

function readJsonFile(file) {
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload = () => { try { resolve(JSON.parse(r.result)); } catch (e) { reject(new Error('File is not valid JSON')); } };
    r.onerror = () => reject(new Error('Cannot read file'));
    r.readAsText(file);
  });
}

function fmtUptime(ms) {
  let s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400); s %= 86400;
  const h = Math.floor(s / 3600); s %= 3600;
  const m = Math.floor(s / 60); s %= 60;
  const p = (n) => String(n).padStart(2, '0');
  return (d ? `${d}d ` : '') + `${h}:${p(m)}:${p(s)}`;
}

function fmtNum(v) {
  if (typeof v !== 'number' || !isFinite(v)) return String(v);
  return Number.isInteger(v) ? String(v) : String(parseFloat(v.toPrecision(6)));
}

/* --------------------------------------------------------------------- tabs */

function showTab(name) {
  state.activeTab = name;
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === `tab-${name}`));
  if (!state.connected) return;
  if (name === 'calib') loadCalib();
  if (name === 'settings') loadSettings();
  if (name === 'main') loadErrors();
  updateDiagTimer();
}

function showSub(name) {
  state.activeSub = name;
  document.querySelectorAll('.subtab').forEach((b) => b.classList.toggle('active', b.dataset.sub === name));
  document.querySelectorAll('.subpanel').forEach((p) => p.classList.toggle('active', p.id === `sub-${name}`));
  if (name === 'diag' && state.connected) loadDiag();
  if (name === 'cli') $('cliInput').focus();
  updateDiagTimer();
}

/* --------------------------------------------------------- status / connect */

async function pollStatus() {
  try {
    const s = await api('/api/status');
    const was = state.connected;
    state.connected = s.connected;
    state.device = s.device;
    state.info = s.info;
    renderStatus(s);
    if (s.connected && !was) onConnected();
  } catch (e) {
    state.connected = false;
    renderStatus({ device: null, connected: false, error: e.message });
  }
}

function onConnected() {
  loadAlgorithms();
  loadErrors();
  if (state.activeTab === 'calib') loadCalib();
  if (state.activeTab === 'settings') loadSettings();
}

function renderStatus(s) {
  const pill = $('connPill');
  pill.classList.toggle('ok', s.connected);
  pill.classList.toggle('err', !s.connected && !!s.device);
  const info = s.info;
  if (s.connected && info) {
    $('connText').textContent = `${info.device_name || 'device'} (${s.device.replace(/:4210$/, '')})`;
  } else if (s.device) {
    $('connText').textContent = `Offline: ${s.device.replace(/:4210$/, '')}`;
    pill.title = s.error || '';
  } else {
    $('connText').textContent = 'Not connected';
  }
  renderInfo(s);
  renderControls(info);
  renderCalibStatus(info);
  // Admin sub-tabs depend on build flags.
  const hasCli = !!(info && info.has_cli), hasDiag = !!(info && info.has_diag);
  $('subCliBtn').hidden = !hasCli;
  $('subDiagBtn').hidden = !hasDiag;
  if ((state.activeSub === 'cli' && !hasCli) || (state.activeSub === 'diag' && !hasDiag)) {
    if (info) showSub('fw');
  }
  if (info && info.active_error_code !== state.lastErrorCode) {
    state.lastErrorCode = info.active_error_code;
    if (state.activeTab === 'main') loadErrors();
  }
}

function renderInfo(s) {
  const dl = $('infoList');
  dl.replaceChildren();
  const add = (k, v) => dl.append(el('dt', { text: k }), el('dd', { text: v }));
  const i = s.info;
  if (!i) {
    add('Status', s.device ? `Unreachable (${s.error || 'no response'})` : 'Not connected');
    return;
  }
  const flags = [];
  if (i.internal_test_build) flags.push('INTERNAL_TEST');
  if (i.has_cli) flags.push('CLI');
  if (i.has_diag) flags.push('DIAG');
  add('Name', i.device_name);
  add('Firmware', `${i.fw_version} (${i.build_date})`);
  add('IP address', i.ip_address);
  add('Mode', i.device_mode_name + (i.calib_dirty ? ' (unsaved params)' : ''));
  add('Uptime', fmtUptime(i.uptime_ms));
  add('Wi-Fi', (WIFI_STATES[i.wifi_state] || i.wifi_state) + (i.wifi_state === 2 ? `, ${i.rssi_dbm} dBm` : ''));
  add('Audio', `${i.sample_rate_hz} Hz, hop ${i.hop_size}`);
  add('LEDs', String(i.num_leds));
  add('Build flags', `0x${i.build_flags.toString(16)} ${flags.join(' ')}`);
  add('Reset reason', String(i.reset_reason));
  add('Active error', i.active_error_code ? String(i.active_error_code) : 'none');
  add('Protocol', `v${i.proto_version}`);
}

async function connectTo(ip) {
  if (!ip) { toast('Enter an IP address', 'error'); return; }
  await guard($('connectBtn'), async () => {
    const r = await api('/api/connect', { method: 'POST', body: { ip } });
    $('ipInput').value = ip;
    toast(`Connected to ${r.info.device_name || r.device}`, 'ok');
    state.connected = false;   // force onConnected() in pollStatus
    await pollStatus();
  });
}

async function scan() {
  await guard($('scanBtn'), async () => {
    $('scanBtn').textContent = 'Scanning...';
    try {
      const r = await api('/api/discover?timeout=1.5');
      const ul = $('deviceList');
      ul.replaceChildren();
      if (!r.devices.length) ul.append(el('li', { class: 'muted', text: 'No devices found. Try the manual IP (AP mode: 192.168.4.1).' }));
      for (const d of r.devices) {
        const addr = d.port === 4210 ? d.ip : `${d.ip}:${d.port}`;
        ul.append(el('li', { class: 'dev', onclick: () => connectTo(addr) },
          el('div', {}, el('div', { class: 'dev-name', text: d.name || '(unnamed)' }),
            el('div', { class: 'dev-meta', text: `${addr}  fw ${d.fw}  ${d.mac}` })),
          el('button', { class: 'small primary', text: 'Connect' })));
      }
      $('beaconNote').textContent = r.beacon_listener === 'active' ? '' : r.beacon_listener;
    } finally {
      $('scanBtn').textContent = 'Scan';
    }
  });
}

/* ------------------------------------------------------------ main controls */

async function loadAlgorithms() {
  try {
    const r = await api('/api/algorithms');
    state.algorithms = r.algorithms;
  } catch (e) {
    state.algorithms = [];
  }
  renderControls(state.info, true);
}

let runConfigBusy = false;

function renderControls(info, force = false) {
  const seg = $('runModeSeg');
  const enabled = !!info && info.device_mode !== MODE_FW;
  seg.querySelectorAll('button').forEach((b) => {
    b.disabled = !enabled;
    b.classList.toggle('active', !!info && Number(b.dataset.value) === info.run_mode);
  });
  const algSel = $('algSelect'), patSel = $('patSelect');
  algSel.disabled = patSel.disabled = $('briSlider').disabled = !enabled;
  if (!info) return;

  // Algorithm options (rebuild only when the list changes).
  const algKey = state.algorithms.map((a) => a.id + a.name).join('|') || `n${info.num_algorithms}`;
  if (force || algSel.dataset.key !== algKey) {
    algSel.dataset.key = algKey;
    algSel.replaceChildren();
    if (state.algorithms.length) {
      for (const a of state.algorithms) algSel.append(el('option', { value: a.id, text: a.name }));
    } else {
      for (let i = 0; i < info.num_algorithms; i++) algSel.append(el('option', { value: i, text: `Algorithm ${i}` }));
    }
  }
  const patKey = String(info.num_patterns);
  if (patSel.dataset.key !== patKey) {
    patSel.dataset.key = patKey;
    patSel.replaceChildren();
    for (let i = 0; i < info.num_patterns; i++) patSel.append(el('option', { value: i, text: `Pattern ${i}` }));
  }
  if (!runConfigBusy) {
    if (document.activeElement !== algSel) algSel.value = String(info.active_algorithm);
    if (document.activeElement !== patSel) patSel.value = String(info.active_pattern);
    if (document.activeElement !== $('briSlider') && !sendBrightness.pending()) {
      $('briSlider').value = info.brightness;
      $('briValue').textContent = info.brightness;
    }
  }
  const alg = state.algorithms.find((a) => a.id === info.active_algorithm);
  $('algDesc').textContent = alg ? alg.description : '';
  $('algField').style.opacity = info.run_mode === 0 ? '1' : '.55';
  $('patField').style.opacity = info.run_mode === 1 ? '1' : '.55';
}

async function setRunConfig(fields) {
  runConfigBusy = true;
  try {
    await api('/api/run_config', { method: 'POST', body: fields });
    await pollStatus();
  } catch (e) {
    toast(e.message, 'error');
  } finally {
    runConfigBusy = false;
  }
}

const sendBrightness = debounce((v) => setRunConfig({ brightness: v }), 150);

/* ---------------------------------------------------------------- live view */

function ensureChildren(container, n, cls) {
  if (container.childElementCount !== n) {
    container.replaceChildren();
    for (let i = 0; i < n; i++) container.append(el('div', { class: cls }));
  }
  return container.children;
}

function renderLive(d) {
  const strip = $('strip');
  strip.style.setProperty('--n', String(Math.max(1, d.num_leds)));
  const leds = ensureChildren(strip, d.num_leds, 'led');
  for (let i = 0; i < d.num_leds; i++) {
    const hex = d.leds.substr(i * 6, 6);
    leds[i].style.background = hex === '000000' ? '#151515' : `#${hex}`;
  }
  $('levelFill').style.width = `${(100 * d.level / 32767).toFixed(1)}%`;
  $('centroidMark').style.left = `calc(${(100 * d.centroid / 32767).toFixed(1)}% - 2px)`;
  $('bpmValue').textContent = d.bpm > 0 ? d.bpm.toFixed(1) : '--';
  $('beatCount').textContent = `beats: ${d.beat_count}`;
  if (state.lastBeat !== null && d.beat_count !== state.lastBeat) flashBeat();
  state.lastBeat = d.beat_count;
  const bands = ensureChildren($('bands'), d.bands.length, 'band');
  d.bands.forEach((v, i) => { bands[i].style.height = `${(100 * v / 32767).toFixed(1)}%`; });
}

function flashBeat() {
  const f = $('beatFlash'), dot = $('beatDot');
  f.classList.add('on'); dot.classList.add('beat');
  setTimeout(() => { f.classList.remove('on'); dot.classList.remove('beat'); }, 90);
}

async function liveLoop() {
  clearTimeout(state.liveTimer);
  const active = state.activeTab === 'main' && $('liveToggle').checked && state.connected
    && !state.fwRunning && document.visibilityState === 'visible'
    && !(state.info && state.info.device_mode === MODE_FW);
  if (active) {
    try {
      renderLive(await api('/api/live'));
      state.liveFail = 0;
      $('liveNote').textContent = '';
    } catch (e) {
      state.liveFail++;
      $('liveNote').textContent = `Live data unavailable: ${e.message}`;
    }
  }
  const hz = Number(state.hostSettings.poll_rate_hz) || 15;
  state.liveTimer = setTimeout(liveLoop, state.liveFail ? 1000 : Math.max(33, 1000 / hz));
}

/* ------------------------------------------------------------------- errors */

async function loadErrors() {
  if (!state.connected) return;
  try {
    const r = await api('/api/errors');
    const tb = $('errTable').tBodies[0];
    tb.replaceChildren();
    if (!r.errors.length) tb.append(el('tr', {}, el('td', { colspan: 5, class: 'muted', text: 'No errors recorded' })));
    for (const e of r.errors) {
      tb.append(el('tr', {},
        el('td', { class: 'num', text: e.code }), el('td', { text: e.name }),
        el('td', { class: `sev-${e.severity}`, text: e.severity_name }),
        el('td', { class: 'num', text: e.occurrences }), el('td', { class: 'num', text: fmtUptime(e.last_uptime_ms) })));
    }
  } catch (e) {
    toast(`Errors: ${e.message}`, 'error');
  }
}

/* -------------------------------------------------------------- calibration */

function inCalibMode() {
  return !!state.info && state.info.device_mode === MODE_CALIB;
}

function renderCalibStatus(info) {
  const badge = $('calibModeBadge'), btn = $('calibModeBtn');
  const calib = !!info && info.device_mode === MODE_CALIB;
  badge.textContent = info ? info.device_mode_name : 'OFFLINE';
  badge.classList.toggle('calib', calib);
  state.calibDirty = !!info && !!info.calib_dirty;
  $('dirtyBadge').hidden = !state.calibDirty;
  btn.disabled = !info || info.device_mode === MODE_FW;
  btn.textContent = calib ? (state.calibDirty ? 'Exit calibration (discard unsaved)' : 'Exit calibration mode')
    : 'Enter calibration mode';
  btn.classList.toggle('primary', !calib);
  refreshCalibEnabled();
}

function refreshCalibEnabled() {
  const on = inCalibMode() && state.connected;
  document.querySelectorAll('.needs-calib').forEach((b) => { b.disabled = !on; });
  document.querySelectorAll('#stageArea input, #stageArea select, #stageArea button').forEach((c) => { c.disabled = !on; });
  $('calibExportBtn').disabled = !state.connected;
}

async function loadCalib() {
  try {
    state.calib = await api('/api/calib');
  } catch (e) {
    $('stageArea').replaceChildren(el('div', { class: 'card muted', text: `Cannot load calibration data: ${e.message}` }));
    return;
  }
  if (state.selectedAlg === null || (state.selectedAlg !== 'all'
      && !state.calib.algorithms.some((a) => a.id === state.selectedAlg))) {
    state.selectedAlg = state.calib.active_algorithm;
  }
  renderAlgList();
  renderStages();
}

function renderAlgList() {
  const ul = $('algList');
  ul.replaceChildren();
  const c = state.calib;
  for (const a of c.algorithms) {
    ul.append(el('li', {
      class: a.id === state.selectedAlg ? 'active' : '', title: a.description,
      onclick: () => { state.selectedAlg = a.id; renderAlgList(); renderStages(); },
    }, a.name, a.id === c.active_algorithm ? el('span', { class: 'alg-live', text: 'active' }) : null,
    el('span', { class: 'alg-stages', text: `stages ${a.stage_ids.join(' → ')}` })));
  }
  ul.append(el('li', {
    class: state.selectedAlg === 'all' ? 'active' : '',
    onclick: () => { state.selectedAlg = 'all'; renderAlgList(); renderStages(); },
  }, 'All stages', el('span', { class: 'alg-stages', text: `${c.stages.length} stages` })));
}

function renderStages() {
  const area = $('stageArea');
  area.replaceChildren();
  const c = state.calib;
  const byId = new Map(c.stages.map((s) => [s.id, s]));
  let ids;
  if (state.selectedAlg === 'all') ids = c.stages.map((s) => s.id);
  else ids = (c.algorithms.find((a) => a.id === state.selectedAlg) || { stage_ids: [] }).stage_ids;
  ids.forEach((sid, idx) => {
    const st = byId.get(sid);
    if (!st) { area.append(el('div', { class: 'card muted', text: `Stage ${sid}: no schema` })); return; }
    if (idx > 0 && state.selectedAlg !== 'all') area.append(el('div', { class: 'pipeline-arrow', text: '↓' }));
    area.append(makeStageCard(st));
  });
  if (!ids.length) area.append(el('div', { class: 'card muted', text: 'This algorithm has no stages.' }));
  refreshCalibEnabled();
}

function makeStageCard(st) {
  const resetBtn = el('button', { class: 'small danger', text: 'Reset stage' });
  resetBtn.addEventListener('click', () => {
    if (!armConfirm(resetBtn, 'Confirm reset')) return;
    guard(resetBtn, async () => {
      await api('/api/params/reset', { method: 'POST', body: { stage: st.id } });
      toast(`Stage "${st.name}" reset to defaults (not saved)`, 'ok');
      await afterCalibChange();
    });
  });
  const card = el('div', { class: 'card' },
    el('div', { class: 'stage-head' },
      el('div', {}, el('h2', {}, el('span', { class: 'stage-id', text: `#${st.id}` }), st.name),
        el('p', { class: 'stage-desc', text: st.description })),
      resetBtn));
  if (!st.params.length) card.append(el('p', { class: 'muted', text: 'No parameters.' }));
  for (const p of st.params) card.append(makeParamRow(st, p));
  return card;
}

function makeParamRow(st, p) {
  const value = st.values[p.id];
  const stateLine = el('div', { class: 'param-state' });
  const row = el('div', { class: 'param' });
  const unitText = p.type === PARAM_ENUM ? '' : p.unit;
  const range = p.type === PARAM_FLOAT || p.type === PARAM_INT ? `${fmtNum(p.min)} .. ${fmtNum(p.max)}` : '';
  row.append(el('div', { class: 'param-name', title: `stage ${st.id}, param ${p.id}` }, p.name,
    el('small', { text: [unitText, range, `default ${fmtNum(p.default)}`].filter(Boolean).join(' · ') })));

  let setDisplay;             // (value) => update editor without sending
  let getValue;
  const key = `${st.id}:${p.id}`;
  let reqSeq = 0;

  const send = debounce(async () => {
    const v = getValue();
    const mySeq = ++reqSeq;
    stateLine.className = 'param-state';
    stateLine.textContent = 'applying...';
    try {
      const r = await api('/api/params', { method: 'POST', body: { stage: st.id, param: p.id, value: v } });
      if (mySeq !== reqSeq) return;          // a newer request superseded this one
      st.values[p.id] = r.applied;
      const clamped = Math.abs(r.applied - v) > Math.max(1e-6, Math.abs(v) * 1e-6);
      setDisplay(r.applied, true);
      stateLine.className = `param-state${clamped ? ' clamped' : ''}`;
      stateLine.textContent = clamped ? `device clamped to ${fmtNum(r.applied)}` : `applied ${fmtNum(r.applied)}`;
      row.classList.toggle('changed', r.applied !== p.default);
      if (!state.calibDirty) { state.calibDirty = true; $('dirtyBadge').hidden = false; }
    } catch (e) {
      if (mySeq !== reqSeq) return;
      stateLine.className = 'param-state err';
      stateLine.textContent = e.message;
    }
  }, 100);

  if (p.type === PARAM_BOOL) {
    const cb = el('input', { type: 'checkbox' });
    cb.checked = value >= 0.5;
    cb.addEventListener('change', send);
    getValue = () => (cb.checked ? 1 : 0);
    setDisplay = (v) => { cb.checked = v >= 0.5; };
    row.append(el('label', { class: 'toggle' }, cb, el('span')), el('div', { class: 'muted', text: '' }));
  } else if (p.type === PARAM_ENUM) {
    const sel = el('select');
    const opts = p.options && p.options.length ? p.options : [];
    const n = Math.max(opts.length, Math.round(p.max) + 1);
    for (let i = Math.max(0, Math.round(p.min)); i < n; i++) sel.append(el('option', { value: i, text: opts[i] || `#${i}` }));
    sel.value = String(Math.round(value));
    sel.addEventListener('change', send);
    getValue = () => Number(sel.value);
    setDisplay = (v) => { sel.value = String(Math.round(v)); };
    row.append(sel, el('div', { class: 'muted', text: '' }));
  } else {
    let step = p.step > 0 ? p.step : (p.type === PARAM_INT ? 1 : (p.max - p.min) / 1000 || 0.001);
    if (p.type === PARAM_INT) step = Math.max(1, Math.round(step));
    const attrs = { min: p.min, max: p.max, step };
    const slider = el('input', { type: 'range', ...attrs });
    const num = el('input', { type: 'number', ...attrs });
    slider.value = value; num.value = fmtNum(value);
    slider.addEventListener('input', () => { num.value = slider.value; send(); });
    num.addEventListener('input', () => {
      if (num.value === '' || isNaN(Number(num.value))) return;
      slider.value = num.value; send();
    });
    getValue = () => {
      const v = Number(num.value);
      return p.type === PARAM_INT ? Math.round(v) : v;
    };
    setDisplay = (v, fromDevice) => {
      slider.value = v;
      if (!(fromDevice && document.activeElement === num && send.pending())) num.value = fmtNum(v);
    };
    row.append(slider, num);
  }
  row.append(stateLine);
  row.classList.toggle('changed', value !== p.default);
  row.dataset.key = key;
  return row;
}

async function afterCalibChange() {
  await pollStatus();
  await loadCalib();
}

async function toggleCalibMode() {
  const btn = $('calibModeBtn');
  const target = inCalibMode() ? MODE_MAIN : MODE_CALIB;
  await guard(btn, async () => {
    await api('/api/device_mode', { method: 'POST', body: { mode: target } });
    toast(target === MODE_CALIB ? 'Calibration mode entered' : 'Back in main mode (unsaved changes discarded)', 'ok');
    await afterCalibChange();
  });
}

async function importParams(file) {
  try {
    const doc = await readJsonFile(file);
    const params = Array.isArray(doc) ? doc : doc.params;
    if (!Array.isArray(params)) throw new Error('JSON must contain a "params" list');
    const r = await api('/api/params/bulk', { method: 'POST', body: { params } });
    toast(`Imported ${r.applied} parameter(s)` + (r.failed ? `, ${r.failed} failed` : '') + ' (not yet saved)',
      r.failed ? 'error' : 'ok');
    await afterCalibChange();
  } catch (e) {
    toast(`Import failed: ${e.message}`, 'error');
  }
}

/* ----------------------------------------------------------------- settings */

const DEVICE_SETTING_FIELDS = ['device_name', 'wifi_mode', 'sta_ssid', 'sta_password', 'ap_ssid', 'ap_password',
  'ap_channel', 'max_current_ma', 'num_leds', 'color_order'];

async function loadSettings() {
  const form = $('settingsForm');
  try {
    const r = await api('/api/settings');
    state.settingsOrig = r.settings;
    for (const name of DEVICE_SETTING_FIELDS) {
      if (form.elements[name] && r.settings[name] !== undefined) form.elements[name].value = r.settings[name];
    }
    $('settingsStatus').textContent = '';
  } catch (e) {
    $('settingsStatus').textContent = e.message;
  }
}

async function applySettings(ev) {
  ev.preventDefault();
  const form = $('settingsForm');
  const changed = {};
  for (const name of DEVICE_SETTING_FIELDS) {
    const f = form.elements[name];
    if (!f) continue;
    const isNum = f.type === 'number' || f.tagName === 'SELECT';
    const v = isNum ? Number(f.value) : f.value;
    if (String(v) !== String(state.settingsOrig[name])) changed[name] = v;
  }
  if (!Object.keys(changed).length) { toast('Nothing changed'); return; }
  await guard(form.querySelector('button[type=submit]'), async () => {
    const r = await api('/api/settings', { method: 'POST', body: { settings: changed } });
    toast(`Saved: ${r.changed.join(', ') || 'no changes'}`, 'ok');
    if (r.reboot_required) $('rebootNotice').hidden = false;
    await loadSettings();
    await pollStatus();
  });
}

async function importSettings(file) {
  try {
    const doc = await readJsonFile(file);
    const src = doc.settings || doc;
    const known = new Set(DEVICE_SETTING_FIELDS.concat(['run_mode', 'algorithm', 'pattern', 'brightness']));
    const settings = {};
    for (const [k, v] of Object.entries(src)) if (known.has(k)) settings[k] = v;
    if (!Object.keys(settings).length) throw new Error('no known settings in file');
    const r = await api('/api/settings', { method: 'POST', body: { settings } });
    toast(`Imported settings, changed: ${r.changed.join(', ') || 'none'}`, 'ok');
    if (r.reboot_required) $('rebootNotice').hidden = false;
    await loadSettings();
    await pollStatus();
  } catch (e) {
    toast(`Import failed: ${e.message}`, 'error');
  }
}

async function reboot(btn) {
  await guard(btn, async () => {
    await api('/api/reboot', { method: 'POST' });
    $('rebootNotice').hidden = true;
    toast('Device is rebooting...', 'ok');
    state.connected = false;
    setTimeout(pollStatus, 2500);
  });
}

function loadHostForm() {
  const f = $('hostForm');
  f.elements.poll_rate_hz.value = state.hostSettings.poll_rate_hz;
  f.elements.default_device_ip.value = state.hostSettings.default_device_ip || '';
  f.elements.auto_connect.checked = !!state.hostSettings.auto_connect;
}

async function saveHostForm(ev) {
  ev.preventDefault();
  const f = $('hostForm');
  await guard(f.querySelector('button[type=submit]'), async () => {
    state.hostSettings = await api('/api/host_settings', {
      method: 'POST',
      body: {
        poll_rate_hz: Number(f.elements.poll_rate_hz.value),
        default_device_ip: f.elements.default_device_ip.value,
        auto_connect: f.elements.auto_connect.checked,
      },
    });
    loadHostForm();
    toast('Host settings saved', 'ok');
  });
}

/* ---------------------------------------------------------------------- CLI */

function cliPrint(text, cls) {
  const out = $('cliOut');
  out.append(cls ? el('span', { class: cls, text }) : document.createTextNode(text));
  out.scrollTop = out.scrollHeight;
}

async function cliSubmit(ev) {
  ev.preventDefault();
  const input = $('cliInput');
  const cmd = input.value.trim();
  if (!cmd) return;
  input.value = '';
  if (state.cliHistory[state.cliHistory.length - 1] !== cmd) state.cliHistory.push(cmd);
  state.cliIndex = state.cliHistory.length;
  cliPrint(`> ${cmd}\n`, 'cmd');
  try {
    const r = await api('/api/cli', { method: 'POST', body: { cmd } });
    cliPrint(r.output.endsWith('\n') || !r.output ? r.output : `${r.output}\n`);
  } catch (e) {
    cliPrint(`error: ${e.message}\n`, 'err');
  }
}

function cliKey(ev) {
  const input = $('cliInput');
  if (ev.key === 'ArrowUp') {
    if (state.cliIndex > 0) state.cliIndex--;
    input.value = state.cliHistory[state.cliIndex] || '';
  } else if (ev.key === 'ArrowDown') {
    if (state.cliIndex < state.cliHistory.length) state.cliIndex++;
    input.value = state.cliHistory[state.cliIndex] || '';
  } else {
    return;
  }
  ev.preventDefault();
  setTimeout(() => input.setSelectionRange(input.value.length, input.value.length), 0);
}

/* ----------------------------------------------------------------- firmware */

function fwTarget() {
  const b = $('fwTargetSeg').querySelector('button.active');
  return b ? b.dataset.value : 'app';
}

async function fwUpload() {
  const file = $('fwFile').files[0];
  if (!file) { toast('Choose a .bin file first', 'error'); return; }
  await guard($('fwUploadBtn'), async () => {
    const target = fwTarget();
    await api(`/api/fw/upload?target=${target}&name=${encodeURIComponent(file.name)}`, { method: 'POST', body: file });
    toast(`Uploading ${file.name} (${file.size} bytes) to ${target.toUpperCase()}`, 'info');
    pollFw();
  });
}

async function pollFw() {
  if (state.fwPolling) return;
  state.fwPolling = true;
  try {
    for (;;) {
      let j;
      try { j = await api('/api/fw/status'); } catch (e) { $('fwStatus').textContent = e.message; break; }
      renderFw(j);
      if (j.state !== 'running') {
        if (j.state === 'done') toast(`Firmware update finished: ${j.message}`, 'ok', 8000);
        else if (j.state === 'error') toast(`Firmware update failed: ${j.message}`, 'error', 10000);
        else if (j.state === 'aborted') toast('Firmware update aborted');
        if (j.state === 'done' && j.target === 'APP') setTimeout(pollStatus, 3000);
        else pollStatus();
        break;
      }
      await new Promise((r) => setTimeout(r, 300));
    }
  } finally {
    state.fwPolling = false;
  }
}

function renderFw(j) {
  state.fwRunning = j.state === 'running';
  const bar = $('fwProgress');
  bar.style.width = `${j.state === 'idle' ? 0 : j.percent}%`;
  bar.classList.toggle('done', j.state === 'done');
  bar.classList.toggle('error', j.state === 'error' || j.state === 'aborted');
  const kb = (n) => (n / 1024).toFixed(1);
  $('fwStatus').textContent = j.state === 'idle' ? 'Idle'
    : `${j.state.toUpperCase()} ${j.target} ${j.filename ? `"${j.filename}" ` : ''}- ${kb(j.sent)} / ${kb(j.total)} KiB`
      + ` (${j.percent}%), ${(j.rate_bps / 1024).toFixed(1)} KiB/s, ${j.elapsed_s}s - ${j.message}`;
  $('fwUploadBtn').disabled = state.fwRunning;
}

/* -------------------------------------------------------------- diagnostics */

async function loadDiag() {
  try {
    const r = await api('/api/diag');
    const tb = $('diagTable').tBodies[0];
    tb.replaceChildren();
    if (!r.rows.length) tb.append(el('tr', {}, el('td', { colspan: 4, class: 'muted', text: 'No metrics' })));
    let lastGroup = null;
    for (const row of r.rows) {
      tb.append(el('tr', {},
        el('td', { class: 'muted', text: row.group === lastGroup ? '' : row.group }),
        el('td', { text: row.name }), el('td', { class: 'num', text: row.value }), el('td', { class: 'muted', text: row.unit })));
      lastGroup = row.group;
    }
  } catch (e) {
    toast(`Diagnostics: ${e.message}`, 'error');
    $('diagAuto').checked = false;
    updateDiagTimer();
  }
}

function updateDiagTimer() {
  const want = $('diagAuto').checked && state.activeTab === 'admin' && state.activeSub === 'diag' && state.connected;
  if (want && !state.diagTimer) state.diagTimer = setInterval(loadDiag, 1000);
  if (!want && state.diagTimer) { clearInterval(state.diagTimer); state.diagTimer = null; }
}

/* --------------------------------------------------------------------- init */

function wire() {
  document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));
  document.querySelectorAll('.subtab').forEach((b) => b.addEventListener('click', () => showSub(b.dataset.sub)));

  // Main
  $('connectBtn').addEventListener('click', () => connectTo($('ipInput').value.trim()));
  $('ipInput').addEventListener('keydown', (e) => { if (e.key === 'Enter') connectTo($('ipInput').value.trim()); });
  $('disconnectBtn').addEventListener('click', () => guard($('disconnectBtn'), async () => {
    await api('/api/disconnect', { method: 'POST' });
    state.connected = false;
    await pollStatus();
  }));
  $('scanBtn').addEventListener('click', scan);
  $('runModeSeg').querySelectorAll('button').forEach((b) =>
    b.addEventListener('click', () => setRunConfig({ run_mode: Number(b.dataset.value) })));
  $('algSelect').addEventListener('change', (e) => setRunConfig({ algorithm: Number(e.target.value) }));
  $('patSelect').addEventListener('change', (e) => setRunConfig({ pattern: Number(e.target.value) }));
  $('briSlider').addEventListener('input', (e) => {
    $('briValue').textContent = e.target.value;
    sendBrightness(Number(e.target.value));
  });
  $('errRefreshBtn').addEventListener('click', loadErrors);
  $('errClearBtn').addEventListener('click', () => guard($('errClearBtn'), async () => {
    await api('/api/errors/clear', { method: 'POST' });
    await loadErrors();
    await pollStatus();
  }));

  // Calibration
  $('calibModeBtn').addEventListener('click', toggleCalibMode);
  $('calibRefreshBtn').addEventListener('click', afterCalibChange);
  $('calibSaveBtn').addEventListener('click', () => guard($('calibSaveBtn'), async () => {
    await api('/api/params/save', { method: 'POST' });
    toast('Parameters saved to flash', 'ok');
    await pollStatus();
  }));
  $('calibReloadBtn').addEventListener('click', () => {
    if (!armConfirm($('calibReloadBtn'), 'Discard changes?')) return;
    guard($('calibReloadBtn'), async () => {
      await api('/api/params/reload', { method: 'POST' });
      toast('Unsaved changes discarded', 'ok');
      await afterCalibChange();
    });
  });
  $('calibResetAllBtn').addEventListener('click', () => {
    if (!armConfirm($('calibResetAllBtn'), 'Confirm reset all')) return;
    guard($('calibResetAllBtn'), async () => {
      await api('/api/params/reset', { method: 'POST', body: {} });
      toast('All stages reset to defaults (not saved)', 'ok');
      await afterCalibChange();
    });
  });
  $('calibExportBtn').addEventListener('click', () => guard($('calibExportBtn'), () => downloadFrom('/api/params/export')));
  $('calibImportBtn').addEventListener('click', () => $('calibImportFile').click());
  $('calibImportFile').addEventListener('change', (e) => {
    const f = e.target.files[0]; e.target.value = '';
    if (f) importParams(f);
  });

  // Settings
  $('settingsForm').addEventListener('submit', applySettings);
  $('settingsReloadBtn').addEventListener('click', loadSettings);
  document.querySelectorAll('.pw-toggle').forEach((b) => b.addEventListener('click', () => {
    const input = b.previousElementSibling;
    input.type = input.type === 'password' ? 'text' : 'password';
    b.textContent = input.type === 'password' ? 'Show' : 'Hide';
  }));
  $('settingsExportBtn').addEventListener('click', () => guard($('settingsExportBtn'), () => downloadFrom('/api/settings/export')));
  $('settingsImportBtn').addEventListener('click', () => $('settingsImportFile').click());
  $('settingsImportFile').addEventListener('change', (e) => {
    const f = e.target.files[0]; e.target.value = '';
    if (f) importSettings(f);
  });
  $('rebootBtn').addEventListener('click', () => reboot($('rebootBtn')));
  $('rebootNowBtn').addEventListener('click', () => reboot($('rebootNowBtn')));
  $('factoryBtn').addEventListener('click', () => { $('factoryConfirm').hidden = false; });
  $('factoryNoBtn').addEventListener('click', () => { $('factoryConfirm').hidden = true; });
  $('factoryYesBtn').addEventListener('click', () => guard($('factoryYesBtn'), async () => {
    await api('/api/settings/factory_reset', { method: 'POST' });
    $('factoryConfirm').hidden = true;
    $('rebootNotice').hidden = false;
    toast('Settings restored to factory defaults', 'ok');
    await loadSettings();
    await pollStatus();
  }));
  $('hostForm').addEventListener('submit', saveHostForm);

  // Admin
  $('cliForm').addEventListener('submit', cliSubmit);
  $('cliInput').addEventListener('keydown', cliKey);
  $('cliClearBtn').addEventListener('click', () => { $('cliOut').textContent = ''; });
  $('fwTargetSeg').querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
    $('fwTargetSeg').querySelectorAll('button').forEach((x) => x.classList.toggle('active', x === b));
  }));
  $('fwUploadBtn').addEventListener('click', fwUpload);
  $('fwAbortBtn').addEventListener('click', () => guard($('fwAbortBtn'), async () => {
    const r = await api('/api/fw/abort', { method: 'POST' });
    toast(r.message);
    pollFw();
  }));
  $('calibImageBtn').addEventListener('click', () => guard($('calibImageBtn'), () => downloadFrom('/api/calib_image')));
  $('diagAuto').addEventListener('change', updateDiagTimer);
  $('diagRefreshBtn').addEventListener('click', loadDiag);
  $('diagResetBtn').addEventListener('click', () => guard($('diagResetBtn'), async () => {
    await api('/api/diag/reset', { method: 'POST' });
    await loadDiag();
  }));
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') pollStatus(); });
}

async function init() {
  wire();
  try { state.hostSettings = await api('/api/host_settings'); } catch (_) { /* keep defaults */ }
  loadHostForm();
  await pollStatus();
  $('ipInput').value = state.device ? state.device.replace(/:4210$/, '') : (state.hostSettings.default_device_ip || '');
  try {
    const j = await api('/api/fw/status');
    renderFw(j);
    if (j.state === 'running') pollFw();
  } catch (_) { /* ignore */ }
  setInterval(() => { if (document.visibilityState === 'visible') pollStatus(); }, 2000);
  liveLoop();
  if (!state.device) scan();
}

document.addEventListener('DOMContentLoaded', init);
