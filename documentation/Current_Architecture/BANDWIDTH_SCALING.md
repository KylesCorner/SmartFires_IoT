# Bandwidth and Scaling Math (Current Scheme)

This document summarizes sensing, bundle production, TDMA service capacity, and channel occupancy for the current SmartFires node scheme.

## Current Parameters

- Sample rate: `4 Hz`
- Bundle composition: `1 reference + 14 deltas = 15 samples`
- Bundle payload (`PKT_BUNDLE`): `193 bytes` max
- TDMA slot width: `900 ms`
- TDMA guard: `20 ms` on each edge
- Usable slot window: `860 ms`
- Multi-send budget in node radio service: up to `2 bundle-class sends/slot` (conservative `340 ms` budget each)
- ACK summary payload (`PKT_ACK_SUMMARY`): `8 bytes` on LoRa (`PktHeader + AckSummaryPayload`)
- ACK summary cadence (edge default): `1 summary per node every 4 seconds` (`0.25 Hz`)

## Airtime Model

Using SF7/BW125/CR 4/5 with RadioHead 4-byte over-the-air header:

- Bundle airtime (193-byte payload): `312.576 ms`
- ACK summary airtime (8-byte payload): `41.216 ms`

## Core Formulas

Let:

- $f_s$ = sample rate (Hz)
- $S_b$ = samples per bundle
- $N$ = number of nodes
- $W$ = slot width (s)
- $k$ = sends per slot (old: $k=1$, new multi-send: $k=2$)

Bundle production per node:

$$
r_{prod} = \frac{f_s}{S_b}
$$

With current values:

$$
r_{prod} = \frac{4}{15} = 0.2667\ \text{bundles/s}
$$

Per-node TDMA service:

$$
r_{svc} = \frac{k}{N \cdot W}
$$

For $W=0.9$ s:

$$
r_{svc,old} = \frac{1}{0.9N},\quad r_{svc,new} = \frac{2}{0.9N}
$$

Utilization ratio (stable if $\eta \le 1$):

$$
\eta = \frac{r_{prod}}{r_{svc}}
$$

Approximate steady-state drop rate when $\eta > 1$:

$$
\text{loss} \approx \frac{\eta - 1}{\eta}
$$

Offered channel occupancy estimate:

$$
\text{uplink\_occ} = N \cdot r_{prod} \cdot T_{bundle}
$$

$$
\text{ack\_occ} = N \cdot f_{ack} \cdot T_{ack}
$$

$$
\text{total\_occ} = \text{uplink\_occ} + \text{ack\_occ}
$$

with $f_{ack}=0.25$ Hz/node, $T_{bundle}=312.576$ ms, $T_{ack}=41.216$ ms.

## Node Scaling Table (2–10 nodes)

| Nodes | Frame (s) | Service old (1/slot) bundles/s | Service new (2/slot) bundles/s | Util old $\eta$ | Util new $\eta$ | Loss old | Loss new | Uplink occ @ offered load | ACK occ @0.25Hz | Total occ @0.25Hz |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 1.8 | 0.5556 | 1.1111 | 0.48 | 0.24 | 0.0% | 0.0% | 16.7% | 2.1% | 18.8% |
| 3 | 2.7 | 0.3704 | 0.7407 | 0.72 | 0.36 | 0.0% | 0.0% | 25.0% | 3.1% | 28.1% |
| 4 | 3.6 | 0.2778 | 0.5556 | 0.96 | 0.48 | 0.0% | 0.0% | 33.3% | 4.1% | 37.4% |
| 5 | 4.5 | 0.2222 | 0.4444 | 1.20 | 0.60 | 16.7% | 0.0% | 41.7% | 5.2% | 46.9% |
| 6 | 5.4 | 0.1852 | 0.3704 | 1.44 | 0.72 | 30.6% | 0.0% | 50.0% | 6.2% | 56.2% |
| 7 | 6.3 | 0.1587 | 0.3175 | 1.68 | 0.84 | 40.5% | 0.0% | 58.3% | 7.2% | 65.5% |
| 8 | 7.2 | 0.1389 | 0.2778 | 1.92 | 0.96 | 47.9% | 0.0% | 66.7% | 8.2% | 74.9% |
| 9 | 8.1 | 0.1235 | 0.2469 | 2.16 | 1.08 | 53.7% | 7.4% | 75.0% | 9.3% | 84.3% |
| 10 | 9.0 | 0.1111 | 0.2222 | 2.40 | 1.20 | 58.3% | 16.7% | 83.4% | 10.3% | 93.7% |

## Interpretation

- The multi-send TDMA change ($k=2$) roughly doubles per-node service capacity.
- At current settings, 4 nodes at 4 Hz are comfortably stable.
- With $k=2$, 8 nodes are still mathematically stable in queue terms ($\eta=0.96$), but channel occupancy is near saturation once ACK summaries are included.
- At 9–10 nodes, both occupancy and queue stability become problematic even with multi-send.

## Practical Notes

- The table uses offered-load occupancy (what the sensing pipeline attempts to send).
- Real-world reliability may require lower ACK summary frequency at higher node counts.
- If base station LoRa forwarding/summary scheduling is constrained, practical capacity may be lower than this model.
