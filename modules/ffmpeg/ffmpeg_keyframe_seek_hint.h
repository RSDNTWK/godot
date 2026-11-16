#pragma once

#include "core/templates/vector.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <cstdint>

// Route-selection hint only. Capture before starting the packet producer;
// never access the mutable AVStream index from the decoder while it is running.
class FFmpegKeyframeSeekHint {
	static constexpr int MAX_INDEX_ENTRIES = 65536;
	static constexpr int MAX_KEYFRAMES = 4096;
	Vector<int64_t> timestamps;
	AVRational time_base = { 0, 1 };

public:
	void clear() {
		timestamps.clear();
		time_base = { 0, 1 };
	}

	bool capture(AVStream *p_stream) {
		clear();
		if (!p_stream || p_stream->time_base.num <= 0 || p_stream->time_base.den <= 0) {
			return false;
		}
		const int count = avformat_index_get_entries_count(p_stream);
		if (count <= 0 || count > MAX_INDEX_ENTRIES) {
			return false;
		}
		Vector<int64_t> candidate;
		if (candidate.resize(MAX_KEYFRAMES) != OK) {
			return false;
		}
		int keys = 0;
		int64_t previous = INT64_MIN;
		for (int i = 0; i < count; i++) {
			const AVIndexEntry *entry = avformat_index_get_entry(p_stream, i);
			if (!entry || entry->timestamp == AV_NOPTS_VALUE || entry->timestamp < previous) {
				return false;
			}
			previous = entry->timestamp;
			if (!(entry->flags & AVINDEX_KEYFRAME)) {
				continue;
			}
			if (keys == MAX_KEYFRAMES || (keys > 0 && entry->timestamp <= candidate[keys - 1])) {
				return false;
			}
			candidate.write[keys++] = entry->timestamp;
		}
		if (!keys || candidate.resize(keys) != OK) {
			return false;
		}
		timestamps = candidate;
		time_base = p_stream->time_base;
		return true;
	}

	bool prefer_container_seek(double p_target, double p_origin, double p_decoded_position, double &r_index_position) const {
		if (timestamps.is_empty() || !std::isfinite(p_target) || !std::isfinite(p_origin) || !std::isfinite(p_decoded_position)) {
			return false;
		}
		const double ticks = (p_target + p_origin) / av_q2d(time_base);
		if (!std::isfinite(ticks) || ticks < double(INT64_MIN) || ticks >= double(INT64_MAX)) {
			return false;
		}
		// Match the existing container-seek timestamp conversion, not a new clock.
		const int64_t target = int64_t(ticks);
		int lo = 0;
		int hi = timestamps.size();
		while (lo < hi) {
			const int mid = lo + (hi - lo) / 2;
			if (timestamps[mid] <= target) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (!lo) {
			return false;
		}
		const double position = timestamps[lo - 1] * av_q2d(time_base) - p_origin;
		if (!std::isfinite(position) || position <= p_decoded_position || position > p_target) {
			return false;
		}
		r_index_position = position;
		return true;
	}
};
