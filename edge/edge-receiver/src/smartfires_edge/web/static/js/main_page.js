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
  { label: "All time",    ms: null },
  { label: "Last 5 min",  ms: 5 * 60 * 1000  },
  { label: "Last 15 min", ms: 15 * 60 * 1000 },
  { label: "Last hour",   ms: 60 * 60 * 1000 },
];

// The live telemetry buffer holds up to 2000 samples per node server-side
// (see live_state.py) — fetch the whole thing and apply the selected time
// range client-side so pause/step playback can scrub within it without an
// extra round trip per window change.
const TELEMETRY_FETCH_LIMIT = 2000;
// Each step button press moves the view by half the current window.
const STEP_FRACTION = 0.5;

// CSV-backed history: the live ring only covers the last ~25 minutes, so any
// wider window is filled from /api/telemetry/history. Fetched results are
// cached per node+metric; in live mode the baseline is refreshed every
// HISTORY_REFRESH_MS while the ring covers the growing tail in between.
const HISTORY_MAX_POINTS = 1200;
const HISTORY_REFRESH_MS = 30 * 1000;
// A line is broken (null point) when neighbouring samples are further apart
// than this — otherwise Chart.js would draw straight lines across outages.
const GAP_BREAK_MS = 60 * 1000;

// Session timeline strip below the chart.
const TIMELINE_BUCKETS = 300;
const TIMELINE_REFRESH_MS = 10 * 1000;
// Full-rate telemetry is one sample per 750 ms; used to shade activity.
const SAMPLE_PERIOD_MS = 750;

const historyCache = new Map(); // "node|metric" -> {fetchedAt, startMs, endMs, points, bucketMs}

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
  live:            true,
  pausedViewEndMs: null,   // set when paused; the timestamp at the right edge of the chart
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
      // Changing the window while paused keeps the center of the current
      // view stable, rather than anchoring to either edge.
      if (!state.live && state.timeRangeMs && range.ms) {
        const center = state.pausedViewEndMs - state.timeRangeMs / 2;
        state.pausedViewEndMs = Math.min(Date.now(), center + range.ms / 2);
      }
      state.timeRangeMs = range.ms;
      container.querySelectorAll(".time-range-btn").forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");
      renderPlaybackUI();
      refreshChart();
    });
    container.appendChild(btn);
  }
}

// ---------------------------------------------------------------------------
// Playback controls (pause / step / live) — mirrors the TDMA sniffer page's
// "Window" toolbar so the live chart can be frozen and scrubbed back through
// the buffered samples instead of always tracking Date.now().
// ---------------------------------------------------------------------------

function currentViewEndMs() {
  return state.live ? Date.now() : state.pausedViewEndMs;
}

function renderPlaybackUI() {
  const toggleBtn = document.getElementById("chart-play-toggle");
  const indicator = document.getElementById("chart-live-indicator");
  if (state.live) {
    toggleBtn.textContent = "⏸ Pause";
    indicator.innerHTML = `<span class="conn-dot online"></span> LIVE`;
  } else {
    toggleBtn.textContent = "▶ Go Live";
    const end = new Date(state.pausedViewEndMs);
    indicator.innerHTML = state.timeRangeMs
      ? `<span class="conn-dot offline"></span> Viewing ${new Date(state.pausedViewEndMs - state.timeRangeMs).toLocaleTimeString()} – ${end.toLocaleTimeString()}`
      : `<span class="conn-dot offline"></span> Viewing up to ${end.toLocaleTimeString()}`;
  }
  // Stepping needs a finite window size to step by — disable while "All time" is selected.
  document.getElementById("chart-step-back").disabled = state.timeRangeMs === null;
  document.getElementById("chart-step-forward").disabled = state.timeRangeMs === null;
}

function setLive(isLive) {
  if (isLive) {
    state.live = true;
    state.pausedViewEndMs = null;
  } else if (state.live) {
    state.pausedViewEndMs = Date.now();
    state.live = false;
  }
  renderPlaybackUI();
  refreshChart();
}

function togglePlay() {
  setLive(!state.live);
}

function stepView(direction) {
  if (state.timeRangeMs === null) return; // no fixed window to step by
  setLive(false); // stepping always means "look away from now"
  const delta = direction * state.timeRangeMs * STEP_FRACTION;
  state.pausedViewEndMs = Math.min(Date.now(), state.pausedViewEndMs + delta);
  renderPlaybackUI();
  refreshChart();
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

// Fetch (or reuse) the CSV-backed baseline for one node+metric. The cache
// entry is refetched when the requested window reaches outside what it covers,
// or — for views that extend past the fetch time — when it goes stale. The
// CSV is append-only within a session, so purely-historical windows never
// go stale; session changes clear the cache (see pollNodes).
async function getHistory(nodeId, metric, startMs, endMs, ringEarliestMs) {
  const key = `${nodeId}|${metric}`;
  const now = Date.now();
  let entry = historyCache.get(key);

  let fetchNeeded =
    !entry ||
    startMs < entry.startMs - 1000 ||
    (endMs > entry.fetchedAt - 1000 && now - entry.fetchedAt > HISTORY_REFRESH_MS);
  if (!fetchNeeded && endMs > entry.endMs + GAP_BREAK_MS) {
    // The window extends past the cached data — fine if the live ring
    // overlaps the cache end (it covers the tail), otherwise refetch.
    fetchNeeded = ringEarliestMs === null || ringEarliestMs > entry.endMs + GAP_BREAK_MS;
  }
  if (fetchNeeded) {
    const res = await Api.telemetryHistory(
      nodeId, metric, startMs || undefined, endMs, HISTORY_MAX_POINTS
    );
    entry = {
      fetchedAt: now,
      startMs,
      endMs,
      points: res.points ?? [],
      bucketMs: res.bucket_ms ?? 0,
    };
    historyCache.set(key, entry);
  }
  return entry;
}

// Merge the CSV baseline with the live ring tail into one x-sorted series,
// inserting null points so reception outages render as gaps instead of the
// line cutting straight across them.
function mergeSeries(historyEntry, ringPoints, cutoffMs, viewEndMs) {
  const ringStart = ringPoints.length ? ringPoints[0].x : Infinity;
  const merged = historyEntry.points
    .filter(([t]) => t >= cutoffMs && t <= viewEndMs && t < ringStart)
    .map(([t, v]) => ({ x: t, y: v }))
    .concat(ringPoints);

  const gapMs = Math.max(GAP_BREAK_MS, (historyEntry.bucketMs || 0) * 4);
  const withGaps = [];
  for (const pt of merged) {
    const prev = withGaps[withGaps.length - 1];
    if (prev && prev.y !== null && pt.x - prev.x > gapMs) {
      withGaps.push({ x: prev.x + 1, y: null });
    }
    withGaps.push(pt);
  }
  return withGaps;
}

let chartRefreshInFlight = false;

async function refreshChart() {
  if (chartRefreshInFlight) return;
  chartRefreshInFlight = true;
  try {
    await _refreshChart();
  } finally {
    chartRefreshInFlight = false;
  }
}

async function _refreshChart() {
  const nodeIds = [...state.selectedNodes];
  if (nodeIds.length === 0 || state.selectedMetrics.size === 0) {
    state.chart.data.datasets = [];
    state.chart.options.scales = buildBaseScales();
    state.chart.update();
    return;
  }

  const viewEndMs = currentViewEndMs();
  const cutoff = state.timeRangeMs ? viewEndMs - state.timeRangeMs : 0;

  const perNodeSamples = await Promise.all(
    nodeIds.map((nodeId) => Api.telemetryRecent(nodeId, TELEMETRY_FETCH_LIMIT))
  );

  const inWindow = (s) => {
    const t = Date.parse(s.timestamp);
    return t >= cutoff && t <= viewEndMs;
  };

  // One merged series per node+metric: CSV history baseline + live ring tail.
  const seriesByKey = new Map();
  await Promise.all(
    nodeIds.flatMap((nodeId, nodeIdx) =>
      [...state.selectedMetrics].map(async (metric) => {
        const samples = perNodeSamples[nodeIdx];
        const ringPoints = samples
          .filter((s) => {
            if (!inWindow(s)) return false;
            return s[metric] !== "" && s[metric] !== undefined && s[metric] !== null;
          })
          .map((s) => ({ x: Date.parse(s.timestamp), y: Number(s[metric]) }))
          .sort((a, b) => a.x - b.x);
        const ringEarliest = samples.length ? Date.parse(samples[0].timestamp) : null;
        const history = await getHistory(nodeId, metric, cutoff, viewEndMs, ringEarliest);
        seriesByKey.set(`${nodeId}|${metric}`, mergeSeries(history, ringPoints, cutoff, viewEndMs));
      })
    )
  );

  // Per-metric maximum across all nodes within the time window (axis min is always 0).
  const metricMax = {};
  for (const metric of state.selectedMetrics) {
    let hi = 0;
    for (const nodeId of nodeIds) {
      for (const pt of seriesByKey.get(`${nodeId}|${metric}`) ?? []) {
        if (pt.y !== null && pt.y > hi) hi = pt.y;
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

  // Build datasets from the merged series. Nodes sharing the same metric share
  // the same axis; different nodes are distinguished by line dash pattern.
  const datasets = [];
  nodeIds.forEach((nodeId, nodeIdx) => {
    const dashPattern = NODE_DASH_PATTERNS[nodeIdx % NODE_DASH_PATTERNS.length];
    for (const metric of state.selectedMetrics) {
      const color = METRIC_COLORS[metric] ?? "#aab4c0";
      const metaDef = METRICS.find((m) => m.key === metric);
      datasets.push({
        label: `Node ${nodeId} – ${metaDef?.label ?? metric}`,
        data: seriesByKey.get(`${nodeId}|${metric}`) ?? [],
        yAxisID: `y_${metric}`,
        borderColor: color,
        backgroundColor: color + "33",
        borderDash: dashPattern,
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.15,
        spanGaps: false,
      });
    }
  });

  state.chart.options.scales = scales;
  state.chart.data.datasets = datasets;
  state.chart.update();
}

// ---------------------------------------------------------------------------
// Session timeline — one activity strip per node from session start to now.
// Green segments = packets received; red ticks = AWAKEN packets (node boots,
// e.g. watchdog resets); dark background = silence.
// ---------------------------------------------------------------------------

async function refreshTimeline() {
  let data;
  try {
    data = await Api.sessionTimeline(TIMELINE_BUCKETS);
  } catch {
    return;
  }
  const container = document.getElementById("session-timeline");
  const rangeEl = document.getElementById("timeline-range");
  const spanMs = Math.max(data.end_ms - data.start_ms, 1);
  rangeEl.textContent =
    `${new Date(data.start_ms).toLocaleTimeString()} – ${new Date(data.end_ms).toLocaleTimeString()}`;

  const nodeIds = Object.keys(data.nodes).map(Number).sort((a, b) => a - b);
  container.innerHTML = "";
  if (nodeIds.length === 0) {
    container.innerHTML = `<div class="timeline-empty">No node activity yet this session.</div>`;
    return;
  }

  const expectedPerBucket = data.bucket_ms / SAMPLE_PERIOD_MS;
  for (const nodeId of nodeIds) {
    const counts = data.nodes[String(nodeId)];
    const row = document.createElement("div");
    row.className = "timeline-row";
    const label = document.createElement("span");
    label.className = "timeline-label";
    label.textContent = `Node ${nodeId}`;
    const track = document.createElement("div");
    track.className = "timeline-track";

    // Merge consecutive active buckets into one segment; shade by fill rate.
    for (let i = 0; i < counts.length; ) {
      if (counts[i] === 0) { i++; continue; }
      let j = i;
      let total = 0;
      while (j < counts.length && counts[j] > 0) { total += counts[j]; j++; }
      const seg = document.createElement("div");
      seg.className = "timeline-seg";
      seg.style.left = `${(i / counts.length) * 100}%`;
      seg.style.width = `${Math.max(((j - i) / counts.length) * 100, 0.2)}%`;
      const fill = Math.min(total / ((j - i) * expectedPerBucket), 1);
      seg.style.opacity = (0.35 + 0.65 * fill).toFixed(2);
      const t0 = new Date(data.start_ms + i * data.bucket_ms).toLocaleTimeString();
      const t1 = new Date(data.start_ms + j * data.bucket_ms).toLocaleTimeString();
      seg.title = `Node ${nodeId}: active ${t0} – ${t1}`;
      track.appendChild(seg);
      i = j;
    }

    for (const [t, awakenNode] of data.awaken) {
      if (awakenNode !== nodeId) continue;
      const tick = document.createElement("div");
      tick.className = "timeline-awaken";
      tick.style.left = `${((t - data.start_ms) / spanMs) * 100}%`;
      tick.title = `Node ${nodeId} boot (AWAKEN) at ${new Date(t).toLocaleTimeString()}`;
      track.appendChild(tick);
    }

    row.appendChild(label);
    row.appendChild(track);
    container.appendChild(row);
  }
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
    const lastSeen = fmtTime(info.last_seen);
    const heading = info.heading_deg !== null && info.heading_deg !== undefined && info.heading_deg !== ""
      ? `${info.heading_deg}°`
      : "—";
    const headingAcc = info.heading_accuracy_deg !== null && info.heading_accuracy_deg !== undefined && info.heading_accuracy_deg !== ""
      ? `±${info.heading_accuracy_deg}°`
      : "—";
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${info.node_id}</td><td>${fmtSerial(info.uid_hash)}</td><td>${fmt(info.lat)}</td><td>${fmt(
      info.lon
    )}</td><td>${fmt(info.battery_pct)}</td><td>${heading}</td><td>${headingAcc}</td><td>${lastSeen}</td>`;
    tbody.appendChild(tr);
  }
}

async function pollNodes() {
  const [nodes, baseStation, session] = await Promise.all([
    Api.nodes(),
    Api.getBaseStation(),
    Api.session(),
  ]);
  // A session change (New Session pressed here or elsewhere) invalidates all
  // cached CSV history — it belongs to the previous session's file.
  if (state.sessionId !== undefined && session.session_id !== state.sessionId) {
    historyCache.clear();
  }
  state.sessionId = session.session_id;
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
// New session
// ---------------------------------------------------------------------------

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

      historyCache.clear();
      state.chart.data.datasets = [];
      state.chart.options.scales = buildBaseScales();
      state.chart.update();

      state.live = true;
      state.pausedViewEndMs = null;
      renderPlaybackUI();

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
    } catch (e) {
      alert("Failed to start new session: " + (e.message || e));
    } finally {
      btn.disabled = false;
      btn.textContent = "New Session";
    }
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
  wireNewSessionButton();
  document.getElementById("chart-play-toggle").addEventListener("click", togglePlay);
  document.getElementById("chart-step-back").addEventListener("click", () => stepView(-1));
  document.getElementById("chart-step-forward").addEventListener("click", () => stepView(1));
  renderPlaybackUI();
  await pollNodes();
  await refreshChart();
  refreshTimeline();
  setInterval(pollNodes, 2000);
  setInterval(refreshChart, 2000);
  setInterval(refreshTimeline, TIMELINE_REFRESH_MS);
}

document.addEventListener("DOMContentLoaded", init);
