const state = {
  map: null,
  plottedCount: 0,
  mapFitted: false,
  baseMarker: null,
  bounds: [],
};

function rssiColor(rssi) {
  if (rssi === null || rssi === undefined || rssi === "") {
    return "#888888";
  }
  const clamped = Math.max(-120, Math.min(-30, Number(rssi)));
  const t = (clamped + 120) / 90; // 0 weak -> 1 strong
  const r = Math.round(255 * (1 - t));
  const g = Math.round(255 * t);
  return `rgb(${r},${g},80)`;
}

function binColor(count) {
  if (count <= 0) {
    return "#2a2f36";
  }
  const intensity = Math.min(1, count / 5);
  const g = Math.round(80 + intensity * 150);
  return `rgb(40,${g},90)`;
}

function initMap() {
  L.Icon.Default.imagePath = "/static/vendor/leaflet/images/";
  state.map = L.map("history-map").setView([0, 0], 2);
  L.tileLayer("/tiles/{z}/{x}/{y}.png", { maxZoom: 19 }).addTo(state.map);
}

function updateBaseMarker(baseStation) {
  if (!baseStation || baseStation.lat === undefined) {
    return;
  }
  const latlng = [baseStation.lat, baseStation.lon];
  state.bounds.push(latlng);
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

async function pollStatusHistory() {
  const history = await Api.statusHistory(5000);
  const newFixes = history.slice(state.plottedCount);
  state.plottedCount = history.length;

  for (const fix of newFixes) {
    if (fix.lat === null || fix.lat === undefined || fix.lat === "") {
      continue;
    }
    const latlng = [fix.lat, fix.lon];
    state.bounds.push(latlng);
    L.circleMarker(latlng, {
      radius: 5,
      color: rssiColor(fix.rssi),
      fillColor: rssiColor(fix.rssi),
      fillOpacity: 0.75,
      weight: 1,
    })
      .bindPopup(`Node ${fix.node_id}<br>rssi ${fmt(fix.rssi)}`)
      .addTo(state.map);
  }

  if (state.bounds.length && !state.mapFitted) {
    state.map.fitBounds(state.bounds, { maxZoom: 16 });
    state.mapFitted = true;
  }
}

function updateReceptionGrid(timeline) {
  const container = document.getElementById("reception-grid");
  const nodeIds = Object.keys(timeline)
    .map(Number)
    .sort((a, b) => a - b);

  for (const nodeId of nodeIds) {
    let row = document.getElementById(`reception-row-${nodeId}`);
    if (!row) {
      row = document.createElement("div");
      row.className = "reception-row";
      row.id = `reception-row-${nodeId}`;

      const label = document.createElement("div");
      label.className = "node-label";
      label.textContent = `Node ${nodeId}`;

      const bins = document.createElement("div");
      bins.className = "reception-bins";

      row.appendChild(label);
      row.appendChild(bins);
      container.appendChild(row);
    }

    const binsContainer = row.querySelector(".reception-bins");
    const counts = timeline[nodeId];
    while (binsContainer.children.length < counts.length) {
      const cell = document.createElement("div");
      cell.className = "reception-bin";
      binsContainer.appendChild(cell);
    }

    counts.forEach((count, i) => {
      binsContainer.children[i].style.background = binColor(count);
      binsContainer.children[i].title = `${count} packet(s)`;
    });
  }
}

function updateLossTable(nodes) {
  const tbody = document.querySelector("#loss-table tbody");
  tbody.innerHTML = "";
  const sorted = Object.values(nodes).sort((a, b) => a.node_id - b.node_id);
  for (const info of sorted) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${info.node_id}</td><td>${info.loss_percent}%</td><td>${
      info.received
    }</td><td>${info.missing}</td><td>${fmt(info.retx_session)}</td><td>${fmt(
      info.fail_session
    )}</td>`;
    tbody.appendChild(tr);
  }
}

async function pollAll() {
  const [nodes, baseStation, timeline] = await Promise.all([
    Api.nodes(),
    Api.getBaseStation(),
    Api.receptionTimeline(50, 5),
  ]);
  updateBaseMarker(baseStation);
  await pollStatusHistory();
  updateReceptionGrid(timeline);
  updateLossTable(nodes);
}

async function init() {
  renderNav(window.location.pathname);
  initMap();
  await pollAll();
  setInterval(pollAll, 2000);
}

document.addEventListener("DOMContentLoaded", init);
