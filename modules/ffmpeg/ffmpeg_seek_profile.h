#pragma once

#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

// Serialized decode-owner diagnostics. No media bytes, URLs, or clock changes.
class FFmpegSeekProfile {
public:
	enum Stage { STOP_READER, CONTAINER_SEEK, FLUSH, DEMUX, RECEIVE, SEND, CONVERT, STAGE_COUNT };
	struct Counter {
		uint64_t usec = 0;
		uint64_t calls = 0;
	};
	Counter counters[STAGE_COUNT];
	uint64_t began = 0;
	uint64_t sequence = 0;
	uint64_t audio_usec = 0;
	uint64_t video_usec = 0;
	bool parallel_audio = false;
	bool hardware = false;
	double target = 0.0;
	String decoder;

	class Scope {
		FFmpegSeekProfile *profile = nullptr;
		Stage stage;
		uint64_t began = 0;

	public:
		Scope(FFmpegSeekProfile *p_profile, Stage p_stage) :
				profile(p_profile), stage(p_stage) {
			if (profile) {
				began = OS::get_singleton()->get_ticks_usec();
			}
		}
		~Scope() {
			if (profile) {
				profile->counters[stage].usec += OS::get_singleton()->get_ticks_usec() - began;
				profile->counters[stage].calls++;
			}
		}
	};

	void report(Error p_error) const {
		Dictionary row;
		row["sequence"] = int64_t(sequence);
		row["target"] = target;
		row["decoder"] = decoder;
		row["hardware"] = hardware;
		row["error"] = int(p_error);
		row["total_usec"] = int64_t(OS::get_singleton()->get_ticks_usec() - began);
		row["audio_usec"] = int64_t(audio_usec);
		row["video_usec"] = int64_t(video_usec);
		row["parallel_audio"] = parallel_audio;
		row["inclusive_times_not_summed"] = true;
		const char *names[STAGE_COUNT] = { "stop_reader", "container_seek", "flush", "demux", "receive", "send", "convert" };
		Dictionary phases;
		for (int i = 0; i < STAGE_COUNT; i++) {
			Dictionary counter;
			counter["usec"] = int64_t(counters[i].usec);
			counter["calls"] = int64_t(counters[i].calls);
			phases[names[i]] = counter;
		}
		row["phases"] = phases;
		print_line("FFMPEG_SEEK_PROFILE27 " + JSON::stringify(row));
	}
};

class FFmpegSeekProfileSession {
	FFmpegSeekProfile storage;
	FFmpegSeekProfile *&slot;
	const Error &error;
	bool owns_slot = false;

public:
	FFmpegSeekProfileSession(FFmpegSeekProfile *&p_slot, uint64_t &p_reports, bool p_remote, double p_target, const Error &p_error, const char *p_decoder, bool p_hardware) :
			slot(p_slot), error(p_error) {
		if (!slot && p_remote && p_reports < 64 && is_print_verbose_enabled()) {
			storage.began = OS::get_singleton()->get_ticks_usec();
			storage.sequence = ++p_reports;
			storage.target = p_target;
			storage.decoder = p_decoder;
			storage.hardware = p_hardware;
			slot = &storage;
			owns_slot = true;
		}
	}
	~FFmpegSeekProfileSession() {
		if (owns_slot) {
			storage.report(error);
			slot = nullptr;
		}
	}
};
