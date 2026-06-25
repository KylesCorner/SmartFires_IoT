#!/usr/bin/env bash
set -euo pipefail

# -----------------------------------------------------------------------------
# SmartFires power sweep runner
#
# This script:
#   1. Prompts before each power test so you can rewire/check setup.
#   2. Builds/uploads the selected PlatformIO environment.
#   3. Waits for the configured settle time.
#   4. Runs the existing scope logger script.
#   5. Writes one CSV per test plus a run manifest.
#
# Assumes log_scope_power.py supports:
#   --duration
#   --interval
#   --measurement
#   --csv-path
#
# Example:
#   scripts/run_power_sweep.sh \
#     --out-dir /run/media/kyle/YOUR_DRIVE/smartfires_power \
#     --upload-port /dev/ttyACM0
# -----------------------------------------------------------------------------

PROJECT_DIR="."
OUT_DIR=""
LOGGER_SCRIPT="../util/log_scope_power.py"
MEASUREMENT="CUST3"
SKIP_UPLOAD=0
PROMPT_BEFORE_TEST=1
ONLY_ENVS=()

usage() {
  cat <<EOF
Usage:
  $0 --out-dir PATH [options]

Required:
  --out-dir PATH              Directory for timestamped power sweep output

Options:
  --project-dir PATH          PlatformIO project dir, default: .
  --logger PATH               Path to log_scope_power.py, default: log_scope_power.py
  --measurement CUSTN         Scope measurement slot, default: CUST3
  --skip-upload               Do not upload firmware, only run logger
  --no-prompt                 Do not pause before each test
  --only ENV [ENV ...]        Only run selected PlatformIO envs
  -h, --help                  Show this help

Examples:
  $0 \\
    --out-dir /run/media/kyle/YOUR_DRIVE/smartfires_power \\

  $0 \\
    --out-dir /run/media/kyle/YOUR_DRIVE/smartfires_power \\
    --only feather_m0_power_mcu_run feather_m0_power_mcu_standby

  $0 \\
    --out-dir /run/media/kyle/YOUR_DRIVE/smartfires_power \\
    --skip-upload \\
    --only feather_m0_power_mcu_run
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --project-dir)
      PROJECT_DIR="$2"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --logger)
      LOGGER_SCRIPT="$2"
      shift 2
      ;;
    --measurement)
      MEASUREMENT="$2"
      shift 2
      ;;
    --skip-upload)
      SKIP_UPLOAD=1
      shift
      ;;
    --no-prompt)
      PROMPT_BEFORE_TEST=0
      shift
      ;;
    --only)
      shift
      while [[ $# -gt 0 && "$1" != --* ]]; do
        ONLY_ENVS+=("$1")
        shift
      done
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$OUT_DIR" ]]; then
  echo "ERROR: --out-dir is required" >&2
  usage
  exit 1
fi

PROJECT_DIR="$(realpath "$PROJECT_DIR")"
OUT_DIR="$(realpath -m "$OUT_DIR")"

if [[ ! -d "$PROJECT_DIR" ]]; then
  echo "ERROR: project dir does not exist: $PROJECT_DIR" >&2
  exit 1
fi

if [[ ! -f "$PROJECT_DIR/platformio.ini" ]]; then
  echo "ERROR: no platformio.ini found in: $PROJECT_DIR" >&2
  exit 1
fi

# Find logger script.
if [[ ! -f "$LOGGER_SCRIPT" ]]; then
  if [[ -f "$PROJECT_DIR/$LOGGER_SCRIPT" ]]; then
    LOGGER_SCRIPT="$PROJECT_DIR/$LOGGER_SCRIPT"
  elif [[ -f "$PROJECT_DIR/scripts/$LOGGER_SCRIPT" ]]; then
    LOGGER_SCRIPT="$PROJECT_DIR/scripts/$LOGGER_SCRIPT"
  else
    echo "ERROR: logger script not found: $LOGGER_SCRIPT" >&2
    echo "Tried:" >&2
    echo "  $LOGGER_SCRIPT" >&2
    echo "  $PROJECT_DIR/$LOGGER_SCRIPT" >&2
    echo "  $PROJECT_DIR/scripts/$LOGGER_SCRIPT" >&2
    exit 1
  fi
fi

LOGGER_SCRIPT="$(realpath "$LOGGER_SCRIPT")"

RUN_ID="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$OUT_DIR/power_sweep_$RUN_ID"
mkdir -p "$RUN_DIR"

MANIFEST_CSV="$RUN_DIR/run_manifest.csv"

# -----------------------------------------------------------------------------
# CSV helper
# -----------------------------------------------------------------------------

csv_escape() {
  local s="${1:-}"
  s="${s//\"/\"\"}"
  printf '"%s"' "$s"
}

write_manifest_header() {
  {
    csv_escape "env"; printf ","
    csv_escape "csv_file"; printf ","
    csv_escape "description"; printf ","
    csv_escape "setup_note"; printf ","
    csv_escape "settle_s"; printf ","
    csv_escape "duration_s"; printf ","
    csv_escape "interval_s"; printf ","
    csv_escape "uploaded"; printf ","
    csv_escape "started_at"; printf ","
    csv_escape "finished_at"; printf ","
    csv_escape "status"; printf "\n"
  } > "$MANIFEST_CSV"
}

append_manifest_row() {
  local env="$1"
  local csv_file="$2"
  local description="$3"
  local setup_note="$4"
  local settle_s="$5"
  local duration_s="$6"
  local interval_s="$7"
  local uploaded="$8"
  local started_at="$9"
  local finished_at="${10}"
  local status="${11}"

  {
    csv_escape "$env"; printf ","
    csv_escape "$csv_file"; printf ","
    csv_escape "$description"; printf ","
    csv_escape "$setup_note"; printf ","
    csv_escape "$settle_s"; printf ","
    csv_escape "$duration_s"; printf ","
    csv_escape "$interval_s"; printf ","
    csv_escape "$uploaded"; printf ","
    csv_escape "$started_at"; printf ","
    csv_escape "$finished_at"; printf ","
    csv_escape "$status"; printf "\n"
  } >> "$MANIFEST_CSV"
}

write_manifest_header

# -----------------------------------------------------------------------------
# Test list
#
# Format:
#   ENV|CSV_NAME|DESCRIPTION|SETTLE_SECONDS|DURATION_SECONDS|INTERVAL_SECONDS|SETUP_NOTE
#
# Notes:
#   - For MCU standby, settle is 0 so logging starts immediately after upload.
#   - For standby, set POWER_TEST_PRE_SLEEP_DELAY_MS in platformio.ini to 15000
#     or 30000 ms if you want the CSV to capture the run -> sleep transition.
# -----------------------------------------------------------------------------

TESTS=(
  "feather_m0_power_mcu_run|00_mcu_run.csv|MCU running, RFM95 sleep, sensors off|8|120|1|Connect only Feather through the shunt USB cable. LiPo unplugged. No sensors externally powered. No second USB/debug connection."
  "feather_m0_power_mcu_standby|01_mcu_standby.csv|SAMD21 standby forever, RFM95 sleep, sensors off|0|120|1|Same as MCU baseline. Make sure POWER_TEST_PRE_SLEEP_DELAY_MS is long enough to capture the sleep transition. Serial will disconnect after standby."
  "feather_m0_power_i2c_idle|02_i2c_idle.csv|I2C initialized, no active sensor|8|120|1|Connect I2C bus exactly as desired. SHT31/IMU may be connected if testing bus idle cost. Avoid unrelated powered devices."
  "feather_m0_power_radio_standby|03_radio_standby.csv|RFM95 LoRa standby|8|120|1|No sensor changes needed. Feather only through shunt USB cable. This measures radio standby relative to radio sleep."
  "feather_m0_power_radio_rx|04_radio_rx.csv|RFM95 continuous RX|8|120|1|No sensor changes needed. This should show a clear power increase over MCU baseline. Useful as a radio-current proof test."
  "feather_m0_power_sht31|10_sht31.csv|SHT31 only|10|180|1|Connect SHT31 to 3V, GND, SDA, SCL. Disconnect or depower unrelated sensors if possible."
  "feather_m0_power_imu|11_imu.csv|ICM20948 IMU only|10|180|1|Connect IMU to 3V, GND, SDA, SCL. Disconnect or depower unrelated sensors if possible."
  "feather_m0_power_gps|12_gps.csv|PA1010D GPS only|20|300|1|Connect GPS power/GND and data lines as used in the node. Disconnect or depower unrelated sensors if possible."
  "feather_m0_power_sps30|13_sps30.csv|SPS30 only|30|300|1|Connect SPS30 and its 5V/TPS branch. Confirm common ground. Confirm there is no alternate shunt bypass path."
  "feather_m0_power_wind|14_wind.csv|Wind Sensor Rev C only|20|300|1|Connect wind sensor TPS branch, A1 RV, A2 TMP, A3 enable. Confirm boost/TPS wiring before continuing."
)

contains_env() {
  local needle="$1"

  if [[ ${#ONLY_ENVS[@]} -eq 0 ]]; then
    return 0
  fi

  local env
  for env in "${ONLY_ENVS[@]}"; do
    if [[ "$env" == "$needle" ]]; then
      return 0
    fi
  done

  return 1
}

prompt_before_test() {
  local env="$1"
  local csv_name="$2"
  local description="$3"
  local settle_s="$4"
  local duration_s="$5"
  local interval_s="$6"
  local setup_note="$7"

  if [[ "$PROMPT_BEFORE_TEST" -eq 0 ]]; then
    return 0
  fi

  echo
  echo "================================================================================"
  echo "Next SmartFires power test"
  echo "================================================================================"
  echo "Environment:  $env"
  echo "CSV:          $csv_name"
  echo "Description:  $description"
  echo "Settle time:  ${settle_s}s"
  echo "Duration:     ${duration_s}s"
  echo "Interval:     ${interval_s}s"
  echo
  echo "Manual setup:"
  echo "  $setup_note"
  echo
  echo "Checklist:"
  echo "  [ ] Shunt USB cable connected"
  echo "  [ ] Scope CH1 is measuring the low-side shunt"
  echo "  [ ] Scope CUST3 is MATH, MEAN power"
  echo "  [ ] Laptop can reach the scope over LAN"
  echo "  [ ] LiPo unplugged unless this test explicitly requires it"
  echo "  [ ] No alternate ground path around the shunt"
  echo "  [ ] Correct sensor/branch wired for this test"
  echo
  echo "Commands:"
  echo "  Enter : upload and log this test"
  echo "  s     : skip this test"
  echo "  q     : quit sweep"
  echo

  local reply
  read -r -p "> " reply

  case "$reply" in
    "")
      return 0
      ;;
    s|S|skip|SKIP)
      return 1
      ;;
    q|Q|quit|QUIT)
      echo "User requested quit."
      exit 0
      ;;
    *)
      echo "Unknown response: $reply"
      echo "Skipping $env to avoid accidental bad measurement."
      return 1
      ;;
  esac
}
upload_env() {
  local env="$1"

  echo
  echo "================================================================================"
  echo "Uploading PlatformIO environment"
  echo "================================================================================"
  echo "Environment: $env"

  local cmd=(pio run -e "$env" -t upload)

  echo "Command: ${cmd[*]}"

  (
    cd "$PROJECT_DIR"
    "${cmd[@]}"
  )
}
# upload_env() {
#   local env="$1"
#
#   echo
#   echo "================================================================================"
#   echo "Uploading PlatformIO environment"
#   echo "================================================================================"
#   echo "Environment: $env"
#
#   local cmd=(pio run -e "$env" -t upload)
#
#   if [[ -n "$UPLOAD_PORT" ]]; then
#     cmd+=(--upload-port "$UPLOAD_PORT")
#   fi
#
#   echo "Command: ${cmd[*]}"
#
#   (
#     cd "$PROJECT_DIR"
#     "${cmd[@]}"
#   )
# }

run_logger() {
  local env="$1"
  local csv_path="$2"
  local duration="$3"
  local interval="$4"

  echo
  echo "--------------------------------------------------------------------------------"
  echo "Logging power"
  echo "--------------------------------------------------------------------------------"
  echo "Environment: $env"
  echo "CSV:         $csv_path"
  echo "Duration:    ${duration}s"
  echo "Interval:    ${interval}s"
  echo "Measurement: $MEASUREMENT"
  echo

  python "$LOGGER_SCRIPT" \
    --duration "$duration" \
    --interval "$interval" \
    --measurement "$MEASUREMENT" \
    --csv-path "$csv_path"
}

echo
echo "SmartFires power sweep"
echo "================================================================================"
echo "Project:      $PROJECT_DIR"
echo "Logger:       $LOGGER_SCRIPT"
echo "Output:       $RUN_DIR"
echo "Manifest:     $MANIFEST_CSV"
echo "Measurement:  $MEASUREMENT"
echo "Skip upload:  $SKIP_UPLOAD"
echo "Prompt mode:  $PROMPT_BEFORE_TEST"
echo "================================================================================"
echo

ran_any=0

for test in "${TESTS[@]}"; do
  IFS='|' read -r env csv_name description settle_s duration_s interval_s setup_note <<< "$test"

  if ! contains_env "$env"; then
    continue
  fi

  ran_any=1

  if ! prompt_before_test "$env" "$csv_name" "$description" "$settle_s" "$duration_s" "$interval_s" "$setup_note"; then
    echo "Skipping: $env"

    now="$(date --iso-8601=seconds)"
    append_manifest_row \
      "$env" \
      "$csv_name" \
      "$description" \
      "$setup_note" \
      "$settle_s" \
      "$duration_s" \
      "$interval_s" \
      "no" \
      "$now" \
      "$now" \
      "skipped"

    continue
  fi

  csv_path="$RUN_DIR/$csv_name"
  uploaded="no"
  started_at="$(date --iso-8601=seconds)"
  status="ok"

  {
    if [[ "$SKIP_UPLOAD" -eq 0 ]]; then
      upload_env "$env"
      uploaded="yes"
    else
      echo
      echo "Skipping upload for $env"
    fi

    if [[ "$settle_s" != "0" && "$settle_s" != "0.0" ]]; then
      echo
      echo "Settling for ${settle_s}s..."
      sleep "$settle_s"
    fi

    run_logger "$env" "$csv_path" "$duration_s" "$interval_s"

  } || {
    status="failed"
    echo "ERROR: test failed: $env" >&2
  }

  finished_at="$(date --iso-8601=seconds)"

  append_manifest_row \
    "$env" \
    "$csv_name" \
    "$description" \
    "$setup_note" \
    "$settle_s" \
    "$duration_s" \
    "$interval_s" \
    "$uploaded" \
    "$started_at" \
    "$finished_at" \
    "$status"

  if [[ "$status" != "ok" ]]; then
    echo
    echo "Stopping because test failed: $env"
    echo "Output so far:"
    echo "  $RUN_DIR"
    exit 1
  fi
done

if [[ "$ran_any" -eq 0 ]]; then
  echo "ERROR: no tests selected." >&2

  if [[ ${#ONLY_ENVS[@]} -gt 0 ]]; then
    echo "Requested --only envs:" >&2
    printf '  %s\n' "${ONLY_ENVS[@]}" >&2
  fi

  exit 1
fi

echo
echo "================================================================================"
echo "Power sweep complete"
echo "================================================================================"
echo "Output directory:"
echo "  $RUN_DIR"
echo
echo "Manifest:"
echo "  $MANIFEST_CSV"
echo
echo "CSV files:"
find "$RUN_DIR" -maxdepth 1 -type f -name '*.csv' -printf '  %f\n' | sort
echo "================================================================================"
