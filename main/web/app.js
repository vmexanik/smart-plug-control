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

  /* ---------------- tabs ---------------- */
  $$('.tab-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      $$('.tab-btn').forEach((b) => b.classList.remove('active'));
      $$('.tab-content').forEach((c) => c.classList.remove('active'));
      btn.classList.add('active');
      $('#tab-' + btn.dataset.tab).classList.add('active');
      if (btn.dataset.tab === 'settings') loadSettings();
      if (btn.dataset.tab === 'wifi') loadWifiStatus();
      if (btn.dataset.tab === 'log') loadLog();
    });
  });

  /* ---------------- status bar ---------------- */
  async function loadStatus() {
    try {
      const s = await api('/api/status');
      const wifiTxt = s.wifi.sta_connected
        ? `<span class="good">STA: ${s.wifi.ssid} (${s.wifi.sta_ip})</span>`
        : `<span class="bad">STA не підключено</span>`;
      const inputsTxt = s.inputs
        .filter(i => i.enabled)
        .map(i => `${i.label}=${i.level ? 1 : 0}`)
        .join(', ') || 'немає активних входів';
      $('#status-bar').innerHTML = `${wifiTxt} · AP: ${s.wifi.ap_ip} · Входи: ${inputsTxt} · uptime ${formatUptime(s.uptime_s)}`;
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
  async function loadPlugs() {
    const tbody = $('#plugs-tbody');
    tbody.innerHTML = '<tr><td colspan="7">завантаження…</td></tr>';
    try {
      const plugs = await api('/api/plugs');
      if (!plugs.length) {
        tbody.innerHTML = '<tr><td colspan="7">Немає розеток. Натисніть "Додати розетку".</td></tr>';
        return;
      }
      tbody.innerHTML = '';
      plugs.forEach((p) => tbody.appendChild(renderPlugRow(p)));
    } catch (e) {
      tbody.innerHTML = `<tr><td colspan="7" class="error-text">Помилка: ${e.message}</td></tr>`;
    }
  }

  function renderPlugRow(p) {
    const tr = document.createElement('tr');

    const statePill = p.last_cmd_ok
      ? `<span class="pill ${p.last_state ? 'on' : 'off'}">${p.last_state ? 'ON' : 'OFF'}</span>`
      : `<span class="pill fail">помилка</span>`;

    tr.innerHTML = `
      <td>${escapeHtml(p.name)}${p.enabled ? '' : ' <span class="hint">(вимкнена)</span>'}</td>
      <td>${p.driver.toUpperCase()}</td>
      <td>${p.input}</td>
      <td>${p.invert ? 'так' : 'ні'}</td>
      <td>${p.enabled ? 'так' : 'ні'}</td>
      <td>${statePill}</td>
      <td class="row-actions">
        <button data-act="on">ON</button>
        <button data-act="off">OFF</button>
        <button data-act="edit">Редагувати</button>
        <button data-act="del" class="danger">Видалити</button>
      </td>`;

    tr.querySelector('[data-act="on"]').addEventListener('click', () => testPlug(p.id, true));
    tr.querySelector('[data-act="off"]').addEventListener('click', () => testPlug(p.id, false));
    tr.querySelector('[data-act="edit"]').addEventListener('click', () => openPlugModal(p));
    tr.querySelector('[data-act="del"]').addEventListener('click', () => deletePlug(p));

    return tr;
  }

  async function testPlug(id, on) {
    try {
      const r = await api(`/api/plugs/${id}/action`, { method: 'POST', body: JSON.stringify({ action: on ? 'on' : 'off' }) });
      if (!r.ok) alert('Команда не виконана: ' + r.detail);
      await loadPlugs();
    } catch (e) {
      alert('Помилка: ' + e.message);
    }
  }

  async function deletePlug(p) {
    if (!confirm(`Видалити розетку "${p.name}"?`)) return;
    try {
      await api(`/api/plugs/${p.id}`, { method: 'DELETE' });
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

  function setDriverFieldsVisibility() {
    const d = $('#pf-driver').value;
    $('#pf-http-fields').style.display = d === 'http' ? '' : 'none';
    $('#pf-mqtt-fields').style.display = d === 'mqtt' ? '' : 'none';
    $('#pf-zigbee-fields').style.display = d === 'zigbee' ? '' : 'none';
  }
  $('#pf-driver').addEventListener('change', setDriverFieldsVisibility);

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
      $('#pf-driver').value = plug.driver;
      $('#pf-http-on').value = plug.http.on_url;
      $('#pf-http-off').value = plug.http.off_url;
      $('#pf-http-status').value = plug.http.status_url;
      $('#pf-http-post').checked = plug.http.use_post;
      $('#pf-http-user').value = plug.http.auth_user;
      $('#pf-http-pass').value = plug.http.auth_pass;
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
      $('#pf-mqtt-qos').value = '0';
    }
    setDriverFieldsVisibility();
    modal.classList.remove('hidden');
  }

  function closePlugModal() {
    modal.classList.add('hidden');
  }

  $('#btn-add-plug').addEventListener('click', () => openPlugModal(null));
  $('#btn-cancel-plug').addEventListener('click', closePlugModal);
  $('#btn-refresh-plugs').addEventListener('click', loadPlugs);

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const id = $('#pf-id').value;
    const body = {
      name: $('#pf-name').value.trim(),
      enabled: $('#pf-enabled').checked,
      input: $('#pf-input').value,
      invert: $('#pf-invert').checked,
      driver: $('#pf-driver').value,
      http: {
        on_url: $('#pf-http-on').value.trim(),
        off_url: $('#pf-http-off').value.trim(),
        status_url: $('#pf-http-status').value.trim(),
        use_post: $('#pf-http-post').checked,
        auth_user: $('#pf-http-user').value,
        auth_pass: $('#pf-http-pass').value,
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
    if (body.driver === 'http' && (!body.http.on_url || !body.http.off_url)) {
      $('#plug-form-error').textContent = "Для HTTP драйвера потрібні ON і OFF URL";
      return;
    }
    if (body.driver === 'mqtt' && !body.mqtt.cmd_topic) {
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

  /* ---------------- settings ---------------- */
  async function loadSettings() {
    try {
      const s = await api('/api/settings');
      const tbody = $('#inputs-tbody');
      tbody.innerHTML = '';
      s.inputs.forEach((inp, idx) => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td><input type="checkbox" data-idx="${idx}" class="inp-enabled" ${inp.enabled ? 'checked' : ''}></td>
          <td>${inp.label}</td>
          <td><input type="number" data-idx="${idx}" class="inp-gpio" min="0" max="39" value="${inp.gpio}" style="width:70px"></td>`;
        tbody.appendChild(tr);
      });
      $('#set-debounce').value = s.debounce_ms;
      $('#set-poll').value = s.poll_interval_ms;
      $('#set-mqtt-uri').value = s.mqtt_uri;
      $('#set-mqtt-client-id').value = s.mqtt_client_id;
      $('#set-mqtt-user').value = s.mqtt_user;
      $('#set-mqtt-pass').value = s.mqtt_pass;
      $('#set-log-size').value = s.log_max_file_size;
      $('#set-log-files').value = s.log_max_files;
    } catch (e) {
      $('#settings-save-result').textContent = 'Помилка завантаження: ' + e.message;
      $('#settings-save-result').className = 'save-result error';
    }
  }

  $('#btn-save-settings').addEventListener('click', async () => {
    const inputs = [];
    $$('.inp-enabled').forEach((cb, i) => {
      const gpio = parseInt($$('.inp-gpio')[i].value, 10);
      inputs.push({ enabled: cb.checked, gpio: isNaN(gpio) ? 0 : gpio });
    });
    const body = {
      inputs,
      debounce_ms: parseInt($('#set-debounce').value, 10) || 0,
      poll_interval_ms: parseInt($('#set-poll').value, 10) || 100,
      mqtt_uri: $('#set-mqtt-uri').value.trim(),
      mqtt_client_id: $('#set-mqtt-client-id').value.trim(),
      mqtt_user: $('#set-mqtt-user').value,
      mqtt_pass: $('#set-mqtt-pass').value,
      log_max_file_size: parseInt($('#set-log-size').value, 10) || 65536,
      log_max_files: parseInt($('#set-log-files').value, 10) || 5,
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

  /* ---------------- wifi ---------------- */
  async function loadWifiStatus() {
    const el = $('#wifi-status');
    try {
      const w = await api('/api/wifi');
      el.innerHTML = w.connected
        ? `<span class="good">Підключено до "${escapeHtml(w.ssid)}" — IP ${w.sta_ip}</span>`
        : `<span class="bad">Не підключено${w.ssid ? ` (збережено SSID "${escapeHtml(w.ssid)}")` : ''}</span>` +
          `<br>Точка доступу для налаштування: IP ${w.ap_ip}`;
      $('#wifi-ssid').value = w.ssid;
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

  /* ---------------- log ---------------- */
  async function loadLog() {
    const tbody = $('#log-tbody');
    tbody.innerHTML = '<tr><td colspan="6">завантаження…</td></tr>';
    try {
      const limit = $('#log-limit').value;
      const entries = await api(`/api/log?limit=${limit}`);
      if (!entries.length) {
        tbody.innerHTML = '<tr><td colspan="6">Журнал порожній</td></tr>';
        return;
      }
      tbody.innerHTML = '';
      entries.slice().reverse().forEach((e) => {
        const tr = document.createElement('tr');
        const d = new Date((e.t || 0) * 1000);
        tr.innerHTML = `
          <td>${isFinite(d.getTime()) ? d.toLocaleString() : e.t}</td>
          <td>${escapeHtml(e.src || '')}</td>
          <td>${escapeHtml(e.plug || '')}</td>
          <td>${escapeHtml(e.action || '')}</td>
          <td>${e.ok ? '<span class="pill on">OK</span>' : '<span class="pill fail">FAIL</span>'}</td>
          <td>${escapeHtml(e.detail || '')}</td>`;
        tbody.appendChild(tr);
      });
    } catch (e) {
      tbody.innerHTML = `<tr><td colspan="6" class="error-text">Помилка: ${e.message}</td></tr>`;
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
})();
