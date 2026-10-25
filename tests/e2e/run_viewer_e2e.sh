#!/usr/bin/env bash
# End-to-end test of the Godot viewer on a real OpenXR runtime, without a GPU:
# Monado (null compositor, simulated HMD) with software Vulkan (lavapipe) under Xvfb.
#
# Usage: tests/e2e/run_viewer_e2e.sh <godot binary> <emulation layer directory>
#   The layer directory contains XrApiLayer_spatial_container_emulation.json.
#
# Scenarios:
#   emulation  The viewer on the plain runtime, using its own emulation.
#   layer      The viewer's real XR_EXT_spatial_container path through the emulation
#              layer, switching bounds modes every second.
#
# Requires monado-service, xvfb-run and Mesa's Vulkan drivers. Logs go to
# E2E_LOG_DIR (default: a temporary directory, printed at the end).
set -uo pipefail

if (($# != 2)); then
    echo "usage: $0 <godot binary> <emulation layer directory>" >&2
    exit 2
fi
godot=$(realpath "$1")
layer_dir=$(realpath "$2")
repo=$(cd "$(dirname "$0")/../.." && pwd)
viewer="$repo/samples/container-viewer"

# Keep the runtime directory short: Monado's IPC socket path must fit in 108 bytes.
work=$(mktemp -d "${E2E_TMP:-/tmp}/xr-e2e.XXXXXX")
logs=${E2E_LOG_DIR:-$work/logs}
mkdir -p "$logs"
export XDG_RUNTIME_DIR="$work/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"

monado_pid=""
cleanup() {
    if [[ -n "$monado_pid" ]]; then kill "$monado_pid" 2> /dev/null; fi
}
trap cleanup EXIT

start_monado() {
    # Monado watches stdin, which must be pollable: give it a pipe.
    (sleep infinity | XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 exec monado-service > "$logs/monado.log" 2>&1) &
    monado_pid=$!
    for _ in $(seq 1 100); do
        if [[ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ]]; then return 0; fi
        sleep 0.2
    done
    echo "Monado did not start; see $logs/monado.log" >&2
    exit 1
}

failures=0
# On GitHub Actions, put a failure and the end of its log in an annotation.
annotate() {
    local log=$1 message=$2
    if [[ -z "${GITHUB_ACTIONS:-}" ]]; then return; fi
    {
        echo -n "::error title=E2E: $message::"
        grep -vE "^\s+VK_|^Optional Vulkan" "$log" | tail -n 40 | sed -e 's/%/%25/g' -e 's/\r//g' | sed ':a;N;$!ba;s/\n/%0A/g'
    } || true
}
expect() {
    local log=$1 pattern=$2 description=$3
    if grep -qE -- "$pattern" "$log"; then
        echo "  ok    $description"
    else
        echo "  FAIL  $description (no match for: $pattern)"
        annotate "$log" "$description"
        failures=$((failures + 1))
    fi
}
reject() {
    local log=$1 pattern=$2 description=$3
    if grep -qE -- "$pattern" "$log"; then
        echo "  FAIL  $description"
        grep -E -- "$pattern" "$log" | head -3 | sed 's/^/        /'
        annotate "$log" "$description"
        failures=$((failures + 1))
    else
        echo "  ok    $description"
    fi
}

# Runs the viewer for a number of frames: run_viewer <name> <frames> [env...] -- [viewer args...]
run_viewer() {
    local name=$1 frames=$2
    shift 2
    local env_args=()
    while (($#)) && [[ "$1" != "--" ]]; do
        env_args+=("$1")
        shift
    done
    shift
    echo "== $name"
    env "${env_args[@]}" timeout 600 xvfb-run -a -s "-screen 0 1280x720x24" \
        "$godot" --path "$viewer" --rendering-driver vulkan --quit-after "$frames" -- "$@" \
        > "$logs/$name.log" 2>&1
    local status=$?
    if ((status != 0)); then
        echo "  FAIL  Godot exited with $status"
        annotate "$logs/$name.log" "Godot exited with $status ($name)"
        failures=$((failures + 1))
    fi
}

"$godot" --headless --path "$viewer" --import > "$logs/import.log" 2>&1
start_monado

run_viewer emulation 120 XR_API_LAYER_PATH= XR_ENABLE_API_LAYERS= --
log="$logs/emulation.log"
expect "$log" "Running on OpenXR runtime: +Monado" "runs on Monado"
expect "$log" "emulating a bounded container" "uses the viewer's emulation"
expect "$log" "On state focused" "session reaches FOCUSED"
reject "$log" "SCRIPT ERROR" "no script errors"

run_viewer layer 600 XR_API_LAYER_PATH="$layer_dir" \
    XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation XR_SCEMU_LOG=1 -- --toggle-every=1
log="$logs/layer.log"
expect "$log" "instance created: spatial_container=1 self_rendering=1" "layer loaded, both extensions enabled"
expect "$log" "OpenXR: Enabled extension +XR_EXT_spatial_container$" "Godot enabled XR_EXT_spatial_container"
expect "$log" "container 0x[0-9a-f]+ visible=1" "container shown"
expect "$log" "container 0x[0-9a-f]+ interactable=1" "container interactable"
expect "$log" "Bounds changed: mode=1 infinite=true" "switched to immersive"
expect "$log" "Bounds changed: mode=0 infinite=false bounds=\(0\.3, 0\.2, 0\.2\)" "switched back to bounded"
expect "$log" "session summary: [1-9][0-9]* frames to the runtime, [1-9][0-9]* with container layers" \
    "frames with container layers reached the runtime"
reject "$log" "failed to end frame" "every frame ended"
reject "$log" "SCRIPT ERROR" "no script errors"
grep -E "session summary" "$log" | sed 's/^.*\] /  info  /'

echo "Logs: $logs"
if ((failures)); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "All end-to-end checks passed."
