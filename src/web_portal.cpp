#include "web_portal.h"
#include "abcd_bus.h"
#include "water_sensor.h"
#include "controller.h"
#include <WiFi.h>
#include <WebServer.h>

static WebServer s_server(80);
WebPortal g_webPortal;

static void formatHexBytes(const uint8_t* data, size_t len, char* out, size_t outSize) {
    size_t pos = 0;
    for (size_t i = 0; i < len && (pos + 4) < outSize; ++i) {
        pos += snprintf(out + pos, outSize - pos, "%s%02X", (i > 0 ? " " : ""), data[i]);
    }
    if (pos < outSize) {
        out[pos] = '\0';
    }
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Heil G96CTN &amp; Wood Boiler Controller</title>
  <style>
    :root {
      --bg: #0b0f19;
      --card: #131b2e;
      --card-hi: #172542;
      --border: #23304d;
      --text: #e2e8f0;
      --muted: #94a3b8;
      --accent: #38bdf8;
      --green: #10b981;
      --amber: #f59e0b;
      --red: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      padding: 20px 14px 48px;
      max-width: 1200px;
      margin: 0 auto;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 12px;
      padding-bottom: 14px;
      margin-bottom: 16px;
      border-bottom: 1px solid var(--border);
    }
    h1 { font-size: 1.35rem; letter-spacing: 0.01em; }
    .subtitle { color: var(--muted); font-size: 0.83rem; margin-top: 4px; }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 7px 14px;
      border-radius: 999px;
      background: #132238;
      color: #f8fafc;
      border: 1px solid var(--border);
      font-size: 0.8rem;
      font-weight: 700;
    }
    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: #38bdf8;
      box-shadow: 0 0 8px #38bdf8;
    }
    .tabs {
      display: flex;
      gap: 8px;
      margin-bottom: 18px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 10px;
    }
    .tab-btn {
      background: var(--card);
      color: var(--muted);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 9px 16px;
      font-size: 0.88rem;
      font-weight: 600;
      cursor: pointer;
    }
    .tab-btn.active {
      background: #0c4a6e;
      color: #fff;
      border-color: var(--accent);
    }
    .tab-pane { display: none; }
    .tab-pane.active { display: block; }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(215px, 1fr));
      gap: 12px;
      margin-bottom: 16px;
    }
    .card {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 15px;
    }
    .card.highlight {
      background: var(--card-hi);
      border-color: #2e4a7d;
    }
    .label { color: var(--muted); font-size: 0.74rem; text-transform: uppercase; letter-spacing: 0.06em; }
    .value { font-size: 1.35rem; font-weight: 700; margin-top: 6px; }
    .subvalue { color: var(--muted); font-size: 0.78rem; margin-top: 5px; }
    .pill {
      display: inline-block;
      margin-top: 6px;
      padding: 3px 8px;
      border-radius: 6px;
      font-size: 0.73rem;
      font-weight: 700;
      background: #1e293b;
      color: #38bdf8;
    }
    .two-col {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(360px, 1fr));
      gap: 16px;
      margin-bottom: 16px;
    }
    .panel {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 18px;
    }
    .panel h2 {
      font-size: 0.92rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      color: var(--accent);
      margin-bottom: 14px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .form-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 11px;
      font-size: 0.87rem;
    }
    .form-row label { color: var(--text); flex: 1; }
    input[type="number"], select {
      background: #0b1324;
      color: var(--text);
      border: 1px solid #334155;
      border-radius: 6px;
      padding: 7px 10px;
      width: 175px;
      font-size: 0.86rem;
    }
    input[type="range"] { width: 175px; accent-color: var(--accent); }
    .btn-row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; }
    button, .btn-link {
      background: #1e293b;
      color: var(--text);
      border: 1px solid #334155;
      border-radius: 8px;
      padding: 8px 12px;
      font-size: 0.81rem;
      font-weight: 600;
      cursor: pointer;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }
    button:hover, .btn-link:hover { border-color: var(--accent); background: #26354d; }
    button.primary { background: #0284c7; border-color: #38bdf8; color: #fff; }
    button.primary:hover { background: #0369a1; }
    .table-wrap {
      overflow-x: auto;
      margin-top: 12px;
      border: 1px solid var(--border);
      border-radius: 10px;
      background: #090d16;
      max-height: 520px;
      overflow-y: auto;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 0.78rem;
    }
    th, td {
      padding: 8px 10px;
      text-align: left;
      border-bottom: 1px solid #162033;
      white-space: nowrap;
    }
    th {
      background: #111827;
      color: var(--muted);
      position: sticky;
      top: 0;
    }
    .tag {
      padding: 2px 6px;
      border-radius: 4px;
      font-weight: 700;
      font-size: 0.72rem;
    }
    .tag-write { background: #451a03; color: #fbbf24; }
    .tag-read  { background: #0f172a; color: #93c5fd; }
    .tag-ack   { background: #064e3b; color: #34d399; }
    .tag-tx    { background: #3b0764; color: #e9d5ff; }
    .crc-ok    { color: #34d399; }
    .crc-err   { color: #f87171; font-weight: bold; }
    .toast {
      font-size: 0.8rem;
      color: #34d399;
      margin-left: 10px;
      opacity: 0;
      transition: opacity 0.25s;
    }
  </style>
</head>
<body>
  <header>
    <div>
      <h1>Heil G96CTN0601714A1 &amp; Wood Boiler Controller</h1>
      <div class="subtitle">Parallel ABCD Bus Co-Controller &amp; RS-485 Protocol Analyzer &bull; AITRIP ESP32-S3-N16R8</div>
    </div>
    <div class="badge"><span class="dot" id="stateDot"></span><span id="stateBadge">LOADING...</span></div>
  </header>

  <nav class="tabs">
    <button class="tab-btn active" onclick="switchTab('control')" id="tabbtn-control">1. Furnace &amp; Wood Boiler Control</button>
    <button class="tab-btn" onclick="switchTab('sniffer')" id="tabbtn-sniffer">2. RS-485 ABCD Bus Analyzer (<span id="pktCountBadge">0</span>)</button>
  </nav>

  <!-- TAB 1: CONTROL & TELEMETRY -->
  <div class="tab-pane active" id="tab-control">
    <section class="grid">
      <div class="card">
        <div class="label">Wood Boiler Supply Water</div>
        <div class="value" id="supplyTemp">-- °F</div>
        <div class="subvalue" id="returnTemp">Return: -- °F (&Delta;T: -- °F)</div>
      </div>
      <div class="card">
        <div class="label">Ion Sensed Room Temp</div>
        <div class="value" id="roomTemp">-- °F</div>
        <div class="subvalue" id="comfortTarget">Comfort Target: -- °F</div>
      </div>
      <div class="card highlight">
        <div class="label">Ion Thermostat Setpoint (0x2001)</div>
        <div class="value" id="ionSpBig" style="color:#38bdf8;">-- °F</div>
        <div class="pill" id="ionSpStatus">--</div>
      </div>
      <div class="card">
        <div class="label">Heil G96CTN ECM Blower</div>
        <div class="value" id="fanMode">--</div>
        <div class="subvalue" id="cfmRpm">-- CFM &bull; -- RPM</div>
      </div>
      <div class="card">
        <div class="label">Heil G96CTN Heating Stage</div>
        <div class="value" id="furnaceStage" style="font-size:1.05rem;">--</div>
        <div class="subvalue" id="supplyAirInfo">Supply Air: -- °F</div>
      </div>
      <div class="card">
        <div class="label">ESP32-S3 &amp; ABCD Bus Health</div>
        <div class="value" id="wifiInfo" style="font-size:1.05rem;">--</div>
        <div class="subvalue" id="busStats">RX: 0 &bull; TX: 0 &bull; PSRAM: -- MB</div>
      </div>
    </section>

    <div class="two-col">
      <!-- Persistent Controller Configuration -->
      <div class="panel">
        <h2>
          <span>Automated Co-Controller Settings (NVS)</span>
          <span class="toast" id="saveToast">Saved to Flash!</span>
        </h2>
        <div class="form-row">
          <label for="cfgOpMode">Operating Mode</label>
          <select id="cfgOpMode">
            <option value="0">0 - Passive Bus Sniffer Only (DE/RE LOW)</option>
            <option value="1">1 - Auto Wood Boiler Co-Controller</option>
            <option value="2">2 - Manual ECM Blower Override</option>
          </select>
        </div>
        <div class="form-row">
          <label for="cfgBoilerOn">Wood Boiler Active Threshold (°F)</label>
          <input type="number" step="1" min="100" max="210" id="cfgBoilerOn" value="140">
        </div>
        <div class="form-row">
          <label for="cfgBoilerOff">Wood Boiler Cold Fallback (°F)</label>
          <input type="number" step="1" min="90" max="200" id="cfgBoilerOff" value="130">
        </div>
        <div class="form-row">
          <label for="cfgComfortSp">Target Room Comfort Setpoint (°F)</label>
          <input type="number" step="0.5" min="55" max="85" id="cfgComfortSp" value="70.0">
        </div>
        <div class="form-row">
          <label for="cfgFallbackSp">Ion Gas Backup Setpoint When Wood Hot (°F)</label>
          <input type="number" step="1" min="50" max="75" id="cfgFallbackSp" value="64">
        </div>
        <div class="form-row">
          <label for="cfgFanSpeed">Hydronic Heating ECM Blower Speed</label>
          <select id="cfgFanSpeed">
            <option value="1">LOW (650 CFM / Quiet Circulation)</option>
            <option value="2">MED (1,050 CFM / Balanced Hydronic Heat)</option>
            <option value="3">HIGH (1,400 CFM / Max BTU Output)</option>
          </select>
        </div>
        <div class="btn-row">
          <button class="primary" onclick="saveConfig()">Save &amp; Apply Settings</button>
        </div>
      </div>

      <!-- Interactive Bench Simulation Panel -->
      <div class="panel">
        <h2>
          <span>Desk Prototyping &amp; Sensor Simulator</span>
          <span id="probeBadge" style="font-size:0.75rem;color:var(--muted);">Probes: 0</span>
        </h2>
        <div class="form-row">
          <label>Simulated Boiler Supply: <strong><span id="lblSimSupply">155</span> °F</strong></label>
          <input type="range" min="90" max="195" step="1" id="simSupply" value="155" oninput="updateSimLabels()" onchange="pushSim()">
        </div>
        <div class="form-row">
          <label>Simulated Boiler Return: <strong><span id="lblSimReturn">138</span> °F</strong></label>
          <input type="range" min="80" max="185" step="1" id="simReturn" value="138" oninput="updateSimLabels()" onchange="pushSim()">
        </div>
        <div class="form-row">
          <label>Simulated Room Temp: <strong><span id="lblSimRoom">68.5</span> °F</strong></label>
          <input type="range" min="60" max="76" step="0.5" id="simRoom" value="68.5" oninput="updateSimLabels()" onchange="pushSim()">
        </div>
        <div class="form-row">
          <label>Ion Thermostat Setpoint: <strong><span id="lblSimIonSp">64</span> °F</strong></label>
          <input type="range" min="55" max="80" step="1" id="simIonSp" value="64" oninput="updateSimLabels()" onchange="pushIonSp()">
        </div>
        <div class="form-row">
          <label for="simBusToggle">Periodic ABCD Bus Traffic Generator</label>
          <select id="simBusToggle" onchange="pushSim()">
            <option value="1">Enabled (Every 4s on Bench)</option>
            <option value="0">Disabled (Physical RS-485 Only)</option>
          </select>
        </div>
        <div class="btn-row">
          <button onclick="quickScenario(160, 142, 68.0)">1. Boiler Hot + Room Cold (Blower ON, Ion-&gt;64°F)</button>
          <button onclick="quickScenario(160, 148, 71.0)">2. Boiler Hot + Room Warm (Standby, Ion-&gt;64°F)</button>
          <button onclick="quickScenario(115, 110, 71.0)">3. Boiler Cold + Room Warm (Coasting, Ion 64°F)</button>
          <button onclick="quickScenario(115, 110, 68.0)">4. Boiler Cold + Room Cold (Restore Ion-&gt;70°F, Gas ON)</button>
        </div>
      </div>
    </div>
  </div>

  <!-- TAB 2: RS-485 BUS ANALYZER -->
  <div class="tab-pane" id="tab-sniffer">
    <div class="panel">
      <h2>
        <span>Live Carrier / ICP ABCD RS-485 Packet Analyzer (38400 8N1 &bull; 8MB PSRAM Buffer)</span>
        <span id="snifferStats" style="font-size:0.78rem;color:var(--muted);">--</span>
      </h2>
      <div class="btn-row" style="margin-top:0;margin-bottom:10px;">
        <select id="pktFilter" onchange="refreshPackets()" style="width:190px;">
          <option value="all">Filter: All Bus Frames</option>
          <option value="write">Filter: WRITE Commands (0x0C)</option>
          <option value="tx">Filter: ESP32 Co-Controller TX</option>
          <option value="tstat">Filter: Ion Thermostat (0x2001)</option>
          <option value="furnace">Filter: Heil G96CTN (0x4001)</option>
        </select>
        <button onclick="injectBusCycle()" class="primary">Inject Simulated Bus Cycle</button>
        <button onclick="togglePauseCapture()" id="btnPause">Pause Capture</button>
        <button onclick="clearPackets()">Clear PSRAM Buffer</button>
        <a class="btn-link" href="/api/packets.csv" download="abcd_bus_capture.csv">Download Capture (.CSV)</a>
      </div>

      <div class="table-wrap">
        <table>
          <thead>
            <tr>
              <th>#</th>
              <th>Time (s)</th>
              <th>Origin</th>
              <th>Source &rarr; Dest</th>
              <th>Func</th>
              <th>Table:Reg</th>
              <th>Decoded Summary</th>
              <th>CRC-16</th>
              <th>Raw Hex Frame</th>
            </tr>
          </thead>
          <tbody id="pktTableBody">
            <tr><td colspan="9" style="color:var(--muted);">Loading packets from 8MB Octal PSRAM buffer...</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>

  <script>
    let g_activeTab = 'control';
    let g_cfgInitialized = false;
    let g_simInitialized = false;
    let g_capturePaused = false;

    function switchTab(tab) {
      g_activeTab = tab;
      document.getElementById('tab-control').classList.toggle('active', tab === 'control');
      document.getElementById('tab-sniffer').classList.toggle('active', tab === 'sniffer');
      document.getElementById('tabbtn-control').classList.toggle('active', tab === 'control');
      document.getElementById('tabbtn-sniffer').classList.toggle('active', tab === 'sniffer');
      if (tab === 'sniffer') refreshPackets();
    }

    function updateSimLabels() {
      document.getElementById('lblSimSupply').textContent = document.getElementById('simSupply').value;
      document.getElementById('lblSimReturn').textContent = document.getElementById('simReturn').value;
      document.getElementById('lblSimRoom').textContent = document.getElementById('simRoom').value;
      document.getElementById('lblSimIonSp').textContent = document.getElementById('simIonSp').value;
    }

    async function refreshStatus() {
      try {
        const res = await fetch('/api/status');
        const d = await res.json();

        document.getElementById('stateBadge').textContent = d.ctrl_state;
        document.getElementById('stateDot').style.background = d.led_hex;
        document.getElementById('stateDot').style.boxShadow = '0 0 8px ' + d.led_hex;

        document.getElementById('supplyTemp').textContent = d.supply_f.toFixed(1) + ' °F';
        document.getElementById('returnTemp').textContent =
          'Return: ' + d.return_f.toFixed(1) + ' °F (ΔT: ' + d.delta_t_f.toFixed(1) + ' °F)';

        document.getElementById('roomTemp').textContent = d.room_f.toFixed(1) + ' °F';
        document.getElementById('comfortTarget').textContent =
          'Target Comfort: ' + d.cfg.comfort_sp.toFixed(1) + ' °F (Boiler On >= ' + d.cfg.boiler_on.toFixed(0) + ' °F)';

        document.getElementById('ionSpBig').textContent = d.ion_heat_sp_f.toFixed(1) + ' °F';
        const spPill = document.getElementById('ionSpStatus');
        spPill.textContent = d.ion_sp_status;
        if (d.ion_sp_status.includes('RESTORED')) {
          spPill.style.background = '#1e3a8a';
          spPill.style.color = '#93c5fd';
        } else if (d.ion_sp_status.includes('BACKUP')) {
          spPill.style.background = '#064e3b';
          spPill.style.color = '#34d399';
        } else {
          spPill.style.background = '#1e293b';
          spPill.style.color = '#cbd5e1';
        }

        document.getElementById('fanMode').textContent = d.fan_mode;
        document.getElementById('cfmRpm').textContent = d.airflow_cfm + ' CFM • ' + d.blower_rpm + ' RPM';

        document.getElementById('furnaceStage').textContent = d.furnace_stage;
        document.getElementById('supplyAirInfo').textContent = 'Plenum Supply Air: ' + d.supply_air_f.toFixed(0) + ' °F';

        document.getElementById('wifiInfo').textContent = d.ip + ' (' + d.rssi_dbm + ' dBm)';
        document.getElementById('busStats').textContent =
          'RX: ' + d.bus.rx + ' • TX: ' + d.bus.tx + ' • Free PSRAM: ' + (d.free_psram / (1024*1024)).toFixed(2) + ' MB';

        document.getElementById('pktCountBadge').textContent = d.bus.stored;
        document.getElementById('probeBadge').textContent =
          d.water_sim ? 'Mode: Web Simulator (0 physical probes on GPIO 4)' : ('Physical DS18B20 Probes: ' + d.probes);

        g_capturePaused = d.bus.paused;
        document.getElementById('btnPause').textContent = g_capturePaused ? 'Resume Capture' : 'Pause Capture';

        // Keep the Ion Thermostat Setpoint slider synced with the active thermostat setpoint
        document.getElementById('simIonSp').value = Math.round(d.ion_heat_sp_f);
        document.getElementById('lblSimIonSp').textContent = Math.round(d.ion_heat_sp_f);

        if (!g_cfgInitialized) {
          document.getElementById('cfgOpMode').value = d.cfg.op_mode;
          document.getElementById('cfgBoilerOn').value = d.cfg.boiler_on;
          document.getElementById('cfgBoilerOff').value = d.cfg.boiler_off;
          document.getElementById('cfgComfortSp').value = d.cfg.comfort_sp;
          document.getElementById('cfgFallbackSp').value = d.cfg.fallback_sp;
          document.getElementById('cfgFanSpeed').value = d.cfg.fan_speed;
          g_cfgInitialized = true;
        }
        if (!g_simInitialized) {
          document.getElementById('simSupply').value = d.supply_f;
          document.getElementById('simReturn').value = d.return_f;
          document.getElementById('simRoom').value = d.room_f;
          document.getElementById('simBusToggle').value = d.bus.sim_bus ? '1' : '0';
          updateSimLabels();
          g_simInitialized = true;
        }
      } catch (e) {}
    }

    async function saveConfig() {
      const params = new URLSearchParams({
        op_mode: document.getElementById('cfgOpMode').value,
        boiler_on: document.getElementById('cfgBoilerOn').value,
        boiler_off: document.getElementById('cfgBoilerOff').value,
        comfort_sp: document.getElementById('cfgComfortSp').value,
        fallback_sp: document.getElementById('cfgFallbackSp').value,
        fan_speed: document.getElementById('cfgFanSpeed').value
      });
      await fetch('/api/config?' + params.toString(), { method: 'POST' });
      const t = document.getElementById('saveToast');
      t.style.opacity = 1;
      setTimeout(() => { t.style.opacity = 0; }, 2200);
      refreshStatus();
    }

    async function pushSim() {
      const params = new URLSearchParams({
        supply_f: document.getElementById('simSupply').value,
        return_f: document.getElementById('simReturn').value,
        room_f: document.getElementById('simRoom').value,
        sim_bus: document.getElementById('simBusToggle').value
      });
      await fetch('/api/sim?' + params.toString(), { method: 'POST' });
      refreshStatus();
      if (g_activeTab === 'sniffer') refreshPackets();
    }

    async function pushIonSp() {
      const params = new URLSearchParams({
        ion_sp_f: document.getElementById('simIonSp').value
      });
      await fetch('/api/sim?' + params.toString(), { method: 'POST' });
      refreshStatus();
      if (g_activeTab === 'sniffer') refreshPackets();
    }

    async function quickScenario(sup, ret, room) {
      document.getElementById('simSupply').value = sup;
      document.getElementById('simReturn').value = ret;
      document.getElementById('simRoom').value = room;
      updateSimLabels();
      await pushSim();
    }

    async function injectBusCycle() {
      await fetch('/api/sim?inject_cycle=1', { method: 'POST' });
      refreshPackets();
      refreshStatus();
    }

    async function clearPackets() {
      await fetch('/api/sim?clear_packets=1', { method: 'POST' });
      refreshPackets();
      refreshStatus();
    }

    async function togglePauseCapture() {
      await fetch('/api/sim?pause_capture=' + (g_capturePaused ? '0' : '1'), { method: 'POST' });
      refreshStatus();
    }

    async function refreshPackets() {
      try {
        const res = await fetch('/api/packets?limit=60');
        const data = await res.json();
        const filter = document.getElementById('pktFilter').value;
        const tbody = document.getElementById('pktTableBody');

        document.getElementById('snifferStats').textContent =
          'Stored in PSRAM: ' + data.stored + '/250 frames • CRC OK: ' + data.crc_ok + ' • CRC Errors: ' + data.crc_err;

        const filtered = data.packets.filter(p => {
          if (filter === 'write') return p.func === 'WRITE';
          if (filter === 'tx') return p.is_tx;
          if (filter === 'tstat') return p.src.includes('2001') || p.dest.includes('2001');
          if (filter === 'furnace') return p.src.includes('4001') || p.dest.includes('4001');
          return true;
        });

        if (filtered.length === 0) {
          tbody.innerHTML = '<tr><td colspan="9" style="color:var(--muted);">No matching frames in buffer.</td></tr>';
          return;
        }

        tbody.innerHTML = filtered.map(p => {
          let tagClass = 'tag-read';
          if (p.is_tx) tagClass = 'tag-tx';
          else if (p.func === 'WRITE') tagClass = 'tag-write';
          else if (p.func === 'ACK/RESP') tagClass = 'tag-ack';

          const origin = p.is_tx ? (p.sim ? 'SIM-TX' : 'RS485-TX') : (p.sim ? 'SIM-RX' : 'RS485-RX');
          const crcHtml = p.crc_ok
            ? '<span class="crc-ok">OK (' + p.crc + ')</span>'
            : '<span class="crc-err">ERR (' + p.crc + ')</span>';

          return '<tr>' +
            '<td>' + p.seq + '</td>' +
            '<td>' + (p.t_ms / 1000).toFixed(2) + '</td>' +
            '<td><span class="tag ' + tagClass + '">' + origin + '</span></td>' +
            '<td>' + p.src + ' &rarr; ' + p.dest + '</td>' +
            '<td><span class="tag ' + tagClass + '">' + p.func + '</span></td>' +
            '<td>' + p.table + '</td>' +
            '<td>' + p.summary + '</td>' +
            '<td>' + crcHtml + '</td>' +
            '<td style="color:#94a3b8;">' + p.hex + '</td>' +
            '</tr>';
        }).join('');
      } catch (e) {}
    }

    refreshStatus();
    setInterval(() => {
      refreshStatus();
      if (g_activeTab === 'sniffer' && !g_capturePaused) refreshPackets();
    }, 2000);
  </script>
</body>
</html>)rawliteral";

static void handleRoot() {
    s_server.send_P(200, "text/html", INDEX_HTML);
}

static void handleApiStatus() {
    const WaterSensorStatus& ws = g_waterSensor.getStatus();
    const AbcdTelemetry&     bt = g_abcdBus.getTelemetry();
    const AbcdBusStats&      bs = g_abcdBus.getStats();
    const ControllerConfig&  cc = g_controller.getConfig();
    const RgbVisual          lv = g_controller.getStatusLedVisual();

    char json[1280];
    snprintf(json, sizeof(json),
             "{"
             "\"uptime_s\":%lu,"
             "\"ssid\":\"%s\","
             "\"ip\":\"%s\","
             "\"rssi_dbm\":%d,"
             "\"free_heap\":%lu,"
             "\"free_psram\":%lu,"
             "\"core\":%d,"
             "\"ctrl_state\":\"%s\","
             "\"op_mode_name\":\"%s\","
             "\"ion_sp_status\":\"%s\","
             "\"led_color\":\"%s\","
             "\"led_hex\":\"%s\","
             "\"probes\":%u,"
             "\"water_sim\":%s,"
             "\"supply_f\":%.1f,"
             "\"return_f\":%.1f,"
             "\"delta_t_f\":%.1f,"
             "\"room_f\":%.1f,"
             "\"ion_heat_sp_f\":%.1f,"
             "\"fan_mode\":\"%s\","
             "\"furnace_stage\":\"%s\","
             "\"airflow_cfm\":%u,"
             "\"blower_rpm\":%u,"
             "\"supply_air_f\":%.1f,"
             "\"cfg\":{"
               "\"op_mode\":%u,"
               "\"boiler_on\":%.1f,"
               "\"boiler_off\":%.1f,"
               "\"comfort_sp\":%.1f,"
               "\"fallback_sp\":%.1f,"
               "\"fan_speed\":%u"
             "},"
             "\"bus\":{"
               "\"rx\":%lu,"
               "\"tx\":%lu,"
               "\"crc_ok\":%lu,"
               "\"crc_err\":%lu,"
               "\"stored\":%u,"
               "\"idle_ms\":%lu,"
               "\"tx_enabled\":%s,"
               "\"sim_bus\":%s,"
               "\"paused\":%s,"
               "\"psram\":%s"
             "}"
             "}",
             millis() / 1000UL,
             WiFi.isConnected() ? WiFi.SSID().c_str() : "DISCONNECTED",
             WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
             WiFi.isConnected() ? (int)WiFi.RSSI() : 0,
             (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getFreePsram(),
             xPortGetCoreID(),
             g_controller.getStateName(),
             g_controller.getOpModeName(),
             g_controller.getIonSetpointStatusText(),
             lv.label,
             lv.hex,
             (unsigned)ws.detected_probes,
             ws.using_simulation ? "true" : "false",
             ws.supply_temp_f,
             ws.return_temp_f,
             ws.delta_t_f,
             bt.room_temp_f,
             bt.ion_heat_setpoint_f,
             AbcdBusDriver::fanModeToName(bt.ion_fan_mode),
             AbcdBusDriver::stageToName(bt.furnace_stage),
             (unsigned)bt.furnace_airflow_cfm,
             (unsigned)bt.blower_rpm,
             bt.supply_air_temp_f,
             (unsigned)cc.op_mode,
             cc.boiler_on_f,
             cc.boiler_off_f,
             cc.comfort_setpoint_f,
             cc.gas_fallback_setpoint_f,
             (unsigned)cc.target_fan_speed,
             (unsigned long)bs.total_rx_frames,
             (unsigned long)bs.total_tx_frames,
             (unsigned long)bs.crc_ok_count,
             (unsigned long)bs.crc_err_count,
             (unsigned)g_abcdBus.getStoredFrameCount(),
             (unsigned long)g_abcdBus.getBusIdleMs(),
             bs.tx_enabled ? "true" : "false",
             bs.sim_bus_enabled ? "true" : "false",
             bs.capture_paused ? "true" : "false",
             bs.psram_buffer_active ? "true" : "false");

    s_server.send(200, "application/json", json);
}

static void handleApiConfig() {
    ControllerConfig cfg = g_controller.getConfig();

    if (s_server.hasArg("op_mode")) {
        const int m = s_server.arg("op_mode").toInt();
        if (m >= 0 && m <= 2) cfg.op_mode = static_cast<OperatingMode>(m);
    }
    if (s_server.hasArg("boiler_on")) {
        cfg.boiler_on_f = s_server.arg("boiler_on").toFloat();
    }
    if (s_server.hasArg("boiler_off")) {
        cfg.boiler_off_f = s_server.arg("boiler_off").toFloat();
    }
    if (s_server.hasArg("comfort_sp")) {
        cfg.comfort_setpoint_f = s_server.arg("comfort_sp").toFloat();
    }
    if (s_server.hasArg("fallback_sp")) {
        cfg.gas_fallback_setpoint_f = s_server.arg("fallback_sp").toFloat();
    }
    if (s_server.hasArg("fan_speed")) {
        const int f = s_server.arg("fan_speed").toInt();
        if (f >= 1 && f <= 3) cfg.target_fan_speed = static_cast<AbcdFanMode>(f);
    }

    g_controller.updateConfig(cfg, true);
    handleApiStatus();
}

static void handleApiSim() {
    if (s_server.hasArg("supply_f") || s_server.hasArg("return_f")) {
        const float sup = s_server.hasArg("supply_f") ? s_server.arg("supply_f").toFloat()
                                                      : g_waterSensor.getStatus().supply_temp_f;
        const float ret = s_server.hasArg("return_f") ? s_server.arg("return_f").toFloat()
                                                      : g_waterSensor.getStatus().return_temp_f;
        g_waterSensor.setSimulatedTempsF(sup, ret);
    }
    if (s_server.hasArg("room_f")) {
        g_abcdBus.setSimulatedRoomTempF(s_server.arg("room_f").toFloat());
    }
    if (s_server.hasArg("ion_sp_f")) {
        g_abcdBus.setSimulatedIonSetpointF(s_server.arg("ion_sp_f").toFloat());
    }
    if (s_server.hasArg("sim_bus")) {
        g_abcdBus.setSimBusEnabled(s_server.arg("sim_bus").toInt() != 0);
    }
    if (s_server.hasArg("inject_cycle")) {
        g_abcdBus.injectSimulatedCycle();
    }
    if (s_server.hasArg("clear_packets")) {
        g_abcdBus.clearCaptureBuffer();
    }
    if (s_server.hasArg("pause_capture")) {
        g_abcdBus.setCapturePaused(s_server.arg("pause_capture").toInt() != 0);
    }
    if (s_server.hasArg("rescan_onewire")) {
        g_waterSensor.rescanBus();
    }

    g_controller.forceEvaluate();
    if (s_server.hasArg("room_f") || s_server.hasArg("ion_sp_f")) {
        g_abcdBus.injectSimulatedCycle();
    }
    handleApiStatus();
}

static void handleApiPackets() {
    size_t limit = 50;
    if (s_server.hasArg("limit")) {
        const int req = s_server.arg("limit").toInt();
        if (req > 0 && req <= 100) limit = static_cast<size_t>(req);
    }

    const size_t totalStored = g_abcdBus.getStoredFrameCount();
    const size_t countToSend = (totalStored < limit) ? totalStored : limit;
    const AbcdBusStats& bs   = g_abcdBus.getStats();

    String out;
    out.reserve(4096);
    out += "{\"stored\":";
    out += String((unsigned)totalStored);
    out += ",\"crc_ok\":";
    out += String((unsigned long)bs.crc_ok_count);
    out += ",\"crc_err\":";
    out += String((unsigned long)bs.crc_err_count);
    out += ",\"packets\":[";

    AbcdFrameRecord rec{};
    char hexBuf[3 * ABCD_MAX_FRAME_BYTES + 4];
    char rowBuf[512];

    for (size_t i = 0; i < countToSend; ++i) {
        if (!g_abcdBus.getFrameByOffsetFromNewest(i, rec)) {
            break;
        }
        formatHexBytes(rec.raw, rec.raw_len, hexBuf, sizeof(hexBuf));
        snprintf(rowBuf, sizeof(rowBuf),
                 "%s{\"seq\":%lu,\"t_ms\":%lu,\"is_tx\":%s,\"sim\":%s,\"crc_ok\":%s,"
                 "\"src\":\"0x%04X (%s)\",\"dest\":\"0x%04X (%s)\","
                 "\"func\":\"%s\",\"table\":\"%02X:%02X:%02X\","
                 "\"crc\":\"%04X\",\"summary\":\"%s\",\"hex\":\"%s\"}",
                 (i > 0 ? "," : ""),
                 (unsigned long)rec.seq,
                 (unsigned long)rec.timestamp_ms,
                 rec.is_tx ? "true" : "false",
                 rec.is_simulated ? "true" : "false",
                 rec.crc_ok ? "true" : "false",
                 (unsigned)rec.src_addr, AbcdBusDriver::addrToName(rec.src_addr),
                 (unsigned)rec.dest_addr, AbcdBusDriver::addrToName(rec.dest_addr),
                 AbcdBusDriver::funcToName(rec.func),
                 rec.table_id[0], rec.table_id[1], rec.table_id[2],
                 (unsigned)rec.crc_rx,
                 rec.summary,
                 hexBuf);
        out += rowBuf;
    }

    out += "]}";
    s_server.send(200, "application/json", out);
}

static void handleApiPacketsCsv() {
    const size_t totalStored = g_abcdBus.getStoredFrameCount();

    String csv;
    csv.reserve(4096);
    csv += "seq,timestamp_ms,direction,simulated,src_addr,dest_addr,func,table_reg,crc_ok,crc_hex,summary,raw_hex\n";

    AbcdFrameRecord rec{};
    char hexBuf[3 * ABCD_MAX_FRAME_BYTES + 4];
    char lineBuf[512];

    // Export oldest to newest
    for (size_t i = 0; i < totalStored; ++i) {
        const size_t offsetFromNewest = totalStored - 1 - i;
        if (!g_abcdBus.getFrameByOffsetFromNewest(offsetFromNewest, rec)) {
            continue;
        }
        formatHexBytes(rec.raw, rec.raw_len, hexBuf, sizeof(hexBuf));
        snprintf(lineBuf, sizeof(lineBuf),
                 "%lu,%lu,%s,%s,0x%04X,0x%04X,%s,%02X:%02X:%02X,%s,0x%04X,\"%s\",\"%s\"\n",
                 (unsigned long)rec.seq,
                 (unsigned long)rec.timestamp_ms,
                 rec.is_tx ? "TX" : "RX",
                 rec.is_simulated ? "SIM" : "BUS",
                 (unsigned)rec.src_addr,
                 (unsigned)rec.dest_addr,
                 AbcdBusDriver::funcToName(rec.func),
                 rec.table_id[0], rec.table_id[1], rec.table_id[2],
                 rec.crc_ok ? "OK" : "ERR",
                 (unsigned)rec.crc_rx,
                 rec.summary,
                 hexBuf);
        csv += lineBuf;
    }

    s_server.sendHeader("Content-Disposition", "attachment; filename=\"abcd_bus_capture.csv\"");
    s_server.send(200, "text/csv", csv);
}

void WebPortal::begin() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status", HTTP_GET, handleApiStatus);
    s_server.on("/api/config", HTTP_ANY, handleApiConfig);
    s_server.on("/api/sim", HTTP_ANY, handleApiSim);
    s_server.on("/api/packets", HTTP_GET, handleApiPackets);
    s_server.on("/api/packets.csv", HTTP_GET, handleApiPacketsCsv);
    s_server.begin();
}

void WebPortal::loop() {
    s_server.handleClient();
}
