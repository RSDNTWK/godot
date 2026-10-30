extends Node

var completed: int = 0
var guard := Mutex.new()

func _work(_index: int) -> void:
	guard.lock()
	completed += 1
	guard.unlock()

func _nested_work(index: int) -> void:
	_work(index)
	if index == 0:
		var child: int = WorkerThreadPool.add_group_task(_work, 3, 5)
		WorkerThreadPool.wait_for_group_task_completion(child)

func _ready() -> void:
	var threaded := OS.has_feature("threads")
	var groups := 0
	var failures: Array[String] = []
	for repeat in range(32):
		for elements in [0, 1, 9]:
			for tasks in [1, 3, 12]:
				var before := completed
				var group: int = WorkerThreadPool.add_group_task(_work, elements, tasks)
				if not threaded and not WorkerThreadPool.is_group_task_completed(group):
					failures.append("Synchronous group was incomplete")
				WorkerThreadPool.wait_for_group_task_completion(group)
				if completed - before != elements:
					failures.append("Group callback count changed")
				groups += 1
	if not threaded:
		var before := completed
		var parent: int = WorkerThreadPool.add_group_task(_nested_work, 4, 6)
		WorkerThreadPool.wait_for_group_task_completion(parent)
		if completed - before != 7:
			failures.append("Nested group callback count changed")
		groups += 2
	var expected := 960 if threaded else 967
	if completed != expected:
		failures.append("Total callback count changed")
	var report := {"passed": failures.is_empty(), "callbacks": completed, "expected_callbacks": expected, "groups": groups, "threaded": threaded, "failures": failures, "version_hash": Engine.get_version_info().get("hash", "")}
	var text := "GROUP_RECLAMATION_RESULT " + JSON.stringify(report)
	print(text)
	if Engine.has_singleton("JavaScriptBridge"):
		Engine.get_singleton("JavaScriptBridge").eval("var p=document.createElement('pre');p.id='group-result';p.textContent=" + JSON.stringify(text) + ";document.body.appendChild(p);", true)
	await get_tree().create_timer(2.0).timeout
	get_tree().quit(0 if report.passed else 1)
