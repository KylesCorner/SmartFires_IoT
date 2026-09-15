---
name: jetson-sensor-expansion
description: Deferred options for attaching environmental, GPS, particulate, and orientation sensors directly to the Jetson.
category: plan-possible
status: deferred
related_docs:
  - jetson-bridge
---

# Possible: Jetson-local sensor expansion

## Audit result

Audited against the edge package on 2026-09-04. The proposed I2C expansion has not been implemented, so this plan remains possible rather than completed.

What exists today:

- `AnemometerPoller` reads the ES-W302 over Modbus/RS485 and adds `jetson_wind_mps` and `jetson_wind_dir_deg` to telemetry rows.
- `BaseStationStore` persists manually entered base latitude/longitude.
- The ingest service already demonstrates the background-poller, configuration, lifecycle, and latest-value pattern that another local sensor could reuse.

What does not exist:

- no Jetson-connected temperature/humidity sensor;
- no BMV080 integration or vendor SDK dependency;
- no Jetson GPS reader, I2C-to-gpsd bridge, or chrony GPS discipline;
- no Jetson ICM-20948/DMP integration;
- no generic local-sensor data model beyond the two anemometer columns.

The anemometer is useful groundwork but was pre-existing and does not make the broader expansion substantially complete.

## If resumed

Treat each sensor as an independent opt-in feature:

1. Decide whether base-local measurements are still needed and define their storage/API fields before selecting hardware.
2. For temperature/humidity, reuse the poller pattern and add explicit `jetson_*` fields.
3. For BMV080, first verify Linux ARM64 SDK availability and redistribution terms; do not commit to it without a supportable driver path.
4. For GPS, separate position from system time. Position can update `BaseStationStore`; offline clock discipline likely needs an I2C-to-NMEA bridge feeding `gpsd` and `chrony`.
5. For ICM-20948, evaluate reusing SparkFun's portable C core through Linux `i2c-dev`, and define a practical post-boot calibration procedure.
6. Add per-device enable flags, failure isolation, lifecycle tests, and clear freshness/validity fields.

No implementation is required for the current deployment. The existing node network and optional Jetson anemometer remain the supported sensor paths.
