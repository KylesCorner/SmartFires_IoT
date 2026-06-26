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
let _clockOffsetMs = 0; // Jetson epoch_s*1000 - Date.now(), resynced periodically

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
    const { connected, error } = await resp.json();
    if (_baseLinkDot) {
      _baseLinkDot.className = "base-link-dot " + (connected ? "online" : "offline");
      _baseLinkDot.title = connected
        ? "Base station USB link: connected — the Jetson is receiving telemetry from the base station Feather"
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
    _clockEl.textContent = new Date(Date.now() + _clockOffsetMs).toLocaleTimeString();
  }
}
