#include "web_portal.h"
#include "abcd_bus.h"
#include "water_sensor.h"
#include "controller.h"
#include "network_mgr.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <driver/gpio.h>

static WebServer s_server(80);
static bool      s_otaSuccess = false;
static String    s_otaError;

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
      max-width: 1220px;
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
      flex-wrap: wrap;
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
      gap: 8px;
    }
    .form-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 11px;
      font-size: 0.86rem;
    }
    .form-row label { color: var(--text); flex: 1; }
    input[type="number"], input[type="text"], input[type="password"], select {
      background: #0b1324;
      color: var(--text);
      border: 1px solid #334155;
      border-radius: 6px;
      padding: 7px 10px;
      width: 195px;
      font-size: 0.85rem;
    }
    input[type="file"] {
      background: #0b1324;
      color: var(--text);
      border: 1px solid #334155;
      border-radius: 6px;
      padding: 7px;
      width: 100%;
      font-size: 0.83rem;
      margin-bottom: 10px;
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
    button.danger { background: #7f1d1d; border-color: #ef4444; color: #fecaca; }
    button.danger:hover { background: #991b1b; }
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
    tr.clickable-row:hover { background: #172542; cursor: pointer; }
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
    .progress-bar-bg {
      width: 100%;
      height: 16px;
      background: #090d16;
      border: 1px solid var(--border);
      border-radius: 999px;
      overflow: hidden;
      margin-top: 10px;
      display: none;
    }
    .progress-bar-fill {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #0284c7, #10b981);
      transition: width 0.15s ease;
    }
    code.cmd-box {
      display: block;
      background: #090d16;
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 8px 10px;
      font-size: 0.76rem;
      color: #93c5fd;
      margin-top: 8px;
      user-select: all;
    }
  </style>
</head>
<body>
  <header>
    <div>
      <h1>Heil G96CTN0601714A1 &amp; Wood Boiler Controller</h1>
      <div class="subtitle">Parallel ABCD Bus Co-Controller, Protocol Analyzer &amp; Wireless Provisioning &bull; ESP32-S3-N16R8</div>
    </div>
    <div class="badge"><span class="dot" id="stateDot"></span><span id="stateBadge">LOADING...</span></div>
  </header>

  <nav class="tabs">
    <button class="tab-btn active" onclick="switchTab('control')" id="tabbtn-control">1. Furnace &amp; Wood Boiler Control</button>
    <button class="tab-btn" onclick="switchTab('sniffer')" id="tabbtn-sniffer">2. RS-485 ABCD Bus Analyzer (<span id="pktCountBadge">0</span>)</button>
    <button class="tab-btn" onclick="switchTab('network')" id="tabbtn-network">3. Wi-Fi, Network &amp; OTA</button>
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
        <div class="subvalue" id="supplyAirInfo">Relay: IDLE (0x06) &bull; Static: 0.00 in.wc</div>
      </div>
      <div class="card">
        <div class="label">Network &amp; AP Status</div>
        <div class="value" id="wifiInfo" style="font-size:1.02rem;">--</div>
        <div class="subvalue" id="apSummary">--</div>
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
          <select id="cfgOpMode" onchange="updateSimPanelVisibility()">
            <option value="0">0 - Passive Bus Sniffer Only (DE/RE LOW)</option>
            <option value="1">1 - Auto Wood Boiler Co-Controller</option>
            <option value="2">2 - Manual ECM Blower Override</option>
            <option value="3">3 - Prototyping Simulation</option>
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
          <label for="cfgFanSpeed">Hydronic Heating ECM Blower Speed</label>
          <select id="cfgFanSpeed">
            <option value="1">LOW (413 CFM / 563 RPM / ~54W)</option>
            <option value="2">MED (667 CFM / 635 RPM / ~197W)</option>
            <option value="3">HIGH (1,050 CFM / 767 RPM / ~565W)</option>
          </select>
        </div>
        <div class="btn-row">
          <button class="primary" onclick="saveConfig()">Save &amp; Apply Settings</button>
        </div>
      </div>

      <!-- Interactive Bench Simulation Panel (visible only in Prototyping Simulation mode) -->
      <div class="panel" id="simPanel" style="display:none;">
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
          <option value="furnace">Filter: Heil G96CTN (0x4401)</option>
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

    <div class="panel">
      <h2>
        <span>Thermostat Probe (0x2001)</span>
        <span id="probeResult" style="font-size:0.78rem;color:var(--muted);">Ready (Src: 0x9201)</span>
      </h2>
      <div class="btn-row" style="margin-top:0;margin-bottom:8px;">
        <button onclick="probeBus('read', '00:3B:01', '')" class="primary">Read 00:3B:01</button>
        <button onclick="probeBus('read', '00:04:09', '')" class="primary">Read 00:04:09</button>
      </div>
      <div style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;">
        <label style="font-size:0.76rem;color:var(--muted);">Dest:
          <input type="text" id="probeDest" value="2001" style="width:68px;margin-left:4px;" placeholder="2001">
        </label>
        <label style="font-size:0.76rem;color:var(--muted);">Table:
          <input type="text" id="probeTable" value="00:3B:01" style="width:92px;margin-left:4px;" placeholder="00:3B:01">
        </label>
        <label style="font-size:0.76rem;color:var(--muted);flex:1;min-width:180px;">Payload (hex):
          <input type="text" id="probePayload" value="" style="width:180px;margin-left:4px;" placeholder="e.g. 00 00 02 9B">
        </label>
        <button onclick="probeBus('read')">Send Read</button>
        <button onclick="probeBus('write')">Send Write</button>
      </div>
    </div>
  </div>

  <!-- TAB 3: WI-FI, NETWORK & OTA -->
  <div class="tab-pane" id="tab-network">
    <div class="two-col">
      <!-- Card 1: Nearby Wi-Fi Network Scanner -->
      <div class="panel">
        <h2>
          <span>1. Live 2.4 GHz Wi-Fi Network Scanner</span>
          <button class="primary" onclick="scanWifi()" id="btnScanWifi">Scan Nearby Networks</button>
        </h2>
        <p style="font-size:0.8rem;color:var(--muted);margin-bottom:8px;">
          Click any discovered network row below to auto-fill the Station SSID field:
        </p>
        <div class="table-wrap" style="max-height:240px;">
          <table>
            <thead>
              <tr>
                <th>SSID</th>
                <th>Signal</th>
                <th>Ch</th>
                <th>Security</th>
              </tr>
            </thead>
            <tbody id="wifiScanBody">
              <tr><td colspan="4" style="color:var(--muted);">Click "Scan Nearby Networks" to discover 2.4 GHz APs.</td></tr>
            </tbody>
          </table>
        </div>
      </div>

      <!-- Card 2: Home Wi-Fi (Station) & IP Config -->
      <div class="panel">
        <h2>
          <span>2. Home Wi-Fi (Station) &amp; IP Config (NVS)</span>
          <span class="toast" id="netSaveToast">Saved!</span>
        </h2>
        <div class="form-row">
          <label for="netStaSsid">Home Wi-Fi SSID (2.4 GHz)</label>
          <input type="text" id="netStaSsid" placeholder="e.g. HomeNetwork">
        </div>
        <div class="form-row">
          <label for="netStaPass">Wi-Fi Password</label>
          <input type="password" id="netStaPass" placeholder="(Blank = keep current)">
        </div>
        <div class="form-row">
          <label for="netMdns">mDNS Hostname (.local)</label>
          <input type="text" id="netMdns" value="furnace-controller">
        </div>
        <div class="form-row">
          <label for="netStaticMode">IPv4 Assignment Mode</label>
          <select id="netStaticMode" onchange="toggleStaticFields()">
            <option value="0">DHCP (Automatic IP from Router)</option>
            <option value="1">Static IPv4 Configuration</option>
          </select>
        </div>
        <div id="staticIpFields" style="display:none;">
          <div class="form-row">
            <label for="netStaticIp">Static IPv4 Address</label>
            <input type="text" id="netStaticIp" value="192.168.111.240">
          </div>
          <div class="form-row">
            <label for="netStaticGw">Gateway IPv4</label>
            <input type="text" id="netStaticGw" value="192.168.111.1">
          </div>
          <div class="form-row">
            <label for="netStaticMask">Subnet Mask</label>
            <input type="text" id="netStaticMask" value="255.255.255.0">
          </div>
          <div class="form-row">
            <label for="netStaticDns">DNS Server</label>
            <input type="text" id="netStaticDns" value="1.1.1.1">
          </div>
        </div>
        <div class="btn-row">
          <button class="primary" onclick="saveNetConfig(true)">Save &amp; Reconnect Wi-Fi</button>
          <button onclick="togglePassVisibility('netStaPass')">Show/Hide Password</button>
        </div>
      </div>

      <!-- Card 3: Fallback Access Point (AP) & Captive Portal -->
      <div class="panel">
        <h2>
          <span>3. Fallback Access Point (AP) &amp; Captive Portal</span>
          <span class="pill" id="apStateBadge">STANDBY</span>
        </h2>
        <p style="font-size:0.8rem;color:var(--muted);margin-bottom:12px;">
          If your home router is offline or rebooting after a power outage (&gt;15s), the ESP32-S3 automatically launches this Setup AP at <strong>http://192.168.4.1/</strong> and retries your home Wi-Fi every 60 seconds.
        </p>
        <div class="form-row">
          <label for="netApSsid">Fallback AP SSID</label>
          <input type="text" id="netApSsid" value="Furnace-Controller-Setup">
        </div>
        <div class="form-row">
          <label for="netApPass">Fallback AP WPA2 Password</label>
          <input type="password" id="netApPass" placeholder="(Blank = keep current)">
        </div>
        <div class="form-row">
          <label for="netApAlways">AP Operating Policy</label>
          <select id="netApAlways">
            <option value="0">Auto-Fallback Only (When Home Wi-Fi Drops)</option>
            <option value="1">Always-On (Broadcast Setup AP Continuously)</option>
          </select>
        </div>
        <div class="btn-row">
          <button class="primary" onclick="saveNetConfig(false)">Save AP Settings</button>
          <button onclick="toggleApNow()" id="btnApToggle">Turn Setup AP On Now (10m)</button>
        </div>
      </div>

      <!-- Card 4: Over-The-Air (OTA) Firmware Update & System Maintenance -->
      <div class="panel">
        <h2>
          <span>4. Over-The-Air (OTA) Firmware Update &amp; System</span>
          <span class="pill" id="sysPartBadge">app0</span>
        </h2>
        <p style="font-size:0.8rem;color:var(--muted);margin-bottom:8px;">
          Select a compiled <code>firmware.bin</code> image (<code>.pio/build/esp32-s3-devkitc-1-n16r8/firmware.bin</code>) to flash wirelessly with dual-partition rollback protection:
        </p>
        <input type="file" id="otaFile" accept=".bin">
        <div class="btn-row" style="margin-top:4px;">
          <button class="primary" onclick="uploadOtaFirmware()" id="btnOtaUpload">Upload &amp; Flash Firmware (.bin)</button>
        </div>
        <div class="progress-bar-bg" id="otaProgressWrap">
          <div class="progress-bar-fill" id="otaProgressFill"></div>
        </div>
        <div id="otaStatusMsg" style="font-size:0.8rem;color:var(--accent);margin-top:8px;"></div>

        <div style="margin-top:14px;padding-top:12px;border-top:1px solid var(--border);font-size:0.8rem;color:var(--muted);">
          <div><strong>Direct PlatformIO CLI Wireless Upload Command:</strong></div>
          <code class="cmd-box" id="cliOtaCmd">pio run -t upload --upload-port 192.168.111.240</code>
          <div style="margin-top:10px;" id="sysDiagLine">Reset Reason: -- &bull; MAC: --</div>
        </div>

        <div class="btn-row" style="margin-top:12px;">
          <button onclick="rebootDevice()">Reboot Controller</button>
          <button class="danger" onclick="resetDefaults()">Reset NVS Defaults</button>
        </div>
      </div>
    </div>
  </div>

  <script>
    let g_activeTab = 'control';
    let g_cfgInitialized = false;
    let g_simInitialized = false;
    let g_netInitialized = false;
    let g_capturePaused = false;
    let g_simVisible = true;
    let g_apActive = false;

    function switchTab(tab) {
      g_activeTab = tab;
      document.getElementById('tab-control').classList.toggle('active', tab === 'control');
      document.getElementById('tab-sniffer').classList.toggle('active', tab === 'sniffer');
      document.getElementById('tab-network').classList.toggle('active', tab === 'network');
      document.getElementById('tabbtn-control').classList.toggle('active', tab === 'control');
      document.getElementById('tabbtn-sniffer').classList.toggle('active', tab === 'sniffer');
      document.getElementById('tabbtn-network').classList.toggle('active', tab === 'network');
      if (tab === 'sniffer') refreshPackets();
      if (tab === 'network' && !g_netInitialized) loadNetConfig();
    }

    function updateSimPanelVisibility() {
      const isSim = document.getElementById('cfgOpMode').value === '3';
      document.getElementById('simPanel').style.display = isSim ? 'block' : 'none';
    }

    function toggleStaticFields() {
      const en = document.getElementById('netStaticMode').value === '1';
      document.getElementById('staticIpFields').style.display = en ? 'block' : 'none';
    }

    function togglePassVisibility(id) {
      const el = document.getElementById(id);
      el.type = (el.type === 'password') ? 'text' : 'password';
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
        document.getElementById('cfmRpm').textContent =
          d.airflow_cfm + ' CFM • ' + d.blower_rpm + ' RPM' +
          (d.blower_watts !== undefined ? (' • ' + d.blower_watts.toFixed(0) + ' W') : '');

        document.getElementById('furnaceStage').textContent = d.furnace_stage;
        document.getElementById('supplyAirInfo').textContent =
          'Relay: ' + (d.relay_status || 'IDLE') + ' • Static: ' +
          (d.static_press_inwc !== undefined ? d.static_press_inwc.toFixed(2) : '0.00') + ' in.wc';

        document.getElementById('wifiInfo').textContent = d.ip + ' (' + d.rssi_dbm + ' dBm)';
        document.getElementById('apSummary').textContent =
          (d.net.ap_active ? ('Setup AP: ON (' + d.net.ap_ip + ')') : 'Setup AP: Auto-Standby') +
          ' • Up: ' + d.uptime_s + 's';

        document.getElementById('pktCountBadge').textContent = d.bus.stored;
        document.getElementById('probeBadge').textContent =
          d.water_sim ? 'Mode: Web Simulator (0 physical probes on GPIO 4)' : ('Physical DS18B20 Probes: ' + d.probes);

        g_capturePaused = d.bus.paused;
        document.getElementById('btnPause').textContent = g_capturePaused ? 'Resume Capture' : 'Pause Capture';

        g_apActive = d.net.ap_active;
        const apBadge = document.getElementById('apStateBadge');
        if (apBadge) {
          apBadge.textContent = d.net.ap_active ? ('ACTIVE (' + d.net.ap_ip + ')') : 'STANDBY (Auto-Fallback)';
          apBadge.style.background = d.net.ap_active ? '#064e3b' : '#1e293b';
          apBadge.style.color = d.net.ap_active ? '#34d399' : '#94a3b8';
        }
        const btnAp = document.getElementById('btnApToggle');
        if (btnAp) {
          btnAp.textContent = d.net.ap_active ? 'Turn Setup AP Off' : 'Turn Setup AP On Now (10m)';
        }
        document.getElementById('sysPartBadge').textContent = 'Partition: ' + d.net.part;
        document.getElementById('cliOtaCmd').textContent = 'pio run -t upload --upload-port ' + d.ip;
        document.getElementById('sysDiagLine').textContent =
          'Reset: ' + d.net.reset_reason + ' • MAC: ' + d.net.mac + ' • Free PSRAM: ' + (d.free_psram / (1024*1024)).toFixed(2) + ' MB';

        // Keep the Ion Thermostat Setpoint slider synced with the active thermostat setpoint
        document.getElementById('simIonSp').value = Math.round(d.ion_heat_sp_f);
        document.getElementById('lblSimIonSp').textContent = Math.round(d.ion_heat_sp_f);

        if (!g_cfgInitialized) {
          document.getElementById('cfgOpMode').value = d.cfg.op_mode;
          document.getElementById('cfgBoilerOn').value = d.cfg.boiler_on;
          document.getElementById('cfgBoilerOff').value = d.cfg.boiler_off;
          document.getElementById('cfgComfortSp').value = d.cfg.comfort_sp;
          document.getElementById('cfgFanSpeed').value = d.cfg.fan_speed;
          updateSimPanelVisibility();
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

    async function loadNetConfig() {
      try {
        const res = await fetch('/api/net/config');
        const c = await res.json();
        document.getElementById('netStaSsid').value = c.sta_ssid || '';
        document.getElementById('netMdns').value = c.mdns_host || 'furnace-controller';
        document.getElementById('netStaticMode').value = c.use_static_ip ? '1' : '0';
        document.getElementById('netStaticIp').value = c.static_ip || '192.168.111.240';
        document.getElementById('netStaticGw').value = c.static_gw || '192.168.111.1';
        document.getElementById('netStaticMask').value = c.static_mask || '255.255.255.0';
        document.getElementById('netStaticDns').value = c.static_dns || '1.1.1.1';
        document.getElementById('netApSsid').value = c.ap_ssid || 'Furnace-Controller-Setup';
        document.getElementById('netApAlways').value = c.ap_always_on ? '1' : '0';
        toggleStaticFields();
        g_netInitialized = true;
      } catch (e) {}
    }

    async function scanWifi() {
      const btn = document.getElementById('btnScanWifi');
      const tbody = document.getElementById('wifiScanBody');
      btn.textContent = 'Scanning (2s)...';
      btn.disabled = true;
      tbody.innerHTML = '<tr><td colspan="4" style="color:var(--accent);">Scanning 2.4 GHz channels...</td></tr>';
      try {
        const res = await fetch('/api/wifi/scan');
        const data = await res.json();
        if (!data.networks || data.networks.length === 0) {
          tbody.innerHTML = '<tr><td colspan="4" style="color:var(--muted);">No 2.4 GHz networks found.</td></tr>';
        } else {
          tbody.innerHTML = data.networks.map(n => {
            const safeSsid = n.ssid.replace(/'/g, "\\'");
            return '<tr class="clickable-row" onclick="selectScannedSsid(\'' + safeSsid + '\')">' +
              '<td><strong>' + n.ssid + '</strong></td>' +
              '<td>' + n.rssi + ' dBm</td>' +
              '<td>' + n.channel + '</td>' +
              '<td>' + (n.secure ? 'WPA2/WPA3' : 'Open') + '</td>' +
              '</tr>';
          }).join('');
        }
      } catch (e) {
        tbody.innerHTML = '<tr><td colspan="4" style="color:var(--red);">Scan failed.</td></tr>';
      }
      btn.textContent = 'Scan Nearby Networks';
      btn.disabled = false;
    }

    function selectScannedSsid(ssid) {
      document.getElementById('netStaSsid').value = ssid;
      document.getElementById('netStaPass').focus();
    }

    async function saveNetConfig(reconnectNow) {
      const params = new URLSearchParams({
        sta_ssid: document.getElementById('netStaSsid').value,
        sta_pass: document.getElementById('netStaPass').value,
        mdns_host: document.getElementById('netMdns').value,
        use_static_ip: document.getElementById('netStaticMode').value,
        static_ip: document.getElementById('netStaticIp').value,
        static_gw: document.getElementById('netStaticGw').value,
        static_mask: document.getElementById('netStaticMask').value,
        static_dns: document.getElementById('netStaticDns').value,
        ap_ssid: document.getElementById('netApSsid').value,
        ap_pass: document.getElementById('netApPass').value,
        ap_always_on: document.getElementById('netApAlways').value,
        reconnect: reconnectNow ? '1' : '0'
      });
      await fetch('/api/net/config?' + params.toString(), { method: 'POST' });
      document.getElementById('netStaPass').value = '';
      document.getElementById('netApPass').value = '';
      const t = document.getElementById('netSaveToast');
      t.style.opacity = 1;
      setTimeout(() => { t.style.opacity = 0; }, 2500);
      refreshStatus();
    }

    async function toggleApNow() {
      await fetch('/api/net/ap_toggle?enable=' + (g_apActive ? '0' : '1'), { method: 'POST' });
      setTimeout(refreshStatus, 400);
    }

    function uploadOtaFirmware() {
      const fileInput = document.getElementById('otaFile');
      const statusMsg = document.getElementById('otaStatusMsg');
      const wrap = document.getElementById('otaProgressWrap');
      const fill = document.getElementById('otaProgressFill');
      const btn = document.getElementById('btnOtaUpload');

      if (!fileInput.files || fileInput.files.length === 0) {
        statusMsg.style.color = '#f87171';
        statusMsg.textContent = 'Please select a compiled firmware.bin file first.';
        return;
      }

      const file = fileInput.files[0];
      const formData = new FormData();
      formData.append('firmware', file, file.name);

      wrap.style.display = 'block';
      fill.style.width = '0%';
      btn.disabled = true;
      statusMsg.style.color = '#38bdf8';
      statusMsg.textContent = 'Uploading ' + file.name + ' (' + (file.size / 1024).toFixed(0) + ' KB)...';

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota/upload', true);
      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          fill.style.width = pct + '%';
          statusMsg.textContent = 'Uploading & writing to OTA partition: ' + pct + '%';
        }
      };
      xhr.onload = function() {
        btn.disabled = false;
        if (xhr.status === 200) {
          fill.style.width = '100%';
          statusMsg.style.color = '#34d399';
          statusMsg.textContent = 'OTA Flash Verified! Rebooting ESP32-S3 — reloading in 6 seconds...';
          setTimeout(() => { window.location.reload(); }, 6000);
        } else {
          statusMsg.style.color = '#f87171';
          statusMsg.textContent = 'OTA Error: ' + xhr.responseText;
        }
      };
      xhr.onerror = function() {
        btn.disabled = false;
        statusMsg.style.color = '#f87171';
        statusMsg.textContent = 'Network connection lost during OTA upload.';
      };
      xhr.send(formData);
    }

    async function rebootDevice() {
      if (!confirm('Reboot the ESP32-S3 Furnace Controller now?')) return;
      await fetch('/api/sys/reboot', { method: 'POST' });
      setTimeout(() => { window.location.reload(); }, 5000);
    }

    async function resetDefaults() {
      if (!confirm('Reset controller & network NVS settings back to defaults and reboot?')) return;
      await fetch('/api/sys/reset_defaults', { method: 'POST' });
      setTimeout(() => { window.location.reload(); }, 5000);
    }

    async function saveConfig() {
      const params = new URLSearchParams({
        op_mode: document.getElementById('cfgOpMode').value,
        boiler_on: document.getElementById('cfgBoilerOn').value,
        boiler_off: document.getElementById('cfgBoilerOff').value,
        comfort_sp: document.getElementById('cfgComfortSp').value,
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

    async function probeBus(action, tableOverride, payloadOverride) {
      const dest = (document.getElementById('probeDest').value || '2001').trim();
      const table = (tableOverride !== undefined ? tableOverride : document.getElementById('probeTable').value).trim();
      const payload = (payloadOverride !== undefined ? payloadOverride : document.getElementById('probePayload').value).trim();
      const badge = document.getElementById('probeResult');
      badge.textContent = 'Sending ' + action.toUpperCase() + ' ' + table + '...';
      try {
        const params = new URLSearchParams({ action, table, dest });
        if (payload.length > 0) params.set('payload', payload);
        const res = await fetch('/api/bus/probe?' + params.toString(), { method: 'POST' });
        const data = await res.json();
        if (!res.ok || !data.ok) {
          badge.textContent = 'Error: ' + (data.error || ('HTTP ' + res.status));
        } else {
          const txNote = data.sent ? 'RS485-TX' : (data.simulated ? 'SIM-ONLY (TX disabled)' : 'SIM-TX');
          badge.textContent = action.toUpperCase() + ' ' + data.table + ' -> ' + data.dest + ' [' + txNote + ']';
        }
      } catch (e) {
        badge.textContent = 'Probe request failed';
      }
      refreshPackets();
      refreshStatus();
    }

    async function refreshPackets() {
      try {
        const res = await fetch('/api/packets?limit=60');
        const data = await res.json();
        const filter = document.getElementById('pktFilter').value;
        const tbody = document.getElementById('pktTableBody');

        document.getElementById('snifferStats').textContent =
          'Stored in Buffer: ' + data.stored + ' frames • CRC OK: ' + data.crc_ok + ' • CRC Errors: ' + data.crc_err;

        const filtered = data.packets.filter(p => {
          if (filter === 'write') return p.func === 'WRITE';
          if (filter === 'tx') return p.is_tx;
          if (filter === 'tstat') return p.src.includes('2001') || p.dest.includes('2001');
          if (filter === 'furnace') return p.src.includes('4001') || p.dest.includes('4001') || p.src.includes('4401') || p.dest.includes('4401');
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

static void handleCaptivePortalRedirect() {
    s_server.sendHeader("Location", "http://192.168.4.1/", true);
    s_server.send(302, "text/plain", "Redirecting to Furnace Controller Setup Portal...");
}

static void handleApiStatus() {
    const WaterSensorStatus&   ws = g_waterSensor.getStatus();
    const AbcdTelemetry&       bt = g_abcdBus.getTelemetry();
    const AbcdBusStats&        bs = g_abcdBus.getStats();
    const ControllerConfig&    cc = g_controller.getConfig();
    const RgbVisual            lv = g_controller.getStatusLedVisual();
    const NetworkRuntimeStatus ns = g_netMgr.getStatus();

    char json[1536];
    snprintf(json, sizeof(json),
             "{"
             "\"uptime_s\":%lu,"
             "\"ssid\":\"%s\","
             "\"ip\":\"%s\","
             "\"rssi_dbm\":%ld,"
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
             "\"blower_watts\":%.1f,"
             "\"static_press_inwc\":%.2f,"
             "\"relay_status\":\"%s\","
             "\"supply_air_f\":%.1f,"
             "\"cfg\":{"
               "\"op_mode\":%u,"
               "\"boiler_on\":%.1f,"
               "\"boiler_off\":%.1f,"
               "\"comfort_sp\":%.1f,"
               "\"fan_speed\":%u"
             "},"
             "\"bus\":{"
               "\"rx\":%lu,"
               "\"tx\":%lu,"
               "\"crc_ok\":%lu,"
               "\"crc_err\":%lu,"
               "\"stored\":%u,"
               "\"idle_ms\":%lu,"
               "\"rx_pin\":%d,"
               "\"rx_bytes\":%lu,"
               "\"tx_enabled\":%s,"
               "\"sim_bus\":%s,"
               "\"paused\":%s,"
               "\"psram\":%s"
             "},"
             "\"net\":{"
               "\"sta_connected\":%s,"
               "\"ap_active\":%s,"
               "\"ap_ip\":\"%s\","
               "\"ap_clients\":%u,"
               "\"mac\":\"%s\","
               "\"part\":\"%s\","
               "\"reset_reason\":\"%s\""
             "}"
             "}",
             millis() / 1000UL,
             ns.sta_connected ? WiFi.SSID().c_str() : "DISCONNECTED",
             ns.sta_connected ? ns.sta_ip : ns.ap_ip,
             (long)ns.sta_rssi_dbm,
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
             bt.blower_watts,
             bt.static_press_hundredths / 100.0f,
             bt.relay_status[0] ? bt.relay_status : "IDLE (0x06)",
             bt.supply_air_temp_f,
             (unsigned)cc.op_mode,
             cc.boiler_on_f,
             cc.boiler_off_f,
             cc.comfort_setpoint_f,
             (unsigned)cc.target_fan_speed,
             (unsigned long)bs.total_rx_frames,
             (unsigned long)bs.total_tx_frames,
             (unsigned long)bs.crc_ok_count,
             (unsigned long)bs.crc_err_count,
             (unsigned)g_abcdBus.getStoredFrameCount(),
             (unsigned long)g_abcdBus.getBusIdleMs(),
             (int)bs.active_rx_pin,
             (unsigned long)bs.physical_rx_bytes,
             bs.tx_enabled ? "true" : "false",
             bs.sim_bus_enabled ? "true" : "false",
             bs.capture_paused ? "true" : "false",
             bs.psram_buffer_active ? "true" : "false",
             ns.sta_connected ? "true" : "false",
             ns.ap_active ? "true" : "false",
             ns.ap_ip,
             (unsigned)ns.ap_clients,
             ns.mac_addr,
             FurnaceNetworkManager::getRunningPartitionLabel(),
             FurnaceNetworkManager::getResetReasonString());

    s_server.send(200, "application/json", json);
}

static void handleApiConfig() {
    ControllerConfig cfg = g_controller.getConfig();

    if (s_server.hasArg("op_mode")) {
        const int m = s_server.arg("op_mode").toInt();
        if (m >= 0 && m <= 3) cfg.op_mode = static_cast<OperatingMode>(m);
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
    if (s_server.hasArg("rx_pin")) {
        const int rxp = s_server.arg("rx_pin").toInt();
        if (rxp == 17 || rxp == 18) {
            g_abcdBus.setUartPins(static_cast<int8_t>(rxp), static_cast<int8_t>(rxp == 17 ? 18 : 17));
        }
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

static void handleApiWifiScan() {
    String json = g_netMgr.scanNetworksJson();
    s_server.send(200, "application/json", json);
}

static void handleApiNetConfig() {
    if (s_server.method() == HTTP_POST) {
        NetworkConfig cfg = g_netMgr.getConfig();

        if (s_server.hasArg("sta_ssid") && s_server.arg("sta_ssid").length() > 0) {
            strncpy(cfg.sta_ssid, s_server.arg("sta_ssid").c_str(), sizeof(cfg.sta_ssid) - 1);
            cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0';
        }
        if (s_server.hasArg("sta_pass") && s_server.arg("sta_pass").length() > 0) {
            strncpy(cfg.sta_pass, s_server.arg("sta_pass").c_str(), sizeof(cfg.sta_pass) - 1);
            cfg.sta_pass[sizeof(cfg.sta_pass) - 1] = '\0';
        }
        if (s_server.hasArg("mdns_host") && s_server.arg("mdns_host").length() > 0) {
            strncpy(cfg.mdns_host, s_server.arg("mdns_host").c_str(), sizeof(cfg.mdns_host) - 1);
            cfg.mdns_host[sizeof(cfg.mdns_host) - 1] = '\0';
        }
        if (s_server.hasArg("use_static_ip")) {
            cfg.use_static_ip = (s_server.arg("use_static_ip").toInt() != 0);
        }
        if (s_server.hasArg("static_ip") && s_server.arg("static_ip").length() > 0) {
            strncpy(cfg.static_ip, s_server.arg("static_ip").c_str(), sizeof(cfg.static_ip) - 1);
        }
        if (s_server.hasArg("static_gw") && s_server.arg("static_gw").length() > 0) {
            strncpy(cfg.static_gw, s_server.arg("static_gw").c_str(), sizeof(cfg.static_gw) - 1);
        }
        if (s_server.hasArg("static_mask") && s_server.arg("static_mask").length() > 0) {
            strncpy(cfg.static_mask, s_server.arg("static_mask").c_str(), sizeof(cfg.static_mask) - 1);
        }
        if (s_server.hasArg("static_dns") && s_server.arg("static_dns").length() > 0) {
            strncpy(cfg.static_dns, s_server.arg("static_dns").c_str(), sizeof(cfg.static_dns) - 1);
        }
        if (s_server.hasArg("ap_ssid") && s_server.arg("ap_ssid").length() > 0) {
            strncpy(cfg.ap_ssid, s_server.arg("ap_ssid").c_str(), sizeof(cfg.ap_ssid) - 1);
        }
        if (s_server.hasArg("ap_pass") && s_server.arg("ap_pass").length() >= 8) {
            strncpy(cfg.ap_pass, s_server.arg("ap_pass").c_str(), sizeof(cfg.ap_pass) - 1);
        }
        if (s_server.hasArg("ap_always_on")) {
            cfg.ap_always_on = (s_server.arg("ap_always_on").toInt() != 0);
        }

        const bool reconnectNow = !s_server.hasArg("reconnect") || (s_server.arg("reconnect").toInt() != 0);
        g_netMgr.updateConfig(cfg, true, reconnectNow);
    }

    const NetworkConfig& c = g_netMgr.getConfig();
    char json[512];
    snprintf(json, sizeof(json),
             "{"
             "\"sta_ssid\":\"%s\","
             "\"mdns_host\":\"%s\","
             "\"use_static_ip\":%s,"
             "\"static_ip\":\"%s\","
             "\"static_gw\":\"%s\","
             "\"static_mask\":\"%s\","
             "\"static_dns\":\"%s\","
             "\"ap_ssid\":\"%s\","
             "\"ap_always_on\":%s"
             "}",
             c.sta_ssid,
             c.mdns_host,
             c.use_static_ip ? "true" : "false",
             c.static_ip,
             c.static_gw,
             c.static_mask,
             c.static_dns,
             c.ap_ssid,
             c.ap_always_on ? "true" : "false");
    s_server.send(200, "application/json", json);
}

static void handleApiApToggle() {
    const bool en = !s_server.hasArg("enable") || (s_server.arg("enable").toInt() != 0);
    g_netMgr.setManualApOverride(en, 600000UL);
    handleApiStatus();
}

static void handleApiOtaDone() {
    if (s_otaSuccess) {
        s_server.send(200, "application/json", "{\"ok\":true,\"msg\":\"OTA update verified. Rebooting...\"}");
        g_netMgr.scheduleReboot(900);
    } else {
        s_server.send(500, "text/plain", s_otaError.length() > 0 ? s_otaError : "OTA Update Failed");
    }
}

static void handleApiOtaUploadChunk() {
    HTTPUpload& upload = s_server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        s_otaSuccess = false;
        s_otaError   = "";
        Serial.printf("\n[WEB OTA] Receiving firmware image: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            s_otaError = Update.errorString();
            Serial.printf("[WEB OTA] Begin failed: %s\n", s_otaError.c_str());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (s_otaError.length() == 0) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                s_otaError = Update.errorString();
                Serial.printf("[WEB OTA] Write failed: %s\n", s_otaError.c_str());
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (s_otaError.length() == 0) {
            if (Update.end(true)) {
                s_otaSuccess = true;
                Serial.printf("[WEB OTA] Success! Wrote %u bytes. Scheduling reboot...\n", (unsigned)upload.totalSize);
            } else {
                s_otaError = Update.errorString();
                Serial.printf("[WEB OTA] End verification failed: %s\n", s_otaError.c_str());
            }
        }
    }
}

static void handleApiSysReboot() {
    s_server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Rebooting...\"}");
    g_netMgr.scheduleReboot(600);
}

static void handleApiSysResetDefaults() {
    Preferences p;
    p.begin("furnace", false);
    p.clear();
    p.end();

    g_netMgr.resetToDefaults();
    s_server.send(200, "application/json", "{\"ok\":true,\"msg\":\"NVS defaults restored. Rebooting...\"}");
    g_netMgr.scheduleReboot(700);
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
    char rowBuf[3 * ABCD_MAX_FRAME_BYTES + 320];

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
    char lineBuf[3 * ABCD_MAX_FRAME_BYTES + 320];

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

static void handleApiPinscan() {
    static constexpr uint8_t kPins[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 38, 39, 40, 41, 42, 47, 48
    };
    static constexpr size_t kNumPins = sizeof(kPins) / sizeof(kPins[0]);

    const int8_t savedRx = g_abcdBus.getStats().active_rx_pin;
    const int8_t savedTx = g_abcdBus.getStats().active_tx_pin;
    const bool   pullup  = !s_server.hasArg("pullup") || (s_server.arg("pullup").toInt() != 0);

    // Temporarily release UART1 so neither GPIO 17 nor GPIO 18 is driven push-pull by UART1_TX during the scan
    Serial1.end();

    if (s_server.hasArg("dere")) {
        digitalWrite(ABCD_UART_DERE_PIN, s_server.arg("dere").toInt() != 0 ? HIGH : LOW);
    }

    uint8_t  initLevel[kNumPins];
    uint8_t  lastLevel[kNumPins];
    uint32_t edges[kNumPins];

    for (size_t i = 0; i < kNumPins; ++i) {
        const gpio_num_t pin = static_cast<gpio_num_t>(kPins[i]);
        // Enable input buffer on every scanned pin so gpio_get_level reads the physical pad state
        gpio_input_enable(pin);
        if (kPins[i] != 4 && kPins[i] != 21 && kPins[i] != 48 && kPins[i] != 19 && kPins[i] != 20) {
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            if (pullup) {
                gpio_pullup_en(pin);
                gpio_pulldown_dis(pin);
            } else {
                gpio_pullup_dis(pin);
                gpio_pulldown_dis(pin);
            }
        }
    }
    delayMicroseconds(200);

    for (size_t i = 0; i < kNumPins; ++i) {
        const uint8_t lvl = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(kPins[i])));
        initLevel[i] = lvl;
        lastLevel[i] = lvl;
        edges[i]     = 0;
    }

    const uint32_t startMs = millis();
    uint32_t samples = 0;
    while (millis() - startMs < 1000UL) {
        for (size_t i = 0; i < kNumPins; ++i) {
            const uint8_t lvl = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(kPins[i])));
            if (lvl != lastLevel[i]) {
                edges[i]++;
                lastLevel[i] = lvl;
            }
        }
        samples++;
        delayMicroseconds(20);
    }

    // Restore DE/RE LOW (if in passive mode) and restore UART1 pins
    if (!g_abcdBus.isTxEnabled()) {
        digitalWrite(ABCD_UART_DERE_PIN, LOW);
    }
    g_abcdBus.setUartPins(savedRx, savedTx);

    String out;
    out.reserve(2048);
    out += "{\"samples\":";
    out += String(samples);
    out += ",\"pullup\":";
    out += (pullup ? "true" : "false");
    out += ",\"pins\":[";
    for (size_t i = 0; i < kNumPins; ++i) {
        if (i > 0) out += ",";
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"gpio\":%u,\"level\":%u,\"last\":%u,\"edges\":%lu}",
                 (unsigned)kPins[i], (unsigned)initLevel[i], (unsigned)lastLevel[i], (unsigned long)edges[i]);
        out += buf;
    }
    out += "]}";
    s_server.send(200, "application/json", out);
}

static void handleApiProbe() {
    uint16_t destAddr = 0x2001;
    uint16_t srcAddr  = 0x4401;
    uint8_t  t0 = 0x00, t1 = 0x01, t2 = 0x04;

    if (s_server.hasArg("dest")) {
        destAddr = static_cast<uint16_t>(strtoul(s_server.arg("dest").c_str(), nullptr, 16));
    }
    if (s_server.hasArg("src")) {
        srcAddr = static_cast<uint16_t>(strtoul(s_server.arg("src").c_str(), nullptr, 16));
    }
    if (s_server.hasArg("table")) {
        unsigned int a = 0, b = 0, c = 0;
        if (sscanf(s_server.arg("table").c_str(), "%x:%x:%x", &a, &b, &c) == 3) {
            t0 = static_cast<uint8_t>(a);
            t1 = static_cast<uint8_t>(b);
            t2 = static_cast<uint8_t>(c);
        }
    }

    const bool sent = g_abcdBus.sendReadRequest(destAddr, srcAddr, t0, t1, t2);
    // Pump incoming bytes for 60 ms to capture immediate reply from target node
    const uint32_t waitStart = millis();
    while (millis() - waitStart < 60UL) {
        g_abcdBus.loop();
        delay(1);
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"dest\":\"0x%04X\",\"src\":\"0x%04X\",\"table\":\"%02X:%02X:%02X\"}",
             sent ? "true" : "false",
             (unsigned)destAddr, (unsigned)srcAddr,
             (unsigned)t0, (unsigned)t1, (unsigned)t2);
    s_server.send(200, "application/json", buf);
}

static void handleApiBusProbe() {
    String action = s_server.hasArg("action") ? s_server.arg("action") : "read";
    action.trim();
    action.toLowerCase();
    if (action != "read" && action != "write") {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid action (expected read or write)\"}");
        return;
    }

    uint16_t destAddr = ADDR_THERMOSTAT;
    if (s_server.hasArg("dest") && s_server.arg("dest").length() > 0) {
        char* endPtr = nullptr;
        const unsigned long parsedDest = strtoul(s_server.arg("dest").c_str(), &endPtr, 16);
        if (endPtr == s_server.arg("dest").c_str() || parsedDest > 0xFFFFUL) {
            s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid dest hex address\"}");
            return;
        }
        destAddr = static_cast<uint16_t>(parsedDest);
    }

    uint8_t t0 = 0, t1 = 0, t2 = 0;
    unsigned int a = 0, b = 0, c = 0;
    if (!s_server.hasArg("table") || sscanf(s_server.arg("table").c_str(), "%x:%x:%x", &a, &b, &c) != 3 ||
        a > 0xFFU || b > 0xFFU || c > 0xFFU) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid table (expected XX:XX:XX)\"}");
        return;
    }
    t0 = static_cast<uint8_t>(a);
    t1 = static_cast<uint8_t>(b);
    t2 = static_cast<uint8_t>(c);

    uint8_t payload[ABCD_MAX_FRAME_BYTES - 13];
    size_t payloadLen = 0;
    if (s_server.hasArg("payload")) {
        const String rawPayload = s_server.arg("payload");
        int idx = 0;
        const int len = rawPayload.length();
        while (idx < len) {
            while (idx < len && (rawPayload[idx] == ' ' || rawPayload[idx] == ',' || rawPayload[idx] == '\t')) {
                ++idx;
            }
            if (idx >= len) break;
            int end = idx;
            while (end < len && rawPayload[end] != ' ' && rawPayload[end] != ',' && rawPayload[end] != '\t') {
                ++end;
            }
            const String tok = rawPayload.substring(idx, end);
            char* endPtr = nullptr;
            const unsigned long val = strtoul(tok.c_str(), &endPtr, 16);
            if (endPtr == tok.c_str() || *endPtr != '\0' || val > 0xFFUL) {
                s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid hex byte in payload\"}");
                return;
            }
            if (payloadLen >= sizeof(payload)) {
                s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Payload exceeds max ABCD frame size\"}");
                return;
            }
            payload[payloadLen++] = static_cast<uint8_t>(val);
            idx = end;
        }
    }

    bool sent = false;
    bool simulated = false;
    if (action == "write") {
        sent = g_abcdBus.sendWriteRequest(destAddr, ADDR_CONTROLLER, t0, t1, t2, payload, payloadLen);
        simulated = !sent;
    } else {
        sent = g_abcdBus.sendReadRequest(destAddr, ADDR_CONTROLLER, t0, t1, t2);
        simulated = !sent;
    }

    const uint32_t waitStart = millis();
    while (millis() - waitStart < 60UL) {
        g_abcdBus.loop();
        delay(1);
    }

    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"action\":\"%s\",\"sent\":%s,\"tx_enabled\":%s,\"simulated\":%s,"
             "\"dest\":\"0x%04X\",\"src\":\"0x%04X\",\"table\":\"%02X:%02X:%02X\",\"payload_len\":%u}",
             action.c_str(),
             sent ? "true" : "false",
             g_abcdBus.isTxEnabled() ? "true" : "false",
             simulated ? "true" : "false",
             (unsigned)destAddr, (unsigned)ADDR_CONTROLLER,
             (unsigned)t0, (unsigned)t1, (unsigned)t2,
             (unsigned)payloadLen);
    s_server.send(200, "application/json", buf);
}

void WebPortal::begin() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status", HTTP_GET, handleApiStatus);
    s_server.on("/api/config", HTTP_ANY, handleApiConfig);
    s_server.on("/api/sim", HTTP_ANY, handleApiSim);
    s_server.on("/api/packets", HTTP_GET, handleApiPackets);
    s_server.on("/api/packets.csv", HTTP_GET, handleApiPacketsCsv);
    s_server.on("/api/pinscan", HTTP_GET, handleApiPinscan);
    s_server.on("/api/probe", HTTP_ANY, handleApiProbe);
    s_server.on("/api/bus/probe", HTTP_POST, handleApiBusProbe);

    // Wireless Provisioning, AP & OTA Endpoints
    s_server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
    s_server.on("/api/net/config", HTTP_ANY, handleApiNetConfig);
    s_server.on("/api/net/ap_toggle", HTTP_ANY, handleApiApToggle);
    s_server.on("/api/ota/upload", HTTP_POST, handleApiOtaDone, handleApiOtaUploadChunk);
    s_server.on("/api/sys/reboot", HTTP_ANY, handleApiSysReboot);
    s_server.on("/api/sys/reset_defaults", HTTP_ANY, handleApiSysResetDefaults);

    // Captive Portal OS detection routes (Android, iOS/macOS, Windows)
    s_server.on("/generate_204", HTTP_GET, handleCaptivePortalRedirect);
    s_server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalRedirect);
    s_server.on("/ncsi.txt", HTTP_GET, handleCaptivePortalRedirect);
    s_server.onNotFound(handleRoot);

    s_server.begin();
}

void WebPortal::loop() {
    s_server.handleClient();
}
