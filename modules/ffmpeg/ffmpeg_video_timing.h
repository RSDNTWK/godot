#pragma once

#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

// Decode-owner-only wall-clock measurements. Disabled without --verbose, with
// at most 256 bounded cumulative records per playback; no URL, PCM or pixels.
class FFmpegVideoTiming {
public:
	enum Stage { UPDATE, RECEIVE, SEND, TRANSFER, PACK_PREPARE, SCALE, PACK_FINISH, ROW_PACK, IMAGE_CREATE, TEXTURE, WORKER_SCALE, WORKER_TOTAL, STAGE_COUNT };

private:
	struct Counter {
		uint64_t usec = 0, calls = 0;
	};
	Counter counters[STAGE_COUNT];
	bool enabled = is_print_verbose_enabled();
	uint64_t last_report = 0;
	uint64_t reports = 0;
	uint64_t frames = 0;
	uint64_t seeks = 0;
	uint64_t identity = 0;
	double position = 0.0;
	double decoded_position = 0.0;
	int width = 0, height = 0, workers = 0;
	void report(bool p_final = false) {
		if (!enabled || reports >= 256 || counters[UPDATE].calls == 0) {
			return;
		}
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (!p_final && now - last_report < 1000000) {
			return;
		}
		last_report = now;
		Dictionary row;
		row["sequence"] = int64_t(++reports);
		row["backend_id"] = uitos(identity);
		row["wall_usec"] = int64_t(now);
		row["frames"] = int64_t(frames);
		row["seeks"] = int64_t(seeks);
		row["position"] = position;
		row["decoded_position"] = decoded_position;
		row["width"] = width;
		row["height"] = height;
		row["workers"] = workers;
		row["final"] = p_final;
		row["inclusive_wall_times"] = true;
		row["worker_times_overlap_owner"] = counters[WORKER_TOTAL].calls > 0;
		Dictionary stages;
		const char *names[STAGE_COUNT] = { "update", "receive", "send", "transfer", "pack_prepare", "scale", "pack_finish", "row_pack", "image_create", "texture", "worker_scale", "worker_total" };
		for (int i = 0; i < STAGE_COUNT; ++i) {
			Dictionary counter;
			counter["usec"] = int64_t(counters[i].usec);
			counter["calls"] = int64_t(counters[i].calls);
			stages[names[i]] = counter;
		}
		row["stages"] = stages;
		print_line("FFMPEG_VIDEO_TIMING24 " + JSON::stringify(row));
	}

public:
	class Scope {
		FFmpegVideoTiming &owner;
		Stage stage;
		uint64_t began = 0;

	public:
		Scope(FFmpegVideoTiming &p_owner, Stage p_stage) :
				owner(p_owner), stage(p_stage) {
			if (owner.enabled) {
				began = OS::get_singleton()->get_ticks_usec();
			}
		}
		~Scope() {
			if (owner.enabled) {
				owner.counters[stage].usec += OS::get_singleton()->get_ticks_usec() - began;
				owner.counters[stage].calls++;
				if (stage == UPDATE) {
					owner.report();
				}
			}
		}
	};
	void observe(uint64_t p_identity, double p_position, double p_decoded_position) {
		identity = p_identity;
		position = p_position;
		decoded_position = p_decoded_position;
	}
	void converted(int p_width, int p_height, int p_workers) {
		frames++;
		width = p_width;
		height = p_height;
		workers = p_workers;
	}
	void seek() { seeks++; }
	void converted_on_worker(uint64_t p_scale_usec, uint64_t p_total_usec) {
		counters[WORKER_SCALE].usec += p_scale_usec;
		counters[WORKER_SCALE].calls++;
		counters[WORKER_TOTAL].usec += p_total_usec;
		counters[WORKER_TOTAL].calls++;
	}
	void finish() { report(true); }
};
