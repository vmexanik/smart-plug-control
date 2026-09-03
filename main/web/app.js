(() => {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => Array.from(document.querySelectorAll(sel));

  async function api(path, opts) {
    const res = await fetch(path, Object.assign({
      headers: { 'Content-Type': 'application/json' },
    }, opts));
    let data = null;
    try { data = await res.json(); } catch (e) { /* empty body */ }
    if (!res.ok) {
      const msg = (data && data.error) ? data.error : ('HTTP ' + res.status);
      throw new Error(msg);
    }
    return data;
  }

  /* ---------------- toast ---------------- */
  /* Small, non-blocking, auto-dismissing notice -- used instead of alert()
   * for the plug on/off toggle specifically: alert() is a modal dialog that
   * steals focus and has to be dismissed by hand, and mashing an
   * unresponsive plug's toggle a few times in a row used to queue up one of
   * these per click, which is exactly the kind of "clicked 3 times, now
   * dismissing 3 popups" annoyance this replaces. Errors that are genuinely
   * rare/one-off (deleting a plug, clearing the log) still use confirm()/
   * alert() as before -- this is specifically for a button the user might
   * reasonably click several times in a row. */
  function toast(msg, isError) {
    const stack = $('#toast-stack');
    if (!stack) return;
    const el = document.createElement('div');
    el.className = 'toast' + (isError ? ' toast-error' : '');
    el.textContent = msg;
    stack.appendChild(el);
    setTimeout(() => el.classList.add('toast-out'), 2600);
    setTimeout(() => el.remove(), 3200);
  }

  /* ---------------- tabs ---------------- */
  $$('.tab-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      $$('.tab-btn').forEach((b) => b.classList.remove('active'));
      $$('.tab-content').forEach((c) => c.classList.remove('active'));
      btn.classList.add('active');
      $('#tab-' + btn.dataset.tab).classList.add('active');
      if (btn.dataset.tab === 'settings') { loadSettings(); loadWifiStatus(); loadTime(); }
      if (btn.dataset.tab === 'log') loadLog();
    });
  });

  /* ---------------- status bar ---------------- */
  async function loadStatus() {
    try {
      const s = await api('/api/status');
      const wifiTxt = s.wifi.sta_connected
        ? `<span class="good">STA: ${s.wifi.ssid} (${s.wifi.sta_ip})</span>`
        : s.wifi.config_error
          ? `<span class="bad">STA: не вдалося підключитись до "${escapeHtml(s.wifi.ssid)}" — перевірте пароль (Налаштування)</span>`
          : `<span class="bad">STA не підключено</span>`;
      const inputsTxt = s.inputs
        .filter(i => i.enabled)
        .map(i => `${i.label}=${i.level ? 1 : 0}`)
        .join(', ') || 'немає активних входів';
      $('#status-bar').innerHTML = `${wifiTxt} <br> AP: ${s.wifi.ap_ip} <br> Входи: ${inputsTxt} <br> uptime ${formatUptime(s.uptime_s)}`;

      const acEl = $('#ac-mon-status');
      if (acEl && s.ac_mon) {
        const m = s.ac_mon;
        if (!m.have_reading) {
          acEl.innerHTML = `<span class="hint">ще немає показань…</span>`;
        } else if (!m.mains_present) {
          acEl.innerHTML = `<span class="hint">сигнал не виявлено (ZMPT101B не підключено або немає мережі)</span>`;
        } else {
          const bandTxt = m.in_band
            ? `<span class="good">в межах</span>`
            : `<span class="bad">поза межами!</span>`;
          acEl.innerHTML = `U = ${m.vrms.toFixed(1)} В, f = ${m.freq_hz.toFixed(2)} Гц — ${bandTxt}`;
        }
      }
    } catch (e) {
      $('#status-bar').innerHTML = `<span class="bad">Помилка статусу: ${e.message}</span>`;
    }
  }

  function formatUptime(sec) {
    sec = Math.floor(sec || 0);
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    return `${h}г ${m}хв ${s}с`;
  }

  /* ---------------- plugs ---------------- */
  /* No manual "Оновити" button -- refresh is purely automatic, on the 10s
   * interval near the bottom of this file, which lines up with
   * plug_manager.c's METRICS_POLL_INTERVAL_MS so a refresh always picks up
   * genuinely new power/voltage/current readings rather than the same
   * stale numbers the backend hasn't updated yet. Every toggle/add/edit/
   * delete also calls loadPlugs() again for an immediate reflect of that
   * one action. Showing "завантаження…" and
   * wiping the list on every single one of those made the whole card list
   * blank out and pop back for a moment each time -- visible as the cards
   * "blinking" on every action or every ~10s, not just on first page load.
   * Now the loading placeholder only shows before there's anything to show
   * yet; once real content is on screen, a refresh just swaps it in. */
  let plugsLoadedOnce = false;

  async function loadPlugs() {
    const list = $('#plugs-tbody');
    if (!plugsLoadedOnce) list.innerHTML = '<div class="list-empty">завантаження…</div>';
    try {
      const plugs = await api('/api/plugs');
      plugsLoadedOnce = true;
      if (!plugs.length) {
        list.innerHTML = '<div class="list-empty">Немає розеток. Натисніть "Додати розетку".</div>';
        return;
      }
      list.innerHTML = '';
      plugs.forEach((p) => list.appendChild(renderPlugRow(p)));
    } catch (e) {
      if (!plugsLoadedOnce) list.innerHTML = `<div class="list-empty error-text">Помилка: ${e.message}</div>`;
    }
  }

  /* Whole card opens the edit form (all fields besides name/input/state are
   * only editable there); the action button is the one exception -- it
   * stops propagation so it can toggle on/off without opening the form. */
  function renderPlugRow(p) {
    const card = document.createElement('div');
    card.className = 'device-card';

    const statePill = p.last_cmd_ok
      ? `<span class="pill ${p.last_state ? 'on' : 'off'}">${p.last_state ? 'ON' : 'OFF'}</span>`
      : `<span class="pill fail">помилка</span>`;

    /* Power metering, when the plug's family reports it (see
     * driver_http_get_metrics()/metrics_poll_task in plug_manager.c) --
     * voltage/current are omitted per-field since e.g. Shelly Gen1 only
     * ever reports power. */
    let metricsHtml = '';
    if (p.metrics && p.metrics.valid) {
      const parts = [`⚡ ${p.metrics.power_w.toFixed(1)} Вт`];
      if (p.metrics.voltage_v) parts.push(`${p.metrics.voltage_v.toFixed(0)} В`);
      if (p.metrics.current_a) parts.push(`${p.metrics.current_a.toFixed(2)} А`);
      metricsHtml = `<div class="device-metrics">${parts.join(' · ')}</div>`;
    }

    /* p.reachable is only ever false for HTTP plugs with a recognized family
     * (see app_types.h's doc comment on plug_t.reachable) -- the backend
     * itself keeps this passively up to date via the same 10s metrics poll
     * that refreshes power readings, so a plug that comes back online
     * clears this within 10s without needing anyone to click anything.
     * Greying out the toggle here is what stops a dead plug's button from
     * being mashed in the first place -- the backend also fast-fails the
     * same case (see plug_manager.c's do_dispatch()), this is just the UI
     * half of it. */
    const offline = p.reachable === false;

    /* A plug bound to a physical input (AC1/DC1/DC2) is driven exclusively
     * by that input (plug_manager_on_input_change()) -- the backend itself
     * now refuses a manual command for it (plug_manager_test_action(), see
     * app_types.h/plug_manager.c) precisely because letting both the input
     * AND a manual click drive the same relay is a race with no single
     * source of truth (whichever fires last/next wins, unpredictably).
     *
     * This is only a *soft* block in the UI (a CSS class, not the `disabled`
     * attribute): a real `disabled` button never dispatches a click event at
     * all, so the backend's specific rejection reason (a Ukrainian sentence
     * meant to actually be read, see plug_manager_test_action()) would never
     * reach testPlug() and its toast() call -- the user would just see a
     * greyed-out button with nothing happening, and on a touch screen there's
     * no hover to even reveal the `title` tooltip as a fallback. Letting the
     * click through, hitting the backend, and surfacing its detail message
     * via toast is strictly more informative, for both the trigger-controlled
     * and offline cases. */
    const triggerControlled = p.input !== 'NONE';
    const toggleDisabled = offline || triggerControlled;
    let toggleTitle;
    if (triggerControlled) toggleTitle = `Керується входом ${p.input}, вручну не перемикається`;
    else if (offline) toggleTitle = 'Розетка не на зв\'язку';
    else toggleTitle = p.last_state ? 'Вимкнути' : 'Увімкнути';

    card.innerHTML = `
      <div class="device-name">${escapeHtml(p.name)}${p.enabled ? '' : ' <span class="hint">(вимкнена)</span>'}${offline ? ' <span class="hint bad">(не на зв\'язку)</span>' : ''}${metricsHtml}</div>
      <div class="device-input">${p.input === 'NONE' ? '<span class="hint">без входу</span>' : p.input}</div>
      <div class="device-state">${statePill}</div>
      <div class="device-action">
        <button type="button" class="device-action-btn ${p.last_state ? 'state-on' : 'state-off'}${toggleDisabled ? ' is-blocked' : ''}" data-act="toggle"
                title="${toggleTitle}" aria-label="Перемкнути ${escapeHtml(p.name)}">
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M12 2v10"/>
            <path d="M18.4 6.6a9 9 0 1 1-12.77.04"/>
          </svg>
        </button>
      </div>`;

    /* The greyed-out (trigger-controlled / offline) look above is a CSS
     * class now, not the `disabled` attribute, specifically so the button
     * stays clickable (see the big comment above) -- but testPlug() below
     * still sets the real `disabled` attribute for the brief in-flight
     * window of an actual request, and a disabled <button> has
     * pointer-events:none (global "button:disabled" rule in style.css), so
     * a click during that window hit-tests straight through to whatever's
     * underneath -- .device-action here -- and would otherwise bubble up
     * and pop open the edit modal. Checking for .device-action here (rather
     * than the button itself, which won't be ev.target in that case) covers
     * both the normal case (button's own listener already
     * stopPropagation()s, so this is a no-op) and that brief disabled
     * window (where this is the only thing stopping the modal). */
    card.addEventListener('click', (ev) => {
      if (ev.target.closest('.device-action')) return;
      openPlugModal(p);
    });
    const btn = card.querySelector('[data-act="toggle"]');
    btn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      testPlug(p.id, !p.last_state, p.name, btn);
    });

    return card;
  }

  /* Tracks plugs with a request currently in flight so a second click before
   * the first one's response comes back is simply ignored rather than
   * firing another request on top of it -- belt-and-suspenders alongside
   * disabling the button itself (which loadPlugs()'s upcoming re-render
   * will always eventually reset, so this covers the brief window between
   * click and that re-render too). */
  const pendingToggles = new Set();

  async function testPlug(id, on, name, btn) {
    if (pendingToggles.has(id)) return;
    pendingToggles.add(id);
    if (btn) btn.disabled = true;
    try {
      const r = await api(`/api/plugs/${id}/action`, { method: 'POST', body: JSON.stringify({ action: on ? 'on' : 'off' }) });
      if (!r.ok) toast(`"${name}" ${r.detail}`, true);
      await loadPlugs();
    } catch (e) {
      toast(`"${name}" ${e.message}`, true);
    } finally {
      pendingToggles.delete(id);
      if (btn) btn.disabled = false; /* harmless no-op if loadPlugs() already replaced this node */
    }
  }

  async function deletePlug(id, name) {
    if (!confirm(`Видалити розетку "${name}"?`)) return;
    try {
      await api(`/api/plugs/${id}`, { method: 'DELETE' });
      closePlugModal();
      await loadPlugs();
    } catch (e) {
      alert('Помилка: ' + e.message);
    }
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  /* ---------------- plug modal ---------------- */
  const modal = $('#plug-modal');
  const form = $('#plug-form');

  /* The driver select's <option value> encodes "driver" alone (http/mqtt/
   * zigbee) for the generic/manual case, or "driver:family" (e.g.
   * "http:shelly_gen2") when the user picks a recognized protocol directly
   * instead of going through "Знайти в мережі". Splitting it out like this
   * keeps the underlying data model unchanged (driver=http either way,
   * family is still just http.family) while letting Shelly/Tasmota show up
   * as first-class choices in the one dropdown, per request. */
  function splitDriverValue() {
    const [driver, family] = $('#pf-driver').value.split(':');
    return { driver, family };
  }

  /* URL templates mirroring fingerprint.c's try_shelly()/try_tasmota() --
   * kept as the client-side source of truth for "given a host, what would
   * the scan cascade have filled in", so manually picking e.g. "Shelly
   * (Gen2+)" and typing an IP produces the exact same on/off URLs a scan
   * pick would have. Only families with a fixed, host-only template are
   * listed here; ESPHome isn't (see fingerprint.c/README), so it's only
   * ever reachable by editing a plug that scan already recognized. */
  const FAMILY_URL_TEMPLATES = {
    shelly_gen1: (host) => ({ on: `http://${host}/relay/0?turn=on`, off: `http://${host}/relay/0?turn=off`, post: false }),
    shelly_gen2: (host) => ({ on: `http://${host}/rpc/Switch.Set?id=0&on=true`, off: `http://${host}/rpc/Switch.Set?id=0&on=false`, post: false }),
    tasmota: (host) => ({ on: `http://${host}/cm?cmnd=Power1%20On`, off: `http://${host}/cm?cmnd=Power1%20Off`, post: false }),
  };

  function extractHost(url) {
    const m = /^https?:\/\/([^/]+)/i.exec(url || '');
    return m ? m[1] : '';
  }

  function rebuildKnownUrls() {
    const tmpl = FAMILY_URL_TEMPLATES[$('#pf-family').value];
    const host = $('#pf-http-host').value.trim();
    if (!tmpl || !host) return;
    const { on, off, post } = tmpl(host);
    $('#pf-http-on').value = on;
    $('#pf-http-off').value = off;
    $('#pf-http-post').checked = post;
  }
  $('#pf-http-host').addEventListener('input', rebuildKnownUrls);

  function setDriverFieldsVisibility() {
    const { driver } = splitDriverValue();
    $('#pf-http-fields').style.display = driver === 'http' ? '' : 'none';
    $('#pf-mqtt-fields').style.display = driver === 'mqtt' ? '' : 'none';
    $('#pf-zigbee-fields').style.display = driver === 'zigbee' ? '' : 'none';
  }

  /* When the protocol is known (fingerprinted by a scan, or picked directly
   * from the dropdown), the on/off/status URLs are fixed -- editing them
   * serves no purpose and risks the user breaking a working config, so hide
   * the manual URL fields. Families with a URL template additionally get a
   * plain host/IP field instead (auto-generates the URLs); families that
   * are "known" but not templated (ESPHome) just get the read-only summary,
   * same as before. */
  function updateHttpKnownFields() {
    const fam = $('#pf-family').value;
    const known = !!FAMILY_LABEL[fam];
    const templated = !!FAMILY_URL_TEMPLATES[fam];
    $('#pf-http-manual-fields').classList.toggle('hidden', known);
    $('#pf-http-host-field').classList.toggle('hidden', !templated);

    if (templated) {
      if (!$('#pf-http-host').value) {
        $('#pf-http-host').value = extractHost($('#pf-http-on').value);
      }
      rebuildKnownUrls();
    }
  }

  /* Only the dropdown's 'change' event (an explicit user pick) should
   * derive family/kind from it -- programmatic assignment (openPlugModal
   * pre-filling an edit, or the scan-pick handler) already knows the real
   * family (which may be e.g. "esphome", not representable as a dropdown
   * option) and sets it directly, and must not have that overwritten. */
  $('#pf-driver').addEventListener('change', () => {
    const { family } = splitDriverValue();
    $('#pf-family').value = family || 'unknown';
    $('#pf-kind').value = family ? 'plug' : 'unknown';
    if (family) $('#pf-http-host').value = '';
    setDriverFieldsVisibility();
    updateHttpKnownFields();
  });

  function openPlugModal(plug) {
    $('#plug-form-error').textContent = '';
    form.reset();
    if (plug) {
      $('#plug-modal-title').textContent = 'Редагувати розетку: ' + plug.name;
      $('#pf-id').value = plug.id;
      $('#pf-name').value = plug.name;
      $('#pf-enabled').checked = plug.enabled;
      $('#pf-input').value = plug.input;
      $('#pf-invert').checked = plug.invert;
      $('#pf-on-delay').value = String(plug.on_delay_ms || 0);
      /* Composite dropdown value where possible ("http:shelly_gen2"); falls
       * back to the plain driver for MQTT/Zigbee or for a "known but not
       * templated" HTTP family (e.g. esphome) that has no matching option --
       * pf-family below is what actually drives the locked/host-field UI,
       * this only affects which dropdown label shows as selected. */
      $('#pf-driver').value = (plug.driver === 'http' && FAMILY_URL_TEMPLATES[plug.http.family])
        ? `http:${plug.http.family}`
        : plug.driver;
      $('#pf-http-on').value = plug.http.on_url;
      $('#pf-http-off').value = plug.http.off_url;
      $('#pf-http-status').value = plug.http.status_url;
      $('#pf-http-post').checked = plug.http.use_post;
      $('#pf-http-user').value = plug.http.auth_user;
      $('#pf-http-pass').value = plug.http.auth_pass;
      $('#pf-family').value = plug.http.family || 'unknown';
      $('#pf-kind').value = plug.http.kind || 'unknown';
      $('#pf-mqtt-topic').value = plug.mqtt.cmd_topic;
      $('#pf-mqtt-on').value = plug.mqtt.payload_on;
      $('#pf-mqtt-off').value = plug.mqtt.payload_off;
      $('#pf-mqtt-state').value = plug.mqtt.state_topic;
      $('#pf-mqtt-qos').value = String(plug.mqtt.qos);
      $('#pf-mqtt-retain').checked = plug.mqtt.retain;
    } else {
      $('#plug-modal-title').textContent = 'Нова розетка';
      $('#pf-id').value = '';
      $('#pf-enabled').checked = true;
      $('#pf-on-delay').value = '0';
      $('#pf-mqtt-qos').value = '0';
      $('#pf-family').value = 'unknown';
      $('#pf-kind').value = 'unknown';
    }
    $('#btn-delete-plug').classList.toggle('hidden', !plug);
    /* "Увімкнена" only matters once a plug already exists (it's the runtime
     * on/off state); a brand-new plug's initial state now comes from
     * querying the device itself (see plug_manager_add), so hide the
     * checkbox entirely on the add form to avoid implying it's a choice. */
    $('#pf-enabled-row').classList.toggle('hidden', !plug);
    setDriverFieldsVisibility();
    updateHttpKnownFields();
    modal.classList.remove('hidden');
  }

  function closePlugModal() {
    modal.classList.add('hidden');
  }

  $('#btn-add-plug').addEventListener('click', () => openPlugModal(null));
  $('#btn-cancel-plug').addEventListener('click', closePlugModal);
  $('#btn-delete-plug').addEventListener('click', () => {
    const id = $('#pf-id').value;
    const name = $('#pf-name').value;
    if (id) deletePlug(id, name);
  });

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    rebuildKnownUrls(); /* in case the host field changed without a trailing keystroke reaching 'input' */
    const id = $('#pf-id').value;
    const { driver } = splitDriverValue();
    const body = {
      name: $('#pf-name').value.trim(),
      enabled: $('#pf-enabled').checked,
      input: $('#pf-input').value,
      invert: $('#pf-invert').checked,
      on_delay_ms: parseInt($('#pf-on-delay').value, 10) || 0,
      driver: driver,
      http: {
        on_url: $('#pf-http-on').value.trim(),
        off_url: $('#pf-http-off').value.trim(),
        status_url: $('#pf-http-status').value.trim(),
        use_post: $('#pf-http-post').checked,
        auth_user: $('#pf-http-user').value,
        auth_pass: $('#pf-http-pass').value,
        family: $('#pf-family').value,
        kind: $('#pf-kind').value,
      },
      mqtt: {
        cmd_topic: $('#pf-mqtt-topic').value.trim(),
        payload_on: $('#pf-mqtt-on').value,
        payload_off: $('#pf-mqtt-off').value,
        state_topic: $('#pf-mqtt-state').value.trim(),
        qos: parseInt($('#pf-mqtt-qos').value, 10),
        retain: $('#pf-mqtt-retain').checked,
      },
      zigbee: { note: '' },
    };

    if (!body.name) {
      $('#plug-form-error').textContent = "Вкажіть назву розетки";
      return;
    }
    if (driver === 'http' && FAMILY_URL_TEMPLATES[body.http.family] && !$('#pf-http-host').value.trim()) {
      $('#plug-form-error').textContent = "Вкажіть IP або хост пристрою";
      return;
    }
    if (driver === 'http' && (!body.http.on_url || !body.http.off_url)) {
      $('#plug-form-error').textContent = "Для HTTP драйвера потрібні ON і OFF URL";
      return;
    }
    if (driver === 'mqtt' && !body.mqtt.cmd_topic) {
      $('#plug-form-error').textContent = "Для MQTT драйвера потрібен топік команди";
      return;
    }

    try {
      if (id) {
        await api(`/api/plugs/${id}`, { method: 'PUT', body: JSON.stringify(body) });
      } else {
        await api('/api/plugs', { method: 'POST', body: JSON.stringify(body) });
      }
      closePlugModal();
      await loadPlugs();
    } catch (e) {
      $('#plug-form-error').textContent = 'Помилка: ' + e.message;
    }
  });

  /* ---------------- network scan ---------------- */
  const scanModal = $('#scan-modal');
  let scanPoll = null;
  let scanDotsFrame = 0; /* animates the "Іде пошук..." ellipsis, ticks once per poll */

  function stopScanPoll() {
    if (scanPoll) { clearInterval(scanPoll); scanPoll = null; }
  }

  /* Scanning is continuous now (see net_scan.h) -- it only stops when asked
   * to. Best-effort/fire-and-forget: called whenever the modal goes away
   * for any reason (explicit stop, picking a row, or just closing), so the
   * backend task doesn't keep looping in the background after nobody's
   * watching it. A failed/no-op call (e.g. nothing was running) isn't worth
   * surfacing to the user. */
  function stopBackendScan() {
    api('/api/scan', { method: 'DELETE' }).catch(() => {});
  }

  const FAMILY_LABEL = {
    shelly_gen1: 'Shelly (Gen1)',
    shelly_gen2: 'Shelly (Gen2+)',
    tasmota: 'Tasmota',
    esphome: 'ESPHome',
    unknown: null,
  };
  const KIND_WARNING = { relay: 'не розетка, реле', light: 'не розетка, світло' };

  function renderScanRow(r) {
    const tr = document.createElement('tr');
    tr.className = 'row-clickable';

    const familyTxt = FAMILY_LABEL[r.family];
    let typeCell = '<span class="hint">—</span>';
    if (familyTxt) {
      const title = r.model ? `${familyTxt} (${r.model})` : familyTxt;
      typeCell = `<span class="pill on" title="${escapeHtml(title)}">${escapeHtml(familyTxt)}</span>`;
      if (KIND_WARNING[r.kind]) typeCell += ` <span class="hint">${KIND_WARNING[r.kind]}</span>`;
      if (r.needs_auth) typeCell += ' <span class="hint">(потрібен пароль)</span>';
    }

    tr.innerHTML = `
      <td>
        <div class="scan-name">${r.name ? escapeHtml(r.name) : '<span class="hint">без імені</span>'}</div>
        <div class="scan-ip hint">${escapeHtml(r.ip)}${r.ping_ok === false ? ' <span title="Пристрій на зв\'язку (ARP), але не відповідає на ping — типово для файрволу чи деяких IoT-прошивок">(без ping)</span>' : ''}</div>
      </td>
      <td>${typeCell}</td>
      <td>${r.http ? '<span class="pill on">так</span>' : '<span class="pill off">ні</span>'}</td>`;
    tr.addEventListener('click', () => {
      stopScanPoll();
      stopBackendScan();
      scanModal.classList.add('hidden');
      openPlugModal(null);

      const fam = r.family || 'unknown';
      const templated = !!FAMILY_URL_TEMPLATES[fam];
      $('#pf-driver').value = templated ? `http:${fam}` : 'http';
      $('#pf-family').value = fam;
      $('#pf-kind').value = r.kind || 'unknown';
      setDriverFieldsVisibility();

      if (templated) {
        /* Host field + updateHttpKnownFields() below regenerates on/off from
         * the same template fingerprint.c used, so this is equivalent to
         * using r.on_url/r.off_url directly -- but going through the host
         * field keeps it editable/consistent if the device's IP changes. */
        $('#pf-http-host').value = r.ip;
      } else {
        /* Unknown, or a "known but not templated" family (ESPHome) -- use
         * whatever URL the fingerprint cascade worked out, or a bare IP
         * guess if nothing matched at all. */
        $('#pf-http-on').value = r.on_url || `http://${r.ip}/`;
        $('#pf-http-off').value = r.off_url || `http://${r.ip}/`;
        $('#pf-http-post').checked = !!r.use_post;
      }
      updateHttpKnownFields();
      /* Prefer the recognized model/family for the plug name (most
       * descriptive of what it actually IS); fall back to the reverse-DNS
       * hostname (most descriptive of which one it is, e.g. "kitchen-lamp"
       * from the router) when fingerprinting didn't recognize anything. */
      const suggestedName = r.model || familyTxt || r.name;
      if (suggestedName) {
        $('#pf-name').value = suggestedName;
      }
    });
    return tr;
  }

  async function pollScanStatus() {
    const statusEl = $('#scan-status');
    try {
      const s = await api('/api/scan');
      if (!s.have_subnet) {
        statusEl.innerHTML = '<span class="bad">STA не підключено — пошук доступний лише коли хаб підключений до домашньої Wi-Fi мережі</span>';
        stopScanPoll();
        return;
      }
      /* Continuous scan (net_scan.h): "running" stays true pass after pass
       * until explicitly stopped (or the backend safety cap kicks in), so
       * it's normal/expected for this to keep going for as long as the
       * modal is open with nothing picked yet -- that's the whole point.
       * Kept deliberately simple (no pass/scanned counters) -- just "still
       * looking" vs "done, here's the count". */
      if (s.running) {
        scanDotsFrame++;
        statusEl.textContent = 'Іде пошук' + '.'.repeat(scanDotsFrame % 4);
      } else {
        statusEl.textContent = `Пошук завершено, знайдено ${s.results.length} пристроїв`;
      }

      const tbody = $('#scan-tbody');
      tbody.innerHTML = '';
      s.results.forEach((r) => tbody.appendChild(renderScanRow(r)));

      if (!s.running) {
        /* Backend stopped on its own (safety cap, or STA dropped) -- reflect
         * that in the buttons instead of silently leaving "Завершити
         * пошук" showing for a scan that isn't actually running anymore. */
        stopScanPoll();
        setScanRunningUi(false);
      }
    } catch (e) {
      statusEl.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
      stopScanPoll();
      setScanRunningUi(false);
    }
  }

  /* While a continuous scan is running there's nothing useful "start" would
   * do (already-running 409), so it's disabled instead of just left
   * clickable; "Завершити пошук" is the mirror image -- only meaningful
   * while something is actually running. "Закрити" is always enabled: it
   * hides the modal regardless of scan state (and stops the scan too, see
   * its own handler below). */
  function setScanRunningUi(running) {
    $('#btn-scan-go').disabled = running;
    $('#btn-scan-stop').disabled = !running;
  }

  $('#btn-scan-network').addEventListener('click', () => {
    $('#scan-status').textContent = 'Натисніть "Почати пошук"';
    $('#scan-tbody').innerHTML = '';
    setScanRunningUi(false);
    scanModal.classList.remove('hidden');
  });

  /* Stops the backend scan but leaves the modal open -- pollScanStatus()
   * (already running on its 1s interval) picks up running:false on its own
   * next tick and flips the buttons back via setScanRunningUi(false), so
   * there's nothing else to do here. */
  $('#btn-scan-stop').addEventListener('click', () => {
    stopBackendScan();
  });

  /* Always closes the modal, regardless of whether a scan is running --
   * stopBackendScan() is a no-op if nothing was running (see its own
   * comment), so it's safe to call unconditionally rather than checking
   * state first. */
  $('#btn-scan-close').addEventListener('click', () => {
    stopScanPoll();
    stopBackendScan();
    setScanRunningUi(false);
    scanModal.classList.add('hidden');
  });

  $('#btn-scan-go').addEventListener('click', async () => {
    const statusEl = $('#scan-status');
    try {
      await api('/api/scan', { method: 'POST' });
      $('#scan-tbody').innerHTML = '';
      scanDotsFrame = 0;
      statusEl.textContent = 'Іде пошук';
      setScanRunningUi(true);
      stopScanPoll();
      scanPoll = setInterval(pollScanStatus, 1000);
      pollScanStatus();
    } catch (e) {
      statusEl.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
    }
  });

  /* ---------------- wifi network scan (SSID picker) ---------------- */
  const wifiScanModal = $('#wifi-scan-modal');
  let wifiScanPoll = null;

  function stopWifiScanPoll() {
    if (wifiScanPoll) { clearInterval(wifiScanPoll); wifiScanPoll = null; }
  }

  function renderWifiScanRow(n) {
    const tr = document.createElement('tr');
    tr.className = 'row-clickable';
    tr.innerHTML = `
      <td>${escapeHtml(n.ssid)}</td>
      <td>${n.secure ? '' : '<span class="hint">відкрита</span>'}</td>`;
    tr.addEventListener('click', () => {
      stopWifiScanPoll();
      wifiScanModal.classList.add('hidden');
      const ssidField = $('#wifi-ssid');
      const changingNetwork = ssidField.value !== n.ssid;
      ssidField.value = n.ssid;
      /* A different network's password (if any) is almost certainly not
       * the one already in the field -- clear it so the user isn't misled
       * into thinking the old value will work. Re-picking the same SSID
       * (e.g. just to double check the name) leaves it untouched. */
      if (changingNetwork) $('#wifi-pass').value = '';
      if (n.secure) $('#wifi-pass').focus();
    });
    return tr;
  }

  async function pollWifiScanStatus() {
    const statusEl = $('#wifi-scan-status');
    try {
      const s = await api('/api/wifi/scan');
      statusEl.textContent = s.running
        ? 'Пошук мереж...'
        : `Готово: знайдено ${s.results.length}`;

      const tbody = $('#wifi-scan-tbody');
      tbody.innerHTML = '';
      s.results.forEach((n) => tbody.appendChild(renderWifiScanRow(n)));

      if (!s.running) stopWifiScanPoll();
    } catch (e) {
      statusEl.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
      stopWifiScanPoll();
    }
  }

  $('#btn-scan-wifi').addEventListener('click', () => {
    $('#wifi-scan-status').textContent = 'Натисніть "Шукати"';
    $('#wifi-scan-tbody').innerHTML = '';
    wifiScanModal.classList.remove('hidden');
  });

  $('#btn-wifi-scan-close').addEventListener('click', () => {
    stopWifiScanPoll();
    wifiScanModal.classList.add('hidden');
  });

  $('#btn-wifi-scan-go').addEventListener('click', async () => {
    const statusEl = $('#wifi-scan-status');
    try {
      await api('/api/wifi/scan', { method: 'POST' });
      $('#wifi-scan-tbody').innerHTML = '';
      statusEl.textContent = 'Пошук почався...';
      stopWifiScanPoll();
      wifiScanPoll = setInterval(pollWifiScanStatus, 1000);
      pollWifiScanStatus();
    } catch (e) {
      statusEl.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
    }
  });

  /* ---------------- settings ---------------- */
  async function loadSettings() {
    try {
      const s = await api('/api/settings');
      const list = $('#inputs-tbody');
      list.innerHTML = '';
      s.inputs.forEach((inp, idx) => {
        /* <label> wrapping both the checkbox and its text -- clicking
         * anywhere on the row toggles it, same convention as the plug
         * form's "Увімкнена" row (#pf-enabled-row). */
        const row = document.createElement('label');
        row.className = 'input-row';
        row.innerHTML = `
          <input type="checkbox" data-idx="${idx}" class="inp-enabled" ${inp.enabled ? 'checked' : ''}>
          <span>${escapeHtml(inp.label)}</span>`;
        list.appendChild(row);
      });
      $('#set-mqtt-uri').value = s.mqtt_uri;
      $('#set-mqtt-client-id').value = s.mqtt_client_id;
      $('#set-mqtt-user').value = s.mqtt_user;
      $('#set-mqtt-pass').value = s.mqtt_pass;
      $('#set-log-size').value = s.log_max_file_size;
      $('#set-log-files').value = s.log_max_files;
      if (s.ac_mon) {
        $('#set-acmon-enabled').checked = s.ac_mon.enabled;
        $('#set-acmon-vmin').value = s.ac_mon.v_min;
        $('#set-acmon-vmax').value = s.ac_mon.v_max;
        $('#set-acmon-fmin').value = s.ac_mon.f_min;
        $('#set-acmon-fmax').value = s.ac_mon.f_max;
      }
    } catch (e) {
      $('#settings-save-result').textContent = 'Помилка завантаження: ' + e.message;
      $('#settings-save-result').className = 'save-result error';
    }
  }

  $('#btn-save-settings').addEventListener('click', async () => {
    const inputs = [];
    $$('.inp-enabled').forEach((cb) => {
      inputs.push({ enabled: cb.checked }); /* gpio is fixed in firmware, not sent */
    });
    const body = {
      inputs,
      /* debounce_ms/poll_interval_ms are fixed by Kconfig (see settings_from_json
       * in web_server.c) -- not sent, the server ignores them from this endpoint anyway. */
      mqtt_uri: $('#set-mqtt-uri').value.trim(),
      mqtt_client_id: $('#set-mqtt-client-id').value.trim(),
      mqtt_user: $('#set-mqtt-user').value,
      mqtt_pass: $('#set-mqtt-pass').value,
      log_max_file_size: parseInt($('#set-log-size').value, 10) || 65536,
      log_max_files: parseInt($('#set-log-files').value, 10) || 5,
      ac_mon: {
        enabled: $('#set-acmon-enabled').checked,
        v_min: parseInt($('#set-acmon-vmin').value, 10) || 0,
        v_max: parseInt($('#set-acmon-vmax').value, 10) || 0,
        f_min: parseFloat($('#set-acmon-fmin').value) || 0,
        f_max: parseFloat($('#set-acmon-fmax').value) || 0,
      },
    };
    const result = $('#settings-save-result');
    try {
      await api('/api/settings', { method: 'POST', body: JSON.stringify(body) });
      result.textContent = 'Збережено';
      result.className = 'save-result';
      await loadPlugs();
    } catch (e) {
      result.textContent = 'Помилка: ' + e.message;
      result.className = 'save-result error';
    }
  });

  /* ---------------- factory reset ---------------- */
  const resetModal = $('#reset-modal');

  $('#btn-factory-reset').addEventListener('click', () => {
    $('#reset-modal-result').textContent = '';
    $('#reset-modal-result').className = 'save-result';
    $('#btn-reset-cancel').disabled = false;
    $('#btn-reset-confirm').disabled = false;
    resetModal.classList.remove('hidden');
  });

  $('#btn-reset-cancel').addEventListener('click', () => {
    resetModal.classList.add('hidden');
  });

  $('#btn-reset-confirm').addEventListener('click', async () => {
    const result = $('#reset-modal-result');
    $('#btn-reset-cancel').disabled = true;
    $('#btn-reset-confirm').disabled = true;
    result.textContent = 'Стирання даних і перезавантаження…';
    result.className = 'save-result';
    try {
      await api('/api/system/factory_reset', { method: 'POST' });
    } catch (e) {
      /* expected -- the device drops the connection as it restarts */
    }
    result.textContent = 'Пристрій скинуто й перезавантажується. Підключіться до його точки доступу налаштування (SmartPlug-XXXXXX), щоб налаштувати заново.';
  });

  /* ---------------- wifi ---------------- */
  $('#btn-toggle-wifi-pass').addEventListener('click', () => {
    const input = $('#wifi-pass');
    const btn = $('#btn-toggle-wifi-pass');
    const showing = input.type === 'text';
    input.type = showing ? 'password' : 'text';
    btn.textContent = showing ? 'Показати' : 'Сховати';
  });

  async function loadWifiStatus() {
    const el = $('#wifi-status');
    try {
      const w = await api('/api/wifi');
      const failedNote = w.config_error
        ? '<br><span class="bad">Вичерпано спроби підключення (ймовірно невірний пароль або SSID) — введіть дані ще раз і натисніть "Підключити".</span>'
        : '';
      el.innerHTML = (w.connected
        ? `<span class="good">STA: Підключено до "${escapeHtml(w.ssid)}" (${w.sta_ip})</span>`
        : `<span class="bad">STA: Не підключено${w.ssid ? ` (збережено SSID "${escapeHtml(w.ssid)}")` : ''}</span>`) +
        failedNote +
        `<br>AP: Точка доступу для налаштування: IP ${w.ap_ip}`;
      /* Prefill both fields with the real stored values -- otherwise
       * clicking "Підключити" without retyping the password resubmits an
       * empty one and silently breaks a working connection. */
      $('#wifi-ssid').value = w.ssid;
      $('#wifi-pass').value = w.password;
    } catch (e) {
      el.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
    }
  }

  $('#btn-save-wifi').addEventListener('click', async () => {
    const result = $('#wifi-save-result');
    const ssid = $('#wifi-ssid').value.trim();
    const password = $('#wifi-pass').value;
    if (!ssid) { result.textContent = 'Вкажіть SSID'; result.className = 'save-result error'; return; }
    try {
      await api('/api/wifi', { method: 'POST', body: JSON.stringify({ ssid, password }) });
      result.textContent = 'Збережено, з’єднання встановлюється…';
      result.className = 'save-result';
      setTimeout(loadWifiStatus, 3000);
    } catch (e) {
      result.textContent = 'Помилка: ' + e.message;
      result.className = 'save-result error';
    }
  });

  /* ---------------- time ---------------- */
  async function loadTime() {
    const el = $('#time-status');
    try {
      const t = await api('/api/time');
      const d = new Date(t.epoch * 1000);
      const sourceTxt = { ntp: 'NTP', manual: 'вручну', none: 'не синхронізовано' }[t.source] || t.source;
      el.innerHTML = t.synced
        ? `<span class="good">Час пристрою: ${d.toLocaleString()} (${sourceTxt})</span>`
        : `<span class="bad">Час не синхронізовано: ${d.toLocaleString()} — або зачекайте NTP-синхронізацію (потрібен інтернет), або встановіть вручну нижче</span>`;

      /* Prefill the manual input with "now" (browser time) for convenience,
       * but don't clobber it if the user is mid-edit. */
      const input = $('#time-manual-input');
      if (document.activeElement !== input && !input.value) {
        const now = new Date(Date.now() - new Date().getTimezoneOffset() * 60000);
        input.value = now.toISOString().slice(0, 19);
      }
    } catch (e) {
      el.innerHTML = `<span class="bad">Помилка: ${e.message}</span>`;
    }
  }

  $('#btn-set-time').addEventListener('click', async () => {
    const result = $('#time-save-result');
    const val = $('#time-manual-input').value;
    if (!val) { result.textContent = 'Вкажіть дату і час'; result.className = 'save-result error'; return; }
    const epoch = Math.floor(new Date(val).getTime() / 1000);
    if (!isFinite(epoch)) { result.textContent = 'Некоректна дата'; result.className = 'save-result error'; return; }
    try {
      await api('/api/time', { method: 'POST', body: JSON.stringify({ epoch }) });
      result.textContent = 'Час встановлено';
      result.className = 'save-result';
      await loadTime();
      await loadLog();
    } catch (e) {
      result.textContent = 'Помилка: ' + e.message;
      result.className = 'save-result error';
    }
  });

  /* ---------------- log ---------------- */
  async function loadLog() {
    const list = $('#log-tbody');
    list.innerHTML = '<div class="list-empty">завантаження…</div>';
    try {
      const limit = $('#log-limit').value;
      const entries = await api(`/api/log?limit=${limit}`);
      if (!entries.length) {
        list.innerHTML = '<div class="list-empty">Журнал порожній</div>';
        return;
      }
      list.innerHTML = '';
      entries.slice().reverse().forEach((e) => {
        const row = document.createElement('div');
        row.className = 'log-entry';
        const d = new Date((e.t || 0) * 1000);
        const timeTxt = isFinite(d.getTime()) ? d.toLocaleString() : String(e.t);
        row.innerHTML = `
          <div class="log-meta">
            <span class="log-time">${timeTxt}</span>
            <span class="log-src">${escapeHtml(e.src || '')}</span>
            ${e.plug ? `<span class="log-plug">${escapeHtml(e.plug)}</span>` : ''}
            <span class="log-action">${escapeHtml(e.action || '')}</span>
            ${e.ok ? '<span class="pill on">OK</span>' : '<span class="pill fail">FAIL</span>'}
          </div>
          ${e.detail ? `<div class="log-detail">${escapeHtml(e.detail)}</div>` : ''}`;
        list.appendChild(row);
      });
    } catch (e) {
      list.innerHTML = `<div class="list-empty error-text">Помилка: ${e.message}</div>`;
    }
  }

  $('#btn-refresh-log').addEventListener('click', loadLog);
  $('#log-limit').addEventListener('change', loadLog);
  $('#btn-clear-log').addEventListener('click', async () => {
    if (!confirm('Очистити весь журнал подій?')) return;
    try {
      await api('/api/log/clear', { method: 'POST' });
      await loadLog();
    } catch (e) {
      alert('Помилка: ' + e.message);
    }
  });

  /* ---------------- init ---------------- */
  loadPlugs();
  loadStatus();
  setInterval(loadStatus, 5000);
  /* Power metrics update in the background every ~15s (metrics_poll_task in
   * plug_manager.c) -- refresh the list periodically so wattage readings
   * aren't stuck at whatever they were on page load. Skipped while the edit
   * modal is open so a background reload can't yank a card out from under
   * someone mid-edit. */
  setInterval(() => {
    if (modal.classList.contains('hidden')) loadPlugs();
  }, 10000);
})();
