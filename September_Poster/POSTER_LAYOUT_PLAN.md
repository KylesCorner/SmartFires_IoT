# SmartFires three-column poster layout plan

## Core story

The poster should read from left to right:

1. **What we built** — project goal, field node, base station, and communication path.
2. **What we measured** — the controlled-burn setup, thermal ground truth, and the primary synchronized dataset.
3. **How it is used** — live dashboard, map, sensor monitoring, and the main takeaway.

The controlled-burn data is the visual center of gravity. Hardware and dashboard material explain how that result was produced and observed.

## Page grid

- Final size: **48 × 36 inches, landscape**.
- Keep a full-width header approximately **4.5–5 inches** high.
- Keep a restrained footer approximately **0.8–1 inch** high.
- Divide the remaining body into **three equal columns**, with approximately **0.6-inch gutters** and **0.8–1-inch outside margins**.
- Align section tops, image edges, and card edges to the column grid. Avoid a collection of unrelated floating boxes.

## Recommended wireframe

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ TITLE: SmartFires — A Distributed Edge System for Wildfire Data Collection  │
│ Authors • affiliations • logos                              QR / contact      │
├───────────────────────┬────────────────────────┬─────────────────────────────┤
│ 1. PROJECT GOAL       │ 4. CONTROLLED BURN     │ 6. FIELD DEPLOYMENT         │
│ short goal statement  │ large burn photo       │ base station in operation   │
│ + 3 compact priorities│ + paired thermal frame │ short experiment caption    │
├───────────────────────┼────────────────────────┼─────────────────────────────┤
│ 2. FIELD NODE         │ 5. SYNCHRONIZED DATA   │ 7. LIVE DASHBOARD           │
│ exploded/component    │ Temperature / RH       │ large map screenshot        │
│ photo sequence        │ ─────────────────────  │                             │
│ labels + roles        │ Wind                   │ live-reading screenshot     │
│                       │ ─────────────────────  │                             │
│                       │ PM1 / PM2.5            │ 2–3 numbered callouts       │
│                       │ ─────────────────────  │                             │
│                       │ PM4 / PM10             │                             │
│                       │ shared time axis        │                             │
├───────────────────────┼────────────────────────┼─────────────────────────────┤
│ 3. SYSTEM FLOW        │ one-sentence result    │ 8. TAKEAWAY / NEXT STEP     │
│ nodes → LoRa → Jetson │ highlighted finding    │ concise conclusion + QR     │
│ → storage/dashboard   │                        │                             │
└───────────────────────┴────────────────────────┴─────────────────────────────┘
```

## Column 1 — What we built

### Project Goal

Use the rough-draft wording:

> Build a deployable sensing network that can collect synchronized environmental and smoke measurements around active or controlled fires.

Follow it with one compact line or three short bullets:

- Low-power field sensing
- Long-range wireless communication
- Resilient edge collection and rapid visualization

### Field node

Make the node assembly the dominant visual in this column. Prefer one clean exploded view or a vertical sequence of 3–4 photographs rather than many equally weighted small images.

Suggested callouts:

- **Feather M0 + LoRa:** sampling, packet assembly, and long-range uplink
- **SHT31:** temperature and relative humidity
- **SPS30:** PM1, PM2.5, PM4, and PM10
- **Wind sensor:** local wind intensity
- **GPS/IMU:** position and motion context
- **Battery/enclosure:** portable field operation and protection

Use short leader lines directly on the image. Put detailed specifications in a caption only if they support a result shown elsewhere.

### Node and base-station roles

Finish the column with a simple vertical or compact horizontal flow:

`Field nodes → LoRa packets → Jetson base station → validated CSV data + live dashboard`

Node role: sample local conditions, timestamp/package measurements, and transmit them over LoRa.

Base-station role: receive packets, validate and store readings, coordinate local/ground-truth sensors, and serve the dashboard.

This replaces the rough draft’s large architecture panel; the audience only needs one clear pass through the system.

## Column 2 — Burn experiment and main result

### Experiment/ground-truth image pair

At the top, pair:

- one wide photograph that immediately communicates the physical burn setup; and
- one thermal frame from approximately the same stage of the burn.

Give both images a shared caption explaining that thermal imagery supplies ground truth and training/validation labels for fire-state classifiers. If the views are spatially comparable, match their crops and add the same orientation or region-of-interest marker.

### Synchronized sensor plots

Use a **single vertical stack**, not the four side-by-side charts in the rough draft:

1. Temperature and relative humidity
2. Wind intensity
3. PM1 and PM2.5
4. PM4 and PM10

Plot rules:

- All panels use exactly the same start time, end time, and plot width.
- Only the bottom panel needs x-axis labels; upper panels can suppress repeated labels.
- Draw vertical event markers through every panel for events such as **ignition**, **flame arrival / peak burn**, **smoldering**, and **burn end**.
- Lightly shade event intervals when a phase has duration; use lines for point events.
- Put units in every y-axis label and use human-readable elapsed time or local time consistently.
- Preserve the same color for a variable everywhere on the poster and dashboard callouts.
- Prefer direct labels at the right edge of traces over large legends.
- Do not normalize unrelated sensors onto one y-axis. Alignment in time supplies the comparison.
- If multiple nodes make the plot unreadable, show the most interpretable comparison (for example, nodes ordered by distance from the burn) and move secondary traces to a QR-linked supplement.
- Mark gaps or packet loss honestly rather than interpolating across long missing periods.

Directly beneath the stack, reserve a tinted box for a one-sentence result. Write this only after seeing the data, for example: “Particulate concentrations rose after flame arrival while temperature and wind measurements placed that response in environmental context.”

## Column 3 — Field use and dashboard

### Field deployment/base station

Use one strong photograph of the Jetson/base station operating at the experiment. A short caption should connect the physical equipment to the flow diagram in Column 1.

### Dashboard

Make the map screenshot the larger of the two dashboard images. Below it, place the live sensor-reading screenshot. Crop away browser chrome and empty interface areas.

Use 2–3 numbered callouts across the screenshots:

1. Node locations and status on the map
2. Live incoming environmental and particulate readings
3. Rapid field feedback for data quality and burn progression

### Takeaway

End with a concise conclusion, not another methods paragraph. A provisional version is:

> SmartFires combines distributed sensing, long-range communication, edge storage, and live visualization in one deployable platform for controlled-fire data collection.

Add a QR code only if it leads to a stable dashboard demo, repository, dataset, or project page. Label what the reader will get after scanning.

## Tomorrow’s capture checklist

- Wide establishing photograph showing burn, nodes, and spatial arrangement
- Close view of at least one node installed in context
- Base-station/Jetson operating in the field
- Thermal frames at pre-ignition, active flame, peak response, and smoldering
- Matching visible-light photograph near the selected thermal frame time
- Screenshot of the dashboard map with nodes populated
- Screenshot of live readings during a meaningful event
- Exact timestamps for ignition, notable flame movement, suppression (if applicable), smoldering, and experiment end
- Node IDs, sensor-to-node mapping, locations/distances, units, and clock/time-zone information
- Notes on sensor saturation, resets, occlusion, packet loss, or moved equipment

## Placeholder labels to use now

Avoid generic “PLACEHOLDER” text. Label each empty frame with the exact asset needed:

- `BURN OVERVIEW — replace after field collection`
- `THERMAL GROUND TRUTH — match to highlighted event`
- `NODE ASSEMBLY — exploded/component photos`
- `BASE STATION IN FIELD`
- `DASHBOARD MAP — populated run`
- `LIVE SENSOR READINGS — active burn interval`
- `SYNCHRONIZED BURN DATA — shared x-axis and event markers`

## Editing priorities for the existing rough draft

1. Retain the header styling and Project Goal copy.
2. Convert the body to three visually continuous columns.
3. Move all node content and the simplified system flow into Column 1.
4. Replace the full-width bottom row of side-by-side plots with the central stacked plot figure.
5. Put experiment and thermal imagery above the stacked plots.
6. Move the Jetson field photo and both dashboard screenshots into Column 3.
7. Add a result callout and takeaway only after tomorrow’s dataset has been reviewed.
8. Replace the placeholder title, author line, and collaborator text before print export.
