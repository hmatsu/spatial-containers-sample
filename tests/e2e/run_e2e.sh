#!/usr/bin/env bash
# The coexistence scenario counts the viewer's failures in a subshell on purpose.
# shellcheck disable=SC2030,SC2031
# End-to-end tests of the samples on a real OpenXR runtime, without a GPU: Monado
# (null compositor, simulated HMD) with software Vulkan (lavapipe) under Xvfb.
#
# Usage: tests/e2e/run_e2e.sh <godot binary> <emulation layer directory> [<native_widget binary>]
#   The layer directory contains XrApiLayer_spatial_container_emulation.json. Without
#   the widget, only the viewer scenarios run.
#
# Scenarios:
#   emulation     The Godot viewer on the plain runtime, using its own emulation.
#   layer         The viewer's real XR_EXT_spatial_container path through the emulation
#                 layer, switching bounds modes every second.
#   widget        The native widget through the layer, rendering with Vulkan.
#   coexistence   The viewer and the widget at the same time. The widget's runtime
#                 session is an overlay (XR_SCEMU_OVERLAY_PLACEMENT), so Monado shows
#                 both instead of switching to the newer app.
#
# Requires monado-service, xvfb-run and Mesa's Vulkan drivers. Logs go to
# E2E_LOG_DIR (default: a temporary directory, printed at the end).
set -uo pipefail

if (($# != 2 && $# != 3)); then
    echo "usage: $0 <godot binary> <emulation layer directory> [<native_widget binary>]" >&2
    exit 2
fi
godot=$(realpath "$1")
layer_dir=$(realpath "$2")
widget=""
if (($# == 3)); then widget=$(realpath "$3"); fi
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
    check_status $? "$logs/$name.log" "Godot ($name)"
}

check_status() {
    local status=$1 log=$2 what=$3
    if ((status != 0)); then
        echo "  FAIL  $what exited with $status"
        annotate "$log" "$what exited with $status"
        failures=$((failures + 1))
    fi
}

layer_env=(XR_API_LAYER_PATH="$layer_dir" XR_ENABLE_API_LAYERS=XR_APILAYER_spatial_container_emulation XR_SCEMU_LOG=1)

# Runs the widget with the timer started: run_widget <log> <seconds> [env...]
run_widget() {
    local log=$1 seconds=$2
    shift 2
    env "${layer_env[@]}" "$@" timeout 120 "$widget" --start --seconds "$seconds" > "$log" 2>&1
    check_status $? "$log" "Widget"
}

# Checks that a widget run showed its panel and kept it with retained frames.
expect_widget() {
    local log=$1
    expect "$log" "Visible: 1" "widget shown"
    expect "$log" "Interactable: 1" "widget interactable"
    expect "$log" "Shut down after [0-9]+ frames, [0-9]+ panel changes, [1-9][0-9]* uploads, [1-9][0-9]* retained" \
        "panel uploaded on changes and retained in between"
    expect "$log" "session summary: [1-9][0-9]* frames to the runtime, [1-9][0-9]* with container layers" \
        "widget frames with container layers reached the runtime"
    reject "$log" "failed:" "no failed OpenXR calls"
}

# Godot master can crash on exit after re-importing an already imported project; what
# matters is that the import cache exists.
if ! "$godot" --headless --path "$viewer" --import > "$logs/import.log" 2>&1; then
    if [[ ! -d "$viewer/.godot/imported" ]]; then
        echo "Project import failed; see $logs/import.log" >&2
        exit 1
    fi
    echo "warning: Godot exited with an error after importing; continuing (see $logs/import.log)"
fi
start_monado

run_viewer emulation 120 XR_API_LAYER_PATH= XR_ENABLE_API_LAYERS= --
log="$logs/emulation.log"
expect "$log" "Running on OpenXR runtime: +Monado" "runs on Monado"
expect "$log" "emulating a bounded container" "uses the viewer's emulation"
expect "$log" "On state focused" "session reaches FOCUSED"
reject "$log" "SCRIPT ERROR" "no script errors"

run_viewer layer 600 "${layer_env[@]}" -- --toggle-every=1
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

if [[ -n "$widget" ]]; then
    echo "== widget"
    log="$logs/widget.log"
    run_widget "$log" 4
    expect_widget "$log"
    grep -E "^Shut down" "$log" | sed 's/^/  info  /'

    echo "== coexistence"
    # The viewer on the left, the widget on the right, both in front of the user.
    viewer_log="$logs/coexistence-viewer.log"
    widget_log="$logs/coexistence-widget.log"
    # In a subshell, so its failures come back through the exit status.
    (
        failures=0
        run_viewer coexistence-viewer 900 "${layer_env[@]}" XR_SCEMU_POSITION=-0.3,0,-1 --
        exit $((failures > 0))
    ) &
    viewer_job=$!
    for _ in $(seq 1 240); do
        if grep -q "visible=1" "$viewer_log" 2> /dev/null; then break; fi
        sleep 0.5
    done
    expect "$viewer_log" "container 0x[0-9a-f]+ visible=1" "viewer shown before the widget starts"
    start_line=$(wc -l < "$viewer_log")
    run_widget "$widget_log" 6 XR_SCEMU_POSITION=0.3,0,-1 XR_SCEMU_OVERLAY_PLACEMENT=1
    end_line=$(wc -l < "$viewer_log")
    expect "$widget_log" "overlay=1" "widget's runtime session is an overlay"
    expect_widget "$widget_log"
    # Both shown at once: the viewer must not have been hidden while the widget ran.
    if sed -n "$((start_line + 1)),${end_line}p" "$viewer_log" | grep -q "visible=0"; then
        echo "  FAIL  viewer stayed visible while the widget was shown"
        annotate "$viewer_log" "viewer hidden while the widget was shown"
        failures=$((failures + 1))
    else
        echo "  ok    viewer stayed visible while the widget was shown"
    fi
    if ! wait "$viewer_job"; then failures=$((failures + 1)); fi
    expect "$viewer_log" "session summary: [1-9][0-9]* frames to the runtime, [1-9][0-9]* with container layers" \
        "viewer frames with container layers reached the runtime"
    reject "$viewer_log" "failed to end frame" "every viewer frame ended"
    grep -hE "session summary" "$viewer_log" "$widget_log" | sed 's/^.*\] /  info  /'
fi

echo "Logs: $logs"
if ((failures)); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "All end-to-end checks passed."
