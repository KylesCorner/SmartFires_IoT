// Dedicated Live Log page. Two independent, combined (AND) filters:
//   - source: All / Sniffer / Node <id>  (which radio link produced the line)
//   - kind:   All / Status / Bundle      (which packet category the line reports)
// Both are driven by fields LiveState.push_log already attaches to every
// entry (node_id, source, kind) — see live_state.py.

const LOG_MAX_LINES = 5000;

const FIXED_SOURCE_TABS = [null, "sniffer"];
const KIND_TABS = [null, "status", "bundle"];

const state = {
  ws: null,
  entries: [],
  knownNodeIds: new Set(),
  activeSource: null, // null = All, "sniffer" = sniffer-only, number = that node
  activeKind: null,   // null = All, "status", "bundle"
};

function sourceTabLabel(tabId) {
  if (tabId === null) return "All";
  if (tabId === "sniffer") return "Sniffer";
  return `Node ${tabId}`;
}

function kindTabLabel(kindId) {
  if (kindId === null) return "All";
  if (kindId === "status") return "Status";
  if (kindId === "bundle") return "Bundle";
  return kindId;
}

function entryMatchesSource(entry, tabId) {
  if (tabId === null) return true;
  if (tabId === "sniffer") return entry.source === "sniffer";
  return entry.node_id === tabId || entry.node_id === null;
}

function entryMatchesKind(entry, kindId) {
  if (kindId === null) return true;
  return entry.kind === kindId;
}

function entryMatchesFilters(entry) {
  return entryMatchesSource(entry, state.activeSource) && entryMatchesKind(entry, state.activeKind);
}

function renderSourceTabs() {
  const container = document.getElementById("log-source-tabs");
  container.innerHTML = "";

  const tabs = [...FIXED_SOURCE_TABS, ...Array.from(state.knownNodeIds).sort((a, b) => a - b)];
  for (const tabId of tabs) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.activeSource === tabId ? " active" : "");
    btn.textContent = sourceTabLabel(tabId);
    btn.addEventListener("click", () => {
      state.activeSource = tabId;
      renderSourceTabs();
      renderOutput();
    });
    container.appendChild(btn);
  }
}

function renderKindTabs() {
  const container = document.getElementById("log-kind-tabs");
  container.innerHTML = "";

  for (const kindId of KIND_TABS) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.activeKind === kindId ? " active" : "");
    btn.textContent = kindTabLabel(kindId);
    btn.addEventListener("click", () => {
      state.activeKind = kindId;
      renderKindTabs();
      renderOutput();
    });
    container.appendChild(btn);
  }
}

function renderOutput() {
  const el = document.getElementById("log-output");
  const visible = state.entries.filter(entryMatchesFilters);
  const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
  el.textContent = visible.map((e) => `${e.t.slice(11, 23)}  ${e.msg}`).join("\n");
  if (atBottom) {
    el.scrollTop = el.scrollHeight;
  }
}

function onLogEntry(entry) {
  // A fresh session clears history server-side too (live_state.reset()) —
  // mirror that locally instead of accumulating stale entries across sessions.
  if (entry.kind === "session" && entry.msg === "--- NEW SESSION ---") {
    state.entries = [];
    renderOutput();
    return;
  }

  state.entries.push(entry);
  if (state.entries.length > LOG_MAX_LINES) {
    state.entries.splice(0, state.entries.length - LOG_MAX_LINES);
  }

  let tabsChanged = false;
  if (entry.node_id !== null && entry.node_id !== undefined && !state.knownNodeIds.has(entry.node_id)) {
    state.knownNodeIds.add(entry.node_id);
    tabsChanged = true;
  }

  if (tabsChanged) {
    renderSourceTabs();
  }

  if (entryMatchesFilters(entry)) {
    const el = document.getElementById("log-output");
    const atBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + 4;
    el.textContent += `${entry.t.slice(11, 23)}  ${entry.msg}\n`;
    if (atBottom) {
      el.scrollTop = el.scrollHeight;
    }
  }
}

function connectLogSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${proto}//${location.host}/ws/log`);
  state.ws = ws;

  ws.onmessage = (ev) => {
    try {
      onLogEntry(JSON.parse(ev.data));
    } catch (_) {}
  };

  ws.onclose = () => {
    state.ws = null;
    setTimeout(connectLogSocket, 2000);
  };
}

function wireClearButton() {
  document.getElementById("log-clear-btn").addEventListener("click", () => {
    state.entries = [];
    renderOutput();
  });
}

function wireCommandInput() {
  const input = document.getElementById("cmd-input");
  const btn = document.getElementById("cmd-send");

  async function sendCommand() {
    const cmd = input.value.trim();
    if (!cmd) return;
    input.value = "";
    try {
      await Api.postCommand(cmd);
    } catch (_) {}
  }

  btn.addEventListener("click", sendCommand);
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") sendCommand();
  });
}

document.addEventListener("DOMContentLoaded", () => {
  renderNav(window.location.pathname);
  renderSourceTabs();
  renderKindTabs();
  wireClearButton();
  wireCommandInput();
  connectLogSocket();
});
