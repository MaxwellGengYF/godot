extends SceneTree
# Luisa Compute module self-test — Phase C headless runtime verification.
#
# WHAT IT DOES
#   1. Grabs the `LuisaCompute` engine singleton (registered by
#      modules/luisa_compute/register_types.cpp at MODULE_INITIALIZATION_LEVEL_SERVERS).
#   2. `ensure_device()` — imports Godot's live rendering device into a compute-only
#      Luisa device (backend auto-selected from the active rendering driver: "vk" for
#      Vulkan, "dx" for Direct3D 12). First call compiles/loads the DXC toolchain, so
#      allow up to ~a minute on a cold run.
#   3. Creates TWO FRESH RenderingDevice storage buffers (1024 x float32, no initial
#      data). Freshness matters: see the COPY_DEST caveat documented in
#      modules/luisa_compute/luisa_compute.cpp `wrap_storage_buffer_f32()` — a buffer
#      Godot last wrote through `buffer_update()` sits in COPY_DEST in the driver's own
#      state tracker, while the module wraps it assuming COMMON. Never `buffer_update()`
#      a buffer before handing it to the wrap / self-test path.
#   4. `wrap_storage_buffer_f32()` on the second buffer (import-only check).
#   5. `run_buffer_self_test()` on the first buffer: wrap + Luisa DSL ramp kernel
#      (buf[i] = float(i) * 2.0f) + D3D12 state reconciliation + readback through a
#      Luisa-owned staging buffer + CPU validation inside the module.
#   6. Godot-side cross-check: read the SAME buffer back through
#      `RenderingDevice.buffer_get_data()` and decode it as float32. If the state
#      reconciliation / tracker handoff were broken, this read would return stale or
#      garbage data (or the driver would raise a barrier/validation error) — it proves
#      Godot can safely reuse a buffer Luisa wrote.
#
# HOW TO RUN (one code path for both drivers)
#   cd D:/godot/bin
#   ./godot.windows.editor.x86_64.console.exe --rendering-driver vulkan \
#       -s ../modules/luisa_compute/selftest/luisa_selftest.gd
#   ./godot.windows.editor.x86_64.console.exe --rendering-driver d3d12 \
#       -s ../modules/luisa_compute/selftest/luisa_selftest.gd
#
# WHY NOT `--headless` (measured on this build, v4.8.dev.custom_build):
#   `--headless` selects the headless DisplayServer, whose create_func calls
#   RasterizerDummy::make_current() and whose only rendering driver is "dummy"
#   (servers/display/display_server_headless.h), and DisplayServer::can_create_rendering_device()
#   short-circuits to `false` for a display server named "headless"
#   (servers/display/display_server.cpp). No window => no RenderingDevice =>
#   RenderingServer.get_rendering_device() == null and LuisaCompute::ensure_device()
#   fails with "RenderingDevice singleton is not available". A RenderingDevice in this
#   engine is created by the platform DisplayServer while creating the main window, so
#   the RD-backed verification must run with a real display driver. The run below is
#   still fully non-interactive (no project needed, script quits itself, window parked
#   off-screen with --position). The script keeps the --headless case as an explicit,
#   self-explanatory FAIL instead of a mysterious one.
#
# Exit codes: 0 = PASS, 1 = FAIL (also printed as LUISA_SELFTEST PASS/FAIL <driver>).
#
# PHASE C MEASURED RESULTS (see docs/luisa_compute_d3d12_plan.md "Phase C results")
#   d3d12 : PASS end-to-end (wrap + ramp kernel writing through the imported
#           ID3D12Resource + CPU validation + the Godot-side reuse cross-check below).
#   vulkan: the imported device, the DSL ramp kernel and the readback all work, but a
#           Godot storage buffer CANNOT be imported: RenderingDevice::
#           get_driver_resource(DRIVER_RESOURCE_BUFFER) on the Vulkan driver returns
#           the driver's private BufferInfo pointer instead of a VkBuffer, so the
#           module refuses the wrap (LUISA_COMPUTE_VK_NATIVE_BUFFER_WRAP in
#           luisa_rd_bridge.cpp). Handing that value over as a VkBuffer faulted the
#           shared VkDevice ("buffer must be a valid VkBuffer handle" ->
#           ERROR_DEVICE_LOST). The self-test therefore reports leg 1 OK (the compute
#           companion itself works) and then FAILs on the wrap: that blocker is in
#           drivers/vulkan, outside this module's scope.
#   With --gpu-validation on d3d12 the debug layer breaks while recording the dispatch
#   over the imported resource, independently of the module's state-reconcile toggle
#   (reproduced with LUISA_COMPUTE_DX_RECONCILE_VIA_CUSTOM_CMD both 1 and 0).

const ELEM_COUNT := 1024 # float32 elements
const BYTE_SIZE := ELEM_COUNT * 4


func _initialize() -> void:
	var driver_name := RenderingServer.get_current_rendering_driver_name()

	# (a) The singleton must exist (module built with luisa_compute=yes).
	var lc: Object = Engine.get_singleton("LuisaCompute")
	if lc == null:
		_fail(driver_name, "Engine.get_singleton(\"LuisaCompute\") returned null (module not built / not registered)")
		return
	print("LUISA_SELFTEST: singleton OK -> %s" % lc)

	# (b) Import Godot's device into Luisa.
	if not lc.ensure_device():
		_fail(driver_name, "ensure_device() returned false (see the module/driver errors above)")
		return
	print("LUISA_SELFTEST: ensure_device() OK (backend-active message printed by the module above)")

	# (c) Godot's rendering device.
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		_fail(driver_name, "RenderingServer.get_rendering_device() is null — this engine build only creates a\n    RenderingDevice from a platform DisplayServer (see the header comment:\n    --headless forces RasterizerDummy and can_create_rendering_device() returns false).\n    Re-run WITHOUT --headless.")
		return
	print("LUISA_SELFTEST: RenderingDevice OK -> %s" % rd)

	# (d) Two FRESH storage buffers, no initial data (see the COPY_DEST caveat above).
	var rid: RID = rd.storage_buffer_create(BYTE_SIZE)
	if rid == RID():
		_fail(driver_name, "storage_buffer_create(%d) for the self-test buffer returned an empty RID" % BYTE_SIZE)
		return
	var rid2: RID = rd.storage_buffer_create(BYTE_SIZE)
	if rid2 == RID():
		_fail(driver_name, "storage_buffer_create(%d) for the wrap-test buffer returned an empty RID" % BYTE_SIZE)
		return
	print("LUISA_SELFTEST: buffers OK -> %s (self-test), %s (wrap-only)" % [rid, rid2])

	# (e)+(f) Order note: the full self-test runs BEFORE the standalone wrap check, so a
	# Vulkan run still shows leg 1 (device import + DSL dispatch + readback on a
	# Luisa-owned buffer) passing before it stops at the native-buffer refusal.
	# (f) Full wrap + dispatch + reconcile + readback + CPU validation in C++
	#     (prints "LuisaCompute: buffer self-test passed (N elements).").
	if not lc.run_buffer_self_test(rid, ELEM_COUNT):
		_fail(driver_name, "run_buffer_self_test(self-test RID, %d) returned false (see the LuisaCompute/LuisaRDBridge errors above)" % ELEM_COUNT)
		return
	print("LUISA_SELFTEST: run_buffer_self_test() OK")

	# (e) Import-only wrap of the second buffer.
	if not lc.wrap_storage_buffer_f32(rid2, ELEM_COUNT):
		_fail(driver_name, "wrap_storage_buffer_f32(wrap-test RID, %d) returned false" % ELEM_COUNT)
		return
	print("LUISA_SELFTEST: wrap_storage_buffer_f32() OK")

	# (g) Godot-side reuse of the buffer Luisa wrote.
	var data: PackedByteArray = rd.buffer_get_data(rid)
	if data.size() != BYTE_SIZE:
		_fail(driver_name, "buffer_get_data() returned %d bytes, expected %d" % [data.size(), BYTE_SIZE])
		return
	var first_bad := -1
	var got := 0.0
	var want := 0.0
	for i in ELEM_COUNT:
		want = float(i) * 2.0
		got = data.decode_float(i * 4)
		if got != want: # exact compare is safe: i*2 <= 2046 is exact in float32
			first_bad = i
			break
	if first_bad >= 0:
		_fail(driver_name, "Godot-side readback mismatch at index %d (expected %.6f, got %.6f) — the buffer was NOT reusable after the Luisa write (state reconciliation / tracker problem)" % [first_bad, want, got])
		return
	print("LUISA_SELFTEST: Godot readback cross-check OK (%d/%d float32 elements == i*2.0)" % [ELEM_COUNT, ELEM_COUNT])

	# The wrap-only buffer must still be readable by Godot (import alone must not
	# corrupt anything). Its contents are undefined — a GPU storage buffer created
	# without initial data is not zero-filled — so only the access is checked.
	var data2: PackedByteArray = rd.buffer_get_data(rid2)
	if data2.size() != BYTE_SIZE:
		_fail(driver_name, "buffer_get_data() on the wrap-only buffer returned %d bytes, expected %d" % [data2.size(), BYTE_SIZE])
		return
	print("LUISA_SELFTEST: wrap-only buffer still readable by Godot OK")

	lc.shutdown()
	# Free the buffers after shutdown() (the bridge registry is cleared there, so no
	# wrapped RID outlives it); otherwise RenderingDevice reports leaked RIDs at exit.
	rd.free_rid(rid2)
	rd.free_rid(rid)
	print("LUISA_SELFTEST PASS %s" % driver_name)
	quit(0)


func _fail(p_driver: String, p_what: String) -> void:
	push_error("LUISA_SELFTEST FAIL %s: %s" % [p_driver, p_what])
	print("LUISA_SELFTEST FAIL %s: %s" % [p_driver, p_what])
	quit(1)
