// TDMA sniffer timeline: swim-lane canvas + stats table + packet log.
//
// Slot boundaries are derived purely from the most recently anchored event's
// (wall-clock-ms, session_ms) pair plus the fixed 900ms slot width — see
// sniffer_service.py for why that's sufficient (every slot boundary repeats
// every slot_width_ms, so num_slots isn't needed client-side for the grid).

const WINDOW_MS = 60_000;
const SLOT_WIDTH_MS = 900;
const GUARD_MS = 20;
const LANE_HEIGHT = 36;
const HEADER_HEIGHT = 24;
const SLOT_LABEL_HEIGHT = 20;
const LOG_MAX_LINES = 1000;

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
  logLines: 0,
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
  return HEADER_HEIGHT + Math.max(1, sniffer.laneOrder.length) * LANE_HEIGHT + SLOT_LABEL_HEIGHT;
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

  // Lane labels + separators.
  ctx.fillStyle = "#aab4c0";
  ctx.font = "11px sans-serif";
  sniffer.laneOrder.forEach((nodeId, idx) => {
    const y = HEADER_HEIGHT + idx * LANE_HEIGHT;
    ctx.fillText(laneLabel(nodeId), 4, y + 14);
    ctx.strokeStyle = "#2a2f36";
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(widthCss, y);
    ctx.stroke();
  });

  // Events.
  for (const ev of sniffer.events) {
    const wallMs = ev._wallMs;
    if (wallMs < nowMs - WINDOW_MS) continue;
    const x = xForWallMs(widthCss, nowMs, wallMs);
    const color = PKT_COLORS[ev.pkt_type] ?? UNKNOWN_COLOR;

    if (ev.pkt_type === "TIME_SYNC") {
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(x, HEADER_HEIGHT);
      ctx.lineTo(x, lanesBottom);
      ctx.stroke();
      ctx.lineWidth = 1;
      continue;
    }

    const laneIdx = ev.node_id != null ? laneIndexFor(ev.node_id) : null;
    const y = laneIdx != null ? HEADER_HEIGHT + laneIdx * LANE_HEIGHT + LANE_HEIGHT / 2 - 8 : HEADER_HEIGHT;
    ctx.fillStyle = color;
    if (ev.guard_violation) {
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 1.5;
      ctx.strokeRect(x - 3, y, 7, 16);
    }
    ctx.fillRect(x - 3, y, 7, 16);
  }

  // Expected-node label row: which node should be transmitting in each slot
  // interval, per the firmware's compile-time slot=(node_id-1)%num_slots
  // assignment (node IDs are handed out sequentially starting at 1, so the
  // inverse is simply node = slot + 1).
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
      const expectedNode = (((slotIndex % sniffer.numSlots) + sniffer.numSlots) % sniffer.numSlots) + 1;
      const x = xForWallMs(widthCss, nowMs, t) + pixelsPerSlot / 2;
      ctx.fillText(`${expectedNode}`, x, lanesBottom + 14);
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
  sniffer.events.push(ev);
  pruneEvents();

  if (ev.pkt_type === "TIME_SYNC" && ev.session_ms != null) {
    sniffer.lastAnchor = { wallMs: ev._wallMs, sessionMs: ev.session_ms };
  }
  if (ev.num_slots) {
    sniffer.numSlots = ev.num_slots;
  }

  appendSnifferLog(ev);
}

function appendSnifferLog(ev) {
  const el = document.getElementById("sniffer-log-output");
  const time = new Date(ev._wallMs).toISOString().slice(11, 23);
  const jitter = ev.jitter_ms != null ? `${ev.jitter_ms > 0 ? "+" : ""}${ev.jitter_ms}ms` : "—";
  const target = ev.target_node_id != null ? ` target=${ev.target_node_id}` : "";
  const line = `${time}  ${ev.pkt_type.padEnd(10)} node=${ev.node_id ?? "—"}${target} rssi=${ev.rssi ?? "—"} snr=${ev.snr ?? "—"} jitter=${jitter}${ev.guard_violation ? "  GUARD-VIOLATION" : ""}`;

  sniffer.logLines += 1;
  const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
  el.textContent += line + "\n";
  if (sniffer.logLines > LOG_MAX_LINES) {
    const firstNewline = el.textContent.indexOf("\n");
    if (firstNewline !== -1) {
      el.textContent = el.textContent.slice(firstNewline + 1);
    }
    sniffer.logLines -= 1;
  }
  if (atBottom) {
    el.scrollTop = el.scrollHeight;
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
  connectSnifferSocket();
  requestAnimationFrame(tick);
  pollStats();
  setInterval(pollStats, 5000);
}

document.addEventListener("DOMContentLoaded", init);
