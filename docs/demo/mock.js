/* Демо-режим веб-інтерфейсу Smart Plug Control.
 *
 * Підміняє fetch() і XMLHttpRequest, щоб справжній app.js з прошивки працював
 * без пристрою: усі /api/* обслуговує цей файл, дані вигадані й живуть лише в
 * пам'яті сторінки. Нічого нікуди не надсилається.
 *
 * Файл підключається ПЕРЕД app.js (це робить docs/sync-demo.py).
 */
(function () {
  'use strict';

  var K_FACTORY = 0.7667;      /* заводський коефіцієнт (Kconfig) */
  var K_IDEAL = 0.735;         /* "ідеальний" коефіцієнт цього вигаданого модуля: з заводським покази ~4% завищені */
  var T_START = Date.now();

  var state, sim, scan, wifiScan, nextId, timer;

  /* ---------------------------------------------------------------- state */
  function plugBase(over) {
    var p = {
      id: 0, name: '', enabled: true, input: 'NONE', invert: false, on_delay_s: 0,
      driver: 'http',
      http: { on_url: '', off_url: '', status_url: '', use_post: false, auth_user: '', auth_pass: '', family: 'unknown', kind: 'unknown' },
      mqtt: { cmd_topic: '', payload_on: 'on', payload_off: 'off', state_topic: '', qos: 0, retain: false },
      zigbee: { note: '' },
      last_state: false, last_cmd_ok: true, reachable: true,
      metrics: { valid: false, power_w: 0, voltage_v: 0, current_a: 0, updated_us: 0 },
      _w: 0,          /* потужність у ввімкненому стані (для симуляції) */
      _due: 0         /* час відкладеного вмикання, мс */
    };
    for (var k in over) {
      if (over[k] && typeof over[k] === 'object' && !Array.isArray(over[k]) && p[k]) Object.assign(p[k], over[k]);
      else p[k] = over[k];
    }
    return p;
  }

  function shellyG1(ip) { return { on_url: 'http://' + ip + '/relay/0?turn=on', off_url: 'http://' + ip + '/relay/0?turn=off', family: 'shelly_gen1', kind: 'plug' }; }
  function shellyG2(ip) { return { on_url: 'http://' + ip + '/rpc/Switch.Set?id=0&on=true', off_url: 'http://' + ip + '/rpc/Switch.Set?id=0&on=false', family: 'shelly_gen2', kind: 'plug' }; }
  function tasmota(ip) { return { on_url: 'http://' + ip + '/cm?cmnd=Power1%20On', off_url: 'http://' + ip + '/cm?cmnd=Power1%20Off', family: 'tasmota', kind: 'plug' }; }

  function initialState() {
    var now = Math.floor(Date.now() / 1000);
    var s = {
      plugs: [
        plugBase({ id: 1, name: 'Бойлер', input: 'AC1', on_delay_s: 10, http: shellyG1('192.168.1.41'), last_state: true, _w: 1480 }),
        plugBase({ id: 2, name: 'Насос', input: 'DC1', http: shellyG2('192.168.1.42'), last_state: false, _w: 320 }),
        plugBase({ id: 3, name: 'Підсвітка', input: 'NONE', http: tasmota('192.168.1.43'), last_state: true, _w: 8.4 }),
        plugBase({ id: 4, name: 'Гараж: обігрівач', input: 'DC2', reachable: false, http: shellyG1('192.168.1.44'), last_state: false, _w: 900 })
      ],
      inputs: [
        { label: 'AC1', enabled: true },
        { label: 'DC1', enabled: true },
        { label: 'DC2', enabled: false }
      ],
      mqtt: { mqtt_uri: '', mqtt_client_id: 'smartplug-demo', mqtt_user: '', mqtt_pass: '' },
      ac: { v_min: 198, v_max: 253, f_min: 49.5, f_max: 50.5, cal_k: K_FACTORY },
      wifi: { ssid: 'Home-2.4G', password: 'demo-password', connected: true, config_error: false, sta_ip: '192.168.1.50', ap_ip: '192.168.4.1', ap_ssid: 'SmartPlug-A1B2C3', ap_password: 'plugsetup' },
      time: { source: 'ntp', offset: 0 },
      log: [
        { t: now - 3 * 86400 - 3600, src: 'boot', plug: '', action: 'reset_reason', ok: true, detail: 'power-on' },
        { t: now - 7200, src: 'system', plug: '', action: 'heartbeat', ok: true, detail: 'uptime=3d 0h, free_sram=61 KB, sta=up' },
        { t: now - 5400, src: 'ac_mon', plug: 'Бойлер', action: 'off', ok: true, detail: 'AC1 out of band' },
        { t: now - 5100, src: 'ac_mon', plug: 'Бойлер', action: 'on', ok: true, detail: 'AC1 back in band' },
        { t: now - 3600, src: 'system', plug: '', action: 'heartbeat', ok: true, detail: 'uptime=3d 1h, free_sram=61 KB, sta=up' },
        { t: now - 900, src: 'system', plug: 'Гараж: обігрівач', action: 'unreachable', ok: false, detail: 'plug not reachable (offline)' }
      ]
    };
    return s;
  }

  function initialSim() {
    return { trueV: 231.4, dc: { DC1: false, DC2: false }, freq: 50.0 };
  }

  /* ---------------------------------------------------------------- helpers */
  function jitter(a) { return (Math.random() * 2 - 1) * a; }
  function nowSec() { return Math.floor(Date.now() / 1000); }
  function clone(o) { return JSON.parse(JSON.stringify(o)); }

  function addLog(src, plug, action, ok, detail) {
    state.log.push({ t: nowSec(), src: src, plug: plug || '', action: action, ok: ok, detail: detail || '' });
    if (state.log.length > 50) state.log.shift();
  }

  function displayedV() {
    if (sim.trueV < 20) return 0;
    return sim.trueV * state.ac.cal_k / K_IDEAL;
  }

  function mainsPresent() { return sim.trueV >= 20; }

  function inBand() {
    if (!mainsPresent()) return false;
    var v = displayedV(), f = sim.freq;
    return v >= state.ac.v_min && v <= state.ac.v_max && f >= state.ac.f_min && f <= state.ac.f_max;
  }

  function inputEnabled(label) {
    for (var i = 0; i < state.inputs.length; i++) if (state.inputs[i].label === label) return state.inputs[i].enabled;
    return false;
  }

  function inputLevel(label) {
    if (label === 'AC1') return inBand();
    return !!sim.dc[label];
  }

  function refreshMetrics(p) {
    var family = p.http && p.http.family;
    var meterable = p.driver === 'http' && (family === 'shelly_gen1' || family === 'shelly_gen2' || family === 'tasmota');
    if (!meterable || !p.reachable) { if (!p.reachable) return; p.metrics = { valid: false, power_w: 0, voltage_v: 0, current_a: 0, updated_us: 0 }; return; }
    var w = p.last_state ? Math.max(0, p._w + jitter(p._w * 0.01)) : 0;
    var v = family === 'shelly_gen1' ? 0 : 231 + jitter(0.6);
    p.metrics = { valid: true, power_w: w, voltage_v: v, current_a: v && w ? w / v : 0, updated_us: 0 };
  }

  function setPlug(p, on, src, why) {
    if (p.last_state === on) return;
    p.last_state = on;
    p.last_cmd_ok = true;
    p._due = 0;
    refreshMetrics(p);
    addLog(src, p.name, on ? 'on' : 'off', true, why || '');
  }

  /* Правило керування входами (див. hardware.html): вимикання миттєве,
   * вмикання - після затримки розетки; затримка рахується після інверсії. */
  function applyInputs() {
    var inb = inBand();
    state.plugs.forEach(function (p) {
      if (!p.enabled || p.input === 'NONE' || !inputEnabled(p.input) || !p.reachable) return;
      var lvl = inputLevel(p.input);
      var want = lvl !== p.invert;
      var src = p.input === 'AC1' ? 'ac_mon' : 'input';
      if (!want) {
        p._due = 0;
        setPlug(p, false, src, p.input + (p.input === 'AC1' && !mainsPresent() ? ': немає мережі' : p.input === 'AC1' ? ': поза межами' : ': вхід неактивний'));
      } else if (!p.last_state) {
        var d = Math.max(0, p.on_delay_s | 0);
        if (d === 0) setPlug(p, true, src, p.input + ' активний');
        else if (!p._due) p._due = Date.now() + d * 1000;
        else if (Date.now() >= p._due) setPlug(p, true, src, p.input + ' активний, затримка ' + d + ' с');
      }
    });
    return inb;
  }

  function tick() {
    applyInputs();
    state.plugs.forEach(refreshMetrics);
    renderPanelStatus();
  }

  function publicPlug(p) {
    var o = clone(p);
    delete o._w; delete o._due;
    return o;
  }

  /* ---------------------------------------------------------------- scan data */
  var CANDIDATES = [
    { ip: '192.168.1.1', name: 'router.lan', http: true, family: 'unknown', kind: 'unknown', model: '', needs_auth: true, ping_ok: true },
    { ip: '192.168.1.61', name: 'shellyplug-s-8A21F0', http: true, family: 'shelly_gen1', kind: 'plug', model: 'SHPLG-S', needs_auth: false, ping_ok: true },
    { ip: '192.168.1.62', name: 'ShellyPlusPlugS-E465B8', http: true, family: 'shelly_gen2', kind: 'plug', model: 'SNPL-00112EU', needs_auth: false, ping_ok: true },
    { ip: '192.168.1.63', name: 'tasmota-9F3C12', http: true, family: 'tasmota', kind: 'plug', model: 'Sonoff S31', needs_auth: false, ping_ok: true },
    { ip: '192.168.1.64', name: 'tasmota-strip-4B77', http: true, family: 'tasmota', kind: 'relay', model: 'Power strip 4ch', needs_auth: true, ping_ok: true },
    { ip: '192.168.1.65', name: 'esphome-kitchen', http: true, family: 'esphome', kind: 'plug', model: '', needs_auth: false, ping_ok: false, on_url: 'http://192.168.1.65/switch/relay/turn_on', off_url: 'http://192.168.1.65/switch/relay/turn_off', use_post: true },
    { ip: '192.168.1.70', name: '', http: false, family: 'unknown', kind: 'unknown', model: '', needs_auth: false, ping_ok: true }
  ];

  function scanResults() {
    if (!scan.startedAt) return [];
    var n = Math.min(CANDIDATES.length, Math.floor((Date.now() - scan.startedAt) / 1400) + 1);
    return CANDIDATES.slice(0, n).map(function (c) {
      var r = clone(c);
      if (!r.on_url) { r.on_url = ''; r.off_url = ''; r.use_post = false; }
      return r;
    });
  }

  var WIFI_NETS = [
    { ssid: 'Home-2.4G', rssi: -52, secure: true },
    { ssid: 'Neighbor_WiFi', rssi: -71, secure: true },
    { ssid: 'TP-Link_5C21', rssi: -78, secure: true },
    { ssid: 'Cafe Guest', rssi: -83, secure: false }
  ];

  /* ---------------------------------------------------------------- API */
  function parseBody(opts) {
    try { return opts && opts.body ? JSON.parse(opts.body) : {}; } catch (e) { return {}; }
  }

  function buildPlug(b, base) {
    var p = base || plugBase({ id: nextId++ });
    p.name = String(b.name || '').slice(0, 31);
    if ('enabled' in b) p.enabled = !!b.enabled;
    p.input = b.input || 'NONE';
    p.invert = !!b.invert;
    p.on_delay_s = Math.max(0, Math.min(600, b.on_delay_s | 0));
    p.driver = b.driver || 'http';
    if (b.http) Object.assign(p.http, b.http);
    if (b.mqtt) Object.assign(p.mqtt, b.mqtt);
    if (!base) {
      p.last_state = false;      /* в реальному пристрої опитується справжній стан */
      p._w = p.http.family === 'unknown' ? 0 : 60;
    }
    refreshMetrics(p);
    return p;
  }

  function route(method, url, opts) {
    var u = new URL(url, 'http://demo.local');
    var path = u.pathname, m;
    var body = parseBody(opts);

    if (path === '/api/status' && method === 'GET') {
      var inb = inBand();
      return {
        uptime_s: 3 * 86400 + 4 * 3600 + Math.floor((Date.now() - T_START) / 1000),
        wifi: { sta_connected: state.wifi.connected, ssid: state.wifi.ssid, sta_ip: state.wifi.sta_ip, rssi: -58, config_error: state.wifi.config_error, ap_ip: state.wifi.ap_ip },
        inputs: state.inputs.map(function (i) { return { label: i.label, enabled: i.enabled, level: inputLevel(i.label) }; }),
        ac_mon: {
          have_reading: true, mains_present: mainsPresent(),
          vrms: mainsPresent() ? displayedV() + jitter(0.25) : 0,
          freq_hz: mainsPresent() ? sim.freq + jitter(0.02) : 0,
          in_band: inb
        }
      };
    }

    if (path === '/api/plugs' && method === 'GET') return state.plugs.map(publicPlug);
    if (path === '/api/plugs' && method === 'POST') {
      if (state.plugs.length >= 16) throw httpError(400, 'достатньо розеток (демо: максимум 16)');
      var np = buildPlug(body);
      state.plugs.push(np);
      addLog('api', np.name, 'add', true, '');
      applyInputs();
      return publicPlug(np);
    }
    if ((m = /^\/api\/plugs\/(\d+)$/.exec(path))) {
      var id = +m[1], idx = state.plugs.findIndex(function (p) { return p.id === id; });
      if (idx < 0) throw httpError(404, 'розетку не знайдено');
      if (method === 'PUT') { buildPlug(body, state.plugs[idx]); applyInputs(); return publicPlug(state.plugs[idx]); }
      if (method === 'DELETE') { var gone = state.plugs.splice(idx, 1)[0]; addLog('api', gone.name, 'delete', true, ''); return { ok: true }; }
    }
    if ((m = /^\/api\/plugs\/(\d+)\/action$/.exec(path)) && method === 'POST') {
      var pl = state.plugs.find(function (p) { return p.id === +m[1]; });
      if (!pl) throw httpError(404, 'розетку не знайдено');
      if (pl.input !== 'NONE') return { ok: false, detail: 'керується входом ' + pl.input + ', вручну не перемикається' };
      if (!pl.reachable) return { ok: false, detail: 'не на зв\'язку' };
      setPlug(pl, body.action === 'on', 'manual', '');
      return { ok: true, detail: 'ok' };
    }

    if (path === '/api/settings' && method === 'GET') {
      return {
        inputs: state.inputs.map(function (i) { return { label: i.label, enabled: i.enabled }; }),
        mqtt_uri: state.mqtt.mqtt_uri, mqtt_client_id: state.mqtt.mqtt_client_id,
        mqtt_user: state.mqtt.mqtt_user, mqtt_pass: state.mqtt.mqtt_pass,
        ac_mon: Object.assign({}, state.ac)
      };
    }
    if (path === '/api/settings' && method === 'POST') {
      (body.inputs || []).forEach(function (i, n) { if (state.inputs[n]) state.inputs[n].enabled = !!i.enabled; });
      ['mqtt_uri', 'mqtt_client_id', 'mqtt_user', 'mqtt_pass'].forEach(function (k) { if (k in body) state.mqtt[k] = body[k]; });
      if (body.ac_mon) {
        var a = body.ac_mon;
        if (!(a.v_min >= 100 && a.v_max <= 300 && a.v_min < a.v_max)) throw httpError(400, 'некоректні межі напруги (100–300 В, мін < макс)');
        if (!(a.f_min >= 40 && a.f_max <= 65 && a.f_min < a.f_max)) throw httpError(400, 'некоректні межі частоти (40–65 Гц, мін < макс)');
        state.ac.v_min = a.v_min; state.ac.v_max = a.v_max; state.ac.f_min = a.f_min; state.ac.f_max = a.f_max;
      }
      applyInputs();
      return { ok: true };
    }

    if (path === '/api/ac/calibrate' && method === 'POST') {
      if (body.reset) state.ac.cal_k = K_FACTORY;
      else {
        if (!mainsPresent()) throw httpError(400, 'немає мережі на вході');
        state.ac.cal_k = K_IDEAL * body.vref / sim.trueV;
        addLog('api', '', 'ac_calibrate', true, 'vref=' + body.vref + ' K=' + state.ac.cal_k.toFixed(4));
      }
      applyInputs();
      return { ok: true, cal_k: state.ac.cal_k, vrms: displayedV() };
    }

    if (path === '/api/wifi' && method === 'GET') return Object.assign({}, state.wifi);
    if (path === '/api/wifi' && method === 'POST') {
      state.wifi.ssid = String(body.ssid || ''); state.wifi.password = String(body.password || '');
      state.wifi.connected = true; state.wifi.config_error = false;
      return { ok: true };
    }
    if (path === '/api/wifi/ap' && method === 'POST') {
      var apPass = String(body.password || '');
      if (apPass.length < 8 || apPass.length > 63) throw httpError(400, 'пароль точки доступу: 8–63 символи (лише ASCII)');
      state.wifi.ap_password = apPass;
      addLog('api', '', 'ap_password_changed', true, '');
      return { ok: true };
    }
    if (path === '/api/wifi/scan' && method === 'POST') { wifiScan.startedAt = Date.now(); return { ok: true }; }
    if (path === '/api/wifi/scan' && method === 'GET') {
      var running = wifiScan.startedAt && Date.now() - wifiScan.startedAt < 2500;
      return { running: !!running, results: running || !wifiScan.startedAt ? [] : WIFI_NETS.slice() };
    }

    if (path === '/api/scan' && method === 'POST') { scan.startedAt = Date.now(); scan.running = true; return { ok: true }; }
    if (path === '/api/scan' && method === 'DELETE') { scan.running = false; return { ok: true }; }
    if (path === '/api/scan' && method === 'GET') {
      return { running: scan.running, have_subnet: true, fingerprinting: false, pass: 1, total: 254, scanned: 254, results: scanResults() };
    }

    if (path === '/api/time' && method === 'GET') {
      return { epoch: nowSec() + state.time.offset, synced: true, source: state.time.source,
               last_ntp_sync: state.time.source === 'ntp' ? nowSec() - 3600 : 0, sync_interval_h: 24 };
    }
    if (path === '/api/time' && method === 'POST') {
      state.time.offset = (body.epoch | 0) - nowSec(); state.time.source = 'manual';
      addLog('api', '', 'set_time', true, 'manual');
      return { ok: true };
    }

    if (path === '/api/log' && method === 'GET') return state.log.slice(-(+u.searchParams.get('limit') || 50));
    if (path === '/api/log/clear' && method === 'POST') { state.log = []; return { ok: true }; }

    if (path === '/api/system/ota' && method === 'GET') {
      return { project: 'smart_plug_control', version: '2.1.2 (демо)', built: 'Oct 10 2026', max_size: 4194304 };
    }
    if (path === '/api/system/restart' && method === 'POST') return { ok: true };
    if (path === '/api/system/factory_reset' && method === 'POST') {
      var keptCalK = state.ac.cal_k; /* калібрування вольтметра переживає скидання */
      state = initialState(); sim = initialSim();
      state.ac.cal_k = keptCalK;
      state.plugs = []; state.wifi.ssid = ''; state.wifi.password = ''; state.wifi.connected = false;
      state.log = [];
      return { ok: true };
    }

    throw httpError(404, 'У демо цей запит не підтримується: ' + method + ' ' + path);
  }

  function httpError(status, msg) { var e = new Error(msg); e.status = status; return e; }

  function fakeFetch(url, opts) {
    var method = ((opts && opts.method) || 'GET').toUpperCase();
    var delay = 60 + Math.random() * 120;
    return new Promise(function (resolve) {
      setTimeout(function () {
        var status = 200, data;
        try { data = route(method, String(url), opts); }
        catch (e) { status = e.status || 500; data = { error: e.message }; }
        resolve(new Response(JSON.stringify(data), { status: status, headers: { 'Content-Type': 'application/json' } }));
      }, delay);
    });
  }

  /* Прошивка по XHR: імітуємо прогрес і успіх. */
  function FakeXHR() {
    var self = this;
    this.upload = {}; this.status = 0; this.responseText = '';
    this.open = function () {};
    this.setRequestHeader = function () {};
    this.send = function (file) {
      var total = (file && file.size) || 1, loaded = 0, steps = 12, n = 0;
      var iv = setInterval(function () {
        n++; loaded = Math.min(total, Math.round(total * n / steps));
        if (self.upload.onprogress) self.upload.onprogress({ lengthComputable: true, loaded: loaded, total: total });
        if (n >= steps) {
          clearInterval(iv);
          self.status = 200; self.responseText = '{"ok":true}';
          addLog('api', '', 'ota', true, 'демо: файл не записано');
          if (self.onload) self.onload();
        }
      }, 120);
    };
  }

  /* ---------------------------------------------------------------- UI: банер та симулятор входів */
  var panelEls = {};

  function css() {
    var s = document.createElement('style');
    s.textContent =
      '.demo-banner{background:#fff4d6;color:#664d03;border-bottom:1px solid #f0b429;padding:.5rem 1.25rem;font-size:.85rem;display:flex;flex-wrap:wrap;gap:.4rem 1rem;align-items:center;justify-content:space-between}' +
      '.demo-banner a{color:#0a58ca;font-weight:600}' +
      '.demo-sim{position:fixed;right:12px;bottom:12px;z-index:50;width:min(20rem,calc(100vw - 24px));background:#fff;border:1px solid #dee2e6;border-radius:12px;box-shadow:0 8px 28px rgba(0,0,0,.2);font-size:.85rem;color:#212529}' +
      '.demo-sim summary{cursor:pointer;padding:.65rem .9rem;font-weight:650;list-style:none;display:flex;justify-content:space-between;gap:.5rem}' +
      '.demo-sim summary::-webkit-details-marker{display:none}' +
      '.demo-sim[open] summary{border-bottom:1px solid #dee2e6}' +
      '.demo-sim .body{padding:.7rem .9rem .9rem;display:grid;gap:.65rem}' +
      '.demo-sim label{display:grid;gap:.2rem}' +
      '.demo-sim .row{display:flex;justify-content:space-between;gap:.5rem;align-items:center}' +
      '.demo-sim input[type=range]{width:100%}' +
      '.demo-sim .hintx{color:#6c757d;font-size:.78rem;line-height:1.35}' +
      '.demo-sim .stat{font-variant-numeric:tabular-nums;background:#f1f3f5;border-radius:8px;padding:.4rem .6rem}' +
      '.demo-sim button{font:inherit;padding:.3rem .6rem;border:1px solid #dee2e6;border-radius:7px;background:#f8f9fa;cursor:pointer}' +
      '@media(max-width:600px){.demo-sim{right:8px;bottom:8px}}';
    document.head.appendChild(s);
  }

  function buildUi() {
    css();
    var b = document.createElement('div');
    b.className = 'demo-banner';
    b.innerHTML = '<span><strong>Демо.</strong> Це справжній веб-інтерфейс пристрою з вигаданими даними. Нічого не надсилається, а після перезавантаження сторінки все скидається.</span>' +
      '<a href="../index.html">← До інструкції</a>';
    document.body.insertBefore(b, document.body.firstChild);

    var d = document.createElement('details');
    d.className = 'demo-sim';
    d.open = window.innerWidth >= 1500;   /* на вужчих екранах не перекриваємо список розеток */
    d.innerHTML =
      '<summary><span>▲ Симулятор входів: спробуйте!</span><span id="ds-sum" class="hintx"></span></summary>' +
      '<div class="body">' +
      '<label><span class="row"><span>Напруга в мережі (мультиметр)</span><b id="ds-v"></b></span>' +
      '<input id="ds-vr" type="range" min="0" max="280" step="0.5"></label>' +
      '<div class="stat" id="ds-dev"></div>' +
      '<div class="row"><label class="row" style="justify-content:flex-start;gap:.4rem"><input id="ds-dc1" type="checkbox"> DC1 активний</label>' +
      '<label class="row" style="justify-content:flex-start;gap:.4rem"><input id="ds-dc2" type="checkbox"> DC2 активний</label></div>' +
      '<div class="hintx">Опустіть напругу нижче 198 В: розетки, прив\'язані до AC1, вимкнуться одразу, а ввімкнуться знову після своєї затримки (у «Бойлера» 10 с). Заводський коефіцієнт завищує покази приблизно на 4%: відкалібруйте його у «Системних налаштуваннях». DC2 не слухається, доки його не позначено у «Вхідних тригерах» і збережено.</div>' +
      '<div class="row"><button id="ds-230" type="button">230 В</button><button id="ds-0" type="button">Немає мережі</button><button id="ds-cal" type="button">Скинути демо</button></div>' +
      '</div>';
    document.body.appendChild(d);

    panelEls.v = d.querySelector('#ds-v'); panelEls.vr = d.querySelector('#ds-vr');
    panelEls.dev = d.querySelector('#ds-dev'); panelEls.sum = d.querySelector('#ds-sum');
    panelEls.dc1 = d.querySelector('#ds-dc1'); panelEls.dc2 = d.querySelector('#ds-dc2');

    panelEls.vr.value = sim.trueV;
    panelEls.vr.addEventListener('input', function () { sim.trueV = +panelEls.vr.value; applyInputs(); renderPanelStatus(); });
    panelEls.dc1.addEventListener('change', function () { sim.dc.DC1 = panelEls.dc1.checked; applyInputs(); renderPanelStatus(); });
    panelEls.dc2.addEventListener('change', function () { sim.dc.DC2 = panelEls.dc2.checked; applyInputs(); renderPanelStatus(); });
    d.querySelector('#ds-230').addEventListener('click', function () { sim.trueV = 230; panelEls.vr.value = 230; applyInputs(); renderPanelStatus(); });
    d.querySelector('#ds-0').addEventListener('click', function () { sim.trueV = 0; panelEls.vr.value = 0; applyInputs(); renderPanelStatus(); });
    d.querySelector('#ds-cal').addEventListener('click', function () { location.reload(); });
    renderPanelStatus();
  }

  function renderPanelStatus() {
    if (!panelEls.v) return;
    panelEls.v.textContent = sim.trueV.toFixed(1) + ' В';
    var shown = mainsPresent() ? displayedV().toFixed(1) + ' В' : 'немає мережі';
    panelEls.dev.innerHTML = 'Пристрій показує: <b>' + shown + '</b> · AC1 ' + (inBand() ? '<b style="color:#198754">в межах</b>' : '<b style="color:#bb2d3b">поза межами</b>');
    panelEls.sum.textContent = 'AC1=' + (inBand() ? 1 : 0);
  }

  /* ---------------------------------------------------------------- init */
  state = initialState(); sim = initialSim();
  scan = { running: false, startedAt: 0 };
  wifiScan = { startedAt: 0 };
  nextId = 5;

  window.fetch = fakeFetch;
  window.XMLHttpRequest = FakeXHR;

  /* У демо оновлення списку розеток і статусу прискорено (на пристрої вони раз
   * на 10 с і 5 с), щоб рух повзунка симулятора було видно одразу. */
  var realSetInterval = window.setInterval;
  window.setInterval = function (fn, ms) {
    var args = Array.prototype.slice.call(arguments, 2);
    var scaled = ms === 10000 ? 2000 : ms === 5000 ? 1500 : ms;
    return realSetInterval.apply(window, [fn, scaled].concat(args));
  };

  /* початковий стан розеток узгоджуємо з входами */
  state.plugs.forEach(refreshMetrics);
  timer = setInterval(tick, 1000);

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', buildUi);
  else buildUi();
})();
