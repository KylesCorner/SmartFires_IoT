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
let _networkProfileButton = null;
let _networkProfilePanel = null;
let _clockOffsetMs = 0; // Jetson epoch_s*1000 - Date.now(), resynced periodically
let _observedSessionId = null;
let _sessionReloadStarted = false;

// Latest normalized /api/network_profile response. Pages with timing-sensitive
// views consume this directly and also listen for smartfires:network-profile.
window.smartfiresNetworkProfile = null;

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

  const profileControl = document.createElement("div");
  profileControl.className = "network-profile-control";
  _networkProfileButton = document.createElement("button");
  _networkProfileButton.type = "button";
  _networkProfileButton.className = "network-profile-badge unknown";
  _networkProfileButton.textContent = "SF unknown";
  _networkProfileButton.setAttribute("aria-expanded", "false");
  _networkProfileButton.title = "No network profile has been announced by the base yet";
  _networkProfilePanel = document.createElement("div");
  _networkProfilePanel.className = "network-profile-panel";
  _networkProfilePanel.hidden = true;
  _networkProfileButton.addEventListener("click", (event) => {
    event.stopPropagation();
    _networkProfilePanel.hidden = !_networkProfilePanel.hidden;
    _networkProfileButton.setAttribute("aria-expanded", String(!_networkProfilePanel.hidden));
  });
  _networkProfilePanel.addEventListener("click", (event) => event.stopPropagation());
  profileControl.append(_networkProfileButton, _networkProfilePanel);
  nav.appendChild(profileControl);

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

  document.addEventListener("click", () => {
    if (_networkProfilePanel) _networkProfilePanel.hidden = true;
    if (_networkProfileButton) _networkProfileButton.setAttribute("aria-expanded", "false");
  });

  _pollConnectivity();
  setInterval(_pollConnectivity, 30_000);
  _pollBaseLink();
  setInterval(_pollBaseLink, 3_000);
  _pollServerTime();
  setInterval(_pollServerTime, 30_000);
  setInterval(_tickClock, 1000);
  _pollSession();
  setInterval(_pollSession, 5_000);
  _pollNetworkProfile();
  setInterval(_pollNetworkProfile, 5_000);
}

function _profileCodingRate(profile) {
  if (profile?.coding_rate) return profile.coding_rate;
  return profile?.coding_rate_denominator ? `4/${profile.coding_rate_denominator}` : "—";
}

function _profileBandwidth(profile) {
  if (!profile?.bandwidth_hz) return "—";
  return profile.bandwidth_hz % 1000 === 0
    ? `${profile.bandwidth_hz / 1000} kHz`
    : `${profile.bandwidth_hz} Hz`;
}

function _profileDuration(value) {
  if (value === null || value === undefined) return "—";
  return value >= 1000 && value % 1000 === 0 ? `${value / 1000} s` : `${value} ms`;
}

function _profileIdentity(profile) {
  if (!profile) return "—";
  const id = profile.profile_id ?? (profile.spreading_factor ? `sf${profile.spreading_factor}` : "unknown");
  return profile.fingerprint ? `${id} · ${profile.fingerprint}` : id;
}

function _appendProfileDetail(label, value, className = "") {
  const row = document.createElement("div");
  row.className = "network-profile-detail" + (className ? ` ${className}` : "");
  const key = document.createElement("span");
  key.textContent = label;
  const text = document.createElement("strong");
  text.textContent = value ?? "—";
  row.append(key, text);
  _networkProfilePanel.appendChild(row);
}

function _renderNetworkProfile(status) {
  if (!_networkProfileButton || !_networkProfilePanel) return;
  const profile = status?.active;
  const mismatches = status?.mismatches ?? [];
  const mismatch = status?.state === "mismatch" || mismatches.length > 0;
  _networkProfilePanel.innerHTML = "";

  if (!profile) {
    _networkProfileButton.className = "network-profile-badge unknown";
    _networkProfileButton.textContent = "SF unknown";
    _networkProfileButton.title = "No valid network profile has been announced by the base yet";
    _appendProfileDetail("Network profile", "Unknown — waiting for the base announcement", "warning");
    return;
  }

  const sf = profile.spreading_factor ?? "?";
  const source = status.source ?? "unknown source";
  const badgeState = mismatch ? "mismatch" : source === "override" ? "override" : "active";
  _networkProfileButton.className = `network-profile-badge ${badgeState}`;
  _networkProfileButton.textContent = `${mismatch ? "⚠ " : ""}SF${sf} · ${_profileBandwidth(profile)} · ${_profileCodingRate(profile)} · ${source}`;
  _networkProfileButton.title = `${profile.profile_id ?? `SF${sf}`} from ${source}; click for network timing details`;

  _appendProfileDetail("Profile", profile.profile_id ?? `sf${sf}`);
  _appendProfileDetail("Authority", `${status.state}${status.source ? ` · ${status.source}` : ""}`,
    mismatch ? "warning" : "");
  _appendProfileDetail("Radio", `SF${sf} · ${_profileBandwidth(profile)} · CR ${_profileCodingRate(profile)}`);
  _appendProfileDetail(
    "TDMA",
    `${profile.num_slots ?? "—"} slots · ${_profileDuration(profile.slot_width_ms)} width · ${_profileDuration(profile.guard_ms)} guard`
  );
  _appendProfileDetail("Continuous sample", _profileDuration(profile.continuous_sample_period_ms));
  _appendProfileDetail("Timed sample", _profileDuration(profile.timed_sample_period_ms));
  _appendProfileDetail("STATUS interval", _profileDuration(profile.status_interval_ms));
  _appendProfileDetail("Bundle cap", profile.max_bundle_deltas == null ? "—" : `${profile.max_bundle_deltas} deltas`);
  _appendProfileDetail("Fingerprint", profile.fingerprint ?? "—");
  if (mismatch) {
    _appendProfileDetail("Base profile", _profileIdentity(status.base), "warning");
    if (status.override) _appendProfileDetail("Override profile", _profileIdentity(status.override), "warning");
  }
  for (const entry of mismatches) {
    const text = typeof entry === "string" ? entry : JSON.stringify(entry);
    _appendProfileDetail("Mismatch", text, "warning");
  }
}

async function _pollNetworkProfile() {
  try {
    const status = await Api.networkProfile();
    window.smartfiresNetworkProfile = status;
    _renderNetworkProfile(status);
    window.dispatchEvent(new CustomEvent("smartfires:network-profile", { detail: status }));
  } catch (_) {
    // Keep a previously observed profile visible through a brief API outage.
    if (!window.smartfiresNetworkProfile) {
      _renderNetworkProfile({ state: "unknown", source: null, active: null, mismatches: [] });
    }
  }
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
