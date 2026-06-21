const METRICS = [
  { key: "temp_c",       label: "Temp (°C)" },
  { key: "humidity_pct", label: "Humidity (%)" },
  { key: "wind_mps",     label: "Wind (m/s)" },
  { key: "pm1_0_ug_m3",  label: "PM1.0" },
  { key: "pm2_5_ug_m3",  label: "PM2.5" },
  { key: "pm4_0_ug_m3",  label: "PM4.0" },
  { key: "pm10_ug_m3",   label: "PM10" },
];

const METRIC_COLORS = {
  temp_c:       "#ff6384",
  humidity_pct: "#36a2eb",
  wind_mps:     "#ffce56",
  pm1_0_ug_m3:  "#4bc0c0",
  pm2_5_ug_m3:  "#9966ff",
  pm4_0_ug_m3:  "#ff9f40",
  pm10_ug_m3:   "#c9cbcf",
};

// Solid line for node 0, dashed for node 1, dotted for node 2, etc.
const NODE_DASH_PATTERNS = [[], [5, 5], [2, 3], [8, 3, 2, 3]];

const TIME_RANGES = [
  { label: "All time",    ms: null,            fetchLimit: 2000 },
  { label: "Last 5 min",  ms: 5 * 60 * 1000,  fetchLimit: 150  },
  { label: "Last 15 min", ms: 15 * 60 * 1000, fetchLimit: 400  },
  { label: "Last hour",   ms: 60 * 60 * 1000, fetchLimit: 2000 },
];

const LOG_MAX_LINES = 2000;

const state = {
  selectedNodes:   new Set(),
  knownNodes:      new Set(),
  selectedMetrics: new Set(["temp_c"]),
  timeRangeMs:     null,   // null = all available data
  chart:           null,
  map:             null,
  markers:         {},
  baseMarker:      null,
  mapFitted:       false,
};

const logState = {
  entries:      [],
  activeTab:    null,
  knownNodeIds: new Set(),
  ws:           null,
};

// ---------------------------------------------------------------------------
// Sensor chart
// ---------------------------------------------------------------------------

function buildMetricCheckboxes() {
  const container = document.getElementById("metric-checkboxes");
  for (const metric of METRICS) {
    const label = document.createElement("label");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.value = metric.key;
    cb.checked = state.selectedMetrics.has(metric.key);
    cb.addEventListener("change", () => {
      if (cb.checked) {
        state.selectedMetrics.add(metric.key);
      } else {
        state.selectedMetrics.delete(metric.key);
      }
      refreshChart();
    });
    label.appendChild(cb);
    label.appendChild(document.createTextNode(" " + metric.label));
    container.appendChild(label);
  }
}

function buildNodeCheckboxes(nodeIds) {
  const container = document.getElementById("node-checkboxes");
  for (const nodeId of nodeIds) {
    if (state.knownNodes.has(nodeId)) {
      continue;
    }
    state.knownNodes.add(nodeId);
    state.selectedNodes.add(nodeId);

    const label = document.createElement("label");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = true;
    cb.addEventListener("change", () => {
      if (cb.checked) {
        state.selectedNodes.add(nodeId);
      } else {
        state.selectedNodes.delete(nodeId);
      }
      refreshChart();
    });
    label.appendChild(cb);
    label.appendChild(document.createTextNode(" Node " + nodeId));
    container.appendChild(label);
  }
}

function buildTimeRangeButtons() {
  const container = document.getElementById("time-range-buttons");
  for (const range of TIME_RANGES) {
    const btn = document.createElement("button");
    btn.className = "time-range-btn" + (state.timeRangeMs === range.ms ? " active" : "");
    btn.textContent = range.label;
    btn.addEventListener("click", () => {
      state.timeRangeMs = range.ms;
      container.querySelectorAll(".time-range-btn").forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");
      refreshChart();
    });
    container.appendChild(btn);
  }
}

function buildBaseScales() {
  return {
    x: {
      type: "linear",
      ticks: {
        callback: (value) => new Date(value).toLocaleTimeString(),
        color: "#aab4c0",
        maxTicksLimit: 8,
      },
      grid: { color: "#2a2f36" },
    },
  };
}

function initChart() {
  const ctx = document.getElementById("sensor-chart").getContext("2d");
  state.chart = new Chart(ctx, {
    type: "line",
    data: { datasets: [] },
    options: {
      animation: false,
      parsing: false,
      maintainAspectRatio: false,
      scales: buildBaseScales(),
      plugins: {
        legend: { labels: { color: "#e6e6e6" } },
        tooltip: {
          callbacks: {
            label: (ctx) => `${ctx.dataset.label}: ${_fmtVal(ctx.raw.y)}`,
          },
        },
      },
    },
  });
}

function _fmtVal(v) {
  if (Math.abs(v) >= 1000) return v.toFixed(1);
  if (Math.abs(v) >= 10)   return v.toFixed(2);
  return v.toFixed(3);
}

async function refreshChart() {
  const nodeIds = [...state.selectedNodes];
  if (nodeIds.length === 0 || state.selectedMetrics.size === 0) {
    state.chart.data.datasets = [];
    state.chart.options.scales = buildBaseScales();
    state.chart.update();
    return;
  }

  const currentRange = TIME_RANGES.find((r) => r.ms === state.timeRangeMs) ?? TIME_RANGES[0];
  const cutoff = state.timeRangeMs ? Date.now() - state.timeRangeMs : 0;

  const perNodeSamples = await Promise.all(
    nodeIds.map((nodeId) => Api.telemetryRecent(nodeId, currentRange.fetchLimit))
  );

  // Per-metric maximum across all nodes within the time window (axis min is always 0).
  const metricMax = {};
  for (const metric of state.selectedMetrics) {
    let hi = 0;
    for (const samples of perNodeSamples) {
      for (const s of samples) {
        if (Date.parse(s.timestamp) < cutoff) continue;
        if (s[metric] !== "" && s[metric] !== undefined && s[metric] !== null) {
          const v = Number(s[metric]);
          if (v > hi) hi = v;
        }
      }
    }
    metricMax[metric] = hi;
  }

  // Build one Y axis per selected metric on the right side.
  // Only the first axis draws chart-area grid lines; the rest show labels only.
  const scales = buildBaseScales();
  let axisIdx = 0;
  for (const metric of state.selectedMetrics) {
    const axisMax = metricMax[metric] > 0 ? metricMax[metric] * 1.1 : 1;
    const color = METRIC_COLORS[metric] ?? "#aab4c0";
    const metaDef = METRICS.find((m) => m.key === metric);
    scales[`y_${metric}`] = {
      type: "linear",
      position: "right",
      min: 0,
      max: axisMax,
      grid: {
        drawOnChartArea: axisIdx === 0,
        color: "#2a2f36",
      },
      ticks: {
        color,
        callback: (v) => _fmtVal(v),
        maxTicksLimit: 6,
      },
      title: {
        display: true,
        text: metaDef?.label ?? metric,
        color,
        font: { size: 10 },
      },
    };
    axisIdx++;
  }

  // Build datasets with raw values. Nodes sharing the same metric share the same axis;
  // different nodes are distinguished by line dash pattern.
  const datasets = [];
  nodeIds.forEach((nodeId, nodeIdx) => {
    const samples = perNodeSamples[nodeIdx];
    const dashPattern = NODE_DASH_PATTERNS[nodeIdx % NODE_DASH_PATTERNS.length];
    for (const metric of state.selectedMetrics) {
      const color = METRIC_COLORS[metric] ?? "#aab4c0";
      const metaDef = METRICS.find((m) => m.key === metric);
      const points = samples
        .filter((s) => {
          if (Date.parse(s.timestamp) < cutoff) return false;
          return s[metric] !== "" && s[metric] !== undefined && s[metric] !== null;
        })
        .map((s) => ({ x: Date.parse(s.timestamp), y: Number(s[metric]) }))
        .sort((a, b) => a.x - b.x);
      datasets.push({
        label: `Node ${nodeId} – ${metaDef?.label ?? metric}`,
        data: points,
        yAxisID: `y_${metric}`,
        borderColor: color,
        backgroundColor: color + "33",
        borderDash: dashPattern,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.15,
      });
    }
  });

  state.chart.options.scales = scales;
  state.chart.data.datasets = datasets;
  state.chart.update();
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

function initMap() {
  L.Icon.Default.imagePath = "/static/vendor/leaflet/images/";
  state.map = L.map("node-map").setView([0, 0], 2);
  createSmartFiresTileLayer().addTo(state.map);
}

function updateMap(nodes, baseStation) {
  const bounds = [];

  for (const info of Object.values(nodes)) {
    if (info.lat === null || info.lat === undefined || info.lat === "") {
      continue;
    }
    const latlng = [info.lat, info.lon];
    bounds.push(latlng);
    prefetchTilesForLocation(info.lat, info.lon);

    if (!state.markers[info.node_id]) {
      state.markers[info.node_id] = L.marker(latlng).addTo(state.map);
    } else {
      state.markers[info.node_id].setLatLng(latlng);
    }
    state.markers[info.node_id].bindPopup(
      `Node ${info.node_id}<br>battery ${fmt(info.battery_pct)}%<br>rssi ${fmt(info.rssi)}`
    );
  }

  if (baseStation && baseStation.lat !== undefined) {
    const latlng = [baseStation.lat, baseStation.lon];
    bounds.push(latlng);
    prefetchTilesForLocation(baseStation.lat, baseStation.lon);
    if (!state.baseMarker) {
      state.baseMarker = L.circleMarker(latlng, {
        radius: 9,
        color: "#e8743a",
        fillColor: "#e8743a",
        fillOpacity: 0.9,
      }).addTo(state.map);
    } else {
      state.baseMarker.setLatLng(latlng);
    }
    state.baseMarker.bindPopup("Base station");
  }

  if (bounds.length && !state.mapFitted) {
    state.map.fitBounds(bounds, { maxZoom: 16 });
    state.mapFitted = true;
  }
}

function updateNodeTable(nodes) {
  const tbody = document.querySelector("#node-table tbody");
  tbody.innerHTML = "";
  const sorted = Object.values(nodes).sort((a, b) => a.node_id - b.node_id);
  for (const info of sorted) {
    const lastSeen = info.last_seen
      ? new Date(info.last_seen * 1000).toLocaleTimeString()
      : "—";
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${info.node_id}</td><td>${fmt(info.lat)}</td><td>${fmt(
      info.lon
    )}</td><td>${fmt(info.battery_pct)}</td><td>${lastSeen}</td>`;
    tbody.appendChild(tr);
  }
}

async function pollNodes() {
  const [nodes, baseStation] = await Promise.all([Api.nodes(), Api.getBaseStation()]);
  buildNodeCheckboxes(Object.keys(nodes).map(Number));
  updateMap(nodes, baseStation);
  updateNodeTable(nodes);
}

function wireBaseStationForm() {
  document.getElementById("base-save").addEventListener("click", async () => {
    const lat = parseFloat(document.getElementById("base-lat").value);
    const lon = parseFloat(document.getElementById("base-lon").value);
    if (Number.isNaN(lat) || Number.isNaN(lon)) {
      return;
    }
    await Api.setBaseStation(lat, lon);
    pollNodes();
  });
}

// ---------------------------------------------------------------------------
// Log panel
// ---------------------------------------------------------------------------

function renderLogTabs() {
  const container = document.getElementById("log-tabs");
  container.innerHTML = "";

  const tabs = [null, ...Array.from(logState.knownNodeIds).sort((a, b) => a - b)];
  for (const tabId of tabs) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (logState.activeTab === tabId ? " active" : "");
    btn.textContent = tabId === null ? "All" : `Node ${tabId}`;
    btn.addEventListener("click", () => {
      logState.activeTab = tabId;
      renderLogTabs();
      renderLogOutput();
    });
    container.appendChild(btn);
  }
}

function renderLogOutput() {
  const el = document.getElementById("log-output");
  const active = logState.activeTab;
  const visible = active === null
    ? logState.entries
    : logState.entries.filter((e) => e.node_id === active || e.node_id === null);

  const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
  el.textContent = visible.map((e) => `${e.t.slice(11, 23)}  ${e.msg}`).join("\n");
  if (atBottom) {
    el.scrollTop = el.scrollHeight;
  }
}

function onLogEntry(entry) {
  logState.entries.push(entry);
  if (logState.entries.length > LOG_MAX_LINES) {
    logState.entries.splice(0, logState.entries.length - LOG_MAX_LINES);
  }

  let tabsChanged = false;
  if (entry.node_id !== null && entry.node_id !== undefined && !logState.knownNodeIds.has(entry.node_id)) {
    logState.knownNodeIds.add(entry.node_id);
    tabsChanged = true;
  }

  if (tabsChanged) {
    renderLogTabs();
  }

  const active = logState.activeTab;
  if (active === null || entry.node_id === active || entry.node_id === null) {
    const el = document.getElementById("log-output");
    const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
    el.textContent += `${entry.t.slice(11, 23)}  ${entry.msg}\n`;
    if (logState.entries.length > LOG_MAX_LINES) {
      const firstNewline = el.textContent.indexOf("\n");
      if (firstNewline !== -1) {
        el.textContent = el.textContent.slice(firstNewline + 1);
      }
    }
    if (atBottom) {
      el.scrollTop = el.scrollHeight;
    }
  }
}

function connectLogSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${proto}//${location.host}/ws/log`);
  logState.ws = ws;

  ws.onmessage = (ev) => {
    try {
      onLogEntry(JSON.parse(ev.data));
    } catch (_) {}
  };

  ws.onclose = () => {
    logState.ws = null;
    setTimeout(connectLogSocket, 2000);
  };
}

function wireNewSessionButton() {
  const btn = document.getElementById("new-session-btn");
  btn.addEventListener("click", async () => {
    if (!confirm("Save the current session CSV and start a new one?\nAll live data in the dashboard will be cleared.")) {
      return;
    }
    btn.disabled = true;
    btn.textContent = "Resetting…";
    try {
      await Api.newSession();

      state.chart.data.datasets = [];
      state.chart.options.scales = buildBaseScales();
      state.chart.update();

      state.knownNodes.clear();
      state.selectedNodes.clear();
      document.getElementById("node-checkboxes").innerHTML = "";

      for (const marker of Object.values(state.markers)) {
        marker.remove();
      }
      state.markers = {};
      state.mapFitted = false;
      if (state.baseMarker) {
        state.baseMarker.remove();
        state.baseMarker = null;
      }

      logState.entries = [];
      logState.knownNodeIds.clear();
      logState.activeTab = null;
      renderLogTabs();
      renderLogOutput();
    } catch (e) {
      alert("Failed to start new session: " + (e.message || e));
    } finally {
      btn.disabled = false;
      btn.textContent = "New Session";
    }
  });
}

function wireCommandInput() {
  const input = document.getElementById("cmd-input");
  const btn = document.getElementById("cmd-send");

  async function sendCommand() {
    const cmd = input.value.trim();
    if (!cmd) return;
    input.value = "";
    try {
      await Api.postCommand(cmd);
    } catch (_) {}
  }

  btn.addEventListener("click", sendCommand);
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") sendCommand();
  });
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

async function init() {
  renderNav(window.location.pathname);
  buildMetricCheckboxes();
  buildTimeRangeButtons();
  initChart();
  initMap();
  wireBaseStationForm();
  renderLogTabs();
  connectLogSocket();
  wireCommandInput();
  wireNewSessionButton();
  await pollNodes();
  await refreshChart();
  setInterval(pollNodes, 2000);
  setInterval(refreshChart, 2000);
}

document.addEventListener("DOMContentLoaded", init);
