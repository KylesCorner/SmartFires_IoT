const NAV_LINKS = [
  { href: "/", label: "Main" },
  { href: "/map-history", label: "Map & History" },
  { href: "/sniffer", label: "TDMA Sniffer" },
  { href: "/debug", label: "Debug Log" },
  { href: "/live-log", label: "Live Log" },
];

let _connDot = null;
let _baseLinkDot = null;
let _clockEl = null;
let _sessionEl = null;
let _clockOffsetMs = 0; // Jetson epoch_s*1000 - Date.now(), resynced periodically
let _observedSessionId = null;
let _sessionReloadStarted = false;

try {
  _observedSessionId = window.sessionStorage.getItem("smartfires-session-id");
} catch (_) {}

// Absolute timestamps are local, dated, and carry a numeric UTC offset.
// Input units are explicit; timezone-less ISO values are historical UTC.
function timestampDate(value, inputType = "iso") {
  if (value === null || value === undefined || value === "") return null;
  if (inputType === "epoch-seconds") return new Date(Number(value) * 1000);
  if (inputType === "epoch-milliseconds") return new Date(Number(value));
  if (inputType !== "iso") throw new Error(`Unsupported timestamp input type: ${inputType}`);
  const text = String(value);
  return new Date(/(?:Z|[+-]\d\d:?\d\d)$/.test(text) ? text : `${text}Z`);
}

function formatTimestamp(value, inputType = "iso", options = {}) {
  const date = value instanceof Date ? value : timestampDate(value, inputType);
  if (!date || Number.isNaN(date.getTime())) return "—";
  // Keep the calendar portion ISO-like regardless of browser locale so it is
  // unambiguous in screenshots, exports, and DST investigations.
  const datePart = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
  const timePart = date.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
  const milliseconds = options.milliseconds ? `.${String(date.getMilliseconds()).padStart(3, "0")}` : "";
  const offsetMinutes = -date.getTimezoneOffset();
  const sign = offsetMinutes >= 0 ? "+" : "-";
  const absoluteOffset = Math.abs(offsetMinutes);
  const offset = `UTC${sign}${String(Math.floor(absoluteOffset / 60)).padStart(2, "0")}:${String(absoluteOffset % 60).padStart(2, "0")}`;
  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone || "local";
  return `${datePart} ${timePart}${milliseconds} ${zone} (${offset})`;
}

function renderNav(activePath) {
  const nav = document.createElement("nav");
  nav.className = "topnav";
  for (const link of NAV_LINKS) {
    const a = document.createElement("a");
    a.href = link.href;
    a.textContent = link.label;
    if (link.href === activePath) {
      a.classList.add("active");
    }
    nav.appendChild(a);
  }

  _clockEl = document.createElement("span");
  _clockEl.className = "nav-clock";
  _clockEl.textContent = "--:--:--";
  _clockEl.title = "Jetson system clock — the time the edge computer itself is reporting";
  nav.appendChild(_clockEl);

  _sessionEl = document.createElement("span");
  _sessionEl.className = "nav-clock";
  _sessionEl.textContent = "session —";
  _sessionEl.title = "Current ingest session id — changes when a new session is started";
  nav.appendChild(_sessionEl);

  _connDot = document.createElement("span");
  _connDot.className = "conn-dot";
  _connDot.title = "Map tile connectivity: checking whether the Jetson can reach the internet to fetch new map tiles…";
  nav.appendChild(_connDot);

  _baseLinkDot = document.createElement("span");
  _baseLinkDot.className = "base-link-dot";
  _baseLinkDot.title = "Base station USB link: checking whether the Jetson can talk to the base station Feather over serial…";
  nav.appendChild(_baseLinkDot);

  document.body.prepend(nav);

  _pollConnectivity();
  setInterval(_pollConnectivity, 30_000);
  _pollBaseLink();
  setInterval(_pollBaseLink, 3_000);
  _pollServerTime();
  setInterval(_pollServerTime, 30_000);
  setInterval(_tickClock, 1000);
  _pollSession();
  setInterval(_pollSession, 5_000);
}

async function _pollConnectivity() {
  try {
    const resp = await fetch("/api/connectivity");
    const { online } = await resp.json();
    if (_connDot) {
      _connDot.className = "conn-dot " + (online ? "online" : "offline");
      _connDot.title = online
        ? "Map tile connectivity: online — new map areas will be cached automatically"
        : "Map tile connectivity: offline — only previously-cached map tiles are available";
    }
  } catch (_) {}
}

async function _pollBaseLink() {
  try {
    const resp = await fetch("/api/base_link");
    const { connected, ready, error } = await resp.json();
    if (_baseLinkDot) {
      _baseLinkDot.className = "base-link-dot " + (ready ? "online" : "offline");
      _baseLinkDot.title = ready
        ? "Base station USB link: ready — the port is open and startup reset/time synchronization completed"
        : connected
          ? "Base station USB link: connected — startup reset/time synchronization is still in progress"
        : "Base station USB link: disconnected — the Jetson cannot reach the base station Feather over serial" + (error ? ` (${error})` : "");
    }
  } catch (_) {}
}

async function _pollServerTime() {
  try {
    const resp = await fetch("/api/server_time");
    const { epoch_s } = await resp.json();
    _clockOffsetMs = epoch_s * 1000 - Date.now();
    _tickClock();
  } catch (_) {}
}

function _tickClock() {
  if (_clockEl) {
    _clockEl.textContent = formatTimestamp(Date.now() + _clockOffsetMs, "epoch-milliseconds");
  }
}

async function _pollSession() {
  try {
    const resp = await fetch("/api/session", { cache: "no-store" });
    const { session_id } = await resp.json();
    if (_sessionEl) {
      _sessionEl.textContent = session_id ? `session ${session_id}` : "session —";
    }
    if (session_id && _observedSessionId === null) {
      _observedSessionId = session_id;
      try { window.sessionStorage.setItem("smartfires-session-id", session_id); } catch (_) {}
    } else if (session_id && session_id !== _observedSessionId && !_sessionReloadStarted) {
      // Reloading is the common reset path for every page and every tab. It
      // discards charts, websocket rings, sniffer anchors, filters, and caches
      // even when this tab did not submit the New Session request itself.
      _sessionReloadStarted = true;
      _observedSessionId = session_id;
      try { window.sessionStorage.setItem("smartfires-session-id", session_id); } catch (_) {}
      window.location.reload();
    }
  } catch (_) {}
}
