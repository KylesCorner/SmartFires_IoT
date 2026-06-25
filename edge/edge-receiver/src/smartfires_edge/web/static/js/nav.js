const NAV_LINKS = [
  { href: "/", label: "Main" },
  { href: "/map-history", label: "Map & History" },
  { href: "/sniffer", label: "TDMA Sniffer" },
  { href: "/debug", label: "Debug Log" },
  { href: "/live-log", label: "Live Log" },
];

let _connDot = null;
let _baseLinkDot = null;

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

  _connDot = document.createElement("span");
  _connDot.className = "conn-dot";
  _connDot.title = "Checking map tile connectivity…";
  nav.appendChild(_connDot);

  _baseLinkDot = document.createElement("span");
  _baseLinkDot.className = "base-link-dot";
  _baseLinkDot.title = "Checking base station link…";
  nav.appendChild(_baseLinkDot);

  document.body.prepend(nav);

  _pollConnectivity();
  setInterval(_pollConnectivity, 30_000);
  _pollBaseLink();
  setInterval(_pollBaseLink, 3_000);
}

async function _pollConnectivity() {
  try {
    const resp = await fetch("/api/connectivity");
    const { online } = await resp.json();
    if (_connDot) {
      _connDot.className = "conn-dot " + (online ? "online" : "offline");
      _connDot.title = online
        ? "Map tiles: online — new areas will be cached automatically"
        : "Map tiles: offline — using cached tiles";
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
        ? "Base station: connected"
        : "Base station: disconnected" + (error ? ` — ${error}` : "");
    }
  } catch (_) {}
}
