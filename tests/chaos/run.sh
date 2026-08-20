#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FCLAW_BIN="${FCLAW_CHAOS_BIN:-$ROOT/bin/fclaw}"
CHECK_BIN="${FCLAW_CHAOS_CHECK_BIN:-$ROOT/bin/fclaw_chaos_check}"
CHAOS_ITERATIONS="${CHAOS_ITERATIONS:-500}"
CHAOS_SEED="${CHAOS_SEED:-424242}"
CHAOS_MODE="${CHAOS_MODE:-crash}"
CHAOS_SPAWN_DELAY_MS="${CHAOS_SPAWN_DELAY_MS:-30}"
CHAOS_RAMDISK_MIB="${CHAOS_RAMDISK_MIB:-16}"
CHAOS_KEEP="${CHAOS_KEEP:-0}"
GATEWAY_PID=0
CURRENT_SCRATCH=""
GATEWAY_LOG_PATH=""
RAMDISK_DEVICE=""
RAMDISK_MOUNT=""
RAMDISK_FILLER=""

die() {
  printf 'chaos: FAIL seed=%s %s\n' "$CHAOS_SEED" "$*" >&2
  exit 1
}

sleep_ms() {
  local ms="$1"
  local seconds
  printf -v seconds '0.%03d' "$ms"
  sleep "$seconds"
}

# `bus publish` speaks the public -a contract: one compact JSON object.
# Extract the envelope id; matching deliveries on the raw JSON can never
# succeed.
publish_chaos_text() {
  local text="$1" out id
  out="$("$FCLAW_BIN" bus publish --channel chaos --text "$text")" || return 1
  id="$(sed -n 's/.*"event_id":"\([^"]*\)".*/\1/p' <<<"$out")"
  [[ -n "$id" ]] || return 1
  printf '%s\n' "$id"
}

cleanup() {
  local exit_code=$?
  if [[ "$GATEWAY_PID" -gt 1 ]]; then
    kill -KILL "$GATEWAY_PID" 2>/dev/null || true
    wait "$GATEWAY_PID" 2>/dev/null || true
    GATEWAY_PID=0
  fi
  cd "$ROOT"
  if [[ -n "$RAMDISK_DEVICE" ]]; then
    if [[ "$RAMDISK_DEVICE" == /dev/disk[0-9]* ]]; then
      hdiutil detach "$RAMDISK_DEVICE" -force >/dev/null 2>&1 || true
    fi
    RAMDISK_DEVICE=""
    RAMDISK_MOUNT=""
    RAMDISK_FILLER=""
  fi
  if [[ -n "$CURRENT_SCRATCH" && -d "$CURRENT_SCRATCH" ]]; then
    if [[ "$CHAOS_KEEP" == "1" && "$exit_code" != "0" ]]; then
      printf 'chaos: retained failure workspace %s\n' "$CURRENT_SCRATCH" >&2
    else
      rm -rf -- "$CURRENT_SCRATCH"
    fi
  fi
}
trap cleanup EXIT INT TERM

new_scratch() {
  local mode="$1"
  cd "$ROOT"
  CURRENT_SCRATCH="$(mktemp -d "$ROOT/.chaos-${mode}.XXXXXX")"
  ln -s "$ROOT/floops" "$CURRENT_SCRATCH/floops"
  ln -s "$ROOT/actions" "$CURRENT_SCRATCH/actions"
  ln -s "$ROOT/config" "$CURRENT_SCRATCH/config"
  mkdir -p "$CURRENT_SCRATCH/workspace/logs"
  GATEWAY_LOG_PATH="$CURRENT_SCRATCH/workspace/logs/chaos_gateway.log"
  cd "$CURRENT_SCRATCH"
}

ramdisk_mount_point() {
  diskutil info -plist "$RAMDISK_DEVICE" | plutil -extract MountPoint raw -
}

new_ramdisk_scratch() {
  local mode="$1"
  local sectors label
  cd "$ROOT"
  CURRENT_SCRATCH="$(mktemp -d "$ROOT/.chaos-${mode}-control.XXXXXX")"
  sectors=$((CHAOS_RAMDISK_MIB * 2048))
  RAMDISK_DEVICE="$(hdiutil attach -nomount "ram://$sectors" | awk 'NR == 1 {print $1}')"
  [[ "$RAMDISK_DEVICE" == /dev/disk[0-9]* ]] ||
    die "unexpected ramdisk device=$RAMDISK_DEVICE"
  label="FCLAW_CHAOS_${$}_${mode}"
  diskutil eraseVolume HFS+ "$label" "$RAMDISK_DEVICE" >/dev/null ||
    die "ramdisk format failed device=$RAMDISK_DEVICE"
  RAMDISK_MOUNT="$(ramdisk_mount_point)"
  [[ "$RAMDISK_MOUNT" == /Volumes/* && -d "$RAMDISK_MOUNT" ]] ||
    die "unexpected ramdisk mount=$RAMDISK_MOUNT"
  mkdir -p "$RAMDISK_MOUNT/workspace/logs"
  ln -s "$ROOT/floops" "$CURRENT_SCRATCH/floops"
  ln -s "$ROOT/actions" "$CURRENT_SCRATCH/actions"
  ln -s "$ROOT/config" "$CURRENT_SCRATCH/config"
  ln -s "$RAMDISK_MOUNT/workspace" "$CURRENT_SCRATCH/workspace"
  GATEWAY_LOG_PATH="$CURRENT_SCRATCH/chaos_gateway.log"
  cd "$CURRENT_SCRATCH"
}

remove_ramdisk_scratch() {
  local doomed="$CURRENT_SCRATCH"
  cd "$ROOT"
  [[ "$RAMDISK_DEVICE" == /dev/disk[0-9]* ]] ||
    die "invalid ramdisk detach device=$RAMDISK_DEVICE"
  if ! hdiutil detach "$RAMDISK_DEVICE" >/dev/null; then
    hdiutil detach "$RAMDISK_DEVICE" -force >/dev/null ||
      die "ramdisk detach failed device=$RAMDISK_DEVICE"
  fi
  RAMDISK_DEVICE=""
  RAMDISK_MOUNT=""
  RAMDISK_FILLER=""
  [[ -n "$doomed" && -d "$doomed" ]] || die "control scratch path vanished"
  rm -rf -- "$doomed"
  CURRENT_SCRATCH=""
  GATEWAY_LOG_PATH=""
}

remove_scratch() {
  local doomed="$CURRENT_SCRATCH"
  cd "$ROOT"
  [[ -n "$doomed" && -d "$doomed" ]] || die "scratch path vanished"
  rm -rf -- "$doomed"
  CURRENT_SCRATCH=""
  GATEWAY_LOG_PATH=""
}

start_gateway() {
  local marker="${1:-}"
  local atomic_match="${2:-}"
  local append_match="${3:-}"
  local atomic_before_match="${4:-}"
  local attempt pid_file
  FCLAW_GATEWAY_DISABLE_CHANNELS=1 \
  FCLAW_GATEWAY_POLL_MS=1 \
  FCLAW_TEST_CHAOS_MARKER="$marker" \
  FCLAW_TEST_CHAOS_ATOMIC_MATCH="$atomic_match" \
  FCLAW_TEST_CHAOS_APPEND_MATCH="$append_match" \
  FCLAW_TEST_CHAOS_ATOMIC_BEFORE_MATCH="$atomic_before_match" \
    "$FCLAW_BIN" gateway run --floop fast \
      >> "$GATEWAY_LOG_PATH" 2>&1 &
  GATEWAY_PID=$!
  for ((attempt = 0; attempt < 300; attempt++)); do
    if [[ -f .fclaw/run/gateway.pid ]]; then
      IFS= read -r pid_file < .fclaw/run/gateway.pid || true
      if [[ "$pid_file" == "$GATEWAY_PID" ]] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
        return 0
      fi
    fi
    kill -0 "$GATEWAY_PID" 2>/dev/null ||
      die "gateway exited during startup scratch=$CURRENT_SCRATCH"
    sleep 0.01
  done
  die "gateway readiness timeout pid=$GATEWAY_PID scratch=$CURRENT_SCRATCH"
}

kill_gateway_hard() {
  local victim="$GATEWAY_PID"
  [[ "$victim" -gt 1 ]] || die "invalid gateway pid before SIGKILL"
  kill -KILL "$victim" 2>/dev/null ||
    die "SIGKILL failed pid=$victim scratch=$CURRENT_SCRATCH"
  wait "$victim" 2>/dev/null || true
  GATEWAY_PID=0
}

stop_gateway() {
  local victim="$GATEWAY_PID"
  [[ "$victim" -gt 1 ]] || return 0
  kill -TERM "$victim" 2>/dev/null || true
  wait "$victim" 2>/dev/null || true
  GATEWAY_PID=0
}

delivery_count() {
  local origin="$1"
  local line count=0
  if [[ -f workspace/logs/deliveries.jsonl ]]; then
    while IFS= read -r line; do
      [[ "$line" == *"\"origin_event_id\":\"$origin\""* ]] && ((count += 1))
    done < workspace/logs/deliveries.jsonl
  fi
  printf '%d\n' "$count"
}

wait_delivery_once() {
  local origin="$1"
  local iteration="$2"
  local attempt count
  for ((attempt = 0; attempt < 500; attempt++)); do
    count="$(delivery_count "$origin")"
    if [[ "$count" == "1" ]]; then return 0; fi
    if [[ "$count" -gt 1 ]]; then
      die "duplicate delivery mode=$iteration origin=$origin count=$count scratch=$CURRENT_SCRATCH"
    fi
    kill -0 "$GATEWAY_PID" 2>/dev/null ||
      die "restart crashed mode=$iteration origin=$origin scratch=$CURRENT_SCRATCH"
    sleep 0.01
  done
  die "delivery timeout mode=$iteration origin=$origin count=$(delivery_count "$origin") scratch=$CURRENT_SCRATCH"
}

run_random_campaign() {
  local iteration delay bus_id
  new_scratch random
  RANDOM="$CHAOS_SEED"
  start_gateway
  for ((iteration = 1; iteration <= CHAOS_ITERATIONS; iteration++)); do
    bus_id="$(publish_chaos_text "chaos seed=$CHAOS_SEED iteration=$iteration")" ||
      die "publish failed iteration=$iteration scratch=$CURRENT_SCRATCH"
    delay=$((RANDOM % 8 + 1))
    sleep_ms "$delay"
    kill_gateway_hard
    start_gateway
    wait_delivery_once "$bus_id" "random iteration=$iteration delay_ms=$delay"
    kill -0 "$GATEWAY_PID" 2>/dev/null ||
      die "gateway not up after random iteration=$iteration"
    sleep_ms "$CHAOS_SPAWN_DELAY_MS"
    if (( iteration % 25 == 0 || iteration == CHAOS_ITERATIONS )); then
      printf 'chaos: random seed=%s clean=%d/%d\n' \
        "$CHAOS_SEED" "$iteration" "$CHAOS_ITERATIONS"
    fi
  done
  stop_gateway
  "$CHECK_BIN" "$CHAOS_ITERATIONS"
  remove_scratch
}

wait_marker() {
  local marker="$1"
  local mode="$2"
  local attempt
  for ((attempt = 0; attempt < 500; attempt++)); do
    [[ -f "$marker" ]] && return 0
    kill -0 "$GATEWAY_PID" 2>/dev/null ||
      die "gateway exited before $mode pause marker scratch=$CURRENT_SCRATCH"
    sleep 0.01
  done
  die "$mode pause marker timeout scratch=$CURRENT_SCRATCH"
}

run_targeted() {
  local mode="$1"
  local marker bus_id atomic_match="" append_match=""
  new_scratch "$mode"
  marker="$CURRENT_SCRATCH/.chaos_pause"
  if [[ "$mode" == "state-write" ]]; then
    atomic_match="/state.json"
  else
    append_match="/event_log.jsonl"
  fi
  start_gateway "$marker" "$atomic_match" "$append_match"
  bus_id="$(publish_chaos_text "chaos seed=$CHAOS_SEED targeted=$mode")" ||
    die "targeted publish failed mode=$mode scratch=$CURRENT_SCRATCH"
  wait_marker "$marker" "$mode"
  kill_gateway_hard
  start_gateway
  wait_delivery_once "$bus_id" "$mode"
  kill -0 "$GATEWAY_PID" 2>/dev/null ||
    die "gateway not up after targeted mode=$mode"
  stop_gateway
  "$CHECK_BIN" 1
  printf 'chaos: targeted mode=%s clean=1/1\n' "$mode"
  remove_scratch
}

file_contains() {
  local path="$1"
  local needle="$2"
  local line
  [[ -f "$path" ]] || return 1
  while IFS= read -r line; do
    [[ "$line" == *"$needle"* ]] && return 0
  done < "$path"
  return 1
}

wait_file_contains() {
  local path="$1"
  local needle="$2"
  local mode="$3"
  local attempt
  for ((attempt = 0; attempt < 500; attempt++)); do
    file_contains "$path" "$needle" && return 0
    kill -0 "$GATEWAY_PID" 2>/dev/null ||
      die "gateway crashed waiting for $mode evidence"
    sleep 0.01
  done
  die "timeout waiting for $mode evidence path=$path needle=$needle"
}

fill_ramdisk() {
  local available
  RAMDISK_FILLER="$RAMDISK_MOUNT/.fclaw-chaos-fill"
  dd if=/dev/zero of="$RAMDISK_FILLER" bs=1048576 \
    > /dev/null 2> "$CURRENT_SCRATCH/fill.stderr" || true
  dd if=/dev/zero bs=4096 >> "$RAMDISK_FILLER" \
    2>> "$CURRENT_SCRATCH/fill.stderr" || true
  available="$(df -k "$RAMDISK_MOUNT" | awk 'NR == 2 {print $4}')"
  file_contains "$CURRENT_SCRATCH/fill.stderr" "No space left on device" ||
    die "ramdisk filler did not reach ENOSPC available_kib=$available"
  [[ "$available" =~ ^[0-9]+$ && "$available" -le 64 ]] ||
    die "ramdisk reserve unexpectedly large available_kib=$available"
}

restore_ramdisk_space() {
  [[ -n "$RAMDISK_FILLER" && -f "$RAMDISK_FILLER" ]] ||
    die "ramdisk filler missing before restore"
  rm -f -- "$RAMDISK_FILLER"
  RAMDISK_FILLER=""
}

remount_ramdisk() {
  local access="$1"
  local new_mount mount_line
  cd "$CURRENT_SCRATCH"
  diskutil unmount "$RAMDISK_DEVICE" >/dev/null ||
    die "ramdisk unmount failed access=$access"
  if [[ "$access" == "readonly" ]]; then
    diskutil mount readOnly nobrowse "$RAMDISK_DEVICE" >/dev/null ||
      die "ramdisk read-only mount failed"
  else
    diskutil mount nobrowse "$RAMDISK_DEVICE" >/dev/null ||
      die "ramdisk read-write mount failed"
  fi
  new_mount="$(ramdisk_mount_point)"
  [[ "$new_mount" == "$RAMDISK_MOUNT" ]] ||
    die "ramdisk mount path changed old=$RAMDISK_MOUNT new=$new_mount"
  mount_line="$(mount | while IFS= read -r line; do
    [[ "$line" == *" on $RAMDISK_MOUNT "* ]] && { printf '%s\n' "$line"; break; }
  done)"
  if [[ "$access" == "readonly" ]]; then
    [[ "$mount_line" == *", read-only,"* ]] ||
      die "ramdisk is not read-only mount=$mount_line"
  else
    [[ "$mount_line" != *", read-only,"* ]] ||
      die "ramdisk remained read-only mount=$mount_line"
  fi
}

run_filesystem_drill() {
  local mode="$1"
  local marker origin probe expected_deliveries expected_failed
  new_ramdisk_scratch "$mode"
  marker="$CURRENT_SCRATCH/.chaos_pause"
  start_gateway "$marker" "" "" "/state.json"
  origin="$(publish_chaos_text "chaos seed=$CHAOS_SEED filesystem=$mode original")" ||
    die "filesystem publish failed mode=$mode"
  wait_marker "$marker" "$mode"
  if [[ "$mode" == "disk-full" ]]; then
    fill_ramdisk
  else
    remount_ramdisk readonly
  fi
  kill -CONT "$GATEWAY_PID" || die "failed to continue gateway mode=$mode"
  sleep 0.25
  kill -0 "$GATEWAY_PID" 2>/dev/null ||
    die "gateway crashed under filesystem fault mode=$mode"
  if [[ "$mode" == "disk-full" ]]; then
    restore_ramdisk_space
  else
    remount_ramdisk readwrite
  fi
  wait_file_contains "workspace/logs/narration.jsonl" "RUN FAILED" "$mode narration"
  kill -0 "$GATEWAY_PID" 2>/dev/null ||
    die "gateway crashed after storage restore mode=$mode"
  probe="$(publish_chaos_text "chaos seed=$CHAOS_SEED filesystem=$mode recovery-probe")" ||
    die "recovery probe publish failed mode=$mode"
  wait_delivery_once "$probe" "$mode recovery probe"
  kill -0 "$GATEWAY_PID" 2>/dev/null ||
    die "surviving gateway crashed on recovery probe mode=$mode"
  stop_gateway
  start_gateway
  if [[ "$mode" == "read-only" ]]; then
    wait_delivery_once "$origin" "$mode recovered original"
    expected_deliveries=2
    expected_failed=0
  else
    expected_deliveries=1
    expected_failed=1
  fi
  kill -0 "$GATEWAY_PID" 2>/dev/null ||
    die "gateway not up after durable recovery mode=$mode"
  stop_gateway
  "$CHECK_BIN" 2 "$expected_deliveries" "$expected_failed"
  printf 'chaos: filesystem mode=%s clean=1/1\n' "$mode"
  remove_ramdisk_scratch
}

[[ "$CHAOS_ITERATIONS" =~ ^[1-9][0-9]*$ ]] || die "CHAOS_ITERATIONS must be positive"
[[ "$CHAOS_ITERATIONS" -le 2000 ]] || die "CHAOS_ITERATIONS exceeds safe checker cap"
[[ "$CHAOS_RAMDISK_MIB" =~ ^[1-9][0-9]*$ ]] || die "CHAOS_RAMDISK_MIB must be positive"
[[ "$CHAOS_RAMDISK_MIB" -ge 8 && "$CHAOS_RAMDISK_MIB" -le 64 ]] ||
  die "CHAOS_RAMDISK_MIB must be between 8 and 64"
[[ -x "$FCLAW_BIN" ]] || die "missing executable $FCLAW_BIN"
[[ -x "$CHECK_BIN" ]] || die "missing checker $CHECK_BIN"

case "$CHAOS_MODE" in
  crash)
    printf 'chaos: long-lived driver seed=%s iterations=%s spawn_delay_ms=%s\n' \
      "$CHAOS_SEED" "$CHAOS_ITERATIONS" "$CHAOS_SPAWN_DELAY_MS"
    run_random_campaign
    run_targeted state-write
    run_targeted event-append
    printf 'chaos: PASS seed=%s random=%s targeted=2\n' \
      "$CHAOS_SEED" "$CHAOS_ITERATIONS"
    ;;
  filesystem)
    printf 'chaos: filesystem driver seed=%s ramdisk_mib=%s\n' \
      "$CHAOS_SEED" "$CHAOS_RAMDISK_MIB"
    run_filesystem_drill disk-full
    run_filesystem_drill read-only
    printf 'chaos: PASS seed=%s filesystem=2\n' "$CHAOS_SEED"
    ;;
  *) die "unknown CHAOS_MODE=$CHAOS_MODE" ;;
esac
