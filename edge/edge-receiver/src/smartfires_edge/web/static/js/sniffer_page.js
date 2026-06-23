// TDMA sniffer timeline: swim-lane canvas + stats table + packet detail.
//
// Slot boundaries are derived purely from the most recently anchored event's
// (wall-clock-ms, session_ms) pair plus the fixed 900ms slot width — see
// sniffer_service.py for why that's sufficient (every slot boundary repeats
// every slot_width_ms, so num_slots isn't needed client-side for the grid).
//
// The full packet log (including sniffer activity) lives on the main
// dashboard's Live Log panel now, filterable via the "Sniffer" tab — see
// main_page.js. This page only keeps the canvas-click detail panel.

const WINDOW_MS = 60_000;
const SLOT_WIDTH_MS = 900;
const GUARD_MS = 20;
const LANE_HEIGHT = 36;
const HEADER_HEIGHT = 24;
const SLOT_LABEL_HEIGHT = 20;
// Dedicated row, always shown directly above the Base Station lane, for
// bare RadioHead frames (RH_ACK/RH_RAW) that have no SmartFires node_id —
// keeps them from overlapping or interleaving with real per-node traffic.
const RH_LANE_HEIGHT = LANE_HEIGHT;
const RH_LANE_LABEL = "RadioHead / Unknown";

const PKT_COLORS = {
  BUNDLE: "#3b82c4",
  STATUS: "#3fae5c",
  AWAKEN: "#d4b340",
  FULL_STATE: "#3b82c4",
  TIME_SYNC: "#7a828c",
  ACK_SUMMARY: "#9b59b6",
  CMD_CALIBRATE: "#e8743a",
  CMD_RESET: "#e8743a",
  CMD_ACK: "#5dade2",
  // Bare RadioHead link-layer frames with no SmartFires magic byte — most
  // commonly RHReliableDatagram ACKs (zero-length payload). Given their own
  // colors/labels so they're distinguishable from genuinely-unrecognized
  // SmartFires packet types (which still fall through to UNKNOWN_COLOR).
  RH_ACK: "#6b7280",
  RH_RAW: "#a35400",
};
const UNKNOWN_COLOR = "#c0392b";
const UNKNOWN_LABEL = "UNKNOWN / other";

const sniffer = {
  events: [],            // recent events within WINDOW_MS (+ small buffer)
  laneOf: new Map(),     // node_id -> lane index
  laneOrder: [],         // node_ids in lane order
  lastAnchor: null,      // {wallMs, sessionMs}
  numSlots: null,
  ws: null,
  notConfigured: false,
  idCounter: 0,
  selectedId: null,
  hitRects: [],          // canvas marker bounding boxes for click hit-testing
};

function laneLabel(nodeId) {
  // node_id 0 is the broadcast/command convention (TIME_SYNC/ACK_SUMMARY/CMD_*
  // are sent by the base station, never by a real TDMA node) — give it its
  // own dedicated lane rather than mixing it into a numbered node lane.
  return nodeId === 0 ? "Base Station" : `Node ${nodeId}`;
}

function laneIndexFor(nodeId) {
  if (sniffer.laneOf.has(nodeId)) return sniffer.laneOf.get(nodeId);
  sniffer.laneOrder.push(nodeId);
  sniffer.laneOrder.sort((a, b) => a - b);
  sniffer.laneOf.clear();
  sniffer.laneOrder.forEach((id, idx) => sniffer.laneOf.set(id, idx));
  resizeCanvas();
  return sniffer.laneOf.get(nodeId);
}

function canvasHeight() {
  return (
    HEADER_HEIGHT + RH_LANE_HEIGHT + Math.max(1, sniffer.laneOrder.length) * LANE_HEIGHT + SLOT_LABEL_HEIGHT
  );
}

function renderLegend() {
  const el = document.getElementById("sniffer-legend");
  el.innerHTML = "";
  const entries = [...Object.entries(PKT_COLORS), ["UNKNOWN", UNKNOWN_COLOR]];
  for (const [pktType, color] of entries) {
    const item = document.createElement("div");
    item.className = "sniffer-legend-item";
    const swatch = document.createElement("span");
    swatch.className = "sniffer-legend-swatch";
    swatch.style.background = color;
    const label = document.createElement("span");
    label.textContent = pktType === "UNKNOWN" ? UNKNOWN_LABEL : pktType;
    item.appendChild(swatch);
    item.appendChild(label);
    el.appendChild(item);
  }
}

function resizeCanvas() {
  const canvas = document.getElementById("sniffer-timeline");
  const wrapper = document.getElementById("sniffer-canvas-wrapper");
  const dpr = window.devicePixelRatio || 1;
  const widthCss = wrapper.clientWidth;
  const heightCss = canvasHeight();
  wrapper.style.height = `${heightCss}px`;
  canvas.width = widthCss * dpr;
  canvas.height = heightCss * dpr;
  canvas.style.width = `${widthCss}px`;
  canvas.style.height = `${heightCss}px`;
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

function xForWallMs(canvasWidthCss, nowMs, wallMs) {
  return canvasWidthCss - ((nowMs - wallMs) / WINDOW_MS) * canvasWidthCss;
}

function draw() {
  const canvas = document.getElementById("sniffer-timeline");
  const ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const widthCss = canvas.width / dpr;
  const heightCss = canvas.height / dpr;
  const nowMs = Date.now();

  ctx.clearRect(0, 0, widthCss, heightCss);
  const lanesBottom = heightCss - SLOT_LABEL_HEIGHT;

  // Slot boundary grid + guard bands, anchored to the last TIME_SYNC heard.
  // Also collect each gridline's wall-ms so the label row below can derive
  // which node is expected to transmit in that interval.
  const gridlineWallMs = [];
  if (sniffer.lastAnchor) {
    const { wallMs, sessionMs } = sniffer.lastAnchor;
    const phaseAnchor = ((wallMs - sessionMs) % SLOT_WIDTH_MS + SLOT_WIDTH_MS) % SLOT_WIDTH_MS;
    const earliest = nowMs - WINDOW_MS;
    let t = phaseAnchor + Math.ceil((earliest - phaseAnchor) / SLOT_WIDTH_MS) * SLOT_WIDTH_MS;
    const guardWidthPx = (GUARD_MS / WINDOW_MS) * widthCss;
    ctx.strokeStyle = "#3a4049";
    ctx.setLineDash([4, 4]);
    for (; t <= nowMs + SLOT_WIDTH_MS; t += SLOT_WIDTH_MS) {
      gridlineWallMs.push(t);
      const x = xForWallMs(widthCss, nowMs, t);
      ctx.fillStyle = "rgba(122,130,140,0.08)";
      ctx.fillRect(x - guardWidthPx, HEADER_HEIGHT, 2 * guardWidthPx, lanesBottom - HEADER_HEIGHT);
      ctx.beginPath();
      ctx.moveTo(x, HEADER_HEIGHT);
      ctx.lineTo(x, lanesBottom);
      ctx.stroke();
    }
    ctx.setLineDash([]);
  }

  // RH/Unknown dedicated lane — always shown, directly under the header and
  // above every node lane, so radio-level noise never overlaps real traffic.
  const lanesTop = HEADER_HEIGHT + RH_LANE_HEIGHT;
  ctx.fillStyle = "#aab4c0";
  ctx.font = "11px sans-serif";
  ctx.fillText(RH_LANE_LABEL, 4, HEADER_HEIGHT + 14);

  // Lane labels + separators (including the line separating the RH lane
  // from the first real lane, drawn at idx=0's top edge == lanesTop).
  sniffer.laneOrder.forEach((nodeId, idx) => {
    const y = lanesTop + idx * LANE_HEIGHT;
    ctx.fillText(laneLabel(nodeId), 4, y + 14);
    ctx.strokeStyle = "#2a2f36";
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(widthCss, y);
    ctx.stroke();
  });

  // Events.
  sniffer.hitRects = [];
  for (const ev of sniffer.events) {
    const wallMs = ev._wallMs;
    if (wallMs < nowMs - WINDOW_MS) continue;
    const x = xForWallMs(widthCss, nowMs, wallMs);
    const color = PKT_COLORS[ev.pkt_type] ?? UNKNOWN_COLOR;
    const isSelected = ev._id === sniffer.selectedId;

    if (ev.pkt_type === "TIME_SYNC") {
      ctx.strokeStyle = color;
      ctx.lineWidth = isSelected ? 3 : 1.5;
      ctx.beginPath();
      ctx.moveTo(x, HEADER_HEIGHT);
      ctx.lineTo(x, lanesBottom);
      ctx.stroke();
      ctx.lineWidth = 1;
      sniffer.hitRects.push({ x0: x - 4, y0: HEADER_HEIGHT, x1: x + 4, y1: lanesBottom, ev });
      continue;
    }

    let y;
    if (ev.pkt_type === "RH_ACK" || ev.pkt_type === "RH_RAW") {
      y = HEADER_HEIGHT + RH_LANE_HEIGHT / 2 - 8;
    } else {
      const laneIdx = ev.node_id != null ? laneIndexFor(ev.node_id) : null;
      y = laneIdx != null ? lanesTop + laneIdx * LANE_HEIGHT + LANE_HEIGHT / 2 - 8 : HEADER_HEIGHT;
    }
    ctx.fillStyle = color;
    if (ev.guard_violation) {
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 1.5;
      ctx.strokeRect(x - 3, y, 7, 16);
    }
    ctx.fillRect(x - 3, y, 7, 16);
    if (isSelected) {
      ctx.strokeStyle = "#ffd166";
      ctx.lineWidth = 2;
      ctx.strokeRect(x - 4, y - 1, 9, 18);
      ctx.lineWidth = 1;
    }
    sniffer.hitRects.push({ x0: x - 3, y0: y, x1: x + 4, y1: y + 16, ev });
  }

  // Expected-transmitter label row: who should be transmitting in each slot
  // interval. Slot 0 is permanently reserved for the base station (node_id 1
  // is never assigned to a real node — see config/BaseConfig.h's
  // kFirstNodeId=2); labeled "0" to match the lane's node_id rather than a
  // separate "Base" string. Slot s (s >= 1) maps to node (s + 1) via the
  // firmware's compile-time slot=(node_id-1)%num_slots assignment.
  ctx.fillStyle = "#15191e";
  ctx.fillRect(0, lanesBottom, widthCss, SLOT_LABEL_HEIGHT);
  if (sniffer.lastAnchor && sniffer.numSlots) {
    const { wallMs, sessionMs } = sniffer.lastAnchor;
    const pixelsPerSlot = (SLOT_WIDTH_MS / WINDOW_MS) * widthCss;
    ctx.fillStyle = "#7a828c";
    ctx.font = "11px sans-serif";
    ctx.textAlign = "center";
    for (const t of gridlineWallMs) {
      const sessionMsAtT = sessionMs + (t - wallMs);
      const slotIndex = Math.round(sessionMsAtT / SLOT_WIDTH_MS);
      const slotNumber = ((slotIndex % sniffer.numSlots) + sniffer.numSlots) % sniffer.numSlots;
      const expectedLabel = slotNumber === 0 ? "0" : `${slotNumber + 1}`;
      const x = xForWallMs(widthCss, nowMs, t) + pixelsPerSlot / 2;
      ctx.fillText(expectedLabel, x, lanesBottom + 14);
    }
    ctx.textAlign = "left";
  }
}

function tick() {
  draw();
  requestAnimationFrame(tick);
}

function pruneEvents() {
  const cutoff = Date.now() - WINDOW_MS - 5000;
  while (sniffer.events.length && sniffer.events[0]._wallMs < cutoff) {
    sniffer.events.shift();
  }
}

function onSnifferEvent(ev) {
  ev._wallMs = Date.parse(ev.wall_t);
  ev._id = ++sniffer.idCounter;
  sniffer.events.push(ev);
  pruneEvents();

  if (ev.pkt_type === "TIME_SYNC" && ev.session_ms != null) {
    sniffer.lastAnchor = { wallMs: ev._wallMs, sessionMs: ev.session_ms };
  }
  if (ev.num_slots) {
    sniffer.numSlots = ev.num_slots;
  }
}

// --- Packet detail panel -------------------------------------------------

function fmtRhFlags(flags) {
  if (flags == null) return "—";
  const bits = [];
  if (flags & 0x80) bits.push("ACK");
  if (flags & 0x40) bits.push("RETRY");
  const appBits = flags & 0x0f;
  if (appBits) bits.push(`app=0x${appBits.toString(16)}`);
  const hex = `0x${flags.toString(16).padStart(2, "0")}`;
  return bits.length ? `${hex} (${bits.join(", ")})` : hex;
}

function fmtPayloadHex(hex) {
  if (!hex) return "(empty)";
  return hex.match(/.{1,2}/g).join(" ");
}

// RadioHead addresses use 1 for the base station (RadioHeadTdmaDriver::Config::
// radioHeadCfg(0x01) in main.cpp); display it as "0" to match the node_id=0
// "Base Station" convention used everywhere else in this UI.
function rhAddrLabel(addr) {
  if (addr == null) return "—";
  return addr === 1 ? "0" : String(addr);
}

function rhAddrName(addr) {
  if (addr == null) return "—";
  return addr === 1 ? "Base Station" : `Node ${addr}`;
}

// PKT_ACK_SUMMARY's payload is a 16-bit bitmap over a window of sequence
// numbers starting at ack_base_seq — see PACKET_RELIABILITY.md. Decode it
// into the actual (wrapping mod 256) seq numbers being acknowledged.
function ackedSeqs(baseSeq, mask) {
  if (baseSeq == null || mask == null) return undefined;
  const seqs = [];
  for (let bit = 0; bit < 16; bit++) {
    if (mask & (1 << bit)) seqs.push((baseSeq + bit) & 0xff);
  }
  return seqs;
}

function fmtAckedSeqs(baseSeq, mask) {
  const seqs = ackedSeqs(baseSeq, mask);
  if (seqs === undefined) return undefined;
  const maskHex = `0x${mask.toString(16).padStart(4, "0")}`;
  return seqs.length
    ? `${seqs.join(", ")}  (base=${baseSeq}, mask=${maskHex})`
    : `none  (base=${baseSeq}, mask=${maskHex})`;
}

// [label, getter, isFlagRow] — getter returning undefined hides the row.
const DETAIL_FIELDS = [
  ["Type", (ev) => ev.pkt_type, false],
  ["Node", (ev) => ev.node_id ?? "—", false],
  ["Seq", (ev) => ev.seq ?? "—", false],
  ["Target node", (ev) => ev.target_node_id, false],
  ["RSSI", (ev) => (ev.rssi != null ? `${ev.rssi} dBm` : "—"), false],
  ["SNR", (ev) => (ev.snr != null ? `${ev.snr} dB` : "—"), false],
  ["Jitter", (ev) => (ev.jitter_ms != null ? `${ev.jitter_ms} ms` : "—"), false],
  ["Guard violation", (ev) => (ev.guard_violation ? "YES" : "no"), (ev) => ev.guard_violation],
  ["Session ms", (ev) => ev.session_ms, false],
  ["Session id", (ev) => ev.session_id, false],
  ["Anchored to TIME_SYNC", (ev) => (ev.anchored ? "yes" : "no"), false],
  ["Num slots", (ev) => ev.num_slots, false],
  ["Battery %", (ev) => ev.battery_pct, false],
  ["UID hash", (ev) => (ev.uid_hash != null ? `0x${ev.uid_hash.toString(16)}` : undefined), false],
  ["RadioHead to", (ev) => ev.rh_to, false],
  ["RadioHead from", (ev) => ev.rh_from, false],
  ["RadioHead msg id", (ev) => ev.rh_id, false],
  ["RadioHead flags", (ev) => fmtRhFlags(ev.rh_flags), (ev) => ev.rh_is_ack || ev.rh_is_retry],
  [
    "Acker → Acked",
    (ev) => {
      if (ev.rh_is_ack) {
        // A RadioHead ACK's rh_from is whoever is transmitting THIS ack
        // frame (the receiver of the original message); rh_to is the
        // original sender being acknowledged. "0 → 2" reads as "node 0 is
        // acking node 2".
        return `${rhAddrLabel(ev.rh_from)} → ${rhAddrLabel(ev.rh_to)}  (${rhAddrName(ev.rh_from)} acks ${rhAddrName(ev.rh_to)})`;
      }
      if (ev.pkt_type === "ACK_SUMMARY" && ev.target_node_id != null) {
        // App-layer ACK_SUMMARY is always sent by the base (node_id 0 in
        // the SmartFires header); the node being acked is its own payload
        // field, decoded separately as target_node_id.
        return `0 → ${ev.target_node_id}  (Base Station acks Node ${ev.target_node_id})`;
      }
      return undefined;
    },
    true,
  ],
  [
    "Acked sequence numbers",
    (ev) => (ev.pkt_type === "ACK_SUMMARY" ? fmtAckedSeqs(ev.ack_base_seq, ev.ack_mask) : undefined),
    false,
  ],
  [
    "Attributed to (slot owner)",
    (ev) => (ev.rh_owner_node_id != null ? (ev.rh_owner_node_id === 0 ? "Base Station" : `Node ${ev.rh_owner_node_id}`) : undefined),
    false,
  ],
  ["Wall time", (ev) => ev.wall_t, false],
  ["Sniffer t (ms)", (ev) => ev.sniffer_t_ms, false],
  ["Payload length", (ev) => (ev.payload_hex ? ev.payload_hex.length / 2 : 0), false],
  ["Payload (hex)", (ev) => fmtPayloadHex(ev.payload_hex), false],
];

function renderDetail(ev) {
  const tbody = document.querySelector("#sniffer-detail-table tbody");
  tbody.innerHTML = "";
  for (const [label, getter, isFlag] of DETAIL_FIELDS) {
    const value = getter(ev);
    if (value === undefined) continue;
    const tr = document.createElement("tr");
    if (typeof isFlag === "function" ? isFlag(ev) : isFlag) {
      tr.classList.add("sniffer-detail-flag");
    }
    const tdLabel = document.createElement("td");
    tdLabel.textContent = label;
    const tdValue = document.createElement("td");
    tdValue.textContent = String(value);
    tr.append(tdLabel, tdValue);
    tbody.appendChild(tr);
  }
  document.getElementById("sniffer-detail-empty").style.display = "none";
  document.getElementById("sniffer-detail-table").style.display = "";
}

function selectEvent(ev) {
  sniffer.selectedId = ev._id;
  renderDetail(ev);
}

function onCanvasClick(e) {
  for (let i = sniffer.hitRects.length - 1; i >= 0; i--) {
    const r = sniffer.hitRects[i];
    if (e.offsetX >= r.x0 && e.offsetX <= r.x1 && e.offsetY >= r.y0 && e.offsetY <= r.y1) {
      selectEvent(r.ev);
      return;
    }
  }
}

function showDisabled() {
  sniffer.notConfigured = true;
  document.getElementById("sniffer-disabled").style.display = "block";
  document.getElementById("sniffer-canvas-wrapper").style.display = "none";
}

function connectSnifferSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${proto}//${location.host}/ws/sniffer`);
  sniffer.ws = ws;

  ws.onmessage = (msg) => {
    try {
      const data = JSON.parse(msg.data);
      if (data.event === "not_configured") {
        showDisabled();
        return;
      }
      onSnifferEvent(data);
    } catch (_) {}
  };

  ws.onclose = () => {
    sniffer.ws = null;
    if (!sniffer.notConfigured) {
      setTimeout(connectSnifferSocket, 2000);
    }
  };
}

async function pollStats() {
  if (sniffer.notConfigured) return;
  try {
    const stats = await Api.snifferStats();
    const tbody = document.querySelector("#sniffer-stats-table tbody");
    tbody.innerHTML = "";
    const sorted = Object.values(stats).sort((a, b) => a.node_id - b.node_id);
    for (const s of sorted) {
      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${s.node_id === 0 ? "Base" : s.node_id}</td>
        <td>${fmt(s.packets)}</td>
        <td>${fmt(s.avg_rssi)}</td>
        <td>${fmt(s.avg_snr)}</td>
        <td>${fmt(s.jitter_std_ms)}</td>
        <td>${fmt(s.guard_violations)}</td>
      `;
      tbody.appendChild(tr);
    }
  } catch (_) {}
}

function init() {
  renderNav(window.location.pathname);
  renderLegend();
  resizeCanvas();
  window.addEventListener("resize", resizeCanvas);
  document.getElementById("sniffer-timeline").addEventListener("click", onCanvasClick);
  connectSnifferSocket();
  requestAnimationFrame(tick);
  pollStats();
  setInterval(pollStats, 5000);
}

document.addEventListener("DOMContentLoaded", init);
