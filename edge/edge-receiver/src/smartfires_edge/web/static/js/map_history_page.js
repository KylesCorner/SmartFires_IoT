// ── Map state ──────────────────────────────────────────────────────────────
const mapState = {
  map: null,
  plottedCount: 0,
  mapFitted: false,
  baseMarker: null,
  bounds: [],
};

// ── Colours ────────────────────────────────────────────────────────────────
function rssiColor(rssi) {
  if (rssi === null || rssi === undefined || rssi === "") return "#888888";
  const clamped = Math.max(-120, Math.min(-30, Number(rssi)));
  const t = (clamped + 120) / 90;
  return `rgb(${Math.round(255 * (1 - t))},${Math.round(255 * t)},80)`;
}

function binColor(slotState) {
  if (slotState === "received") return "#28a055";
  if (slotState === "missing")  return "#7a2020";
  return "#2a2f36";
}

function slotTitle(slot) {
  if (slot.seq === null || slot.seq === undefined) return "(awaiting data)";
  if (slot.state === "before")  return `seq ${slot.seq} (before session)`;
  if (slot.state === "missing") return `seq ${slot.seq} — missing`;
  return `seq ${slot.seq}`;
}

// ── Map ────────────────────────────────────────────────────────────────────
function initMap() {
  L.Icon.Default.imagePath = "/static/vendor/leaflet/images/";
  mapState.map = L.map("history-map").setView([0, 0], 2);
  createSmartFiresTileLayer().addTo(mapState.map);
}

function updateBaseMarker(baseStation) {
  if (!baseStation || baseStation.lat === undefined) return;
  const latlng = [baseStation.lat, baseStation.lon];
  mapState.bounds.push(latlng);
  if (!mapState.baseMarker) {
    mapState.baseMarker = L.circleMarker(latlng, {
      radius: 9, color: "#e8743a", fillColor: "#e8743a", fillOpacity: 0.9,
    }).addTo(mapState.map);
  } else {
    mapState.baseMarker.setLatLng(latlng);
  }
  mapState.baseMarker.bindPopup("Base station");
}

async function pollStatusHistory() {
  const history = await Api.statusHistory(5000);
  const newFixes = history.slice(mapState.plottedCount);
  mapState.plottedCount = history.length;

  for (const fix of newFixes) {
    if (fix.lat === null || fix.lat === undefined || fix.lat === "") continue;
    const latlng = [fix.lat, fix.lon];
    mapState.bounds.push(latlng);
    prefetchTilesForLocation(fix.lat, fix.lon);
    L.circleMarker(latlng, {
      radius: 5,
      color: rssiColor(fix.rssi),
      fillColor: rssiColor(fix.rssi),
      fillOpacity: 0.75,
      weight: 1,
    })
      .bindPopup(`Node ${fix.node_id}<br>rssi ${fmt(fix.rssi)}`)
      .addTo(mapState.map);
  }

  if (mapState.bounds.length && !mapState.mapFitted) {
    mapState.map.fitBounds(mapState.bounds, { maxZoom: 16 });
    mapState.mapFitted = true;
  }
}

// ── Reception Timeline ──────────────────────────────────────────────────────
const PLACEHOLDER_BINS = 50;

function makeBinCol(slot) {
  const col = document.createElement("div");
  col.className = "rt-bin-col";

  const label = document.createElement("div");
  label.className = "rt-bin-label";
  label.textContent =
    slot.seq !== null && slot.seq !== undefined ? String(slot.seq) : "—";

  const box = document.createElement("div");
  box.className = "rt-bin-box";
  box.style.background = binColor(slot.state);
  box.title = slotTitle(slot);

  col.appendChild(label);
  col.appendChild(box);
  return col;
}

function renderBins(wrapper, bins) {
  wrapper.innerHTML = "";
  if (!bins || bins.length === 0) {
    for (let i = 0; i < PLACEHOLDER_BINS; i++) {
      wrapper.appendChild(makeBinCol({ seq: null, state: "before" }));
    }
    return;
  }
  for (const slot of bins) {
    wrapper.appendChild(makeBinCol(slot));
  }
}

function updateReceptionTimeline(allNodes, timeline) {
  const container = document.getElementById("reception-timeline");

  // Union of node IDs seen in either source.
  const nodeIds = new Set([
    ...Object.keys(allNodes).map(Number),
    ...Object.keys(timeline).map(Number),
  ]);
  const sorted = [...nodeIds].sort((a, b) => a - b);

  // Empty state.
  let notice = document.getElementById("rt-empty");
  if (sorted.length === 0) {
    if (!notice) {
      notice = document.createElement("p");
      notice.id = "rt-empty";
      notice.className = "section-hint";
      notice.textContent = "No nodes connected this session yet.";
      container.appendChild(notice);
    }
    return;
  }
  if (notice) notice.remove();

  for (const nodeId of sorted) {
    const nodeData = timeline[nodeId];
    const bins     = nodeData ? (nodeData.bins || []) : [];
    const lastSeq  = nodeData ? nodeData.last_seq : null;

    // Create the node block on first appearance.
    let block = document.getElementById(`rt-node-${nodeId}`);
    if (!block) {
      block = document.createElement("div");
      block.className = "rt-node-block";
      block.id = `rt-node-${nodeId}`;

      const header = document.createElement("div");
      header.className = "rt-node-header";

      const nameEl = document.createElement("span");
      nameEl.className = "rt-node-name";
      nameEl.textContent = `Node ${nodeId}`;

      const seqBadge = document.createElement("span");
      seqBadge.className = "rt-last-seq";
      seqBadge.id = `rt-lastseq-${nodeId}`;

      header.appendChild(nameEl);
      header.appendChild(seqBadge);

      const scroll = document.createElement("div");
      scroll.className = "rt-scroll";

      const binsWrapper = document.createElement("div");
      binsWrapper.className = "rt-bins-wrapper";
      binsWrapper.id = `rt-bins-${nodeId}`;

      scroll.appendChild(binsWrapper);
      block.appendChild(header);
      block.appendChild(scroll);
      container.appendChild(block);
    }

    // Update last-seq badge.
    const badge = document.getElementById(`rt-lastseq-${nodeId}`);
    if (badge) {
      badge.textContent =
        lastSeq !== null && lastSeq !== undefined
          ? `last seq ${lastSeq}`
          : "no packets yet";
    }

    // Re-render bin columns (the window shifts every poll, so full rebuild is correct).
    const binsWrapper = document.getElementById(`rt-bins-${nodeId}`);
    renderBins(binsWrapper, bins);
  }

  // Remove blocks for nodes that have disappeared (e.g. session reset).
  for (const el of container.querySelectorAll(".rt-node-block")) {
    const id = Number(el.id.replace("rt-node-", ""));
    if (!nodeIds.has(id)) el.remove();
  }
}

// ── Link Quality table ──────────────────────────────────────────────────────
function updateLossTable(nodes) {
  const tbody = document.querySelector("#loss-table tbody");
  tbody.innerHTML = "";
  const sorted = Object.values(nodes || {}).sort((a, b) => a.node_id - b.node_id);
  if (sorted.length === 0) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td colspan="10" style="color:#6a7480;font-style:italic">No nodes connected this session yet.</td>`;
    tbody.appendChild(tr);
    return;
  }

  for (const info of sorted) {
    const loss = info.loss_percent ?? 0;
    const lossClass =
      loss > 10 ? "loss-high" : loss > 2 ? "loss-med" : "";

    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${info.node_id}</td>
      <td class="${lossClass}">${loss.toFixed(1)}%</td>
      <td>${fmt(info.received)}</td>
      <td>${fmt(info.missing)}</td>
      <td>${fmt(info.duplicates)}</td>
      <td>${fmt(info.last_rssi)}</td>
      <td>${fmt(info.retx_session)}</td>
      <td>${fmt(info.fail_session)}</td>
      <td>${fmtTime(info.last_seen)}</td>
      <td><button class="reset-node-btn" data-node-id="${info.node_id}">Reset</button></td>
    `;
    tbody.appendChild(tr);
  }
}

// ── Node Reboot Events ───────────────────────────────────────────────────────
function formatResetCause(ev) {
  if (ev.reset_cause === null || ev.reset_cause === undefined) return "—";
  const names = (ev.reset_cause_names || []).join(", ") || "NONE";
  return names;
}

function isWatchdogCause(ev) {
  return Array.isArray(ev.reset_cause_names) && ev.reset_cause_names.includes("WDT");
}

function updateAwakenTable(events) {
  const tbody = document.querySelector("#awaken-table tbody");
  tbody.innerHTML = "";
  if (!events || events.length === 0) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td colspan="6" style="color:#6a7480;font-style:italic">No AWAKEN events this session yet.</td>`;
    tbody.appendChild(tr);
    return;
  }

  for (const ev of events) {
    const tr = document.createElement("tr");
    const causeClass = isWatchdogCause(ev) ? "loss-high" : "";
    const causeTitle =
      ev.reset_cause !== null && ev.reset_cause !== undefined
        ? `raw reset_cause=0x${Number(ev.reset_cause).toString(16).padStart(2, "0")}`
        : "legacy AWAKEN frame — node not flashed with reset diagnostics";
    tr.innerHTML = `
      <td>${new Date(ev.t_ms).toLocaleString()}</td>
      <td>${ev.node_id}</td>
      <td class="${causeClass}" title="${causeTitle}">${formatResetCause(ev)}</td>
      <td>${fmt(ev.hang_zone_name)}</td>
      <td>${fmt(ev.seq)}</td>
      <td>${fmt(ev.rssi)}</td>
    `;
    tbody.appendChild(tr);
  }
}

async function pollAwakenEvents() {
  const events = await Api.awakenEvents(500);
  updateAwakenTable(events);
}

// ── Per-node hard reset ──────────────────────────────────────────────────────
function wireResetButtons() {
  const tbody = document.querySelector("#loss-table tbody");
  tbody.addEventListener("click", async (ev) => {
    const btn = ev.target.closest(".reset-node-btn");
    if (!btn) return;

    const nodeId = Number(btn.dataset.nodeId);
    if (!confirm(`Hard-reset node ${nodeId}?\nThis reboots its MCU — it will go offline and resync via AWAKEN.`)) {
      return;
    }

    btn.disabled = true;
    btn.textContent = "Resetting…";
    try {
      await Api.resetNode(nodeId);
    } catch (err) {
      console.error("SmartFires reset request failed:", err);
      alert(`Failed to send reset for node ${nodeId}.`);
    }
    // Table rebuilds on the next poll tick (2s), restoring the button.
  });
}

// ── Poll loop ───────────────────────────────────────────────────────────────
async function pollAll() {
  let nodes, baseStation, timeline;
  try {
    [nodes, baseStation, timeline] = await Promise.all([
      Api.nodes(),
      Api.getBaseStation(),
      Api.receptionTimeline(50),
    ]);
  } catch (err) {
    console.error("SmartFires API fetch failed:", err);
    return;
  }

  updateBaseMarker(baseStation);
  try { await pollStatusHistory(); } catch (_) {}
  try { await pollAwakenEvents(); } catch (_) {}
  updateReceptionTimeline(nodes, timeline);
  updateLossTable(nodes);
}

async function init() {
  renderNav(window.location.pathname);
  initMap();
  wireResetButtons();
  await pollAll();
  setInterval(pollAll, 2000);
}

document.addEventListener("DOMContentLoaded", init);
