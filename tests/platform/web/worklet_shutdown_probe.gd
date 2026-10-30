extends Node

func _ready() -> void:
	var headless := DisplayServer.get_name() == "headless"
	var expected := "Dummy" if headless else "AudioWorklet"
	var generator := AudioStreamGenerator.new()
	generator.mix_rate = AudioServer.get_mix_rate()
	generator.buffer_length = 0.1
	var player := AudioStreamPlayer.new()
	player.stream = generator
	add_child(player)
	player.play()
	for frame in range(24):
		await get_tree().process_frame
	var playback := player.get_stream_playback() as AudioStreamGeneratorPlayback
	if playback != null:
		for sample in range(mini(playback.get_frames_available(), 1024)):
			playback.push_frame(Vector2(0.02, -0.02))
	var report := {
		"driver": AudioServer.get_driver_name(),
		"expected_driver": expected,
		"headless": headless,
		"threaded": OS.has_feature("threads"),
		"web": OS.has_feature("web"),
		"player_started": player.playing and playback != null,
	}
	report["passed"] = report.web and report.threaded and report.driver == expected and report.player_started
	print("WORKLET_SHUTDOWN_RESULT " + JSON.stringify(report))
	await get_tree().create_timer(0.25).timeout
	get_tree().quit(0 if report.passed else 1)
