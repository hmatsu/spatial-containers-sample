extends SceneTree
## Unit tests for the viewer (main.gd). They run without an XR runtime, so main.gd
## stays in emulation mode unless a test sets the extension state itself.
##
## Usage: godot --headless --path samples/container-viewer -s res://tests/run_tests.gd
## If an OpenXR runtime is installed, disable it, e.g. XR_RUNTIME_JSON=/nonexistent.json.
## The exit code is the number of failed tests.

const MainScene := preload("res://main.tscn")
const EPSILON := 1e-4

var _failures: PackedStringArray = []
var _current := ""
var _checks := 0


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var tests: Array[String] = []
	for method in get_script().get_script_method_list():
		if method.name.begins_with("test_"):
			tests.append(method.name)
	var failed_tests := 0
	for test in tests:
		_current = test
		var before := _failures.size()
		var viewer: Node3D = _make_viewer()
		call(test, viewer)
		viewer.free()
		if _failures.size() > before:
			failed_tests += 1
			print("FAIL ", test)
		else:
			print("ok   ", test)
	for failure in _failures:
		printerr(failure)
	print("%d tests, %d checks, %d failed" % [tests.size(), _checks, failed_tests])
	quit(failed_tests)


## A viewer with the built-in model, as main.gd sets it up without XR.
func _make_viewer() -> Node3D:
	var viewer: Node3D = MainScene.instantiate()
	root.add_child(viewer)  # _ready() returns early: OpenXR is not available here.
	viewer.bounds = Vector3(0.3, 0.2, 0.2)
	viewer._set_model(viewer._build_default_model())
	return viewer


func _check(condition: bool, message: String) -> void:
	_checks += 1
	if not condition:
		_failures.append("%s: %s" % [_current, message])


func _check_near(actual: float, expected: float, message: String) -> void:
	_check(absf(actual - expected) < EPSILON, "%s: %f, expected %f" % [message, actual, expected])


## AABB of the model in container-root space, including the model's own transform.
func _model_aabb(viewer: Node3D) -> AABB:
	return viewer._compute_aabb(viewer.model_pivot)


func _key(viewer: Node3D, keycode: Key) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.pressed = true
	viewer._unhandled_input(event)


# --- Argument parsing ------------------------------------------------------------


func test_parse_vector3_arg(viewer: Node3D) -> void:
	var args := PackedStringArray(["--model=a.glb", "--offset=0, -0.1 ,-0.5"])
	_check(viewer._parse_vector3_arg(args, "--offset=") == Vector3(0, -0.1, -0.5), "offset parsed")
	_check(viewer._parse_vector3_arg(args, "--bounds=") == null, "missing argument is null")
	_check(viewer._parse_vector3_arg(PackedStringArray(["--offset=1,2"]), "--offset=") == null, "two values")
	_check(viewer._parse_vector3_arg(PackedStringArray(["--offset=1,x,3"]), "--offset=") == null, "not a number")


func test_parse_string_arg(viewer: Node3D) -> void:
	var args := PackedStringArray(["--offset=0,0,0", "--model=C:/models/a b.glb"])
	_check(viewer._parse_string_arg(args, "--model=") == "C:/models/a b.glb", "model path")
	_check(viewer._parse_string_arg(PackedStringArray(), "--model=") == "", "missing argument is empty")


# --- Layout ------------------------------------------------------------------------


func test_bounded_model_fits_at_any_rotation(viewer: Node3D) -> void:
	var half: Vector3 = viewer.bounds * 0.5
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	for i in 20:
		viewer.model_pivot.rotation = Vector3(rng.randf() * TAU, rng.randf() * TAU, rng.randf() * TAU)
		# The pivot's rotation is applied to the model's corners in container-root space.
		var aabb := _model_aabb(viewer)
		for corner_index in 8:
			var corner: Vector3 = viewer.model_pivot.transform * aabb.get_endpoint(corner_index)
			var inside := absf(corner.x) <= half.x + EPSILON and absf(corner.y) <= half.y + EPSILON
			inside = inside and absf(corner.z) <= half.z + EPSILON
			_check(inside, "corner %s outside bounds at rotation %s" % [corner, viewer.model_pivot.rotation])


func test_bounded_scale_uses_the_smallest_side(viewer: Node3D) -> void:
	# The bounding sphere's diameter matches the smallest side times fill_ratio.
	var diameter: float = viewer.model_radius * 2.0 * viewer.model.scale.x
	_check_near(diameter, 0.2 * viewer.fill_ratio, "fitted diameter")
	viewer.bounds = Vector3(1.0, 0.5, 2.0)
	viewer._apply_layout()
	_check_near(viewer.model_radius * 2.0 * viewer.model.scale.x, 0.5 * viewer.fill_ratio, "after bounds change")


func test_bounded_model_is_centered(viewer: Node3D) -> void:
	var center := _model_aabb(viewer).get_center()
	_check(center.length() < EPSILON, "model center %s is not at the container center" % center)


func test_immersive_is_real_scale_and_zoomable(viewer: Node3D) -> void:
	viewer._toggle_bounds_mode()  # Emulation switches locally.
	_check(not viewer._is_bounded(), "mode is immersive")
	_check_near(viewer.model.scale.x, 1.0, "real scale")
	_key(viewer, KEY_EQUAL)
	_check_near(viewer.model.scale.x, viewer.ZOOM_STEP, "zoomed in")
	_key(viewer, KEY_R)
	_check_near(viewer.model.scale.x, 1.0, "reset")


func test_placement_in_emulation_and_with_the_extension(viewer: Node3D) -> void:
	_check(viewer.container_root.position == viewer.immersive_offset, "emulation uses the offset")
	viewer.container_ext_active = true
	viewer._apply_layout()
	_check(viewer.container_root.position == Vector3.ZERO, "bounded with the extension uses the container center")
	viewer.bounds_mode = OpenXRSpatialContainerState.BOUNDS_MODE_IMMERSIVE
	viewer._apply_layout()
	_check(viewer.container_root.position == viewer.immersive_offset, "immersive with the extension uses the offset")


func test_bounds_box_follows_the_mode(viewer: Node3D) -> void:
	_check(viewer.bounds_box.visible, "box visible when bounded")
	_key(viewer, KEY_B)
	_check(not viewer.bounds_box.visible, "B hides the box")
	_key(viewer, KEY_B)
	viewer._toggle_bounds_mode()
	_check(not viewer.bounds_box.visible, "no box in immersive")


func test_load_gltf_scales_any_model_into_the_bounds(viewer: Node3D) -> void:
	# A 20x larger model, through a glTF file, ends up the same size in the container.
	var big: Node3D = viewer._build_default_model()
	big.scale = Vector3.ONE * 20.0
	var holder := Node3D.new()
	holder.add_child(big)
	root.add_child(holder)
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	document.append_from_scene(holder, state)
	var path := OS.get_user_data_dir().path_join("unit_test_model.glb")
	_check(document.write_to_filesystem(state, path) == OK, "export glb")
	holder.free()

	var expected_diameter: float = viewer.model_radius * 2.0 * viewer.model.scale.x
	_check(viewer._load_model(path), "load glb")
	_check_near(viewer.model_radius * 2.0 * viewer.model.scale.x, expected_diameter, "same fitted size")
	_check(not viewer._load_model(path + ".missing"), "missing file fails")


# --- Extension events and input --------------------------------------------------


func test_bounds_changed_event(viewer: Node3D) -> void:
	viewer.container_ext_active = true
	viewer._on_bounds_changed(RID(), true, OpenXRSpatialContainerState.BOUNDS_MODE_IMMERSIVE, Vector3.ZERO)
	_check(not viewer._is_bounded(), "immersive")
	_check(viewer.bounds == Vector3.ZERO, "infinite bounds are stored as zero")
	_check_near(viewer.model.scale.x, 1.0, "real scale")
	viewer._on_bounds_changed(RID(), false, OpenXRSpatialContainerState.BOUNDS_MODE_BOUNDED, Vector3(0.4, 0.4, 0.4))
	_check(viewer.bounds == Vector3(0.4, 0.4, 0.4), "new bounds")
	_check_near(viewer.model_radius * 2.0 * viewer.model.scale.x, 0.4 * viewer.fill_ratio, "refitted")


func test_input_requires_interactable_with_the_extension(viewer: Node3D) -> void:
	_check(viewer._accepts_input(), "emulation always accepts input")
	viewer.container_ext_active = true
	viewer.interactable = false
	_check(not viewer._accepts_input(), "not interactable")
	var offset: Vector3 = viewer.immersive_offset
	_key(viewer, KEY_I)
	_check(viewer.immersive_offset == offset, "keys are ignored while not interactable")
	viewer._on_interactable_changed(RID(), true)
	_check(viewer._accepts_input(), "interactable")


func test_offset_nudging(viewer: Node3D) -> void:
	var start: Vector3 = viewer.immersive_offset
	_key(viewer, KEY_I)
	_key(viewer, KEY_I)
	_key(viewer, KEY_L)
	_key(viewer, KEY_U)
	var moved: Vector3 = viewer.immersive_offset - start
	_check_near(moved.z, -0.02, "forward 2 cm")
	_check_near(moved.x, 0.01, "right 1 cm")
	_check_near(moved.y, -0.01, "down 1 cm")
	_check(viewer.container_root.position == viewer.immersive_offset, "the content follows")


func test_hidden_container_stops_processing(viewer: Node3D) -> void:
	viewer.container_ext_active = true
	viewer._on_visible_changed(RID(), false)
	_check(not viewer.is_processing(), "processing stops while hidden")
	viewer._on_visible_changed(RID(), true)
	_check(viewer.is_processing(), "processing resumes")


func test_auto_toggle(viewer: Node3D) -> void:
	_check(viewer._start_auto_toggle(0.0) == null, "disabled for 0")
	var timer: Timer = viewer._start_auto_toggle(2.5)
	_check(timer != null and timer.wait_time == 2.5, "timer created")
	timer.timeout.emit()
	_check(not viewer._is_bounded(), "first timeout switches to immersive")
	timer.timeout.emit()
	_check(viewer._is_bounded(), "second timeout switches back")


func test_status_label(viewer: Node3D) -> void:
	var text: String = viewer.status_label.text
	_check(text.contains("ext: emulated"), "emulated: " + text)
	_check(text.contains("mode: bounded"), "bounded: " + text)
	_check(text.contains("bounds: 0.30 x 0.20 x 0.20 m"), "bounds: " + text)
	viewer.container_ext_active = true
	viewer.visible_state = true
	viewer.interactable = true
	viewer._update_status()
	text = viewer.status_label.text
	_check(text.contains("ext: active"), "active: " + text)
	_check(text.contains("visible: true  interactable: true"), "state: " + text)
