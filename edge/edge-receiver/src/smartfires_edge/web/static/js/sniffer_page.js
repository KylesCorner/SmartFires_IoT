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
//
// Playback model: `sniffer.live` controls whether the right edge of the
// canvas tracks Date.now() every frame (live tail) or is frozen at
// `sniffer.pausedViewEndMs` (paused/scrubbed). Zooming changes the visible
// window (`sniffer.windowMs`) without touching live/paused state; stepping
// back/forward always implies pausing first — there's no sensible "step"
// while the view is still chasing "now" every frame.

const ZOOM_PRESETS_MS = [15_000, 30_000, 60_000, 5 * 60_000, 15 * 60_000];
const DEFAULT_WINDOW_MS = 60_000;
// How far back the client keeps buffered events for scrubbing — independent
// of the visible window. Packet rate here is low (well under 1/s typical),
// so 30 min of buffered history costs nothing.
const MAX_RETAIN_MS = 30 * 60_000;
// Each step button press moves the view by half the current window.
const STEP_FRACTION = 0.5;

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
// Marks a packet whose rh_flags had RH_FLAGS_RETRY set — RHReliableDatagram
// resent it at the link layer after missing an ack. ACK_SUMMARY is by far
// the most common offender (the base still sends it with link-layer
// reliability on), but the dot is drawn for any packet type that qualifies.
const RESEND_DOT_COLOR = "#ffa500";

// --- Audio feedback ----------------------------------------------------
//
// The canvas already reads like a DAW timeline (swim lanes + time grid), so
// sound follows the same piano-roll convention: row position -> pitch,
// packet type -> timbre. That way the same packet type sounds recognizably
// the same on every row, just transposed. Pitch is quantized to a
// pentatonic scale (rather than a raw linear row->Hz mapping) so it stays
// musical as more node lanes fill in, instead of getting dense/atonal.
// TIME_SYNC is the one exception: it's already drawn as a full-height line
// across every lane (the session-clock anchor), so it gets its own
// fixed-pitch metronome "tick" rather than a row pitch.

const PENTATONIC_SEMITONES = [0, 2, 4, 7, 9]; // major pentatonic
const ROW_ROOT_HZ = 196; // G3
const TIME_SYNC_TICK_HZ = 1760; // A6 — well above any row's pitch, reads as a click track

function rowPitchHz(rowIndex) {
  const semitone = PENTATONIC_SEMITONES[rowIndex % 5] + 12 * Math.floor(rowIndex / 5);
  return ROW_ROOT_HZ * Math.pow(2, semitone / 12);
}

// RH/Unknown is always row 0 and Base Station is always row 1 (mirroring
// their fixed lanes above the node lanes); nodes ascend from row 2 in the
// same order as their canvas lane (see laneIndexFor()).
function rowIndexFor(ev) {
  if (ev.pkt_type === "RH_ACK" || ev.pkt_type === "RH_RAW") return 0;
  if (ev.node_id === 0) return 1;
  if (ev.node_id != null) return 2 + laneIndexFor(ev.node_id);
  return 0;
}

// [oscillator wave, duration, peak gain] per packet type, tuned so frequent
// benign traffic (BUNDLE/STATUS) stays soft and short, commands/handshakes
// stand out, and link-layer noise (RH_*) stays quiet in the background.
const PKT_TIMBRE = {
  BUNDLE: { wave: "triangle", durationMs: 90, gain: 0.12 },
  STATUS: { wave: "sine", durationMs: 140, gain: 0.14 },
  FULL_STATE: { wave: "triangle", durationMs: 90, gain: 0.12 },
  AWAKEN: { wave: "sawtooth", durationMs: 260, gain: 0.16, sweep: 1.5 }, // upward chirp
  ACK_SUMMARY: { wave: "square", durationMs: 60, gain: 0.08 },
  CMD_CALIBRATE: { wave: "sawtooth", durationMs: 180, gain: 0.18 },
  CMD_RESET: { wave: "sawtooth", durationMs: 180, gain: 0.18 },
  CMD_ACK: { wave: "sine", durationMs: 70, gain: 0.1 },
  RH_ACK: { wave: "sine", durationMs: 30, gain: 0.04 },
  RH_RAW: { wave: "square", durationMs: 50, gain: 0.05 },
};
const UNKNOWN_TIMBRE = { wave: "square", durationMs: 120, gain: 0.15, detune: -700 }; // dissonant

const audio = {
  ctx: null,
  enabled: false,
};

function initAudioContext() {
  if (!audio.ctx) {
    audio.ctx = new (window.AudioContext || window.webkitAudioContext)();
  }
  if (audio.ctx.state === "suspended") audio.ctx.resume();
}

function playTone(freq, { wave, durationMs, gain, sweep, detune }) {
  const ctx = audio.ctx;
  const now = ctx.currentTime;
  const osc = ctx.createOscillator();
  const amp = ctx.createGain();
  osc.type = wave;
  osc.frequency.setValueAtTime(freq, now);
  if (sweep) osc.frequency.exponentialRampToValueAtTime(freq * sweep, now + durationMs / 1000);
  if (detune) osc.detune.setValueAtTime(detune, now);
  amp.gain.setValueAtTime(gain, now);
  amp.gain.exponentialRampToValueAtTime(0.001, now + durationMs / 1000);
  osc.connect(amp).connect(ctx.destination);
  osc.start(now);
  osc.stop(now + durationMs / 1000 + 0.02);
}

function playPacketSound(ev) {
  if (!audio.enabled || !audio.ctx) return;

  if (ev.pkt_type === "TIME_SYNC") {
    playTone(TIME_SYNC_TICK_HZ, { wave: "square", durationMs: 35, gain: 0.06 });
    return;
  }

  const timbre = PKT_TIMBRE[ev.pkt_type] ?? UNKNOWN_TIMBRE;
  const freq = rowPitchHz(rowIndexFor(ev));
  playTone(freq, timbre);

  // Guard violations are the one thing already flagged visually (white
  // stroke) regardless of packet type, so layer a sharp dissonant overtone
  // on top rather than inventing a separate per-type "bad" timbre.
  if (ev.guard_violation) {
    playTone(freq * 1.05, { wave: "sawtooth", durationMs: 60, gain: 0.08, detune: -50 });
  }
}

function renderAudioToggle() {
  const btn = document.getElementById("sniffer-audio-toggle");
  btn.textContent = audio.enabled ? "🔊" : "🔇";
  btn.classList.toggle("active", audio.enabled);
  btn.title = audio.enabled ? "Mute sound effects" : "Enable sound effects";
}

function setAudioEnabled(enabled) {
  audio.enabled = enabled;
  if (enabled) initAudioContext();
  localStorage.setItem("snifferAudioEnabled", enabled ? "1" : "0");
  renderAudioToggle();
}

function initAudio() {
  audio.enabled = localStorage.getItem("snifferAudioEnabled") === "1";
  if (audio.enabled) initAudioContext();
  renderAudioToggle();
  document.getElementById("sniffer-audio-toggle").addEventListener("click", () => setAudioEnabled(!audio.enabled));
}

const sniffer = {
  events: [],            // buffered events within MAX_RETAIN_MS
  laneOf: new Map(),     // node_id -> lane index
  laneOrder: [],         // node_ids in lane order
  lastAnchor: null,      // {wallMs, sessionMs}
  numSlots: null,
  ws: null,
  notConfigured: false,
  idCounter: 0,
  selectedId: null,
  hitRects: [],          // canvas marker bounding boxes for click hit-testing
  windowMs: DEFAULT_WINDOW_MS,
  live: true,
  pausedViewEndMs: null, // set when paused; the timestamp at the canvas's right edge
};

// The timestamp at the canvas's right edge for this frame/action.
function currentViewEndMs() {
  return sniffer.live ? Date.now() : sniffer.pausedViewEndMs;
}

function clampViewEnd(ms, windowMs) {
  const maxEnd = Date.now();
  const minEnd = maxEnd - MAX_RETAIN_MS + windowMs;
  return Math.min(maxEnd, Math.max(minEnd, ms));
}

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

  const resendItem = document.createElement("div");
  resendItem.className = "sniffer-legend-item";
  const resendSwatch = document.createElement("span");
  resendSwatch.className = "sniffer-legend-swatch sniffer-legend-dot";
  resendSwatch.style.background = RESEND_DOT_COLOR;
  const resendLabel = document.createElement("span");
  resendLabel.textContent = "Resend (link-layer retry)";
  resendItem.append(resendSwatch, resendLabel);
  el.appendChild(resendItem);
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

function xForWallMs(canvasWidthCss, viewEndMs, windowMs, wallMs) {
  return canvasWidthCss - ((viewEndMs - wallMs) / windowMs) * canvasWidthCss;
}

function draw() {
  const canvas = document.getElementById("sniffer-timeline");
  const ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const widthCss = canvas.width / dpr;
  const heightCss = canvas.height / dpr;
  const viewEndMs = currentViewEndMs();
  const windowMs = sniffer.windowMs;

  ctx.clearRect(0, 0, widthCss, heightCss);
  const lanesBottom = heightCss - SLOT_LABEL_HEIGHT;

  // Slot boundary grid + guard bands, anchored to the last TIME_SYNC heard.
  // Also collect each gridline's wall-ms so the label row below can derive
  // which node is expected to transmit in that interval.
  const gridlineWallMs = [];
  if (sniffer.lastAnchor) {
    const { wallMs, sessionMs } = sniffer.lastAnchor;
    const phaseAnchor = ((wallMs - sessionMs) % SLOT_WIDTH_MS + SLOT_WIDTH_MS) % SLOT_WIDTH_MS;
    const earliest = viewEndMs - windowMs;
    let t = phaseAnchor + Math.ceil((earliest - phaseAnchor) / SLOT_WIDTH_MS) * SLOT_WIDTH_MS;
    const guardWidthPx = (GUARD_MS / windowMs) * widthCss;
    ctx.strokeStyle = "#3a4049";
    ctx.setLineDash([4, 4]);
    for (; t <= viewEndMs + SLOT_WIDTH_MS; t += SLOT_WIDTH_MS) {
      gridlineWallMs.push(t);
      const x = xForWallMs(widthCss, viewEndMs, windowMs, t);
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
    if (wallMs < viewEndMs - windowMs || wallMs > viewEndMs) continue;
    const x = xForWallMs(widthCss, viewEndMs, windowMs, wallMs);
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
    if (ev.rh_is_retry) {
      ctx.fillStyle = RESEND_DOT_COLOR;
      ctx.beginPath();
      ctx.arc(x, y - 4, 2.5, 0, Math.PI * 2);
      ctx.fill();
    }
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
    const pixelsPerSlot = (SLOT_WIDTH_MS / windowMs) * widthCss;
    ctx.fillStyle = "#7a828c";
    ctx.font = "11px sans-serif";
    ctx.textAlign = "center";
    for (const t of gridlineWallMs) {
      const sessionMsAtT = sessionMs + (t - wallMs);
      const slotIndex = Math.round(sessionMsAtT / SLOT_WIDTH_MS);
      const slotNumber = ((slotIndex % sniffer.numSlots) + sniffer.numSlots) % sniffer.numSlots;
      const expectedLabel = slotNumber === 0 ? "0" : `${slotNumber + 1}`;
      const x = xForWallMs(widthCss, viewEndMs, windowMs, t) + pixelsPerSlot / 2;
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
  // Retention is independent of the visible window — it bounds how far back
  // scrubbing can go, not what's currently drawn. Always measured from real
  // time so the buffer keeps growing even while paused.
  const cutoff = Date.now() - MAX_RETAIN_MS;
  while (sniffer.events.length && sniffer.events[0]._wallMs < cutoff) {
    sniffer.events.shift();
  }
}

function onSnifferEvent(ev) {
  ev._wallMs = Date.parse(ev.wall_t);
  ev._id = ++sniffer.idCounter;
  sniffer.events.push(ev);
  pruneEvents();
  playPacketSound(ev);

  if (ev.pkt_type === "TIME_SYNC" && ev.session_ms != null) {
    sniffer.lastAnchor = { wallMs: ev._wallMs, sessionMs: ev.session_ms };
  }
  if (ev.num_slots) {
    sniffer.numSlots = ev.num_slots;
  }
}

// --- Zoom + playback controls ---------------------------------------------

function fmtWindowLabel(ms) {
  return ms < 60_000 ? `${ms / 1000}s` : `${ms / 60_000}m`;
}

function renderZoomButtons() {
  const container = document.getElementById("sniffer-zoom-buttons");
  container.innerHTML = "";
  for (const ms of ZOOM_PRESETS_MS) {
    const btn = document.createElement("button");
    btn.className = "time-range-btn" + (sniffer.windowMs === ms ? " active" : "");
    btn.textContent = fmtWindowLabel(ms);
    btn.addEventListener("click", () => setWindowMs(ms));
    container.appendChild(btn);
  }
}

function setWindowMs(ms) {
  if (!sniffer.live) {
    // Zooming while paused keeps the center of the current view stable,
    // rather than anchoring to either edge.
    const center = sniffer.pausedViewEndMs - sniffer.windowMs / 2;
    sniffer.pausedViewEndMs = clampViewEnd(center + ms / 2, ms);
  }
  sniffer.windowMs = ms;
  renderZoomButtons();
}

function renderPlaybackUI() {
  const toggleBtn = document.getElementById("sniffer-play-toggle");
  const indicator = document.getElementById("sniffer-live-indicator");
  if (sniffer.live) {
    toggleBtn.textContent = "⏸ Pause";
    indicator.innerHTML = `<span class="conn-dot online"></span> LIVE`;
  } else {
    toggleBtn.textContent = "▶ Go Live";
    const start = new Date(sniffer.pausedViewEndMs - sniffer.windowMs);
    const end = new Date(sniffer.pausedViewEndMs);
    indicator.innerHTML =
      `<span class="conn-dot offline"></span> Viewing ${start.toLocaleTimeString()} – ${end.toLocaleTimeString()}`;
  }
}

function setLive(isLive) {
  if (isLive) {
    sniffer.live = true;
    sniffer.pausedViewEndMs = null;
  } else if (sniffer.live) {
    sniffer.pausedViewEndMs = Date.now();
    sniffer.live = false;
  }
  renderPlaybackUI();
}

function togglePlay() {
  setLive(!sniffer.live);
}

function stepView(direction) {
  setLive(false); // stepping always means "look away from now"
  const delta = direction * sniffer.windowMs * STEP_FRACTION;
  sniffer.pausedViewEndMs = clampViewEnd(sniffer.pausedViewEndMs + delta, sniffer.windowMs);
  renderPlaybackUI();
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

// PKT_ACK_SUMMARY's payload is ack_base_seq (highest contiguous seq acked)
// plus a 16-bit bitmap where bit N set means (ack_base_seq + N + 1) is also
// acked — see PACKET_RELIABILITY.md. ack_base_seq itself is always acked
// (it's the cumulative high-water mark), even when the mask is all zero.
function ackedSeqs(baseSeq, mask) {
  if (baseSeq == null || mask == null) return undefined;
  const seqs = [baseSeq & 0xff];
  for (let bit = 0; bit < 16; bit++) {
    if (mask & (1 << bit)) seqs.push((baseSeq + bit + 1) & 0xff);
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

// --- Sub-tabs (Timeline / Slot Activity / Plots) ----------------------------

let snifferActiveTab = "timeline";

function setActiveTab(tab) {
  snifferActiveTab = tab;
  document.querySelectorAll(".sniffer-subnav-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.tab === tab);
  });
  document.querySelectorAll(".sniffer-tab").forEach((el) => {
    el.classList.toggle("active", el.id === `sniffer-tab-${tab}`);
  });
  if (tab === "activity") refreshActivityTable();
  if (tab === "plots") refreshPlots();
}

function initSubnav() {
  document.querySelectorAll(".sniffer-subnav-btn").forEach((btn) => {
    btn.addEventListener("click", () => setActiveTab(btn.dataset.tab));
  });
}

// --- Shared TDMA slot-match helper -------------------------------------------
//
// Mirrors the firmware's compile-time slot=(node_id-1)%num_slots assignment
// (same convention as the expected-transmitter label row in draw() above) to
// flag packets whose node_id doesn't match the slot their session_ms falls
// in. Purely derived from fields the backend already emits per event
// (session_ms, num_slots, node_id) — no new wire data needed.

function frameAndSlotFor(sessionMs, numSlots) {
  const framePeriodMs = numSlots * SLOT_WIDTH_MS;
  const framePhase = ((sessionMs % framePeriodMs) + framePeriodMs) % framePeriodMs;
  return {
    frame: Math.floor(sessionMs / framePeriodMs),
    slot: Math.floor(framePhase / SLOT_WIDTH_MS),
  };
}

function expectedNodeForSlot(slot) {
  return slot === 0 ? 0 : slot + 1;
}

// Returns null when slot-match isn't applicable: TIME_SYNC is the anchor
// itself, bare RadioHead frames carry no SmartFires node_id, or we haven't
// heard a TIME_SYNC yet (no num_slots).
function slotInfoFor(ev) {
  if (ev.pkt_type === "TIME_SYNC" || ev.node_id == null || ev.session_ms == null || !sniffer.numSlots) {
    return null;
  }
  const { frame, slot } = frameAndSlotFor(ev.session_ms, sniffer.numSlots);
  const expected = expectedNodeForSlot(slot);
  return { frame, slot, expected, match: ev.node_id === expected };
}

// --- Top-line summary counters ----------------------------------------------

function renderSummaryCard(container, value, label, warn) {
  const card = document.createElement("div");
  card.className = "sniffer-summary-card" + (warn ? " warn" : "");
  const v = document.createElement("div");
  v.className = "sniffer-summary-value";
  v.textContent = value;
  const l = document.createElement("div");
  l.className = "sniffer-summary-label";
  l.textContent = label;
  card.append(v, l);
  container.appendChild(card);
}

function refreshSummary() {
  const container = document.getElementById("sniffer-summary-row");
  if (!container) return;
  container.innerHTML = "";

  const events = sniffer.events;
  const now = Date.now();
  const last60s = events.filter((ev) => ev._wallMs >= now - 60_000).length;
  const nodeIds = new Set(events.filter((ev) => ev.node_id != null).map((ev) => ev.node_id));
  const rssiSamples = events.filter((ev) => ev.rssi != null).slice(-100).map((ev) => ev.rssi);
  const avgRssi = rssiSamples.length
    ? rssiSamples.reduce((a, b) => a + b, 0) / rssiSamples.length
    : null;

  let guardViolations = 0;
  let wrongSlot = 0;
  for (const ev of events) {
    if (ev.guard_violation) guardViolations++;
    const info = slotInfoFor(ev);
    if (info && !info.match) wrongSlot++;
  }

  renderSummaryCard(container, fmt(events.length), "Buffered Packets");
  renderSummaryCard(container, fmt(last60s), "Last 60s");
  renderSummaryCard(container, fmt(nodeIds.size), "Unique Nodes");
  renderSummaryCard(container, avgRssi != null ? `${avgRssi.toFixed(1)} dBm` : "—", "Avg RSSI (last 100)");
  renderSummaryCard(container, fmt(wrongSlot), "Wrong-Slot Packets", wrongSlot > 0);
  renderSummaryCard(container, fmt(guardViolations), "Guard Violations", guardViolations > 0);
}

// --- Slot Activity tab: per (frame, slot) table -----------------------------

const MAX_ACTIVITY_FRAMES = 40;

function refreshActivityTable() {
  const tbody = document.querySelector("#sniffer-activity-table tbody");
  if (!tbody) return;
  tbody.innerHTML = "";
  if (!sniffer.numSlots) return;

  const groups = new Map(); // "frame:slot" -> aggregate
  for (const ev of sniffer.events) {
    const info = slotInfoFor(ev);
    if (!info) continue;
    const key = `${info.frame}:${info.slot}`;
    let g = groups.get(key);
    if (!g) {
      g = {
        frame: info.frame,
        slot: info.slot,
        expected: info.expected,
        nodes: new Set(),
        packets: 0,
        rssiSum: 0,
        rssiCount: 0,
        jitterMin: null,
        jitterMax: null,
        guardViolations: 0,
        wrongSlot: 0,
      };
      groups.set(key, g);
    }
    g.packets++;
    g.nodes.add(ev.node_id);
    if (ev.rssi != null) {
      g.rssiSum += ev.rssi;
      g.rssiCount++;
    }
    if (ev.jitter_ms != null) {
      g.jitterMin = g.jitterMin == null ? ev.jitter_ms : Math.min(g.jitterMin, ev.jitter_ms);
      g.jitterMax = g.jitterMax == null ? ev.jitter_ms : Math.max(g.jitterMax, ev.jitter_ms);
    }
    if (ev.guard_violation) g.guardViolations++;
    if (!info.match) g.wrongSlot++;
  }

  const frames = [...new Set([...groups.values()].map((g) => g.frame))]
    .sort((a, b) => b - a)
    .slice(0, MAX_ACTIVITY_FRAMES);
  const frameSet = new Set(frames);

  const rows = [...groups.values()]
    .filter((g) => frameSet.has(g.frame))
    .sort((a, b) => b.frame - a.frame || a.slot - b.slot);

  for (const g of rows) {
    const tr = document.createElement("tr");
    if (g.wrongSlot > 0) tr.classList.add("sniffer-row-warn");
    const seenFrom = [...g.nodes].sort((a, b) => a - b).map((n) => (n === 0 ? "Base" : n)).join(", ");
    const avgRssi = g.rssiCount ? (g.rssiSum / g.rssiCount).toFixed(1) : "—";
    const jitterRange = g.jitterMin != null ? `${g.jitterMin.toFixed(1)} … ${g.jitterMax.toFixed(1)}` : "—";
    tr.innerHTML = `
      <td>${g.frame}</td>
      <td>${g.slot}</td>
      <td>${g.expected === 0 ? "Base" : g.expected}</td>
      <td>${seenFrom}</td>
      <td>${g.packets}</td>
      <td>${avgRssi}</td>
      <td>${jitterRange}</td>
      <td>${g.guardViolations}</td>
      <td>${g.wrongSlot > 0 ? g.wrongSlot : "—"}</td>
    `;
    tbody.appendChild(tr);
  }
}

// --- Plots tab: RSSI / jitter over time --------------------------------------

const NODE_COLOR_PALETTE = ["#3b82c4", "#3fae5c", "#d4b340", "#9b59b6", "#e8743a", "#5dade2", "#c0392b", "#1abc9c"];

function colorForNode(nodeId) {
  if (nodeId == null) return "#6b7280";
  return NODE_COLOR_PALETTE[((nodeId % NODE_COLOR_PALETTE.length) + NODE_COLOR_PALETTE.length) % NODE_COLOR_PALETTE.length];
}

function plotScales() {
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
    y: {
      ticks: { color: "#aab4c0" },
      grid: { color: "#2a2f36" },
    },
  };
}

let rssiChart = null;
let jitterChart = null;

function initPlots() {
  rssiChart = new Chart(document.getElementById("sniffer-rssi-chart").getContext("2d"), {
    type: "scatter",
    data: { datasets: [] },
    options: {
      animation: false,
      maintainAspectRatio: false,
      scales: plotScales(),
      plugins: { legend: { labels: { color: "#e6e6e6" } } },
    },
  });

  jitterChart = new Chart(document.getElementById("sniffer-jitter-chart").getContext("2d"), {
    type: "scatter",
    data: { datasets: [] },
    options: {
      animation: false,
      maintainAspectRatio: false,
      scales: plotScales(),
      plugins: { legend: { labels: { color: "#e6e6e6" } } },
    },
  });
}

function refreshPlots() {
  if (!rssiChart || !jitterChart) return;

  const byNode = new Map(); // node_id, or "rh" for bare RadioHead frames -> events
  for (const ev of sniffer.events) {
    if (ev.pkt_type === "TIME_SYNC") continue;
    const key = ev.node_id != null ? ev.node_id : "rh";
    if (!byNode.has(key)) byNode.set(key, []);
    byNode.get(key).push(ev);
  }

  const rssiDatasets = [];
  const jitterDatasets = [];
  for (const [key, evs] of byNode) {
    const nodeId = key === "rh" ? null : key;
    const label = nodeId == null ? "RadioHead / Unknown" : laneLabel(nodeId);
    const color = colorForNode(nodeId);

    const rssiPoints = evs.filter((ev) => ev.rssi != null).map((ev) => ({ x: ev._wallMs, y: ev.rssi }));
    if (rssiPoints.length) {
      rssiDatasets.push({ label, data: rssiPoints, backgroundColor: color, pointRadius: 3 });
    }

    const jitterEvs = evs.filter((ev) => ev.jitter_ms != null);
    if (jitterEvs.length) {
      jitterDatasets.push({
        label,
        data: jitterEvs.map((ev) => ({ x: ev._wallMs, y: ev.jitter_ms })),
        backgroundColor: jitterEvs.map((ev) => (ev.guard_violation ? "#e74c3c" : color)),
        pointRadius: 3,
      });
    }
  }

  rssiChart.data.datasets = rssiDatasets;
  rssiChart.update();

  jitterChart.data.datasets = jitterDatasets;
  jitterChart.update();
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
        <td>${fmt(s.ack_summary_resends)}</td>
        <td>${fmtTime(s.last_seen)}</td>
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
  renderZoomButtons();
  renderPlaybackUI();
  document.getElementById("sniffer-play-toggle").addEventListener("click", togglePlay);
  document.getElementById("sniffer-step-back").addEventListener("click", () => stepView(-1));
  document.getElementById("sniffer-step-forward").addEventListener("click", () => stepView(1));
  initAudio();
  initSubnav();
  initPlots();
  connectSnifferSocket();
  requestAnimationFrame(tick);
  pollStats();
  setInterval(pollStats, 5000);
  refreshSummary();
  setInterval(refreshSummary, 2000);
  setInterval(() => {
    if (snifferActiveTab === "activity") refreshActivityTable();
    if (snifferActiveTab === "plots") refreshPlots();
  }, 2000);
}

document.addEventListener("DOMContentLoaded", init);
