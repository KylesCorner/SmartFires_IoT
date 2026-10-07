const Api = {
  async nodes() {
    return (await fetch("/api/nodes")).json();
  },
  async telemetryRecent(nodeId, limit = 300) {
    return (await fetch(`/api/telemetry/recent?node=${nodeId}&limit=${limit}`)).json();
  },
  async telemetryHistory(nodeId, metric, startMs, endMs, maxPoints = 1200) {
    const params = new URLSearchParams({ node: nodeId, metric, max_points: maxPoints });
    if (startMs) params.set("start_ms", Math.floor(startMs));
    if (endMs) params.set("end_ms", Math.floor(endMs));
    return (await fetch(`/api/telemetry/history?${params}`)).json();
  },
  async sessionTimeline(buckets = 300) {
    return (await fetch(`/api/session/timeline?buckets=${buckets}`)).json();
  },
  async statusHistory(limit = 5000) {
    return (await fetch(`/api/status_history?limit=${limit}`)).json();
  },
  async awakenEvents(limit = 500) {
    return (await fetch(`/api/awaken_events?limit=${limit}`)).json();
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
    const response = await fetch("/api/new_session", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
    });
    if (!response.ok) {
      const body = await response.json().catch(() => ({}));
      throw new Error(body.detail || `HTTP ${response.status}`);
    }
    return response.json();
  },
  async waitForSessionChange(previousSessionId, timeoutMs = 90_000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 3000);
      try {
        const response = await fetch("/api/session", {
          cache: "no-store",
          signal: controller.signal,
        });
        if (response.ok) {
          const current = await response.json();
          if (current.session_id && current.session_id !== previousSessionId) return current;
        }
      } catch (_) {
        // An outage is expected while the service supervisor relaunches the process.
      } finally {
        clearTimeout(timer);
      }
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
    throw new Error("The edge service did not return with a new session within 90 seconds");
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
  // action: "set" | "increase" | "decrease" | "dynamic" | "static".
  // increase/decrease are resolved to an absolute dBm server-side from the
  // node's last reported power — the wire protocol has no relative form.
  async setTxPower(nodeId, action, txPowerDbm) {
    const body = { node_id: nodeId, action };
    if (txPowerDbm !== undefined && txPowerDbm !== null) {
      body.tx_power_dbm = txPowerDbm;
    }
    const res = await fetch("/api/tx_power", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      const detail = await res.json().catch(() => ({}));
      throw new Error(detail.detail || `HTTP ${res.status}`);
    }
    return res.json();
  },
  async serverTime() {
    return (await fetch("/api/server_time")).json();
  },
  async session() {
    return (await fetch("/api/session")).json();
  },
  async networkProfile() {
    const response = await fetch("/api/network_profile", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    return normalizeNetworkProfileResponse(await response.json());
  },
};

// Keep the browser tolerant during staged edge/firmware rollouts. The
// canonical API envelope is {state, source, active, base, override,
// mismatches}; aliases below accept early profile-announcement builds without
// embedding a second SF timing table in JavaScript.
function normalizeNetworkProfile(profile) {
  if (!profile || typeof profile !== "object") return null;

  const numberOrNull = (...values) => {
    const value = values.find((candidate) => candidate !== undefined && candidate !== null && candidate !== "");
    if (value === undefined) return null;
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
  };
  const explicitBandwidthHz = numberOrNull(profile.bandwidth_hz);
  const genericBandwidth = numberOrNull(profile.bandwidth);
  const bandwidthKhz = numberOrNull(profile.bandwidth_khz);
  const spreadingFactor = numberOrNull(profile.spreading_factor, profile.sf, profile.lora_sf);
  const profileId = profile.profile_id ?? profile.id ?? profile.name ??
    (spreadingFactor !== null ? `sf${spreadingFactor}` : null);

  return {
    ...profile,
    profile_id: profileId,
    spreading_factor: spreadingFactor,
    bandwidth_hz: explicitBandwidthHz ??
      (bandwidthKhz !== null ? bandwidthKhz * 1000 : null) ??
      (genericBandwidth !== null ? (genericBandwidth <= 1000 ? genericBandwidth * 1000 : genericBandwidth) : null),
    coding_rate: profile.coding_rate ?? profile.cr ?? null,
    coding_rate_denominator: numberOrNull(profile.coding_rate_denominator, profile.cr_denominator),
    num_slots: numberOrNull(profile.num_slots, profile.slot_count),
    slot_width_ms: numberOrNull(profile.slot_width_ms, profile.tdma_slot_width_ms),
    guard_ms: numberOrNull(profile.guard_ms, profile.slot_guard_ms, profile.tdma_guard_ms),
    max_bundle_deltas: numberOrNull(profile.max_bundle_deltas, profile.operational_bundle_deltas),
    continuous_sample_period_ms: numberOrNull(
      profile.continuous_sample_period_ms,
      profile.sample_period_ms,
      profile.continuous_sample_ms
    ),
    timed_sample_period_ms: numberOrNull(profile.timed_sample_period_ms, profile.timed_sample_ms),
    status_interval_ms: numberOrNull(profile.status_interval_ms),
    fingerprint: profile.fingerprint ?? profile.profile_fingerprint ?? null,
  };
}

function normalizeNetworkProfileResponse(payload) {
  const envelope = payload && typeof payload === "object" ? payload : {};
  const looksFlat = envelope.profile_id !== undefined || envelope.spreading_factor !== undefined || envelope.sf !== undefined;
  const active = normalizeNetworkProfile(
    envelope.active ?? envelope.profile ?? envelope.network_profile ?? (looksFlat ? envelope : null)
  );
  const base = normalizeNetworkProfile(envelope.base ?? envelope.base_profile);
  const override = normalizeNetworkProfile(envelope.override ?? envelope.override_profile);
  const mismatches = Array.isArray(envelope.mismatches)
    ? envelope.mismatches
    : envelope.mismatch
      ? [envelope.mismatch]
      : [];
  let state = envelope.state ?? (mismatches.length ? "mismatch" : active ? "active" : "unknown");
  if (state === "known") state = "active";

  return {
    state,
    source: envelope.source ?? active?.source ?? null,
    active,
    base,
    override,
    mismatches,
    history: Array.isArray(envelope.history) ? envelope.history : [],
  };
}

function fmt(value) {
  return value === null || value === undefined || value === "" ? "—" : value;
}

// Formats an epoch-seconds timestamp (e.g. live_state's "last_seen" fields)
// as a local clock time, consistent across every page that shows one.
function fmtTime(epochSeconds) {
  return epochSeconds ? formatTimestamp(epochSeconds, "epoch-seconds") : "—";
}

// Formats a node's hardware serial (SAMD21 uid_hash) the same way it's shown
// elsewhere in the dashboard (e.g. AWAKEN/STATUS log lines): 0x-prefixed,
// zero-padded 32-bit hex.
function fmtSerial(uidHash) {
  return uidHash === null || uidHash === undefined
    ? "—"
    : "0x" + Number(uidHash).toString(16).padStart(8, "0");
}
