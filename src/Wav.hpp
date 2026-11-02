// Wav.hpp
// Reads and writes WAV files for Pollen. Cue points are read but ignored.
// Reading accepts 8/16/24/32-bit PCM and 32/64-bit float, mono or stereo, at
// any sample rate (converted to 48 kHz). Writing is 48 kHz 32-bit float
// stereo. No VCV Rack dependencies.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace pl {

struct WavData {
	std::vector<float> L, R;
	std::vector<int> cues;   // frame positions
	int sampleRate = 48000;
};

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Returns an empty string on success, or an error message.
inline std::string readWav(const std::string& path, WavData& out, size_t maxFrames) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return "Could not open the file.";
	std::vector<uint8_t> d;
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 12 || size > 1024L * 1024 * 1024) { std::fclose(f); return "Not a WAV file (or too big)."; }
	d.resize(size);
	size_t got = std::fread(d.data(), 1, size, f);
	std::fclose(f);
	if (got != (size_t) size) return "Could not read the file.";
	if (std::memcmp(&d[0], "RIFF", 4) || std::memcmp(&d[8], "WAVE", 4)) return "Not a WAV file.";

	int fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* data = nullptr;
	size_t dataLen = 0;
	std::vector<uint32_t> cues;
	size_t p = 12;
	while (p + 8 <= d.size()) {
		uint32_t len = rd32(&d[p + 4]);
		const uint8_t* body = &d[p + 8];
		size_t avail = std::min<size_t>(len, d.size() - p - 8);
		if (!std::memcmp(&d[p], "fmt ", 4) && avail >= 16) {
			fmt = rd16(body);
			channels = rd16(body + 2);
			rate = rd32(body + 4);
			bits = rd16(body + 14);
			if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24);  // extensible
		}
		else if (!std::memcmp(&d[p], "data", 4)) {
			data = body;
			dataLen = avail;
		}
		else if (!std::memcmp(&d[p], "cue ", 4) && avail >= 4) {
			uint32_t n = rd32(body);
			for (uint32_t i = 0; i < n && 4 + i * 24 + 24 <= avail; i++)
				cues.push_back(rd32(body + 4 + i * 24 + 20));  // sample offset
		}
		p += 8 + len + (len & 1);
	}
	if (!data || channels < 1 || rate < 1000) return "The WAV file has no audio data.";
	if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64))))
		return "Unsupported WAV format (use PCM or float).";

	int bps = bits / 8;
	size_t frames = dataLen / (bps * channels);
	auto sample = [&](size_t fr, int ch) -> float {
		const uint8_t* s = data + (fr * channels + ch) * bps;
		if (fmt == 3) {
			if (bits == 32) { float v; std::memcpy(&v, s, 4); return v; }
			double v; std::memcpy(&v, s, 8); return (float) v;
		}
		switch (bits) {
			case 8: return (s[0] - 128) / 128.f;
			case 16: return (int16_t) rd16(s) / 32768.f;
			case 24: { int32_t v = (s[0] << 8) | (s[1] << 16) | (s[2] << 24); return v / 2147483648.f; }
			default: return (int32_t) rd32(s) / 2147483648.f;
		}
	};

	// convert to 48 kHz (linear interpolation) and stereo
	double step = (double) rate / 48000.0;
	size_t outFrames = (size_t) (frames / step);
	outFrames = std::min(outFrames, maxFrames);
	out.L.resize(outFrames);
	out.R.resize(outFrames);
	for (size_t i = 0; i < outFrames; i++) {
		double src = i * step;
		size_t a = (size_t) src;
		size_t b = std::min(a + 1, frames - 1);
		float t = (float) (src - a);
		float l = sample(a, 0) + (sample(b, 0) - sample(a, 0)) * t;
		float r = l;
		if (channels > 1) r = sample(a, 1) + (sample(b, 1) - sample(a, 1)) * t;
		out.L[i] = l;
		out.R[i] = r;
	}
	out.cues.clear();
	for (uint32_t c : cues) {
		size_t pos = (size_t) (c / step);
		if (pos > 0 && pos < outFrames) out.cues.push_back((int) pos);
	}
	std::sort(out.cues.begin(), out.cues.end());
	out.cues.erase(std::unique(out.cues.begin(), out.cues.end()), out.cues.end());
	out.sampleRate = 48000;
	if (frames > 0 && outFrames == maxFrames && frames / step > maxFrames)
		return "";  // trimmed to the maximum reel length; not an error
	return "";
}

inline void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF); }
inline void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
inline void putTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

// Write frames [0, length) as 48 kHz float stereo with cue points at markers.
inline std::string writeWav(const std::string& path, const float* L, const float* R, int length, const int* markers, int numMarkers) {
	std::vector<uint8_t> head;
	putTag(head, "RIFF");
	put32(head, 0);  // patched below
	putTag(head, "WAVE");
	putTag(head, "fmt ");
	put32(head, 16);
	put16(head, 3);      // float
	put16(head, 2);      // stereo
	put32(head, 48000);
	put32(head, 48000 * 8);
	put16(head, 8);
	put16(head, 32);
	putTag(head, "data");
	put32(head, (uint32_t) length * 8);

	std::vector<uint8_t> cue;
	int n = 0;
	for (int i = 0; i < numMarkers; i++) if (markers[i] > 0) n++;
	if (n > 0) {
		putTag(cue, "cue ");
		put32(cue, 4 + 24 * n);
		put32(cue, n);
		int id = 1;
		for (int i = 0; i < numMarkers; i++) {
			if (markers[i] <= 0) continue;
			put32(cue, id++);
			put32(cue, markers[i]);
			putTag(cue, "data");
			put32(cue, 0);
			put32(cue, 0);
			put32(cue, markers[i]);
		}
	}
	uint32_t riffLen = (uint32_t) (4 + 24 + 8 + (size_t) length * 8 + cue.size());
	for (int i = 0; i < 4; i++) head[4 + i] = (riffLen >> (8 * i)) & 0xFF;

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return "Could not write the file.";
	std::fwrite(head.data(), 1, head.size(), f);
	std::vector<float> chunk;
	const int CH = 4096;
	for (int s = 0; s < length; s += CH) {
		int e = std::min(length, s + CH);
		chunk.resize((e - s) * 2);
		for (int i = s; i < e; i++) {
			chunk[(i - s) * 2] = L[i];
			chunk[(i - s) * 2 + 1] = R[i];
		}
		std::fwrite(chunk.data(), sizeof(float), chunk.size(), f);
	}
	if (!cue.empty()) std::fwrite(cue.data(), 1, cue.size(), f);
	std::fclose(f);
	return "";
}

} // namespace pl
