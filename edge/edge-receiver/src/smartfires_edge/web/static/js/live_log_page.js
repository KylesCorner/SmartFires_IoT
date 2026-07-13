// Dedicated Live Log page. Two independent, combined (AND) multi-select filters:
//   - source: any combination of Base / Node <id> / Sniffer
//   - kind:   any combination of Status / Bundle / Other
// Both are driven by fields LiveState.push_log already attaches to every
// entry (node_id, source, kind) — see live_state.py.
//
// Sniffer cross-filter: a sniffed frame's node_id reflects the SmartFires
// header it carries (0 = base broadcast, e.g. TIME_SYNC/ACK_SUMMARY; N = that
// node), or for bare RadioHead frames with no header (RH_ACK/RH_RAW), the
// TDMA-slot owner inferred server-side (rh_owner_node_id — see
// sniffer_service.py). null only remains for frames neither header nor
// RadioHead addressing could attribute. If "Sniffer" is the only source
// checked, all sniffer traffic shows. If Base/Node boxes are also checked,
// sniffer entries are restricted to those — so checking "Node 2" +
// "Sniffer" shows only sniffer-captured frames attributable to node 2, not
// every node's sniffed traffic.

const LOG_MAX_LINES = 5000;
const BASE_BUCKET = "base";

const state = {
  ws: null,
  entries: [],
  knownNodeIds: new Set(),
  activeSources: new Set([BASE_BUCKET, "sniffer"]), // grows as nodes are discovered
  activeKinds: new Set(["status", "bundle", "other"]),
};

function ingestEntryBucket(entry) {
  return entry.node_id === null || entry.node_id === undefined ? BASE_BUCKET : entry.node_id;
}

// Returns null for sniffed frames with no node attribution at all (unknown/raw).
function snifferEntryBucket(entry) {
  if (entry.node_id === null || entry.node_id === undefined) return null;
  return entry.node_id === 0 ? BASE_BUCKET : entry.node_id;
}

function entryMatchesSource(entry) {
  if (state.activeSources.size === 0) return false;

  if (entry.source === "sniffer") {
    if (!state.activeSources.has("sniffer")) return false;
    const nodeBoxesChecked = [...state.activeSources].filter((s) => s !== "sniffer");
    if (nodeBoxesChecked.length === 0) return true; // sniffer-only: show everything sniffed
    const bucket = snifferEntryBucket(entry);
    return bucket !== null && nodeBoxesChecked.includes(bucket);
  }

  return state.activeSources.has(ingestEntryBucket(entry));
}

function entryKindBucket(entry) {
  return entry.kind === "status" || entry.kind === "bundle" ? entry.kind : "other";
}

function entryMatchesKind(entry) {
  if (state.activeKinds.size === 0) return false;
  return state.activeKinds.has(entryKindBucket(entry));
}

function entryMatchesFilters(entry) {
  return entryMatchesSource(entry) && entryMatchesKind(entry);
}

function sourceTabLabel(bucket) {
  if (bucket === BASE_BUCKET) return "Base";
  if (bucket === "sniffer") return "Sniffer";
  return `Node ${bucket}`;
}

function allSourceBuckets() {
  return [BASE_BUCKET, ...Array.from(state.knownNodeIds).sort((a, b) => a - b), "sniffer"];
}

function toggleSetMember(set, value) {
  if (set.has(value)) {
    set.delete(value);
  } else {
    set.add(value);
  }
}

function renderSourceTabs() {
  const container = document.getElementById("log-source-tabs");
  container.innerHTML = "";

  const buckets = allSourceBuckets();

  const allBtn = document.createElement("button");
  const allSelected = buckets.every((b) => state.activeSources.has(b));
  allBtn.className = "log-tab" + (allSelected ? " active" : "");
  allBtn.textContent = "All";
  allBtn.addEventListener("click", () => {
    if (allSelected) {
      state.activeSources.clear();
    } else {
      state.activeSources = new Set(buckets);
    }
    renderSourceTabs();
    renderOutput();
  });
  container.appendChild(allBtn);

  for (const bucket of buckets) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.activeSources.has(bucket) ? " active" : "");
    btn.textContent = sourceTabLabel(bucket);
    btn.addEventListener("click", () => {
      toggleSetMember(state.activeSources, bucket);
      renderSourceTabs();
      renderOutput();
    });
    container.appendChild(btn);
  }
}

const KIND_BUCKETS = ["status", "bundle", "other"];

function kindTabLabel(kind) {
  if (kind === "status") return "Status";
  if (kind === "bundle") return "Bundle";
  return "Other";
}

function renderKindTabs() {
  const container = document.getElementById("log-kind-tabs");
  container.innerHTML = "";

  const allBtn = document.createElement("button");
  const allSelected = KIND_BUCKETS.every((k) => state.activeKinds.has(k));
  allBtn.className = "log-tab" + (allSelected ? " active" : "");
  allBtn.textContent = "All";
  allBtn.addEventListener("click", () => {
    if (allSelected) {
      state.activeKinds.clear();
    } else {
      state.activeKinds = new Set(KIND_BUCKETS);
    }
    renderKindTabs();
    renderOutput();
  });
  container.appendChild(allBtn);

  for (const kind of KIND_BUCKETS) {
    const btn = document.createElement("button");
    btn.className = "log-tab" + (state.activeKinds.has(kind) ? " active" : "");
    btn.textContent = kindTabLabel(kind);
    btn.addEventListener("click", () => {
      toggleSetMember(state.activeKinds, kind);
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
  if (entry.node_id !== null && entry.node_id !== undefined && entry.node_id !== 0 && !state.knownNodeIds.has(entry.node_id)) {
    state.knownNodeIds.add(entry.node_id);
    state.activeSources.add(entry.node_id); // newly discovered nodes default to visible
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

function downloadTextFile(filename, text) {
  const blob = new Blob([text], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

function wireExportButton() {
  document.getElementById("log-export-btn").addEventListener("click", () => {
    const visible = state.entries.filter(entryMatchesFilters);
    const text = visible.map((e) => `${e.t}  ${e.msg}`).join("\n") + "\n";
    const stamp = new Date().toISOString().replace(/[:.]/g, "-");
    downloadTextFile(`smartfires-live-log-${stamp}.txt`, text);
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
  wireExportButton();
  wireCommandInput();
  connectLogSocket();
});
