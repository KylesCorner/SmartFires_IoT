// Mirrors the filtering semantics of platformio/monitor/filter_smartfires_debug.py
// (SFDBG_SRC / SFDBG_MIN_LEVEL) so muscle memory from `pio device monitor`
// carries over to this page. See documentation/User_Reference/DEBUG_FILTER.md.

const LEVEL_RANK = { T: 0, D: 1, I: 2, W: 3, E: 4 };
const LEVELS = ["T", "D", "I", "W", "E"];
const MAX_LINES = 5000;

const state = {
  ws: null,
  entries: [],
  minLevel: "T",
  knownSrcs: [],
  activeSrcs: new Set(), // empty = no filter (show all), matches SFDBG_SRC semantics
};

function levelAllows(entryLevel, minLevel) {
  const r = LEVEL_RANK[entryLevel];
  const min = LEVEL_RANK[minLevel];
  return r === undefined || min === undefined || r >= min;
}

function entryMatchesFilters(entry) {
  if (!levelAllows(entry.lvl, state.minLevel)) return false;
  if (state.activeSrcs.size > 0 && !state.activeSrcs.has(entry.src)) return false;
  return true;
}

function renderLevelTabs() {
  const container = document.getElementById("debug-level-tabs");
  container.innerHTML = "";
  for (const lvl of LEVELS) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.minLevel === lvl ? " active" : "");
    btn.textContent = lvl;
    btn.addEventListener("click", () => {
      state.minLevel = lvl;
      renderLevelTabs();
      renderOutput();
    });
    container.appendChild(btn);
  }
}

function renderSrcTabs() {
  const container = document.getElementById("debug-src-tabs");
  container.innerHTML = "";

  const allBtn = document.createElement("button");
  allBtn.className = "log-tab" + (state.activeSrcs.size === 0 ? " active" : "");
  allBtn.textContent = "All";
  allBtn.addEventListener("click", () => {
    state.activeSrcs.clear();
    renderSrcTabs();
    renderOutput();
  });
  container.appendChild(allBtn);

  for (const src of state.knownSrcs) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.activeSrcs.has(src) ? " active" : "");
    btn.textContent = src;
    btn.addEventListener("click", () => {
      if (state.activeSrcs.has(src)) {
        state.activeSrcs.delete(src);
      } else {
        state.activeSrcs.add(src);
      }
      renderSrcTabs();
      renderOutput();
    });
    container.appendChild(btn);
  }
}

function formatEntry(e) {
  const wall = new Date().toLocaleTimeString();
  return `${wall} [${e.lvl}] node=${e.node} src=${e.src} seq=${e.seq} t=${e.t}  ${e.msg}`;
}

function renderOutput() {
  const el = document.getElementById("debug-output");
  const visible = state.entries.filter(entryMatchesFilters);
  const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
  el.textContent = visible.map(formatEntry).join("\n");
  if (atBottom) {
    el.scrollTop = el.scrollHeight;
  }
}

function onDebugEntry(entry) {
  let srcsChanged = false;
  if (entry.src && !state.knownSrcs.includes(entry.src)) {
    state.knownSrcs.push(entry.src);
    state.knownSrcs.sort();
    srcsChanged = true;
  }

  state.entries.push(entry);
  if (state.entries.length > MAX_LINES) {
    state.entries.splice(0, state.entries.length - MAX_LINES);
  }

  if (srcsChanged) {
    renderSrcTabs();
  }
  if (entryMatchesFilters(entry)) {
    renderOutput();
  }
}

function connectDebugSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${proto}//${location.host}/ws/base-debug`);
  state.ws = ws;

  ws.onmessage = (ev) => {
    try {
      onDebugEntry(JSON.parse(ev.data));
    } catch (_) {}
  };

  ws.onclose = () => {
    state.ws = null;
    setTimeout(connectDebugSocket, 2000);
  };
}

function wireClearButton() {
  document.getElementById("debug-clear-btn").addEventListener("click", () => {
    state.entries = [];
    renderOutput();
  });
}

document.addEventListener("DOMContentLoaded", () => {
  renderNav(window.location.pathname);
  renderLevelTabs();
  renderSrcTabs();
  wireClearButton();
  connectDebugSocket();
});
