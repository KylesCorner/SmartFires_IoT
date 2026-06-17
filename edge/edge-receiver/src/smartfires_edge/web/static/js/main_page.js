const METRICS = [
  { key: "temp_c", label: "Temp (°C)" },
  { key: "humidity_pct", label: "Humidity (%)" },
  { key: "wind_mps", label: "Wind (m/s)" },
  { key: "pm1_0_ug_m3", label: "PM1.0" },
  { key: "pm2_5_ug_m3", label: "PM2.5" },
  { key: "pm4_0_ug_m3", label: "PM4.0" },
  { key: "pm10_ug_m3", label: "PM10" },
];

const LOG_MAX_LINES = 2000;

const state = {
  selectedNodes: new Set(),
  knownNodes: new Set(),
  selectedMetrics: new Set(["temp_c"]),
  chart: null,
  map: null,
  markers: {},
  baseMarker: null,
  mapFitted: false,
};

const logState = {
  entries: [],        // {t, msg, node_id} — ring of up to LOG_MAX_LINES
  activeTab: null,    // null = "All", number = specific node_id
  knownNodeIds: new Set(),
  ws: null,
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

function initChart() {
  const ctx = document.getElementById("sensor-chart").getContext("2d");
  state.chart = new Chart(ctx, {
    type: "line",
    data: { datasets: [] },
    options: {
      animation: false,
      parsing: false,
      scales: {
        x: {
          type: "linear",
          ticks: {
            callback: (value) => new Date(value).toLocaleTimeString(),
          },
        },
        y: { beginAtZero: false },
      },
      plugins: {
        legend: { labels: { color: "#e6e6e6" } },
      },
    },
  });
}

async function refreshChart() {
  const nodeIds = [...state.selectedNodes];
  if (nodeIds.length === 0 || state.selectedMetrics.size === 0) {
    state.chart.data.datasets = [];
    state.chart.update();
    return;
  }

  const perNodeSamples = await Promise.all(
    nodeIds.map((nodeId) => Api.telemetryRecent(nodeId, 300))
  );

  const datasets = [];
  nodeIds.forEach((nodeId, idx) => {
    const samples = perNodeSamples[idx];
    for (const metric of state.selectedMetrics) {
      const points = samples
        .filter((s) => s[metric] !== "" && s[metric] !== undefined && s[metric] !== null)
        .map((s) => ({ x: Date.parse(s.timestamp), y: Number(s[metric]) }));
      datasets.push({
        label: `node ${nodeId} – ${metric}`,
        data: points,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.15,
      });
    }
  });

  state.chart.data.datasets = datasets;
  state.chart.update();
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

function initMap() {
  L.Icon.Default.imagePath = "/static/vendor/leaflet/images/";
  state.map = L.map("node-map").setView([0, 0], 2);
  L.tileLayer("/tiles/{z}/{x}/{y}.png", { maxZoom: 19 }).addTo(state.map);
}

function updateMap(nodes, baseStation) {
  const bounds = [];

  for (const info of Object.values(nodes)) {
    if (info.lat === null || info.lat === undefined || info.lat === "") {
      continue;
    }
    const latlng = [info.lat, info.lon];
    bounds.push(latlng);

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
      // Trim first line from the display too
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
  initChart();
  initMap();
  wireBaseStationForm();
  renderLogTabs();
  connectLogSocket();
  wireCommandInput();
  await pollNodes();
  await refreshChart();
  setInterval(pollNodes, 2000);
  setInterval(refreshChart, 2000);
}

document.addEventListener("DOMContentLoaded", init);
