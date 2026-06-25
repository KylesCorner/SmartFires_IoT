const Api = {
  async nodes() {
    return (await fetch("/api/nodes")).json();
  },
  async telemetryRecent(nodeId, limit = 300) {
    return (await fetch(`/api/telemetry/recent?node=${nodeId}&limit=${limit}`)).json();
  },
  async statusHistory(limit = 5000) {
    return (await fetch(`/api/status_history?limit=${limit}`)).json();
  },
  async receptionTimeline(bins = 50) {
    return (await fetch(`/api/reception_timeline?bins=${bins}`)).json();
  },
  async getBaseStation() {
    return (await fetch("/api/base_station")).json();
  },
  async setBaseStation(lat, lon) {
    return (
      await fetch("/api/base_station", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ lat, lon }),
      })
    ).json();
  },
  async postCommand(command) {
    return (
      await fetch("/api/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ command }),
      })
    ).json();
  },
  async newSession() {
    return (
      await fetch("/api/new_session", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
      })
    ).json();
  },
  async snifferStats() {
    return (await fetch("/api/sniffer/stats")).json();
  },
  async resetNode(nodeId) {
    return (
      await fetch("/api/node_reset", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ node_id: nodeId }),
      })
    ).json();
  },
};

function fmt(value) {
  return value === null || value === undefined || value === "" ? "—" : value;
}
