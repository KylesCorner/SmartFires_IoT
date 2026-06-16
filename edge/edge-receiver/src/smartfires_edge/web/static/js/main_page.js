const METRICS = [
  { key: "temp_c", label: "Temp (°C)" },
  { key: "humidity_pct", label: "Humidity (%)" },
  { key: "wind_mps", label: "Wind (m/s)" },
  { key: "pm1_0_ug_m3", label: "PM1.0" },
  { key: "pm2_5_ug_m3", label: "PM2.5" },
  { key: "pm4_0_ug_m3", label: "PM4.0" },
  { key: "pm10_ug_m3", label: "PM10" },
];

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

async function init() {
  renderNav(window.location.pathname);
  buildMetricCheckboxes();
  initChart();
  initMap();
  wireBaseStationForm();
  await pollNodes();
  await refreshChart();
  setInterval(pollNodes, 2000);
  setInterval(refreshChart, 2000);
}

document.addEventListener("DOMContentLoaded", init);
