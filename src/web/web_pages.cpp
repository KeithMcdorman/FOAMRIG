#include <Arduino.h>

// Globals defined in src/main.cpp
extern const char* FW_VERSION;

const char* updatePage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Foam Rig Firmware Update</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #020617;
      --panel: #020617;
      --panel-border: #1e293b;
      --accent: #38bdf8;
      --accent-soft: rgba(56,189,248,0.1);
      --accent-soft-border: rgba(56,189,248,0.35);
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
      --good: #22c55e;
      --bad: #ef4444;
      --gap: 0.4rem;
    }

    * {
      box-sizing: border-box;
      -webkit-font-smoothing: antialiased;
    }

    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      display: flex;
      justify-content: center;
      align-items: flex-start;
      min-height: 100vh;
      padding: 1.25rem;
    }

    .app {
      width: 100%;
      max-width: 480px;
      background: linear-gradient(145deg, rgba(15,23,42,0.95), rgba(15,23,42,0.9));
      border-radius: 1.25rem;
      border: 1px solid rgba(148,163,184,0.18);
      box-shadow:
        0 22px 55px rgba(15,23,42,0.9),
        0 0 0 1px rgba(15,23,42,0.6);
      padding: 1.4rem 1.5rem 1.7rem;
    }

    h1 {
      margin: 0 0 0.3rem;
      font-size: 1.25rem;
      letter-spacing: 0.02em;
    }

    .subtitle {
      font-size: 0.85rem;
      color: var(--text-muted);
      margin-bottom: 1rem;
    }

    .card {
      border-radius: 1rem;
      border: 1px solid rgba(30,64,175,0.7);
      background:
        radial-gradient(circle at 0 0, rgba(56,189,248,0.12), transparent 55%),
        radial-gradient(circle at 100% 0, rgba(59,130,246,0.16), transparent 55%),
        #020617;
      padding: 1rem 1rem 0.9rem;
    }

    .field {
      display: flex;
      flex-direction: column;
      gap: 0.35rem;
      margin-bottom: 0.7rem;
      font-size: 0.85rem;
    }

    label {
      color: var(--text-muted);
    }

    input[type="file"] {
      font-size: 0.85rem;
      color: var(--text-main);
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      border-radius: 999px;
      border: none;
      padding: 0.55rem 1.35rem;
      background: linear-gradient(135deg,#38bdf8,#0ea5e9);
      color: #020617;
      font-size: 0.9rem;
      font-weight: 600;
      cursor: pointer;
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 14px 30px rgba(56,189,248,0.45);
      transition: transform 0.1s ease, box-shadow 0.1s ease, opacity 0.1s ease;
    }

    .btn:active {
      transform: translateY(1px) scale(0.99);
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 8px 18px rgba(56,189,248,0.35);
    }

    .btn:disabled {
      opacity: 0.55;
      cursor: default;
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 6px 14px rgba(15,23,42,0.85);
    }

    .upload-row {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 0.6rem;
      margin-top: 0.4rem;
    }

    .upload-status {
      font-size: 0.8rem;
      color: var(--text-muted);
    }

    .version {
      font-size: 0.78rem;
      color: var(--text-muted);
      margin-top: 0.7rem;
    }

    .nav-row {
      display: flex;
      justify-content: space-between;
      margin-top: 1rem;
      font-size: 0.8rem;
    }

    a.nav {
      color: var(--accent);
      text-decoration: none;
    }
  </style>
</head>
<body>
<div class="app">
  <h1>Firmware Update</h1>
  <div class="subtitle">
    Current firmware: <strong>{{FW_VERSION}}</strong><br/>
    Upload a compiled <code>firmware.bin</code> to update the rig.
  </div>

  <div class="card">
    <form id="otaForm" method="POST" action="/update" enctype="multipart/form-data">
      <div class="field">
        <label for="firmware">Firmware binary (.bin)</label>
        <!-- Limit picker to .bin files; browser still remembers last folder -->
        <input type="file" id="firmware" name="firmware" accept=".bin" required />
      </div>
      <div class="upload-row">
        <button class="btn" type="submit" id="uploadBtn">Upload &amp; Flash</button>
        <div id="uploadStatus" class="upload-status"></div>
      </div>
    </form>
    <div class="version">
      After a successful update the rig will reboot automatically.
    </div>
  </div>

  <div class="nav-row">
    <a class="nav" href="/settings">⬅ Settings</a>
    <a class="nav" href="/">Live Dashboard ➜</a>
  </div>
</div>

<script>
  document.addEventListener('DOMContentLoaded', function() {
    var form   = document.getElementById('otaForm');
    var btn    = document.getElementById('uploadBtn');
    var status = document.getElementById('uploadStatus');

    if (!form) return;

    form.addEventListener('submit', function() {
      if (btn)    btn.disabled = true;
      if (status) status.textContent = 'Uploading Please wait....';
    });
  });
</script>
</body>
</html>
)rawliteral";

// ---------- MAIN PAGE (live gauges) ----------

const char* mainPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Spray Foam Pressure Monitor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <!-- iOS fullscreen when “Add to Home Screen” -->
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #0f172a;
      --card-bg: #020617;
      --card-border: #1e293b;
      --accent: #38bdf8;
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
      --good: #22c55e;
      --bad: #ef4444;
    }

    * {
      box-sizing: border-box;
      -webkit-font-smoothing: antialiased;
    }

    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      height: 100vh;
      overflow: hidden; /* ensure the dashboard fits on screen */
    }

    .app {
      height: 100vh;
      padding: 0.4rem 0.8rem 0.6rem;
      display: flex;
      flex-direction: column;
    }

    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 0.25rem;
    }

    .title-group {
      display: flex;
      flex-direction: column;
    }

    h1 {
      margin: 0;
      font-size: 1.25rem;
      letter-spacing: 0.04em;
      text-transform: uppercase;
    }

    .subtitle {
      font-size: 0.75rem;
      color: var(--text-muted);
      margin-top: 0.1rem;
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 0.35rem;
      font-size: 0.7rem;
      padding: 0.2rem 0.55rem;
      border-radius: 999px;
      background: rgba(15, 23, 42, 0.85);
      border: 1px solid rgba(148, 163, 184, 0.35);
    }

    .status-dot {
      width: 0.45rem;
      height: 0.45rem;
      border-radius: 999px;
      background: var(--bad);
      box-shadow: 0 0 8px rgba(239, 68, 68, 0.8);
    }

    .settings-btn {
      border-radius: 999px;
      padding: 0.3rem 0.8rem;
      background: rgba(15, 23, 42, 0.9);
      border: 1px solid rgba(148, 163, 184, 0.5);
      color: var(--text-main);
      font-size: 0.8rem;
      display: inline-flex;
      align-items: center;
      gap: 0.3rem;
      text-decoration: none;
    }

    .settings-btn span.icon {
      font-size: 0.9rem;
    }

    
    .reset-btn {
      border-radius: 999px;
      padding: 0.3rem 0.8rem;
      background: rgba(239, 68, 68, 0.14);
      border: 1px solid rgba(239, 68, 68, 0.55);
      color: var(--text-main);
      font-size: 0.8rem;
      display: inline-flex;
      align-items: center;
      gap: 0.3rem;
      cursor: pointer;
      user-select: none;
    }
    .reset-btn:hover { filter: brightness(1.08); }
    .reset-btn:disabled { opacity: 0.55; cursor: not-allowed; }

    .is-hidden { display: none !important; }
.settings-btn:active {
      transform: translateY(1px);
    }

    /* Global alert/status bar above the gauges */
    .alert-bar {
      min-height: 1.1rem;
      font-size: 0.75rem;
      color: #e5e7eb;
      text-align: center;
      padding: 0.15rem 0.4rem;
      margin-bottom: 0.25rem;
      border-radius: 0.5rem;
      border: 1px solid transparent;
    }
    .alert-active {
      background: rgba(127, 29, 29, 0.35);
      border-color: #b91c1c;
      color: #fca5a5;
      box-shadow: 0 0 10px rgba(248, 113, 113, 0.45);
    }

    /* Layout for gauges */
    .dashboard {
      flex: 1;
      display: flex;
      flex-direction: column;
      gap: 0.4rem;
    }

    .top-row {
  display: grid;
  grid-template-columns: minmax(260px, 1fr) minmax(260px, 0.95fr) minmax(260px, 1fr);
  gap: var(--gap);
  align-items: stretch;
}

.bottom-row {
  display: grid;
  grid-template-columns: repeat(6, minmax(0, 1fr));
  gap: var(--gap);
  align-items: stretch;
}
.gauge-card {
      background: radial-gradient(circle at top left, #0b1120 0, #020617 60%);
      border-radius: 16px;
      border: 1px solid var(--card-border);
      padding: 0.35rem 0.35rem 0.45rem;
      display: flex;
      flex-direction: column;
      align-items: center;
      box-shadow: 0 14px 30px rgba(15, 23, 42, 0.9);
    }

    .gauge-card.large {
  width: 100%;
  max-width: none;
  min-width: 0;
}

    .gauge-card.small {
      width: 20vw;
      max-width: 220px;
      min-width: 160px;
    }

    .placeholder { visibility: hidden; }

    .gauge-header {
      font-size: 0.75rem;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.15rem;
    }

    .gauge-value {
      font-size: 0.85rem;
      margin-top: 0.15rem;
      color: var(--text-main);
    }

    .gauge-value.secondary {
      font-size: 0.75rem;
      color: var(--text-muted);
      margin-top: 0.05rem;
    }

    canvas.gauge {
      width: 100%;
      height: auto;
      display: block;
    }

    /* Center column: Spray (top), Ratio (middle), Drum Air (bottom) */
    .center-column {
      display: flex;
      flex-direction: column;
      gap: var(--gap);
      align-items: stretch;
      height: 100%;
      width: 100%;
      max-width: none;
      min-width: 0;
    }

    /* Make the center stack fill the same vertical space as the big gauges */
    .center-column .action-row,
    .center-column .enable-row {
      flex: 1.2 1 0;
      align-items: stretch;
    }

    .center-column .status-row {
      flex: 1 1 0;
      align-items: stretch;
    }

    .center-column .action-row .mode-btn {
      height: 100%;
    }

    

    .center-column .enable-row .mode-btn {
      height: 100%;
      /* Stack label over status (Hose enable buttons) */
      flex-direction: column;
      gap: 0.18rem;
    }

.center-column .status-row .hose-status-btn,
    .center-column .status-row .ratio-card {
      height: 100%;
      min-height: 0;
    }

    .btn-row {
      display: flex;
      gap: var(--gap);
      width: 100%;
    }

    .triple-row {
      display: flex;
      gap: var(--gap);
      width: 100%;
      align-items: stretch;
    }

    .triple-row > * {
      flex: 1;
    }

    .btn-label {
      display: block;
      font-size: 0.65rem;
      letter-spacing: 0.12em;
      opacity: 0.82;
      margin-bottom: 0.12rem;
    }

    .btn-sub {
      display: block;
      font-size: 1.05rem;
      font-weight: 700;
      letter-spacing: 0.06em;
      opacity: 1;
      margin-top: 0.05rem;
    }

    .hose-status-btn {
      border-radius: 16px;
      border: 1px solid rgba(148, 163, 184, 0.6);
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      padding: 0.35rem 0.35rem;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
          }

    .hose-status-btn.heating {
      background: linear-gradient(135deg, #22c55e, #16a34a);
      color: #020617;
      border-color: rgba(22,163,74,0.9);
      box-shadow: 0 0 10px rgba(22,163,74,0.7);
    }

    /* Enabled, not actively heating ("At Temp") */
    .hose-status-btn.attemp {
      background: linear-gradient(135deg, #0ea5e9, #2563eb);
      color: #020617;
      border-color: rgba(37,99,235,0.9);
      box-shadow: 0 0 10px rgba(37,99,235,0.55);
    }

    .hose-label {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.25rem;
    }

    .hose-temp {
      font-size: 1.05rem;
      font-weight: 700;
      line-height: 1.2;
    }

    .hose-state {
      margin-top: 0.15rem;
      font-size: 0.7rem;
      letter-spacing: 0.1em;
      text-transform: uppercase;
      opacity: 0.9;
    }

    .bottom-row .gauge-card.small {
      width: 100%;
      max-width: none;
      min-width: 0;
    }

    .setpoint-card {
      padding: 0;
      overflow: hidden;
      justify-content: stretch;
    }

    .setpoint-card .sp-btn {
      width: 100%;
      flex: 0 0 25%;
      border: none;
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      font-size: 1.25rem;
      font-weight: 800;
      padding: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      border-bottom: 1px solid rgba(148, 163, 184, 0.35);
    }

    .setpoint-card .sp-btn.sp-down {
      border-top: 1px solid rgba(148, 163, 184, 0.35);
      border-bottom: none;
    }

    .setpoint-card .sp-center {
      flex: 0 0 50%;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 0.35rem 0.25rem;
      gap: 0.2rem;
    }

    .setpoint-card .sp-label {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      text-align: center;
    }

    .setpoint-card .sp-value {
      font-size: 1.2rem;
      font-weight: 800;
    }


    .ratio-card {
      padding: 0.3rem 0.3rem 0.45rem;
      background: radial-gradient(circle at top, #020617 0, #020617 55%);
      border-radius: 16px;
      border: 1px solid var(--card-border);
      box-shadow: 0 14px 30px rgba(15, 23, 42, 0.9);
      display: flex;
      flex-direction: column;
      align-items: center;
      flex: 1;
    }

    .ratio-label-top {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.25rem;
    }

    .ratio-circle {
      width: 100%;
      border-radius: 50%;
      border: 2px solid rgba(148, 163, 184, 0.4);
      background: radial-gradient(circle at 30% 0%, rgba(148, 163, 184, 0.15), transparent 55%);
      position: relative;
      padding-top: 100%;
      transition: background-color 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
    }

    .ratio-inner {
      position: absolute;
      inset: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
    }

    .ratio-text {
      font-size: 1.0rem;
      font-weight: 600;
      letter-spacing: 0.3em;
    }

    .ratio-good {
      background-color: rgba(34, 197, 94, 0.16);
      border-color: rgba(34, 197, 94, 0.9);
      box-shadow: 0 0 14px rgba(34, 197, 94, 0.9);
    }

    .ratio-bad {
      background-color: rgba(239, 68, 68, 0.16);
      border-color: rgba(239, 68, 68, 0.9);
      box-shadow: 0 0 14px rgba(239, 68, 68, 0.9);
    }

    .mode-btn {
      width: 100%;
      padding: 0.35rem 0.4rem;
      border-radius: 999px;
      border: 1px solid rgba(148, 163, 184, 0.6);
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.1em;
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
    }

    .mode-btn.on {
      background: linear-gradient(135deg, #22c55e, #16a34a);
      color: #020617;
      border-color: rgba(22,163,74,0.9);
      box-shadow: 0 0 10px rgba(22,163,74,0.7);
    }

    .mode-btn.interlock {
      background: linear-gradient(135deg, #ef4444, #b91c1c);
      color: #fee2e2;
      border-color: rgba(248,113,113,0.9);
      box-shadow: 0 0 10px rgba(248,113,113,0.75);
    }

    footer {
      margin-top: 0.15rem;
      font-size: 0.65rem;
      color: var(--text-muted);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    .footer-right {
      opacity: 0.7;
    }

    @media (max-width: 900px) {
      body {
        overflow-y: auto;
      }
      .app {
        height: auto;
        min-height: 100vh;
      }
      .dashboard {
        gap: 0.6rem;
      }
      .top-row,
      .bottom-row {
        grid-template-columns: 1fr;
        gap: 0.6rem;
      }
      .gauge-card.large,
      .gauge-card.small {
        width: 100%;
        max-width: none;
        min-width: 0;
      }
      .center-column {
        width: 100%;
        max-width: none;
        min-width: 0;
        height: auto;
      }
      .center-column .action-row,
      .center-column .status-row,
      .center-column .enable-row {
        flex: 0 0 auto;
      }
      .ratio-card {
        margin: 0;
      }
    }
  </style>
</head>
<body>
<div class="app">
  <header>
    <div class="title-group">
      <h1>Spray Foam Pressure Monitor</h1>
      <div class="subtitle">foam.local • High-pressure &amp; Air System</div>
    </div>
    <div style="display:flex;align-items:center;gap:0.6rem;">
      <div class="status-pill">
        <div id="liveDot" class="status-dot"></div>
        <span id="liveStatusText">Connecting…</span>
      </div>
      <button id="interlockResetBtn" type="button" class="reset-btn is-hidden">Reset</button>
      <a href="/settings" class="settings-btn">
        <span class="icon">⚙️</span>
        <span>Settings</span>
      </a>
    </div>
  </header>

  <!-- Global interlock / status message bar -->
  <div id="alertBar" class="alert-bar"></div>

  <main class="dashboard">
    <!-- Top row: big high-pressure gauges with center control column -->
    <div class="top-row">
      <div class="gauge-card large left">
        <div class="gauge-header">Iso Pressure</div>
        <canvas id="isoGauge" class="gauge"></canvas>
        <div class="gauge-value" id="isoValue">0 PSI</div>
        <div class="gauge-value secondary" id="isoTempValue">-- °F</div>
      </div>

      <div class="center-column">
        <div class="btn-row action-row">
          <button id="sprayBtn" class="mode-btn">Spray</button>
          <button id="drumAirBtn" class="mode-btn">Drum Air</button>
        </div>

        <div class="triple-row status-row">
          <button id="hose1StatusBtn" class="hose-status-btn">
            <div class="hose-label">Hose 1</div>
            <div class="hose-temp" id="hose1TempDisplay">-- °F</div>
            <div class="hose-state" id="hose1StateDisplay">OFF</div>
          </button>

          <div class="ratio-card">
          <div class="ratio-label-top">Ratio</div>
          <div id="ratioCircle" class="ratio-circle">
            <div class="ratio-inner">
              <div id="ratioText" class="ratio-text">RATIO</div>
            </div>
          </div>
        </div>

          <button id="hose2StatusBtn" class="hose-status-btn">
            <div class="hose-label">Hose 2</div>
            <div class="hose-temp" id="hose2TempDisplay">-- °F</div>
            <div class="hose-state" id="hose2StateDisplay">OFF</div>
          </button>
        </div>

        <div class="btn-row enable-row">
          <button id="hose1EnableBtn" class="mode-btn"><span class="btn-label">Hose 1</span><span class="btn-sub">Standby!</span></button>
          <button id="hose2EnableBtn" class="mode-btn"><span class="btn-label">Hose 2</span><span class="btn-sub">Standby!</span></button>
        </div>
      </div>

      <div class="gauge-card large right">
        <div class="gauge-header">Resin Pressure</div>
        <canvas id="resinGauge" class="gauge"></canvas>
        <div class="gauge-value" id="resinValue">0 PSI</div>
        <div class="gauge-value secondary" id="resinTempValue">-- °F</div>
      </div>
    </div>

    <!-- Bottom row: smaller low-pressure & air gauges -->
<div class="bottom-row">
  <div class="gauge-card small" id="isoLowCard">
    <div class="gauge-header">Iso Low</div>
    <canvas id="isoLowGauge" class="gauge"></canvas>
    <div class="gauge-value" id="isoLowValue">0 PSI</div>
    <div class="gauge-value secondary" id="isoLowTempValue">-- °F</div>
  </div>

  <div class="gauge-card small">
    <div class="gauge-header">Primary Air Piston</div>
    <canvas id="primaryAirGauge" class="gauge"></canvas>
    <div class="gauge-value" id="primaryAirValue">0 PSI</div>
  </div>

  <div class="gauge-card small setpoint-card" id="hose1SetCard">
    <button class="sp-btn sp-up" id="hose1SetUpBtn">▲</button>
    <div class="sp-center">
      <div class="sp-label">Hose Heat 1</div>
      <div class="sp-value" id="hose1SetValue">-- °F</div>
    </div>
    <button class="sp-btn sp-down" id="hose1SetDownBtn">▼</button>
  </div>

  <div class="gauge-card small setpoint-card" id="hose2SetCard">
    <button class="sp-btn sp-up" id="hose2SetUpBtn">▲</button>
    <div class="sp-center">
      <div class="sp-label">Hose Heat 2</div>
      <div class="sp-value" id="hose2SetValue">-- °F</div>
    </div>
    <button class="sp-btn sp-down" id="hose2SetDownBtn">▼</button>
  </div>

  <div class="gauge-card small">
    <div class="gauge-header">Gun AP Air</div>
    <canvas id="gunAirGauge" class="gauge"></canvas>
    <div class="gauge-value" id="gunAirValue">0 PSI</div>
  </div>

  <div class="gauge-card small" id="resinLowCard">
    <div class="gauge-header">Resin Low</div>
    <canvas id="resinLowGauge" class="gauge"></canvas>
    <div class="gauge-value" id="resinLowValue">0 PSI</div>
    <div class="gauge-value secondary" id="resinLowTempValue">-- °F</div>
  </div>
</div>
</main>

  <footer>
    <div>Last update: <span id="lastUpdate">--:--:--</span></div>
    <div class="footer-right">{{FW_VERSION}} • V2: 2-zone hose heat (setpoint + swing) + JSON telemetry + relay interlock + OTA</div>
  </footer>
</div>

<script>
  var target = 1000;
  var margin = 10;
  var diff   = 50;

  var airTarget = 100;
  var gunTarget = 100;
  var isoLowTarget   = 200;
  var resinLowTarget = 200;

  var supplyLow = 150;

  // Temperature configuration
  var isoTempTarget      = 120; // HP Iso
  var resinTempTarget    = 120; // HP Resin
  var isoLowTempTarget   = 120; // Low Iso
  var resinLowTempTarget = 120; // Low Resin
  var tempMin            = 40;
  var tempMax            = 180;

  var isoTemp      = null;
  var resinTemp    = null;
  var isoLowTemp   = null;
  var resinLowTemp = null;

  var drumAir = false;
  var spray   = false;

  // Hose heat (live page)
  var hose1En   = false;
  var hose2En   = false;
  var hose1Heat = false;
  var hose2Heat = false;
  var hose1Temp = null;
  var hose2Temp = null;
  var hose1Set  = null;
  var hose2Set  = null;
  var hose1Tol  = null;
  var hose2Tol  = null;

  var lastIsoLow   = 0;
  var lastResinLow = 0;

  var interlockActive = false;
  var resetInFlight = false;

  function updateResetButton() {
    var btn = document.getElementById('interlockResetBtn');
    if (!btn) return;
    if (interlockActive) btn.classList.remove('is-hidden');
    else btn.classList.add('is-hidden');
    btn.disabled = resetInFlight;
  }

  function requestInterlockReset() {
    if (!interlockActive || resetInFlight) return;
    resetInFlight = true;
    updateResetButton();
    fetch('/api/interlock/reset', { method: 'POST' })
      .then(function(r){ return r.json(); })
      .then(function(res){
        if (res && res.ok) {
          interlockActive = false;
          setStatus('Ready', false);
        } else {
          var msg = (res && (res.error || res.message)) ? (res.error || res.message) : 'Interlock reset denied';
          setStatus(msg, true);
          interlockActive = true; // remain latched visually
        }
        updateResetButton();
      })
      .catch(function(){
        setStatus('Interlock reset failed', true);
        interlockActive = true;
        updateResetButton();
      })
      .finally(function(){
        resetInFlight = false;
        updateResetButton();
      });
  }


  // Persistent status bar helper
  function setStatus(msg, isError) {
    var bar = document.getElementById('alertBar');
    if (!msg) msg = 'Ready';
    bar.textContent = msg;

    if (isError) {
      bar.classList.add('alert-active');
    } else {
      bar.classList.remove('alert-active');
    }
  }

  // Initial status
  setStatus('Ready', false);
  // Interlock reset button
  (function(){
    var btn = document.getElementById('interlockResetBtn');
    if (btn) btn.addEventListener('click', requestInterlockReset);
    updateResetButton();
  })();


  // Load settings from device
  fetch('/api/settings').then(function(r){return r.json();}).then(function(data){
    target = data.target;
    margin = data.margin;
    diff   = data.diff;
    if (data.airTarget !== undefined)      airTarget      = data.airTarget;
    if (data.gunTarget !== undefined)      gunTarget      = data.gunTarget;
    if (data.isoLowTarget !== undefined)   isoLowTarget   = data.isoLowTarget;
    if (data.resinLowTarget !== undefined) resinLowTarget = data.resinLowTarget;
    if (data.supplyLow !== undefined)      supplyLow      = data.supplyLow;

    if (data.isoTempTarget !== undefined)      isoTempTarget      = data.isoTempTarget;
    if (data.resinTempTarget !== undefined)    resinTempTarget    = data.resinTempTarget;
    if (data.isoLowTempTarget !== undefined)   isoLowTempTarget   = data.isoLowTempTarget;
    if (data.resinLowTempTarget !== undefined) resinLowTempTarget = data.resinLowTempTarget;
    if (data.tempMinF !== undefined)           tempMin            = data.tempMinF;
    if (data.tempMaxF !== undefined)           tempMax            = data.tempMaxF;

    // Pressure gauge setpoints
    isoGauge.setPoint        = target;
    resinGauge.setPoint      = target;
    primaryAirGauge.setPoint = airTarget;
    gunAirGauge.setPoint     = gunTarget;
    isoLowGauge.setPoint     = isoLowTarget;
    resinLowGauge.setPoint   = resinLowTarget;

    // Temperature ring configuration on all temp-enabled gauges
    isoGauge.tempMin      = tempMin;
    isoGauge.tempMax      = tempMax;
    resinGauge.tempMin    = tempMin;
    resinGauge.tempMax    = tempMax;
    isoLowGauge.tempMin   = tempMin;
    isoLowGauge.tempMax   = tempMax;
    resinLowGauge.tempMin = tempMin;
    resinLowGauge.tempMax = tempMax;

    // Per-gauge temperature setpoints (drive green bands)
    isoGauge.tempSetPoint      = isoTempTarget;
    resinGauge.tempSetPoint    = resinTempTarget;
    isoLowGauge.tempSetPoint   = isoLowTempTarget;
    resinLowGauge.tempSetPoint = resinLowTempTarget;

    isoGauge.draw(isoGauge.value || 0);
    resinGauge.draw(resinGauge.value || 0);
    primaryAirGauge.draw(primaryAirGauge.value || 0);
    gunAirGauge.draw(gunAirGauge.value || 0);
    isoLowGauge.draw(isoLowGauge.value || 0);
    resinLowGauge.draw(resinLowGauge.value || 0);
  }).catch(function(e){ console.log('Settings load failed', e); });

  // Round gauge class with optional inner temp ring
  function RoundGauge(canvasId, maxVal) {
    this.canvas = document.getElementById(canvasId);
    this.ctx    = this.canvas.getContext('2d');
    this.max    = maxVal;
    this.value  = null;   // smoothed PSI value we draw
    this.size   = 0;
    this.setPoint = target;

    // Temperature ring config
    this.showTempRing = false;
    this.tempValue    = null;   // °F
    this.tempMin      = 40;
    this.tempMax      = 180;
    this.tempSetPoint = null;   // °F

    var self = this;
    this.resize = function() {
      var parentWidth = self.canvas.parentElement.clientWidth;
      var size = parentWidth;
      if (size > 260) size = 260;
      if (size < 160) size = 160;
      self.size = size;

      var scale = window.devicePixelRatio || 1;
      self.canvas.style.width  = size + 'px';
      self.canvas.style.height = size + 'px';
      self.canvas.width  = size * scale;
      self.canvas.height = size * scale;
      self.ctx.setTransform(scale, 0, 0, scale, 0, 0);
      self.draw(self.value !== null ? self.value : 0);
    };

    window.addEventListener('resize', this.resize);
    this.resize();
  }

  // Smooth PSI motion so the needle glides instead of jumping
  RoundGauge.prototype.setValue = function(v) {
    if (v < 0) v = 0;
    if (v > this.max) v = this.max;

    var alpha = 0.30;  // 0..1 (higher = more responsive, lower = smoother)
    if (this.value === null || isNaN(this.value)) {
      this.value = v;
    } else {
      this.value = this.value + alpha * (v - this.value);
    }
    this.draw(this.value);
  };

  RoundGauge.prototype.setTemp = function(tF) {
    this.tempValue = tF;
    if (this.showTempRing) {
      this.draw(this.value !== null ? this.value : 0);
    }
  };

  RoundGauge.prototype.draw = function(value) {
    var canvas = this.canvas;
    var ctx    = this.ctx;
    var w      = this.size;
    var h      = this.size;
    var cx     = w / 2;
    var cy     = h / 2 + 6;
    var r      = Math.min(w, h) / 2 - 20;
    var start  = 0.75 * Math.PI;
    var span   = 1.5 * Math.PI;
    var max    = this.max;

    if (value == null) value = 0;
    if (value < 0)     value = 0;
    if (value > max)   value = max;

    var mPct = margin / 100.0;
    var sp = (this.setPoint !== undefined && this.setPoint !== null) ? this.setPoint : target;
    var low  = Math.max(0, sp * (1 - mPct));
    var high = Math.min(max, sp * (1 + mPct));
    var a0   = start;
    var aMax = start + span;
    var aLow = start + (low  / max) * span;
    var aHigh= start + (high / max) * span;
    var aT   = start + (sp   / max) * span;

    ctx.clearRect(0, 0, w, h);

    // Outer PSI band: red/green segments
    if (low > 0) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, a0, aLow);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#F00';
      ctx.stroke();
    }
    if (low < high) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, aLow, aHigh);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#0F0';
      ctx.stroke();
    }
    if (high < max) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, aHigh, aMax);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#F00';
      ctx.stroke();
    }

    // Outer PSI target line
    ctx.beginPath();
    ctx.moveTo(cx + Math.cos(aT) * (r - 18), cy + Math.sin(aT) * (r - 18));
    ctx.lineTo(cx + Math.cos(aT) * r,        cy + Math.sin(aT) * r);
    ctx.lineWidth = 3.5;
    ctx.strokeStyle = '#00F';
    ctx.stroke();

    // Inner temperature ring, deliberately pulled inward so it stays visually
    // separate from the pressure band. The green band is drawn first and
    // *not* overwritten by the temp arc (which is drawn on a smaller radius).
    if (this.showTempRing) {
      var tMin = this.tempMin;
      var tMax = this.tempMax;
      if (tMax <= tMin) {
        tMin = 40; tMax = 180;
      }

      // Temp ring closer to center than outer PSI band
      var innerR = r - 15;

      // Base temp range background
      ctx.beginPath();
      ctx.arc(cx, cy, innerR, start, start + span);
      ctx.lineWidth = 3;
      ctx.strokeStyle = '#1e293b';
      ctx.stroke();

      // Green temp band around tempSetPoint using same % margin
      if (this.tempSetPoint !== null && typeof this.tempSetPoint === 'number') {
        var spT = this.tempSetPoint;
        var mT  = mPct;

        var halfBand = Math.abs(spT) * mT;
        var lowT  = spT - halfBand;
        var highT = spT + halfBand;

        if (lowT  < tMin) lowT  = tMin;
        if (highT > tMax) highT = tMax;

        if (highT > lowT) {
          var lowF  = (lowT  - tMin) / (tMax - tMin);
          var highF = (highT - tMin) / (tMax - tMin);
          var aLowT  = start + lowF  * span;
          var aHighT = start + highF * span;

          ctx.beginPath();
          ctx.arc(cx, cy, innerR, aLowT, aHighT);
          ctx.lineWidth = 4;
          ctx.strokeStyle = '#22c55e';
          ctx.stroke();
        }
      }

      // Temperature tick marks & labels INSIDE the ring
      var tempTicks = 5; // endpoints + intermediates
      for (var ti = 0; ti < tempTicks; ti++) {
        var tf = ti / (tempTicks - 1);         // 0..1 across temp range
        var ta = start + tf * span;            // angle along arc

        var tInner = innerR - 4;
        var tOuter = innerR + 2;

        // Tick line
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(ta) * tInner, cy + Math.sin(ta) * tInner);
        ctx.lineTo(cx + Math.cos(ta) * tOuter, cy + Math.sin(ta) * tOuter);
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = '#64748b';
        ctx.stroke();

        // Label every tick, inside the ring
        var tVal = Math.round(tMin + tf * (tMax - tMin));
        ctx.font = Math.round(w * 0.035) + 'px Arial';
        ctx.fillStyle = '#9ca3af';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        var labelR = innerR - 16; // inside the temp ring
        ctx.fillText(
          tVal + '°',
          cx + Math.cos(ta) * labelR,
          cy + Math.sin(ta) * labelR
        );
      }

      // Actual temperature: blue arc on a *smaller* radius so the green band
      // remains visible, plus a mini pointer crossing the ring.
      if (this.tempValue !== null && typeof this.tempValue === 'number') {
        var t = this.tempValue;
        if (t < tMin) t = tMin;
        if (t > tMax) t = tMax;
        var tFrac  = (t - tMin) / (tMax - tMin);
        var tAngle = start + tFrac * span;

        var tempR = innerR - 5; // smaller radius than green band

        // Blue temp arc
        ctx.beginPath();
        ctx.arc(cx, cy, tempR, start, tAngle);
        ctx.lineWidth = 3;
        ctx.strokeStyle = '#38bdf8';
        ctx.stroke();

        // Mini pointer crossing the full temp ring
        var pinInner = tempR - 6;
        var pinOuter = innerR + 6;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(tAngle) * pinInner, cy + Math.sin(tAngle) * pinInner);
        ctx.lineTo(cx + Math.cos(tAngle) * pinOuter, cy + Math.sin(tAngle) * pinOuter);
        ctx.lineWidth = 2;
        ctx.strokeStyle = '#e5e7eb';
        ctx.stroke();
      }
    }

    // PSI ticks
    ctx.lineWidth = 2;
    ctx.strokeStyle = '#888';
    var ticks = (max <= 400) ? 9 : 17;
    for (var i = 0; i < ticks; i++) {
      var ta2 = start + (i / (ticks - 1)) * span;
      var tl = (i % 2 === 0) ? 12 : 6;
      var ts = r - tl;
      ctx.beginPath();
      ctx.moveTo(cx + Math.cos(ta2) * ts, cy + Math.sin(ta2) * ts);
      ctx.lineTo(cx + Math.cos(ta2) * r,  cy + Math.sin(ta2) * r);
      ctx.stroke();
      if (i % 2 === 0) {
        var lbl = Math.round((i / (ticks - 1)) * max);
        ctx.font = Math.round(w * 0.04) + 'px Arial';
        ctx.fillStyle = '#ddd';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(lbl, cx + Math.cos(ta2) * (r + 15), cy + Math.sin(ta2) * (r + 15));
      }
    }

    // PSI needle
    var ang = start + (value / max) * span;
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(cx + Math.cos(ang) * (r - 10), cy + Math.sin(ang) * (r - 10));
    ctx.lineWidth = 4;
    ctx.strokeStyle = '#fff';
    ctx.stroke();
    // hub
    ctx.beginPath();
    ctx.arc(cx, cy, 4.5, 0, 2 * Math.PI);
    ctx.fillStyle = '#fff';
    ctx.fill();

    // PSI numeric text on face
    ctx.font = 'bold ' + Math.round(w * 0.06) + 'px Arial';
    ctx.fillStyle = '#e5e7eb';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(Math.round(value) + ' PSI', cx, cy + 34);
  };

  // Gauges
  var isoGauge        = new RoundGauge('isoGauge',        1600);
  var resinGauge      = new RoundGauge('resinGauge',      1600);
  var isoLowGauge     = new RoundGauge('isoLowGauge',      500);
  var resinLowGauge   = new RoundGauge('resinLowGauge',    500);
  var primaryAirGauge = new RoundGauge('primaryAirGauge',  200);
  var gunAirGauge     = new RoundGauge('gunAirGauge',      200);

  isoLowGauge.setPoint   = isoLowTarget;
  resinLowGauge.setPoint = resinLowTarget;

  // Enable inner temp rings on high-pressure and low-side gauges
  isoGauge.showTempRing      = true;
  resinGauge.showTempRing    = true;
  isoLowGauge.showTempRing   = true;
  resinLowGauge.showTempRing = true;

  function updateRatio(iso, resin) {
    var circle = document.getElementById('ratioCircle');
    if (iso < 50 && resin < 50) {
      circle.className = 'ratio-circle';
      return;
    }
    if (Math.abs(iso - resin) <= diff) {
      circle.className = 'ratio-circle ratio-good';
    } else {
      circle.className = 'ratio-circle ratio-bad';
    }
  }

  function updateModeButtons() {
    var drumBtn  = document.getElementById('drumAirBtn');
    var sprayBtn = document.getElementById('sprayBtn');

    // Drum air on/off
    if (drumAir) {
      drumBtn.classList.add('on');
    } else {
      drumBtn.classList.remove('on');
    }

    // Determine if spray *can* be turned on:
    //  - drumAir must be enabled
    //  - both low-side feeds must be above supplyLow
    //  - no interlock is latched
    var supplyOk = (lastIsoLow >= supplyLow && lastResinLow >= supplyLow);
    var canSpray = drumAir && supplyOk && !interlockActive;

    sprayBtn.classList.remove('on');
    sprayBtn.classList.remove('interlock');

    if (spray) {
      // Actively spraying: always green
      sprayBtn.classList.add('on');
    } else if (!canSpray) {
      // Cannot be turned on right now: show red styling
      sprayBtn.classList.add('interlock');
    }
  }


  function fmtTempF(v) {
    if (v === null || v === undefined) return '-- °F';
    var n = Number(v);
    if (!isFinite(n)) return '-- °F';
    return n.toFixed(1) + ' °F';
  }

  function fmtSetF(v) {
    if (v === null || v === undefined) return '-- °F';
    var n = Math.round(Number(v));
    if (!isFinite(n)) return '-- °F';
    return n + ' °F';
  }

  function normalizeHoseState(en, heat) {
    if (!en) return 'OFF';
    return heat ? 'HEATING!' : 'AT TEMP';
  }

  function updateHoseUI() {
    // Enable buttons
    var h1EnBtn = document.getElementById('hose1EnableBtn');
    var h2EnBtn = document.getElementById('hose2EnableBtn');

    if (h1EnBtn) {
      if (hose1En) h1EnBtn.classList.add('on'); else h1EnBtn.classList.remove('on');
      var sub = h1EnBtn.querySelector('.btn-sub');
      if (sub) sub.textContent = hose1En ? 'ON' : 'Standby!';
    }
    if (h2EnBtn) {
      if (hose2En) h2EnBtn.classList.add('on'); else h2EnBtn.classList.remove('on');
      var sub2 = h2EnBtn.querySelector('.btn-sub');
      if (sub2) sub2.textContent = hose2En ? 'ON' : 'Standby!';
    }

    // Status buttons
    var h1StatusBtn = document.getElementById('hose1StatusBtn');
    var h2StatusBtn = document.getElementById('hose2StatusBtn');
    if (h1StatusBtn) {
      // Green when actively heating, Blue when enabled and not heating ("At Temp")
      if (hose1En && hose1Heat) {
        h1StatusBtn.classList.add('heating');
        h1StatusBtn.classList.remove('attemp');
      } else {
        h1StatusBtn.classList.remove('heating');
        if (hose1En && isFinite(Number(hose1Temp))) h1StatusBtn.classList.add('attemp');
        else h1StatusBtn.classList.remove('attemp');
      }
      var t = document.getElementById('hose1TempDisplay');
      if (t) t.textContent = fmtTempF(hose1Temp);
      var s = document.getElementById('hose1StateDisplay');
      if (s) s.textContent = normalizeHoseState(hose1En, hose1Heat);
    }
    if (h2StatusBtn) {
      if (hose2En && hose2Heat) {
        h2StatusBtn.classList.add('heating');
        h2StatusBtn.classList.remove('attemp');
      } else {
        h2StatusBtn.classList.remove('heating');
        if (hose2En && isFinite(Number(hose2Temp))) h2StatusBtn.classList.add('attemp');
        else h2StatusBtn.classList.remove('attemp');
      }
      var t2 = document.getElementById('hose2TempDisplay');
      if (t2) t2.textContent = fmtTempF(hose2Temp);
      var s2 = document.getElementById('hose2StateDisplay');
      if (s2) s2.textContent = normalizeHoseState(hose2En, hose2Heat);
    }

    // Setpoint cards
    var sp1 = document.getElementById('hose1SetValue');
    var sp2 = document.getElementById('hose2SetValue');
    if (sp1) sp1.textContent = fmtSetF(hose1Set);
    if (sp2) sp2.textContent = fmtSetF(hose2Set);
  }

  function clampHoseSetF(v) {
    var n = Math.round(Number(v));
    if (!isFinite(n)) n = 120;
    // Keep within the same min/max bands we show on gauges
    if (n < tempMin) n = tempMin;
    if (n > tempMax) n = tempMax;
    return n;
  }

  function adjustHoseSet(which, delta) {
    var cur = (which === 1) ? hose1Set : hose2Set;
    if (cur === null || cur === undefined) cur = 120;
    var next = clampHoseSetF(Number(cur) + Number(delta));

    var payload = (which === 1) ? { hose1Set: next } : { hose2Set: next };

    // Optimistic update so UI feels responsive
    if (which === 1) hose1Set = next; else hose2Set = next;
    updateHoseUI();

    return sendControl(payload).then(function(body){
      if (which === 1 && body.hose1Set !== undefined) hose1Set = body.hose1Set;
      if (which === 2 && body.hose2Set !== undefined) hose2Set = body.hose2Set;
      updateHoseUI();
    }).catch(function(err){
      setStatus('Hose setpoint error: ' + err.message, true);
      throw err;
    });
  }

  function attachHold(btn, onTapDelta, onHoldDelta, which) {
    var holdT = null;
    var holdI = null;

    function clearTimers() {
      if (holdT) { clearTimeout(holdT); holdT = null; }
      if (holdI) { clearInterval(holdI); holdI = null; }
    }

    function start(e) {
      if (e) e.preventDefault();
      clearTimers();

      // Immediate single-step
      adjustHoseSet(which, onTapDelta);

      // After a short hold, begin repeating at 0.75s with 5-degree steps
      holdT = setTimeout(function(){
        holdI = setInterval(function(){
          adjustHoseSet(which, onHoldDelta);
        }, 750);
      }, 600);
    }

    function stop(e) {
      if (e) e.preventDefault();
      clearTimers();
    }

    btn.addEventListener('mousedown', start);
    btn.addEventListener('touchstart', start, { passive: false });

    btn.addEventListener('mouseup', stop);
    btn.addEventListener('mouseleave', stop);
    btn.addEventListener('touchend', stop);
    btn.addEventListener('touchcancel', stop);
  }



  function updateGauges(data) {
    var iso      = data.iso      || 0;
    var resin    = data.resin    || 0;
    var isoLow   = data.isoLow   || 0;
    var resinLow = data.resinLow || 0;
    var air      = data.airPiston || 0;
    var apAir    = data.apAir     || 0;

    if (data.isoTemp !== undefined)      isoTemp      = data.isoTemp;
    if (data.resinTemp !== undefined)    resinTemp    = data.resinTemp;
    if (data.isoLowTemp !== undefined)   isoLowTemp   = data.isoLowTemp;
    if (data.resinLowTemp !== undefined) resinLowTemp = data.resinLowTemp;
    if (typeof data.drumAir !== 'undefined') drumAir = !!data.drumAir;
    if (typeof data.spray   !== 'undefined') spray   = !!data.spray;

    // Hose heat fields (if present)
    if (typeof data.hose1En  !== 'undefined') hose1En  = !!data.hose1En;
    if (typeof data.hose2En  !== 'undefined') hose2En  = !!data.hose2En;
    if (typeof data.hose1Heat!== 'undefined') hose1Heat= !!data.hose1Heat;
    if (typeof data.hose2Heat!== 'undefined') hose2Heat= !!data.hose2Heat;

    if (data.hose1Temp !== undefined) hose1Temp = data.hose1Temp;
    if (data.hose2Temp !== undefined) hose2Temp = data.hose2Temp;
    if (data.hose1Set  !== undefined) hose1Set  = data.hose1Set;
    if (data.hose2Set  !== undefined) hose2Set  = data.hose2Set;
    if (data.hose1Tol  !== undefined) hose1Tol  = data.hose1Tol;
    if (data.hose2Tol  !== undefined) hose2Tol  = data.hose2Tol;

    lastIsoLow   = isoLow;
    lastResinLow = resinLow;

    // Interlock message from backend (only when key present)
    if (typeof data.interlock !== 'undefined') {
      if (data.interlock) {
        interlockActive = true;
        setStatus(data.interlock, true);
      } else {
        interlockActive = false;
        var bar = document.getElementById('alertBar');
        setStatus(bar.textContent || 'Ready', false);
      }
    }

    updateResetButton();

    isoGauge.setValue(iso);
    resinGauge.setValue(resin);
    isoLowGauge.setValue(isoLow);
    resinLowGauge.setValue(resinLow);
    primaryAirGauge.setValue(air);
    gunAirGauge.setValue(apAir);

    if (isoTemp !== null)        isoGauge.setTemp(isoTemp);
    if (resinTemp !== null)      resinGauge.setTemp(resinTemp);
    if (isoLowTemp !== null)     isoLowGauge.setTemp(isoLowTemp);
    if (resinLowTemp !== null)   resinLowGauge.setTemp(resinLowTemp);

    document.getElementById('isoValue').textContent        = iso.toFixed(0) + ' PSI';
    document.getElementById('resinValue').textContent      = resin.toFixed(0) + ' PSI';
    document.getElementById('isoLowValue').textContent     = isoLow.toFixed(0) + ' PSI';
    document.getElementById('resinLowValue').textContent   = resinLow.toFixed(0) + ' PSI';
    document.getElementById('primaryAirValue').textContent = air.toFixed(0) + ' PSI';
    document.getElementById('gunAirValue').textContent     = apAir.toFixed(0) + ' PSI';

    // Temperature readouts
    var isoTempEl       = document.getElementById('isoTempValue');
    var resinTempEl     = document.getElementById('resinTempValue');
    var isoLowTempEl    = document.getElementById('isoLowTempValue');
    var resinLowTempEl  = document.getElementById('resinLowTempValue');

    if (isoTempEl) {
      isoTempEl.textContent =
        (isoTemp !== null && isoTemp !== undefined)
          ? isoTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (resinTempEl) {
      resinTempEl.textContent =
        (resinTemp !== null && resinTemp !== undefined)
          ? resinTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (isoLowTempEl) {
      isoLowTempEl.textContent =
        (isoLowTemp !== null && isoLowTemp !== undefined)
          ? isoLowTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (resinLowTempEl) {
      resinLowTempEl.textContent =
        (resinLowTemp !== null && resinLowTemp !== undefined)
          ? resinLowTemp.toFixed(1) + ' °F'
          : '-- °F';
    }

    updateRatio(iso, resin);
    updateModeButtons();
    updateHoseUI();

    var now = new Date();
    document.getElementById('lastUpdate').textContent =
      now.toLocaleTimeString([], { hour12: false });
  }

  function setLiveStatus(ok) {
    var text = document.getElementById('liveStatusText');
    var dot  = document.getElementById('liveDot');
    if (ok) {
      text.textContent = 'Live';
      dot.style.backgroundColor = '#22c55e';
      dot.style.boxShadow = '0 0 8px rgba(34,197,94,0.8)';
    } else {
      text.textContent = 'Offline';
      dot.style.backgroundColor = '#ef4444';
      dot.style.boxShadow = '0 0 8px rgba(239,68,68,0.8)';
    }
  }

  function sendControl(payload) {
    return fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(function(r){
      return r.json().then(function(body){
        if (!r.ok || body.ok === false) {
          throw new Error(body.error || ('HTTP ' + r.status));
        }
        if (typeof body.drumAir !== 'undefined') drumAir = !!body.drumAir;
        if (typeof body.spray   !== 'undefined') spray   = !!body.spray;

        if (typeof body.hose1En  !== 'undefined') hose1En  = !!body.hose1En;
        if (typeof body.hose2En  !== 'undefined') hose2En  = !!body.hose2En;
        if (typeof body.hose1Heat!== 'undefined') hose1Heat= !!body.hose1Heat;
        if (typeof body.hose2Heat!== 'undefined') hose2Heat= !!body.hose2Heat;
        if (body.hose1Temp !== undefined) hose1Temp = body.hose1Temp;
        if (body.hose2Temp !== undefined) hose2Temp = body.hose2Temp;
        if (body.hose1Set  !== undefined) hose1Set  = body.hose1Set;
        if (body.hose2Set  !== undefined) hose2Set  = body.hose2Set;
        if (body.hose1Tol  !== undefined) hose1Tol  = body.hose1Tol;
        if (body.hose2Tol  !== undefined) hose2Tol  = body.hose2Tol;

        if (typeof body.interlock !== 'undefined' && body.interlock) {
          // Interlock still active
          interlockActive = true;
          setStatus(body.interlock, true);
        } else {
          // Successful command with no interlock -> clear the red state
          interlockActive = false;
        }

        updateModeButtons();
        updateHoseUI();
        return body;
      });
    });
  }

  document.getElementById('drumAirBtn').addEventListener('click', function(){
    var desired = !drumAir;

    sendControl({ drumAir: desired }).then(function(body){
      var msg = desired ? 'Drum air enabled' : 'Drum air disabled';
      setStatus(msg, false);
    }).catch(function(err){
      console.log('DrumAir control failed:', err);
      setStatus('Drum air error: ' + err.message, true);
    });
  });

  document.getElementById('sprayBtn').addEventListener('click', function(){
    var desired = !spray;

    sendControl({ spray: desired }).then(function(body){
      var msg = desired ? 'Spray enabled' : 'Spray disabled';
      setStatus(msg, false);
    }).catch(function(err){
      console.log('Spray control failed:', err);
      setStatus('Spray error: ' + err.message, true);
    });
  });

  
  // Hose heat enable controls
  var hose1EnableBtn = document.getElementById('hose1EnableBtn');
  var hose2EnableBtn = document.getElementById('hose2EnableBtn');

  if (hose1EnableBtn) {
    hose1EnableBtn.addEventListener('click', function(){
      var desired = !hose1En;
      // optimistic
      hose1En = desired;
      updateHoseUI();

      sendControl({ hose1En: desired }).then(function(){
        setStatus(desired ? 'Hose 1 enabled' : 'Hose 1 standby', false);
      }).catch(function(err){
        setStatus('Hose 1 enable error: ' + err.message, true);
      });
    });
  }

  if (hose2EnableBtn) {
    hose2EnableBtn.addEventListener('click', function(){
      var desired = !hose2En;
      // optimistic
      hose2En = desired;
      updateHoseUI();

      sendControl({ hose2En: desired }).then(function(){
        setStatus(desired ? 'Hose 2 enabled' : 'Hose 2 standby', false);
      }).catch(function(err){
        setStatus('Hose 2 enable error: ' + err.message, true);
      });
    });
  }

  // Hose heat setpoint (press: 1°F, hold: 5°F every 0.75s)
  var hose1Up = document.getElementById('hose1SetUpBtn');
  var hose1Dn = document.getElementById('hose1SetDownBtn');
  var hose2Up = document.getElementById('hose2SetUpBtn');
  var hose2Dn = document.getElementById('hose2SetDownBtn');

  if (hose1Up) attachHold(hose1Up, +1, +5, 1);
  if (hose1Dn) attachHold(hose1Dn, -1, -5, 1);
  if (hose2Up) attachHold(hose2Up, +1, +5, 2);
  if (hose2Dn) attachHold(hose2Dn, -1, -5, 2);


  // Shared WebSocket instance with auto-reconnect
  var ws = null;

  function connectWS() {
    // Avoid creating multiple sockets if one is already open/connecting
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      return;
    }

    var scheme = (location.protocol === 'https:') ? 'wss://' : 'ws://';
    ws = new WebSocket(scheme + location.hostname + ':81/');

    ws.onopen = function () {
      console.log('WS connected');
      setLiveStatus(true);
    };

    ws.onmessage = function (ev) {
      try {
        var d = JSON.parse(ev.data);
        updateGauges(d);
      } catch (e) {
        console.log('Bad WS payload', e);
      }
    };

    ws.onclose = function () {
      console.log('WS closed, scheduling reconnect');
      setLiveStatus(false);
      ws = null;
      setTimeout(connectWS, 1500);
    };

    ws.onerror = function (err) {
      console.log('WS error', err);
      setLiveStatus(false);
      try { ws.close(); } catch (e) {}
    };
  }

  // Initial fetch of relay state
  fetch('/api/control').then(function(r){ return r.json(); }).then(function(data){
    if (typeof data.drumAir !== 'undefined') drumAir = !!data.drumAir;
    if (typeof data.spray   !== 'undefined') spray   = !!data.spray;

    if (typeof data.hose1En  !== 'undefined') hose1En  = !!data.hose1En;
    if (typeof data.hose2En  !== 'undefined') hose2En  = !!data.hose2En;
    if (typeof data.hose1Heat!== 'undefined') hose1Heat= !!data.hose1Heat;
    if (typeof data.hose2Heat!== 'undefined') hose2Heat= !!data.hose2Heat;
    if (data.hose1Temp !== undefined) hose1Temp = data.hose1Temp;
    if (data.hose2Temp !== undefined) hose2Temp = data.hose2Temp;
    if (data.hose1Set  !== undefined) hose1Set  = data.hose1Set;
    if (data.hose2Set  !== undefined) hose2Set  = data.hose2Set;
    if (data.hose1Tol  !== undefined) hose1Tol  = data.hose1Tol;
    if (data.hose2Tol  !== undefined) hose2Tol  = data.hose2Tol;

    updateModeButtons();
    updateHoseUI();
  }).catch(function(e){
    console.log('Failed to load relay state:', e);
  });

  window.addEventListener('load', connectWS);
</script>
</body>
</html>
)rawliteral";

// ---------- SETTINGS PAGE ----------

const char* settingsPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Spray Foam Settings</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #020617;
      --panel: #020617;
      --panel-border: #1e293b;
      --accent: #38bdf8;
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
    }
    * { box-sizing: border-box; -webkit-font-smoothing: antialiased; }
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      min-height: 100vh;
    }
    .app {
      max-width: 960px;
      margin: 0 auto;
      padding: 0.75rem 1.25rem 1.5rem;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 0.75rem;
    }
    h1 {
      margin: 0;
      font-size: 1.35rem;
    }
    .subtitle {
      font-size: 0.8rem;
      color: var(--text-muted);
    }
    .nav-buttons {
      display:flex;
      gap:0.4rem;
    }
    .nav-btn {
      border-radius: 999px;
      padding: 0.4rem 0.9rem;
      border: 1px solid rgba(148,163,184,0.6);
      background: rgba(15,23,42,0.95);
      color: var(--text-main);
      text-decoration: none;
      font-size: 0.85rem;
    }
    form {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 0.9rem;
    }
    fieldset {
      border-radius: 16px;
      border: 1px solid var(--panel-border);
      background: var(--panel);
      padding: 0.9rem 1rem 1rem;
      min-width: 0;
    }
    legend {
      font-size: 0.85rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      color: var(--text-muted);
      padding: 0 0.3rem;
    }
    .field {
      display: flex;
      flex-direction: column;
      margin-bottom: 0.6rem;
    }
    label {
      font-size: 0.8rem;
      color: var(--text-muted);
      margin-bottom: 0.15rem;
    }
    input[type="number"], input[type="text"], input[type="password"], select {
      padding: 0.35rem 0.5rem;
      border-radius: 8px;
      border: 1px solid #4b5563;
      background: #020617;
      color: var(--text-main);
      font-size: 0.85rem;
    }
    .hint {
      font-size: 0.7rem;
      color: var(--text-muted);
      margin-top: 0.15rem;
    }
    .full-width {
      grid-column: 1 / -1;
    }
    .save-bar {
      grid-column: 1 / -1;
      margin-top: 0.5rem;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 0.75rem;
    }
    .save-btn {
      flex: 0 0 auto;
      padding: 0.55rem 1.4rem;
      border-radius: 999px;
      border: none;
      background: linear-gradient(135deg, #38bdf8, #0ea5e9);
      color: #020617;
      font-weight: 600;
      font-size: 0.9rem;
    }
    .save-btn:active { transform: translateY(1px); }
    #saveStatus {
      font-size: 0.8rem;
      color: var(--text-muted);
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.75rem;
      margin-top: 0.4rem;
    }
    th, td {
      border: 1px solid #1f2937;
      padding: 4px 6px;
      text-align: left;
    }
    .ok { color: #22c55e; font-weight: 600; }
    .warn { color: #facc15; font-weight: 600; }
    .bad { color: #ef4444; font-weight: 600; }
    @media (max-width: 800px) {
      form { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
<div class="app">
  <header>
    <div>
      <h1>System Settings</h1>
      <div class="subtitle">
        Pressure ranges, ratio thresholds, temps &amp; calibration
        • Firmware <strong>{{FW_VERSION}}</strong>
      </div>
    </div>
    <div class="nav-buttons">
      <a class="nav-btn" href="/update">OTA Update</a>
      <a class="nav-btn" href="/">⬅ Back to Live</a>
    </div>
  </header>
  <form id="settingsForm">

    <fieldset>
      <legend>High-Pressure &amp; Ratio</legend>
      <div class="field">
        <label for="targetInput">Target Pressure (PSI)</label>
        <input type="number" id="targetInput" name="target" min="0" max="1600" />
        <div class="hint">Nominal operating pressure for Iso/Resin.</div>
      </div>
      <div class="field">
        <label for="marginInput">Margin (%)</label>
        <input type="number" id="marginInput" name="margin" min="0" max="100" />
        <div class="hint">Green band width around target (used for PSI &amp; temp bands).</div>
      </div>
      <div class="field">
        <label for="diffInput">Max Iso/Resin Difference (PSI)</label>
        <input type="number" id="diffInput" name="diff" min="0" max="1600" />
        <div class="hint">Used for ratio indicator and quick checks.</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Air &amp; Low-Side Pressure</legend>
      <div class="field">
        <label for="airTargetInput">Air Piston Set Point (PSI)</label>
        <input type="number" id="airTargetInput" name="airTarget" min="0" max="200" />
        <div class="hint">Target pressure for the Primary Air gauge (0–200&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="gunTargetInput">Gun Air Set Point (PSI)</label>
        <input type="number" id="gunTargetInput" name="gunTarget" min="0" max="200" />
        <div class="hint">Target pressure for the Gun AP gauge (0–200&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="isoLowTargetInput">Iso Low Set Point (PSI)</label>
        <input type="number" id="isoLowTargetInput" name="isoLowTarget" min="0" max="500" />
        <div class="hint">Target for low-pressure Iso feed (0–500&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="resinLowTargetInput">Resin Low Set Point (PSI)</label>
        <input type="number" id="resinLowTargetInput" name="resinLowTarget" min="0" max="500" />
        <div class="hint">Target for low-pressure Resin feed (0–500&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="supplyLowInput">Supply Low Pressure (PSI)</label>
        <input type="number" id="supplyLowInput" name="supplyLow" min="0" max="500" />
        <div class="hint">
          Minimum low-side pressure (either Iso/Resin). Below this in Spray mode will park and cut drum air;
          must be above this to enter Spray.
        </div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Temperature Targets</legend>
      <div class="field">
        <label for="isoTempTargetInput">Iso HP Temp Target (°F)</label>
        <input type="number" id="isoTempTargetInput" name="isoTempTarget" min="-40" max="300" />
        <div class="hint">Target Iso outlet temp; green band shown on HP Iso temp ring.</div>
      </div>
      <div class="field">
        <label for="resinTempTargetInput">Resin HP Temp Target (°F)</label>
        <input type="number" id="resinTempTargetInput" name="resinTempTarget" min="-40" max="300" />
        <div class="hint">Target Resin outlet temp; green band shown on HP Resin temp ring.</div>
      </div>
      <div class="field">
        <label for="isoLowTempTargetInput">Iso Low Temp Target (°F)</label>
        <input type="number" id="isoLowTempTargetInput" name="isoLowTempTarget" min="-40" max="300" />
        <div class="hint">Target temp for low-side Iso feed; drives green band on Iso Low gauge.</div>
      </div>
      <div class="field">
        <label for="resinLowTempTargetInput">Resin Low Temp Target (°F)</label>
        <input type="number" id="resinLowTempTargetInput" name="resinLowTempTarget" min="-40" max="300" />
        <div class="hint">Target temp for low-side Resin feed; drives green band on Resin Low gauge.</div>
      </div>
      <div class="field">
        <label for="tempMinInput">Temp Gauge Minimum (°F)</label>
        <input type="number" id="tempMinInput" name="tempMinF" min="-40" max="300" />
        <div class="hint">Lower bound for the temp ring (all four sensors share this scale).</div>
      </div>
      <div class="field">
        <label for="tempMaxInput">Temp Gauge Maximum (°F)</label>
        <input type="number" id="tempMaxInput" name="tempMaxF" min="-40" max="300" />
        <div class="hint">Upper bound for the temp ring (all four sensors share this scale).</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Hose Heat</legend>
      <div class="field">
        <label for="hose1SetInput">Hose Heat 1 Setpoint (°F)</label>
        <input type="number" id="hose1SetInput" name="hose1Set" min="-40" max="300" />
        <div class="hint">Target hose temperature for section 1.</div>
      </div>
      <div class="field">
        <label for="hose1TolInput">Hose Heat 1 Swing (°F)</label>
        <input type="number" id="hose1TolInput" name="hose1Tol" min="1" max="30" />
        <div class="hint">Heater turns ON at setpoint and OFF at (setpoint + swing).</div>
      </div>
      <div class="field">
        <label for="hose2SetInput">Hose Heat 2 Setpoint (°F)</label>
        <input type="number" id="hose2SetInput" name="hose2Set" min="-40" max="300" />
        <div class="hint">Target hose temperature for section 2.</div>
      </div>
      <div class="field">
        <label for="hose2TolInput">Hose Heat 2 Swing (°F)</label>
        <input type="number" id="hose2TolInput" name="hose2Tol" min="1" max="30" />
        <div class="hint">Heater turns ON at setpoint and OFF at (setpoint + swing).</div>
      </div>
    
      <div class="field">
        <label for="hoseOvertempInput">Hose Overtemp Cutoff (°F)</label>
        <input type="number" id="hoseOvertempInput" name="hoseOvertempF" min="1" max="50" />
        <div class="hint">Safety interlock: if a hose temp exceeds (setpoint + cutoff), all hose heaters shut down and the system is parked.</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Temperature Sensors</legend>
      <div class="field">
        <label for="isoTempSensorSelect">Iso HP Temp Sensor</label>
        <select id="isoTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="isoTempReading">--</span> °F
          <br/>Assign a DS18B20 to the Iso HP outlet.
        </div>
      </div>
      <div class="field">
        <label for="resinTempSensorSelect">Resin HP Temp Sensor</label>
        <select id="resinTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="resinTempReading">--</span> °F
          <br/>Assign a DS18B20 to the Resin HP outlet.
        </div>
      </div>
      <div class="field">
        <label for="isoLowTempSensorSelect">Iso Low Temp Sensor</label>
        <select id="isoLowTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="isoLowTempReading">--</span> °F
          <br/>Assign a DS18B20 to the low-side Iso feed.
        </div>
      </div>
      <div class="field">
        <label for="resinLowTempSensorSelect">Resin Low Temp Sensor</label>
        <select id="resinLowTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="resinLowTempReading">--</span> °F
          <br/>Assign a DS18B20 to the low-side Resin feed.
        </div>
      </div>
      <div class="field">
        <label for="hose1TempSensorSelect">Hose 1 Temp Sensor</label>
        <select id="hose1TempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="hose1TempReading">--</span> °F
          <br/>Assign a DS18B20 to Hose Heat Section 1.
        </div>
      </div>
      <div class="field">
        <label for="hose2TempSensorSelect">Hose 2 Temp Sensor</label>
        <select id="hose2TempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="hose2TempReading">--</span> °F
          <br/>Assign a DS18B20 to Hose Heat Section 2.
        </div>
      </div>
    </fieldset>


    <fieldset>
      <legend>Calibration Controls</legend>
      <div class="field">
        <label for="sensorSelect">Sensor</label>
        <select id="sensorSelect">
          <option value="iso">Iso (0–1600)</option>
          <option value="resin">Resin (0–1600)</option>
          <option value="isoLow">Iso Low (0–500)</option>
          <option value="resinLow">Resin Low (0–500)</option>
          <option value="air">Air Piston (0–300)</option>
          <option value="apAir">Gun AP Air (0–300)</option>
        </select>
      </div>
      <div class="field">
        <label>Zero at 0 PSI</label>
        <button type="button" id="zeroBtn">Zero Selected</button>
        <button type="button" id="zeroAllBtn">Zero All</button>
        <div class="hint">Put all lines at 0 PSI before using these.</div>
      </div>
      <div class="field">
        <label for="spanPressureInput">Known Pressure (PSI) for Span</label>
        <input type="number" id="spanPressureInput" min="0" max="2000" step="1" />
        <button type="button" id="spanBtn" style="margin-top:0.25rem;">Set Span</button>
        <div class="hint">Pressurize to a known value (e.g. 100 or 1000 PSI) then set span.</div>
      </div>
      <div id="calibStatus" class="hint"></div>
    </fieldset>

    <fieldset class="full-width">
      <legend>Network &amp; OTA</legend>
      <div class="field">
        <label for="wifiModeSelect">Network Mode</label>
        <select id="wifiModeSelect" name="wifiMode">
          <option value="0">Access Point (AP)</option>
          <option value="1">WiFi Client (STA)</option>
        </select>
        <div class="hint">
          AP: rig creates its own WiFi network.<br/>
          STA: rig joins an existing WiFi network. Changes take effect after reboot.
        </div>
      </div>
      <div class="field">
        <label for="apSsidInput">AP SSID</label>
        <input type="text" id="apSsidInput" />
        <div class="hint">SSID for the rig's hotspot.</div>
      </div>
      <div class="field">
        <label for="apPassInput">AP Password</label>
        <input type="password" id="apPassInput" />
        <div class="hint">Minimum 8 characters (WPA2).</div>
      </div>

      <div class="field">
        <label for="staSsidInput">WiFi (STA) SSID</label>
        <input type="text" id="staSsidInput" />
        <div class="hint">Existing WiFi network the rig should join.</div>
      </div>
      <div class="field">
        <label for="staPassInput">WiFi (STA) Password</label>
        <input type="password" id="staPassInput" />
        <div class="hint">WPA2 password for that network.</div>
      </div>

      <div class="hint">
        Tip: keep AP enabled until you're sure STA connects. You can always fall back by power-cycling and switching modes.
      </div>
    </fieldset>

    <fieldset class="full-width">
      <legend>Calibration Status</legend>
      <table>
        <thead>
          <tr><th>Sensor</th><th>R0 (raw @0)</th><th>K (scale)</th><th>Note</th></tr>
        </thead>
        <tbody id="statusTable"></tbody>
      </table>
    </fieldset>

    <div class="save-bar">
      <button type="submit" class="save-btn">Save &amp; Return to Live</button>
      <div id="saveStatus">Settings not saved yet.</div>
    </div>
  </form>
</div>
<script>
  var tempSensorMap = {};

  function updateTempReadouts() {
    var isoSel       = document.getElementById('isoTempSensorSelect');
    var resinSel     = document.getElementById('resinTempSensorSelect');
    var isoLowSel    = document.getElementById('isoLowTempSensorSelect');
    var resinLowSel  = document.getElementById('resinLowTempSensorSelect');
    var hose1Sel     = document.getElementById('hose1TempSensorSelect');
    var hose2Sel     = document.getElementById('hose2TempSensorSelect');

    var isoSpan      = document.getElementById('isoTempReading');
    var resinSpan    = document.getElementById('resinTempReading');
    var isoLowSpan   = document.getElementById('isoLowTempReading');
    var resinLowSpan = document.getElementById('resinLowTempReading');
    var hose1Span    = document.getElementById('hose1TempReading');
    var hose2Span    = document.getElementById('hose2TempReading');

    var isoId       = isoSel.value;
    var resinId     = resinSel.value;
    var isoLowId    = isoLowSel.value;
    var resinLowId  = resinLowSel.value;
    var hose1Id     = hose1Sel.value;
    var hose2Id     = hose2Sel.value;

    if (isoId && tempSensorMap.hasOwnProperty(isoId) && tempSensorMap[isoId] != null) {
      isoSpan.textContent = tempSensorMap[isoId].toFixed(1);
    } else {
      isoSpan.textContent = '--';
    }

    if (resinId && tempSensorMap.hasOwnProperty(resinId) && tempSensorMap[resinId] != null) {
      resinSpan.textContent = tempSensorMap[resinId].toFixed(1);
    } else {
      resinSpan.textContent = '--';
    }

    if (isoLowId && tempSensorMap.hasOwnProperty(isoLowId) && tempSensorMap[isoLowId] != null) {
      isoLowSpan.textContent = tempSensorMap[isoLowId].toFixed(1);
    } else {
      isoLowSpan.textContent = '--';
    }

    if (resinLowId && tempSensorMap.hasOwnProperty(resinLowId) && tempSensorMap[resinLowId] != null) {
      resinLowSpan.textContent = tempSensorMap[resinLowId].toFixed(1);
    } else {
      resinLowSpan.textContent = '--';
    }

    if (hose1Id && tempSensorMap.hasOwnProperty(hose1Id) && tempSensorMap[hose1Id] != null) {
      hose1Span.textContent = tempSensorMap[hose1Id].toFixed(1);
    } else {
      hose1Span.textContent = '--';
    }

    if (hose2Id && tempSensorMap.hasOwnProperty(hose2Id) && tempSensorMap[hose2Id] != null) {
      hose2Span.textContent = tempSensorMap[hose2Id].toFixed(1);
    } else {
      hose2Span.textContent = '--';
    }
  }

  // Load existing settings
  fetch('/api/settings').then(function(r){return r.json();}).then(function(data){
    document.getElementById('targetInput').value = data.target;
    document.getElementById('marginInput').value = data.margin;
    document.getElementById('diffInput').value   = data.diff;
    if (data.airTarget !== undefined) {
      document.getElementById('airTargetInput').value = data.airTarget;
    }
    if (data.gunTarget !== undefined) {
      document.getElementById('gunTargetInput').value = data.gunTarget;
    }
    if (data.isoLowTarget !== undefined) {
      document.getElementById('isoLowTargetInput').value = data.isoLowTarget;
    }
    if (data.resinLowTarget !== undefined) {
      document.getElementById('resinLowTargetInput').value = data.resinLowTarget;
    }
    if (data.supplyLow !== undefined) {
      document.getElementById('supplyLowInput').value = data.supplyLow;
    }
    if (data.isoTempTarget !== undefined) {
      document.getElementById('isoTempTargetInput').value = data.isoTempTarget;
    }
    if (data.resinTempTarget !== undefined) {
      document.getElementById('resinTempTargetInput').value = data.resinTempTarget;
    }
    if (data.isoLowTempTarget !== undefined) {
      document.getElementById('isoLowTempTargetInput').value = data.isoLowTempTarget;
    }
    if (data.resinLowTempTarget !== undefined) {
      document.getElementById('resinLowTempTargetInput').value = data.resinLowTempTarget;
    }
    if (data.tempMinF !== undefined) {
      document.getElementById('tempMinInput').value = data.tempMinF;
    }
    if (data.tempMaxF !== undefined) {
      document.getElementById('tempMaxInput').value = data.tempMaxF;
    }

    // Hose heat controls (setpoint + swing)
    if (data.hose1Set !== undefined) {
      document.getElementById('hose1SetInput').value = data.hose1Set;
    }
    if (data.hose2Set !== undefined) {
      document.getElementById('hose2SetInput').value = data.hose2Set;
    }
    if (data.hose1Tol !== undefined) {
      document.getElementById('hose1TolInput').value = data.hose1Tol;
    }
    if (data.hose2Tol !== undefined) {
      document.getElementById('hose2TolInput').value = data.hose2Tol;
    }

    if (data.hoseOvertempF !== undefined) {
      document.getElementById('hoseOvertempInput').value = data.hoseOvertempF;
    }

    if (data.wifiMode !== undefined) {
      document.getElementById('wifiModeSelect').value = data.wifiMode;
    }
    if (data.apSsid !== undefined) {
      document.getElementById('apSsidInput').value = data.apSsid;
    }
    if (data.apPass !== undefined) {
      document.getElementById('apPassInput').value = data.apPass;
    }
    if (data.staSsid !== undefined) {
      document.getElementById('staSsidInput').value = data.staSsid;
    }
    if (data.staPass !== undefined) {
      document.getElementById('staPassInput').value = data.staPass;
    }
  }).catch(function(e){
    document.getElementById('saveStatus').textContent = 'Failed to load settings: ' + e;
  });

  // Load temp sensors and assignments (initial only)
  function refreshTempSensors(initial) {
    var isoSel       = document.getElementById('isoTempSensorSelect');
    var resinSel     = document.getElementById('resinTempSensorSelect');
    var isoLowSel    = document.getElementById('isoLowTempSensorSelect');
    var resinLowSel  = document.getElementById('resinLowTempSensorSelect');
    var hose1Sel     = document.getElementById('hose1TempSensorSelect');
    var hose2Sel     = document.getElementById('hose2TempSensorSelect');

    fetch('/api/temp-sensors')
      .then(function(r){ return r.json(); })
      .then(function(data){
        tempSensorMap = {};

        var sensors = data.sensors || [];
        sensors.forEach(function(s){
          if (s.id) {
            tempSensorMap[s.id] = (typeof s.tempF === 'number') ? s.tempF : null;
          }
        });

        if (initial) {
          // Build select options once from current sensor list
          isoSel.innerHTML      = '';
          resinSel.innerHTML    = '';
          isoLowSel.innerHTML   = '';
          resinLowSel.innerHTML = '';
          hose1Sel.innerHTML    = '';
          hose2Sel.innerHTML    = '';

          var optNone1 = document.createElement('option');
          optNone1.value = '';
          optNone1.textContent = 'Unassigned';
          isoSel.appendChild(optNone1);

          var optNone2 = document.createElement('option');
          optNone2.value = '';
          optNone2.textContent = 'Unassigned';
          resinSel.appendChild(optNone2);

          var optNone3 = document.createElement('option');
          optNone3.value = '';
          optNone3.textContent = 'Unassigned';
          isoLowSel.appendChild(optNone3);

          var optNone4 = document.createElement('option');
          optNone4.value = '';
          optNone4.textContent = 'Unassigned';
          resinLowSel.appendChild(optNone4);

          var optNone5 = document.createElement('option');
          optNone5.value = '';
          optNone5.textContent = 'Unassigned';
          hose1Sel.appendChild(optNone5);

          var optNone6 = document.createElement('option');
          optNone6.value = '';
          optNone6.textContent = 'Unassigned';
          hose2Sel.appendChild(optNone6);

          sensors.forEach(function(s, idx){
            var label = 'Sensor ' + idx + ' (' + s.id + ')';
            if (typeof s.tempF === 'number') {
              label += ' – ' + s.tempF.toFixed(1) + ' °F';
            } else {
              label += ' – n/a';
            }

            [isoSel, resinSel, isoLowSel, resinLowSel, hose1Sel, hose2Sel].forEach(function(sel){
              var opt = document.createElement('option');
              opt.value = s.id;
              opt.textContent = label;
              sel.appendChild(opt);
            });
          });

          // Apply stored assignments from backend
          if (data.iso !== undefined && data.iso !== null) {
            isoSel.value = data.iso;
          }
          if (data.resin !== undefined && data.resin !== null) {
            resinSel.value = data.resin;
          }
          if (data.isoLow !== undefined && data.isoLow !== null) {
            isoLowSel.value = data.isoLow;
          }
          if (data.resinLow !== undefined && data.resinLow !== null) {
            resinLowSel.value = data.resinLow;
          }
          if (data.hose1 !== undefined && data.hose1 !== null) {
            hose1Sel.value = data.hose1;
          }
          if (data.hose2 !== undefined && data.hose2 !== null) {
            hose2Sel.value = data.hose2;
          }
        }

        // Always update readouts from latest temps
        updateTempReadouts();
      })
      .catch(function(e){
        console.log('Failed to load temp sensors:', e);
      });
  }

  refreshTempSensors(true);

  document.getElementById('isoTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('resinTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('isoLowTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('resinLowTempSensorSelect')
    .addEventListener('change', updateTempReadouts);


  document.getElementById('hose1TempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('hose2TempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  // Periodically refresh only the temperature values, not the assignments/options
  setInterval(function(){
    refreshTempSensors(false);
  }, 2000);

  document.getElementById('settingsForm').addEventListener('submit', function(e){
    e.preventDefault();
    var target = parseInt(document.getElementById('targetInput').value || '0', 10);
    var margin = parseInt(document.getElementById('marginInput').value || '0', 10);
    var diff   = parseInt(document.getElementById('diffInput').value   || '0', 10);

    var airTargetVal      = parseInt(document.getElementById('airTargetInput').value      || '0', 10);
    var gunTargetVal      = parseInt(document.getElementById('gunTargetInput').value      || '0', 10);
    var isoLowTargetVal   = parseInt(document.getElementById('isoLowTargetInput').value   || '0', 10);
    var resinLowTargetVal = parseInt(document.getElementById('resinLowTargetInput').value || '0', 10);
    var supplyLowVal      = parseInt(document.getElementById('supplyLowInput').value      || '0', 10);

    var isoTempTargetVal      = parseInt(document.getElementById('isoTempTargetInput').value      || '0', 10);
    var resinTempTargetVal    = parseInt(document.getElementById('resinTempTargetInput').value    || '0', 10);
    var isoLowTempTargetVal   = parseInt(document.getElementById('isoLowTempTargetInput').value   || '0', 10);
    var resinLowTempTargetVal = parseInt(document.getElementById('resinLowTempTargetInput').value || '0', 10);
    var tempMinVal            = parseInt(document.getElementById('tempMinInput').value            || '0', 10);
    var tempMaxVal            = parseInt(document.getElementById('tempMaxInput').value            || '0', 10);

    var hose1SetVal = parseInt(document.getElementById('hose1SetInput').value || '0', 10);
    var hose2SetVal = parseInt(document.getElementById('hose2SetInput').value || '0', 10);
    var hose1TolVal = parseInt(document.getElementById('hose1TolInput').value || '0', 10);
    var hose2TolVal = parseInt(document.getElementById('hose2TolInput').value || '0', 10);
    var hoseOvertempVal = parseInt(document.getElementById('hoseOvertempInput').value || '0', 10);

    var wifiModeVal = parseInt(document.getElementById('wifiModeSelect').value || '0', 10);
    var apSsidVal   = document.getElementById('apSsidInput').value || '';
    var apPassVal   = document.getElementById('apPassInput').value || '';
    var staSsidVal  = document.getElementById('staSsidInput').value || '';
    var staPassVal  = document.getElementById('staPassInput').value || '';

    var isoTempId       = document.getElementById('isoTempSensorSelect').value;
    var resinTempId     = document.getElementById('resinTempSensorSelect').value;
    var isoLowTempId    = document.getElementById('isoLowTempSensorSelect').value;
    var resinLowTempId  = document.getElementById('resinLowTempSensorSelect').value;
    var hose1TempId     = document.getElementById('hose1TempSensorSelect').value;
    var hose2TempId     = document.getElementById('hose2TempSensorSelect').value;

    var settingsPayload = {
      target: target,
      margin: margin,
      diff: diff,
      airTarget: airTargetVal,
      gunTarget: gunTargetVal,
      isoLowTarget: isoLowTargetVal,
      resinLowTarget: resinLowTargetVal,
      supplyLow: supplyLowVal,
      isoTempTarget: isoTempTargetVal,
      resinTempTarget: resinTempTargetVal,
      isoLowTempTarget: isoLowTempTargetVal,
      resinLowTempTarget: resinLowTempTargetVal,
      tempMinF: tempMinVal,
      tempMaxF: tempMaxVal,
      hose1Set: hose1SetVal,
      hose2Set: hose2SetVal,
      hose1Tol: hose1TolVal,
      hose2Tol: hose2TolVal,
      hoseOvertempF: hoseOvertempVal,
      wifiMode: wifiModeVal,
      apSsid: apSsidVal,
      apPass: apPassVal,
      staSsid: staSsidVal,
      staPass: staPassVal
    };

    var tempPayload = {
      iso:      isoTempId,
      resin:    resinTempId,
      isoLow:   isoLowTempId,
      resinLow: resinLowTempId,
      hose1:    hose1TempId,
      hose2:    hose2TempId
    };

    Promise.all([
      fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(settingsPayload)
      }),
      fetch('/api/temp-sensors', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(tempPayload)
      })
    ]).then(function(responses){
      if (!responses[0].ok || !responses[1].ok) {
        throw new Error('HTTP ' + responses[0].status + '/' + responses[1].status);
      }
      document.getElementById('saveStatus').textContent =
        'Settings saved. Network changes apply after reboot. Returning to live view…';
      setTimeout(function(){ window.location.href = '/'; }, 900);
    }).catch(function(err){
      document.getElementById('saveStatus').textContent = 'Save failed: ' + err.message;
    });
  });

  function setCalibStatus(msg){ document.getElementById('calibStatus').textContent = msg; }

  document.getElementById('zeroBtn').onclick = function(){
    var sensor = document.getElementById('sensorSelect').value;
    setCalibStatus('Zeroing ' + sensor + '…');
    fetch('/calibration', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sensor: sensor, action: 'zero' })
    }).then(function(r){ return r.json(); }).then(function(d){
      if(d.ok){
        setCalibStatus('Zero set for ' + sensor + ' (raw0=' + d.raw0.toFixed(2) + ').');
        refreshStatus();
      } else {
        setCalibStatus('Error: ' + (d.error || 'unknown'));
      }
    }).catch(function(e){ setCalibStatus('Zero failed: ' + e); });
  };

  // New: Zero all six sensors in one go
  document.getElementById('zeroAllBtn').onclick = function(){
    var order = ['iso','resin','isoLow','resinLow','air','apAir'];
    var index = 0;

    function zeroNext() {
      if (index >= order.length) {
        setCalibStatus('Zeroed all sensors.');
        refreshStatus();
        return;
      }
      var s = order[index++];
      setCalibStatus('Zeroing ' + s + ' (' + index + '/' + order.length + ')…');
      fetch('/calibration', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ sensor: s, action: 'zero' })
      }).then(function(r){ return r.json(); }).then(function(d){
        if (d.ok) {
          // proceed to the next sensor
          zeroNext();
        } else {
          setCalibStatus('Error on ' + s + ': ' + (d.error || 'unknown'));
        }
      }).catch(function(e){
        setCalibStatus('Zero failed on ' + s + ': ' + e);
      });
    }

    zeroNext();
  };

  document.getElementById('spanBtn').onclick = function(){
    var sensor = document.getElementById('sensorSelect').value;
    var p = parseFloat(document.getElementById('spanPressureInput').value);
    if(isNaN(p) || p <= 0){ setCalibStatus('Enter a valid known pressure first.'); return; }
    setCalibStatus('Setting span for ' + sensor + ' at ' + p + ' PSI…');
    fetch('/calibration', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sensor: sensor, action: 'span', pressure: p })
    }).then(function(r){ return r.json(); }).then(function(d){
      if(d.ok){
        setCalibStatus('Span set for ' + sensor + ' (K=' + d.K.toFixed(4) + ', raw1=' + d.raw1.toFixed(2) + ').');
        refreshStatus();
      } else {
        setCalibStatus('Error: ' + (d.error || 'unknown'));
      }
    }).catch(function(e){ setCalibStatus('Span failed: ' + e); });
  };

  function refreshStatus(){
    fetch('/calibration/status').then(function(r){ return r.json(); }).then(function(s){
      var tbody = document.getElementById('statusTable');
      tbody.innerHTML = '';
      var rows = [
        ['Iso (0–1600)','iso'],
        ['Resin (0–1600)','resin'],
        ['Iso Low (0–500)','isoLow'],
        ['Resin Low (0–500)','resinLow'],
        ['Air Piston (0–300)','air'],
        ['Gun AP Air (0–300)','apAir']
      ];
      rows.forEach(function(row){
        var label = row[0], key = row[1];
        var r0 = s[key].R0, K = s[key].K;
        var note;
        if(Math.abs(K-1.0) < 0.0001 && Math.abs(r0) < 0.0001){
          note = '<span class="warn">Not calibrated</span>';
        } else if (Math.abs(K-1.0) < 0.0001 && Math.abs(r0) >= 0.0001){
          note = '<span class="ok">Zeroed; span pending</span>';
        } else {
          note = '<span class="ok">Zero + Span set</span>';
        }
        tbody.insertAdjacentHTML('beforeend',
          '<tr><td>'+label+'</td><td>'+r0.toFixed(2)+'</td><td>'+K.toFixed(4)+'</td><td>'+note+'</td></tr>'
        );
      });
    });
  }

  refreshStatus();
</script>
</body>
</html>
)rawliteral";

// ---------- WebSocket events ----------

