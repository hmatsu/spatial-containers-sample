extends Node3D
## 3D model viewer sample for XR_EXT_spatial_container.
##
## Bounded mode: the model is scaled to fit inside the container bounds and slowly spins.
## Immersive mode: the model is shown at real scale (1 unit = 1 m) in front of the user.
## Without the extension (e.g. an OpenXR 1.0 runtime), a virtual container with the
## project setting bounds is emulated so the same app still works.

const ROTATE_SPEED := 90.0  # degrees per second
const ZOOM_STEP := 1.1
const OFFSET_STEP := 0.01  # meters

## Fraction of the container the model's bounding sphere may fill.
@export_range(0.1, 1.0) var fill_ratio := 0.9
## Auto spin speed in bounded mode (degrees per second).
@export var spin_speed := 15.0
## Where the content goes when the play space is not the container space
## (immersive mode and emulation). Can be overridden with --offset=x,y,z.
@export var immersive_offset := Vector3(0, 0, -1.0)

var xr_interface: OpenXRInterface
var container_ext_active := false

var bounds_mode := OpenXRSpatialContainerState.BOUNDS_MODE_BOUNDED
var bounds := Vector3.ZERO  # Zero means infinite (immersive).
var visible_state := false
var interactable := false

var model: Node3D
var model_radius := 0.5
var model_center := Vector3.ZERO
var user_zoom := 1.0
var auto_spin := true
var show_bounds_box := true

@onready var container_root: Node3D = $XROrigin3D/ContainerRoot
@onready var model_pivot: Node3D = $XROrigin3D/ContainerRoot/ModelPivot
@onready var bounds_box: MeshInstance3D = $XROrigin3D/ContainerRoot/BoundsBox
@onready var status_label: Label3D = $XROrigin3D/ContainerRoot/StatusLabel


func _ready() -> void:
	xr_interface = XRServer.find_interface("OpenXR")
	if not xr_interface or not xr_interface.is_initialized():
		push_error("OpenXR not initialized, quitting.")
		get_tree().quit()
		return

	get_viewport().use_xr = true
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	xr_interface.session_begun.connect(_on_session_begun)

	var ext := OpenXRSpatialContainerExtension
	container_ext_active = ext.is_enabled()
	if container_ext_active:
		ext.spatial_container_bounds_changed.connect(_on_bounds_changed)
		ext.spatial_container_bounds_mode_request_denied.connect(_on_bounds_mode_request_denied)
		ext.spatial_container_visible_changed.connect(_on_visible_changed)
		ext.spatial_container_visible_request_denied.connect(_on_visible_request_denied)
		ext.spatial_container_interactable_changed.connect(_on_interactable_changed)
		ext.spatial_container_closed.connect(_on_closed)
	else:
		print("Spatial container extension not available, emulating a bounded container.")
		bounds = ProjectSettings.get_setting("xr/openxr/extensions/spatial_container/bounds")
		var bounds_arg = _parse_vector3_arg(OS.get_cmdline_user_args(), "--bounds=")
		if bounds_arg != null:
			bounds = bounds_arg

	var offset_arg = _parse_vector3_arg(OS.get_cmdline_user_args(), "--offset=")
	if offset_arg != null:
		immersive_offset = offset_arg

	for controller: XRController3D in [$XROrigin3D/LeftController, $XROrigin3D/RightController]:
		controller.button_pressed.connect(_on_controller_button_pressed)

	get_window().files_dropped.connect(_on_files_dropped)

	var toggle_every := _parse_string_arg(OS.get_cmdline_user_args(), "--toggle-every=")
	if toggle_every.is_valid_float():
		_start_auto_toggle(toggle_every.to_float())

	var path := _parse_string_arg(OS.get_cmdline_user_args(), "--model=")
	if path.is_empty() or not _load_model(path):
		_set_model(_build_default_model())
	_apply_layout()


func _process(delta: float) -> void:
	var yaw := 0.0
	var pitch := 0.0
	if _accepts_input():
		yaw = Input.get_axis("ui_left", "ui_right")
		pitch = Input.get_axis("ui_up", "ui_down")
		for controller: XRController3D in [$XROrigin3D/LeftController, $XROrigin3D/RightController]:
			var stick := controller.get_vector2("primary")
			yaw += stick.x
			pitch -= stick.y
	if absf(yaw) > 0.1 or absf(pitch) > 0.1:
		auto_spin = false
	if auto_spin and _is_bounded():
		yaw = spin_speed / ROTATE_SPEED

	model_pivot.rotate_y(deg_to_rad(yaw * ROTATE_SPEED * delta))
	model_pivot.rotate_object_local(Vector3.RIGHT, deg_to_rad(pitch * ROTATE_SPEED * delta))
	_update_status()


func _unhandled_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed or not _accepts_input():
		return
	# Offset nudging repeats while held; other keys act once per press.
	var nudge: Vector3 = (
		{
			KEY_J: Vector3.LEFT,
			KEY_L: Vector3.RIGHT,
			KEY_U: Vector3.DOWN,
			KEY_O: Vector3.UP,
			KEY_I: Vector3.FORWARD,
			KEY_K: Vector3.BACK,
		}
		. get(event.keycode, Vector3.ZERO)
	)
	if nudge != Vector3.ZERO:
		immersive_offset += nudge * OFFSET_STEP
		print("Offset: --offset=%.2f,%.2f,%.2f" % [immersive_offset.x, immersive_offset.y, immersive_offset.z])
		_apply_layout()
		return
	if event.echo:
		return
	match event.keycode:
		KEY_SPACE, KEY_ENTER:
			_toggle_bounds_mode()
		KEY_B:
			show_bounds_box = not show_bounds_box
			_update_bounds_box()
		KEY_R:
			model_pivot.transform = Transform3D.IDENTITY
			user_zoom = 1.0
			auto_spin = true
			_apply_layout()
		KEY_EQUAL, KEY_KP_ADD:
			user_zoom *= ZOOM_STEP
			_apply_layout()
		KEY_MINUS, KEY_KP_SUBTRACT:
			user_zoom /= ZOOM_STEP
			_apply_layout()


func _on_controller_button_pressed(button: String) -> void:
	if not _accepts_input():
		return
	if button == "ax_button" or button == "trigger_click":
		_toggle_bounds_mode()


## The spec lets the runtime decide which containers receive input.
## In emulation there is no runtime policy, so input is always accepted.
func _accepts_input() -> bool:
	return interactable or not container_ext_active


# --- Spatial container lifecycle -------------------------------------------------


func _on_session_begun() -> void:
	if not container_ext_active:
		return
	print("Supported bounds modes: ", OpenXRSpatialContainerExtension.get_supported_bounds_modes())
	var state := OpenXRSpatialContainerExtension.get_spatial_container_state()
	if state:
		bounds_mode = state.get_bounds_mode()
		visible_state = state.is_visible()
		interactable = state.is_interactable()
	bounds = OpenXRSpatialContainerExtension.get_spatial_container_bounds()
	_apply_layout()


func _on_bounds_changed(
	_rid: RID, infinite_bounds: bool, mode: OpenXRSpatialContainerState.BoundsMode, new_bounds: Vector3
) -> void:
	print("Bounds changed: mode=%d infinite=%s bounds=%s" % [mode, infinite_bounds, new_bounds])
	bounds_mode = mode
	bounds = Vector3.ZERO if infinite_bounds else new_bounds
	_apply_layout()


func _on_bounds_mode_request_denied(_rid: RID) -> void:
	print("Bounds mode request denied by the runtime.")


func _on_visible_changed(_rid: RID, is_visible: bool) -> void:
	print("Visible changed: ", is_visible)
	visible_state = is_visible
	# Nothing is shown while hidden, so stop per-frame work.
	set_process(is_visible)


func _on_visible_request_denied(_rid: RID) -> void:
	print("Visible request denied by the runtime.")


func _on_interactable_changed(_rid: RID, is_interactable: bool) -> void:
	print("Interactable changed: ", is_interactable)
	interactable = is_interactable


func _on_closed(_rid: RID) -> void:
	print("Spatial container closed by the runtime, quitting.")
	get_tree().quit()


func _toggle_bounds_mode() -> void:
	var next := (
		OpenXRSpatialContainerState.BOUNDS_MODE_IMMERSIVE
		if _is_bounded()
		else OpenXRSpatialContainerState.BOUNDS_MODE_BOUNDED
	)
	if not container_ext_active:
		# Emulation: switch locally.
		bounds_mode = next
		_apply_layout()
		return
	if not OpenXRSpatialContainerExtension.get_supported_bounds_modes().has(next):
		print("Bounds mode %d is not supported by this runtime." % next)
		return
	# Asynchronous: the result arrives as bounds_changed or bounds_mode_request_denied.
	OpenXRSpatialContainerExtension.request_spatial_container_bounds_mode(next)


## Switches the bounds mode every `seconds`, for unattended runs (soak tests on a
## display, automated end-to-end tests). Returns the timer, or null if disabled.
func _start_auto_toggle(seconds: float) -> Timer:
	if seconds <= 0.0:
		return null
	var timer := Timer.new()
	timer.name = "AutoToggle"
	timer.wait_time = seconds
	timer.autostart = true
	timer.timeout.connect(_toggle_bounds_mode)
	add_child(timer)
	print("Switching bounds mode every %.1f s" % seconds)
	return timer


func _is_bounded() -> bool:
	return bounds_mode == OpenXRSpatialContainerState.BOUNDS_MODE_BOUNDED


# --- Layout ----------------------------------------------------------------------


func _apply_layout() -> void:
	if not model:
		return

	# With the extension in bounded mode, Godot uses the container space as play space,
	# so the origin is already the container center. Otherwise place content in front.
	var in_container_space := container_ext_active and _is_bounded()
	container_root.position = Vector3.ZERO if in_container_space else immersive_offset

	var scale_factor := user_zoom
	if _is_bounded() and bounds != Vector3.ZERO:
		# Fit the bounding sphere so the model stays inside under any rotation.
		var smallest := minf(bounds.x, minf(bounds.y, bounds.z))
		scale_factor = fill_ratio * smallest * 0.5 / model_radius
	model.scale = Vector3.ONE * scale_factor
	model.position = -model_center * scale_factor

	_update_bounds_box()
	_update_status()


func _update_bounds_box() -> void:
	var box_visible := _is_bounded() and bounds != Vector3.ZERO
	bounds_box.visible = show_bounds_box and box_visible
	status_label.position = Vector3(0, -bounds.y * 0.5 - 0.01, bounds.z * 0.5) if box_visible else Vector3(0, -0.3, 0)
	if not box_visible:
		return

	var h := bounds * 0.5
	var corners: Array[Vector3] = []
	for i in 8:
		corners.append(Vector3(h.x if i & 1 else -h.x, h.y if i & 2 else -h.y, h.z if i & 4 else -h.z))
	var mesh := ImmediateMesh.new()
	mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for edge in [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]]:
		mesh.surface_add_vertex(corners[edge[0]])
		mesh.surface_add_vertex(corners[edge[1]])
	mesh.surface_end()
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(0.3, 0.8, 1.0)
	mesh.surface_set_material(0, material)
	bounds_box.mesh = mesh


func _update_status() -> void:
	var lines: PackedStringArray = []
	lines.append("ext: %s" % ("active" if container_ext_active else "emulated"))
	lines.append("mode: %s" % ("bounded" if _is_bounded() else "immersive"))
	if bounds != Vector3.ZERO:
		lines.append("bounds: %.2f x %.2f x %.2f m" % [bounds.x, bounds.y, bounds.z])
	if container_ext_active:
		lines.append("visible: %s  interactable: %s" % [visible_state, interactable])
	if container_root.position != Vector3.ZERO:
		lines.append("offset: %.2f, %.2f, %.2f m" % [immersive_offset.x, immersive_offset.y, immersive_offset.z])
	status_label.text = "\n".join(lines)


# --- Model loading ---------------------------------------------------------------


## Parses "<prefix>x,y,z" from command line arguments. Returns null if absent or invalid.
func _parse_vector3_arg(args: PackedStringArray, prefix: String) -> Variant:
	for arg in args:
		if not arg.begins_with(prefix):
			continue
		var parts := arg.trim_prefix(prefix).split(",")
		if parts.size() == 3 and Array(parts).all(func(p: String) -> bool: return p.strip_edges().is_valid_float()):
			return Vector3(parts[0].to_float(), parts[1].to_float(), parts[2].to_float())
		push_warning("Ignoring invalid argument: " + arg)
	return null


## Returns the value of "<prefix>value" from command line arguments, or "" if absent.
func _parse_string_arg(args: PackedStringArray, prefix: String) -> String:
	for arg in args:
		if arg.begins_with(prefix):
			return arg.trim_prefix(prefix)
	return ""


func _on_files_dropped(files: PackedStringArray) -> void:
	for file in files:
		if file.get_extension().to_lower() in ["glb", "gltf"] and _load_model(file):
			return


func _load_model(path: String) -> bool:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	if document.append_from_file(path, state) != OK:
		push_error("Failed to load glTF: " + path)
		return false
	var scene := document.generate_scene(state)
	if not scene is Node3D:
		push_error("glTF has no 3D root: " + path)
		return false
	print("Loaded model: ", path)
	_set_model(scene)
	return true


func _set_model(new_model: Node3D) -> void:
	if model:
		model.queue_free()
	model = new_model
	model_pivot.add_child(model)
	model_pivot.transform = Transform3D.IDENTITY
	auto_spin = true

	var aabb := _compute_aabb(model)
	model_center = aabb.get_center()
	model_radius = maxf(aabb.size.length() * 0.5, 0.001)
	_apply_layout()


func _compute_aabb(root: Node3D) -> AABB:
	var result := AABB()
	var first := true
	var to_root := root.global_transform.affine_inverse()
	for node in root.find_children("*", "VisualInstance3D", true, false):
		var vi := node as VisualInstance3D
		var aabb: AABB = (to_root * vi.global_transform) * vi.get_aabb()
		result = aabb if first else result.merge(aabb)
		first = false
	return result if not first else AABB(Vector3(-0.5, -0.5, -0.5), Vector3.ONE)


## Simple built-in model (about 30 cm tall) used when no glTF is given.
func _build_default_model() -> Node3D:
	var root := Node3D.new()
	root.name = "DefaultModel"
	var parts := [
		[BoxMesh.new(), Vector3(0, 0.05, 0), Color(0.85, 0.85, 0.9)],
		[SphereMesh.new(), Vector3(0, 0.2, 0), Color(0.28, 0.55, 0.75)],
		[TorusMesh.new(), Vector3(0, 0.2, 0), Color(0.95, 0.6, 0.2)],
	]
	(parts[0][0] as BoxMesh).size = Vector3(0.2, 0.1, 0.2)
	(parts[1][0] as SphereMesh).radius = 0.08
	(parts[1][0] as SphereMesh).height = 0.16
	(parts[2][0] as TorusMesh).inner_radius = 0.1
	(parts[2][0] as TorusMesh).outer_radius = 0.12
	for part in parts:
		var mi := MeshInstance3D.new()
		mi.mesh = part[0]
		mi.position = part[1]
		var material := StandardMaterial3D.new()
		material.albedo_color = part[2]
		mi.material_override = material
		root.add_child(mi)
	root.get_child(2).rotation_degrees = Vector3(70, 0, 0)
	return root
