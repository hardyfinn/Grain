// GrainEngine.hpp
// The sound engine for Pollen: a stereo granular sampler with 4 Sounds, each
// holding a sample (or a live input buffer) and two Scenes, A and B, that the
// MORPH control blends between.
//
// How it works, in short:
//   * A grain is a short slice of a Sound's audio, played with an envelope.
//   * START / WRAP / SCAN choose where in the audio each new grain begins.
//   * RATE says how often grains fire; SIZE how long each one lasts;
//     PITCH how fast each grain plays its slice.
//   * SHAPE, LEVEL and TONE colour each grain; BLUR sends it to a reverb.
//   * Every one of those can be modulated from X, Y, Z (CV inputs) and RAND
//     (a new random value for every grain), with a depth stored per Scene.
//
// No VCV Rack dependencies, so it can be tested on its own.
#pragma once
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace pl {

const int BUF_RATE = 48000;                  // all audio buffers are stored at 48 kHz
const int MAX_SECONDS = 32;
const int MAX_FRAMES = MAX_SECONDS * BUF_RATE;
const int NUM_SOUNDS = 4;
const int NUM_VOICES = 64;
const int PEAK_BLOCK = 256;
const int NUM_PEAKS = MAX_FRAMES / PEAK_BLOCK + 1;
const int NUM_SHAPES = 7;

enum ParamIdx { P_START, P_WRAP, P_SCAN, P_RATE, P_SIZE, P_PITCH, P_SHAPE, P_LEVEL, P_TONE, P_BLUR, NUM_P };
enum SrcIdx { SRC_X, SRC_Y, SRC_Z, SRC_RAND, NUM_SRC };

static const char* SHAPE_NAMES[NUM_SHAPES] = {"Bell", "Sinc", "Triangle", "Decay", "Ramp", "Tukey", "Square"};
static const char* PARAM_NAMES[NUM_P] = {"START", "WRAP", "SCAN", "RATE", "SIZE", "PITCH", "SHAPE", "LEVEL", "TONE", "BLUR"};

// Panel ranges of each parameter (min, max, default).
static const float P_MIN[NUM_P] = {0.f, 0.f, -1.f, 0.f, 0.f, -1.f, 0.f, 0.f, -1.f, 0.f};
static const float P_MAX[NUM_P] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 6.f, 1.f, 1.f, 1.f};
static const float P_DEF[NUM_P] = {0.f, 0.25f, 0.f, 0.45f, 0.35f, 0.f, 0.f, 0.8f, 0.f, 0.15f};

// ---- value mappings (knob position -> real units) ------------------------
inline float sizeSeconds(float u) {          // 20 ms .. 4 s, about 0.5 s at noon
	u = std::min(1.f, std::max(0.f, u));
	return 0.02f * std::pow(200.f, std::pow(u, 0.6f));
}
const float RATE_OFF = 0.01f;                // below this, RATE fires one grain per trigger
inline float rateHz(float u) {               // 0.25 Hz .. 130.8 Hz (C3)
	if (u < RATE_OFF) return 0.f;
	return 0.25f * std::pow(130.813f / 0.25f, (u - RATE_OFF) / (1.f - RATE_OFF));
}
inline float rateMult(float u) { return 1.f + 7.f * std::min(1.f, std::max(0.f, u)); }  // RATE-SIZE link: 1x .. 8x overlap
static const float CLOCK_STEPS[9] = {1 / 16.f, 1 / 8.f, 1 / 4.f, 1 / 2.f, 1.f, 2.f, 4.f, 8.f, 16.f};
static const char* CLOCK_STEP_NAMES[9] = {"/16", "/8", "/4", "/2", "x1", "x2", "x4", "x8", "x16"};
inline int clockStep(float u) { return std::min(8, std::max(0, (int) std::lround(u * 8.f))); }
inline float scanSpeed(float s) {            // multiples of real time, +-2
	if (std::fabs(s) < 0.03f) return 0.f;
	float a = (std::fabs(s) - 0.03f) / 0.97f;
	return (s < 0 ? -2.f : 2.f) * a * a;
}
inline float toneCutoff(float t) {           // <0 low-pass, >0 high-pass, centre = off
	float a = std::fabs(t);
	if (a < 0.03f) return 0.f;
	a = (a - 0.03f) / 0.97f;
	if (t < 0) return 18000.f * std::pow(60.f / 18000.f, a);
	return 20.f * std::pow(8000.f / 20.f, a);
}

// envelope value for phase x in [0,1]
inline float shapeEnv(int shape, float x) {
	const float PI = 3.14159265f;
	switch (shape) {
		case 0: return 0.5f - 0.5f * std::cos(2 * PI * x);                         // Bell (Hann)
		case 1: {                                                                   // Sinc
			float t = (2 * x - 1) * 3.f * PI;
			float s = std::fabs(t) < 1e-4f ? 1.f : std::sin(t) / t;
			return s * (0.5f - 0.5f * std::cos(2 * PI * x));
		}
		case 2: return 1.f - std::fabs(2 * x - 1);                                  // Triangle
		case 3: return std::exp(-6.f * x) * std::min(1.f, x * 200.f);               // Decay
		case 4: return x * std::min(1.f, (1.f - x) * 200.f);                        // Ramp (rising)
		case 5: {                                                                   // Tukey (flat top)
			const float a = 0.5f;
			if (x < a / 2) return 0.5f - 0.5f * std::cos(2 * PI * x / a);
			if (x > 1 - a / 2) return 0.5f - 0.5f * std::cos(2 * PI * (1 - x) / a);
			return 1.f;
		}
		default: return 1.f;                                                        // Square
	}
}

struct Scene {
	float p[NUM_P];
	float mod[NUM_SRC][NUM_P];  // depth -1..1, 1 = the parameter's full range
	bool reverse = false;
	void reset() {
		for (int i = 0; i < NUM_P; i++) p[i] = P_DEF[i];
		std::memset(mod, 0, sizeof(mod));
		reverse = false;
	}
	Scene() { reset(); }
};

struct Sound {
	std::vector<float> L, R;    // capacity MAX_FRAMES, never reallocated while running
	int length = 0;
	std::vector<float> peaks;   // per PEAK_BLOCK frames, for the display
	Scene scene[2];
	bool live = false;          // grains come from the live input buffer
	bool pingPong = false;      // alternate forward / reverse grains
	bool linkRateSize = false;
	bool linkSizePitch = false;
	int quantMask = 0;          // 12-bit mask of allowed semitones, 0 = off
	std::string name;
	bool dirty = false;
	Sound() {
		L.assign(MAX_FRAMES, 0.f);
		R.assign(MAX_FRAMES, 0.f);
		peaks.assign(NUM_PEAKS, 0.f);
	}
	void updatePeaks(int from, int to) {
		for (int b = std::max(0, from / PEAK_BLOCK); b <= to / PEAK_BLOCK && b < NUM_PEAKS; b++) {
			int s = b * PEAK_BLOCK, e = std::min(length, s + PEAK_BLOCK);
			float m = 0.f;
			for (int i = s; i < e; i++) m = std::max(m, std::max(std::fabs(L[i]), std::fabs(R[i])));
			peaks[b] = m;
		}
	}
	void resetSettings() {
		scene[0].reset();
		scene[1].reset();
		live = pingPong = linkRateSize = linkSizePitch = false;
		quantMask = 0;
	}
};

// ---- simple stereo reverb for BLUR (4-line feedback delay network) --------
struct Reverb {
	static const int N = 4;
	std::vector<float> line[N];
	int len[N], idx[N] = {0, 0, 0, 0};
	float lp[N] = {0, 0, 0, 0};
	std::vector<float> ap[2];
	int apLen[2], apIdx[2] = {0, 0};
	float decay = 0.7f;
	void init(float sr) {
		const float ms[N] = {29.7f, 37.1f, 41.1f, 43.7f};
		for (int i = 0; i < N; i++) {
			len[i] = std::max(8, (int) (ms[i] * 0.001f * sr * 1.6f));
			line[i].assign(len[i], 0.f);
			idx[i] = 0;
			lp[i] = 0.f;
		}
		const float ams[2] = {5.f, 1.7f};
		for (int i = 0; i < 2; i++) {
			apLen[i] = std::max(4, (int) (ams[i] * 0.001f * sr));
			ap[i].assign(apLen[i], 0.f);
			apIdx[i] = 0;
		}
	}
	void process(float inL, float inR, float& outL, float& outR) {
		// diffuse the mono-ish input
		float x = 0.5f * (inL + inR);
		for (int i = 0; i < 2; i++) {
			float d = ap[i][apIdx[i]];
			float y = -0.6f * x + d;
			ap[i][apIdx[i]] = x + 0.6f * y;
			apIdx[i] = (apIdx[i] + 1) % apLen[i];
			x = y;
		}
		float o[N];
		for (int i = 0; i < N; i++) o[i] = line[i][idx[i]];
		// Householder mix
		float sum = 0.5f * (o[0] + o[1] + o[2] + o[3]);
		for (int i = 0; i < N; i++) {
			float fb = (o[i] - sum) * decay;
			lp[i] += 0.45f * (fb - lp[i]);   // damping
			float in = (i & 1 ? inR : inL) * 0.25f + x * 0.25f;
			line[i][idx[i]] = lp[i] + in;
			idx[i] = (idx[i] + 1) % len[i];
		}
		outL = o[0] + o[2];
		outR = o[1] + o[3];
	}
};

struct Engine {
	Sound sound[NUM_SOUNDS];
	float sampleRate = 48000.f;
	std::mt19937 rng{12345};

	// ---- live input buffer (always listening) ----
	std::vector<float> liveL, liveR, livePeaks;
	int liveHead = 0;           // next frame to write
	int liveFilled = 0;         // frames written so far (up to MAX_FRAMES)
	int liveSeconds = 16;       // how much of the buffer Live Sounds use
	bool liveFrozen = false;
	float recPhase = 0.f, prevInL = 0.f, prevInR = 0.f;

	// ---- recorder ----
	bool recording = false;
	bool recWaiting = false;    // waiting for signal above threshold
	bool recThreshold = false;  // option
	bool recNormalize = true;   // option
	bool recResample = false;   // record the outputs instead of the input
	int recSound = 0;
	int recPos = 0;

	// ---- play state ----
	int selected = 0;           // Sound being played and edited
	int editScene = 0;          // Scene the knobs edit
	bool latch = false;
	bool held = false;          // a SOUND button or GATE is holding the sound on
	float morph = 0.f;          // 0 = A, 1 = B
	float cv[3] = {0, 0, 0};    // X, Y, Z in -1..1
	bool thru = false;
	bool gateResetsScan = true;

	// ---- clock ----
	bool syncOn = true;
	bool syncConnected = false;
	float clockPeriod = 0.5f;   // seconds
	float sinceClock = 0.f;
	bool clockValid = false;

	// ---- grain scheduling ----
	double grainPhase = 0.0;
	double scanPos[NUM_SOUNDS] = {0, 0, 0, 0};   // frames, inside the window
	bool pingFlip = false;

	struct Voice {
		bool active = false;
		int snd = 0;
		bool live = false;
		double pos = 0, inc = 1;
		int age = 0, dur = 1;
		int shape = 0;
		float amp = 1.f, gl = 1.f, gr = 1.f;
		float send = 0.f;
		int toneMode = 0;       // 0 off, 1 LP, 2 HP
		float g = 0, k = 1.4142f, ic1L = 0, ic2L = 0, ic1R = 0, ic2R = 0;
		float env = 0.f;
	};
	Voice voices[NUM_VOICES];
	Reverb reverb;

	// ---- what the display and outputs read ----
	float outL = 0, outR = 0;
	bool grainFired = false;
	float lastEnv = 0.f;        // envelope of the newest grain
	float lastPitch = 0.f;      // semitones of the newest grain
	float lastRand = 0.f;       // random value of the newest grain
	int lastShape = 0;
	float inFollow = 0.f, outFollow = 0.f;
	float shownRate = 0.f, shownSize = 0.5f, shownPitch = 0.f;
	float effective[NUM_P];     // morphed + CV-modulated values (without RAND), for the display
	int activeVoices = 0;

	Engine() {
		liveL.assign(MAX_FRAMES, 0.f);
		liveR.assign(MAX_FRAMES, 0.f);
		livePeaks.assign(NUM_PEAKS, 0.f);
		for (int i = 0; i < NUM_P; i++) effective[i] = P_DEF[i];
		setSampleRate(48000.f);
	}

	void setSampleRate(float sr) {
		sampleRate = sr;
		reverb.init(sr);
	}

	int liveLength() const { return std::min(liveFilled, liveSeconds * BUF_RATE); }

	// length in frames of the audio a Sound plays
	int soundLength(int s) const { return sound[s].live ? liveLength() : sound[s].length; }
	bool soundEmpty(int s) const { return soundLength(s) < 64; }

	float rnd() { return std::uniform_real_distribution<float>(-1.f, 1.f)(rng); }
	float rnd01() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); }

	// morphed value of a parameter plus CV modulation; r = this grain's random value
	float value(int s, int p, float r, bool withRand) const {
		const Scene& A = sound[s].scene[0];
		const Scene& B = sound[s].scene[1];
		float m = morph;
		float v = A.p[p] + (B.p[p] - A.p[p]) * m;
		float range = P_MAX[p] - P_MIN[p];
		for (int src = 0; src < 3; src++) {
			float d = A.mod[src][p] + (B.mod[src][p] - A.mod[src][p]) * m;
			v += d * cv[src] * range;
		}
		if (withRand) {
			float d = A.mod[SRC_RAND][p] + (B.mod[SRC_RAND][p] - A.mod[SRC_RAND][p]) * m;
			v += std::fabs(d) * r * range;
		}
		if (p == P_PITCH) return std::min(1.5f, std::max(-1.5f, v));   // +-36 semitones with modulation
		return std::min(P_MAX[p], std::max(P_MIN[p], v));
	}

	float quantize(int s, float semis) const {
		int mask = sound[s].quantMask;
		if (mask == 0) return semis;
		float best = semis, bestD = 1e9f;
		int base = (int) std::floor(semis);
		for (int n = base - 12; n <= base + 12; n++) {
			int pc = ((n % 12) + 12) % 12;
			if (!(mask & (1 << pc))) continue;
			float d = std::fabs(n - semis);
			if (d < bestD) { bestD = d; best = (float) n; }
		}
		return best;
	}

	// grain length in seconds, before the SIZE-PITCH link
	float grainSeconds(int s, float sizeVal) const {
		if (syncOn && syncConnected && clockValid && sound[s].linkRateSize)
			return std::min(4.f, clockPeriod * CLOCK_STEPS[clockStep(sizeVal)] );
		return sizeSeconds(sizeVal);
	}

	float grainRate(int s, float rateVal, float sizeVal) const {
		if (syncOn && syncConnected && clockValid)
			return CLOCK_STEPS[clockStep(rateVal)] / clockPeriod;
		if (rateVal < RATE_OFF) return 0.f;
		if (sound[s].linkRateSize) return rateMult(rateVal) / grainSeconds(s, sizeVal);
		return rateHz(rateVal);
	}

	// read a frame from a Sound with linear interpolation (position wraps)
	inline void readFrame(const Voice& v, double pos, float& l, float& r) const {
		const float* bl;
		const float* br;
		int len;
		double p = pos;
		if (v.live) {
			bl = liveL.data();
			br = liveR.data();
			len = MAX_FRAMES;
		}
		else {
			bl = sound[v.snd].L.data();
			br = sound[v.snd].R.data();
			len = std::max(1, sound[v.snd].length);
		}
		p = std::fmod(p, (double) len);
		if (p < 0) p += len;
		int i0 = (int) p;
		int i1 = i0 + 1 >= len ? 0 : i0 + 1;
		float t = (float) (p - i0);
		l = bl[i0] + (bl[i1] - bl[i0]) * t;
		r = br[i0] + (br[i1] - br[i0]) * t;
	}

	void spawnGrain() {
		int s = selected;
		if (soundEmpty(s)) return;
		Sound& S = sound[s];
		int slot = -1;
		for (int i = 0; i < NUM_VOICES; i++) if (!voices[i].active) { slot = i; break; }
		if (slot < 0) return;   // all voices busy: skip this grain rather than cut one off (no clicks)
		// one random value per parameter for this grain
		float r[NUM_P];
		for (int p = 0; p < NUM_P; p++) r[p] = rnd();
		float vals[NUM_P];
		for (int p = 0; p < NUM_P; p++) vals[p] = value(s, p, r[p], true);

		int len = soundLength(s);
		double winStart = vals[P_START] * len;
		double winLen = vals[P_WRAP] * len;
		double start = winStart + (winLen > 1 ? std::fmod(scanPos[s], winLen) : 0.0);

		float semis = quantize(s, vals[P_PITCH] * 24.f);
		float ratio = std::pow(2.f, semis / 12.f);
		float secs = grainSeconds(s, vals[P_SIZE]);
		if (S.linkSizePitch) secs /= ratio;          // keep the same slice of audio whatever the pitch
		secs = std::min(8.f, std::max(0.004f, secs));

		// SHAPE and REVERSE don't blend: each grain picks Scene A's or B's by chance
		bool useB = rnd01() < morph;
		const Scene& sc = S.scene[useB ? 1 : 0];
		int shape = (int) std::lround(sc.p[P_SHAPE]);
		float shapeMod = 0.f;
		for (int src = 0; src < 3; src++) shapeMod += (S.scene[0].mod[src][P_SHAPE] * (1 - morph) + S.scene[1].mod[src][P_SHAPE] * morph) * cv[src] * 6.f;
		shapeMod += std::fabs(S.scene[0].mod[SRC_RAND][P_SHAPE] * (1 - morph) + S.scene[1].mod[SRC_RAND][P_SHAPE] * morph) * r[P_SHAPE] * 6.f;
		shape = std::min(NUM_SHAPES - 1, std::max(0, (int) std::lround(shape + shapeMod)));
		bool rev = sc.reverse;
		if (S.pingPong) { pingFlip = !pingFlip; rev = rev != pingFlip; }

		Voice& v = voices[slot];
		v.active = true;
		v.snd = s;
		v.live = S.live;
		v.dur = std::max(16, (int) (secs * sampleRate));
		v.age = 0;
		v.shape = shape;
		double inc = ratio * (double) BUF_RATE / sampleRate;
		double content = inc * v.dur;
		if (S.live) {
			// live window ends at the write head
			int head = liveHead;
			double base = head - len;
			double startAbs = base + start;
			// keep the grain behind the write head
			if (!rev && startAbs + content > head - 1 && !liveFrozen) startAbs = head - 1 - content;
			v.pos = rev ? startAbs + content : startAbs;
		}
		else v.pos = rev ? start + content : start;
		v.inc = rev ? -inc : inc;
		float level = vals[P_LEVEL];
		// when many grains overlap, turn each one down so the sum stays usable
		float overlap = std::max(1.f, shownRate * secs);
		v.amp = level * level * 1.2f / std::sqrt(std::min(overlap, (float) NUM_VOICES));
		// negative RAND depth on LEVEL spreads grains across the stereo field
		float randLevel = S.scene[0].mod[SRC_RAND][P_LEVEL] * (1 - morph) + S.scene[1].mod[SRC_RAND][P_LEVEL] * morph;
		float pan = randLevel < 0 ? -randLevel * rnd() : 0.f;
		v.gl = std::sqrt(0.5f * (1.f - pan)) * 1.41421f;
		v.gr = std::sqrt(0.5f * (1.f + pan)) * 1.41421f;
		v.send = vals[P_BLUR];
		float fc = toneCutoff(vals[P_TONE]);
		v.toneMode = fc <= 0.f ? 0 : (vals[P_TONE] < 0 ? 1 : 2);
		if (v.toneMode) {
			fc = std::min(fc, sampleRate * 0.45f);
			v.g = std::tan(3.14159265f * fc / sampleRate);
			v.k = 1.2f;
			v.ic1L = v.ic2L = v.ic1R = v.ic2R = 0.f;
		}
		grainFired = true;
		lastPitch = semis;
		lastRand = r[P_START];
		lastShape = shape;
	}

	// call on GATE rise / SOUND button press: fire a grain now and restart timing
	void trigger() {
		grainPhase = 0.0;
		if (gateResetsScan && !latch) scanPos[selected] = 0.0;
		spawnGrain();
	}

	void resetScan() { for (int s = 0; s < NUM_SOUNDS; s++) scanPos[s] = 0.0; }

	void clockEdge() {
		if (sinceClock > 0.02f && sinceClock < 8.f) {
			// follow small jitter smoothly, jump straight to a new tempo
			bool jump = !clockValid || std::fabs(sinceClock - clockPeriod) > 0.2f * clockPeriod;
			clockPeriod = jump ? sinceClock : clockPeriod + (sinceClock - clockPeriod) * 0.5f;
			clockValid = true;
		}
		sinceClock = 0.f;
	}

	// ---- recording ----
	void startRecording(int s) {
		if (sound[s].live) { liveFrozen = !liveFrozen; return; }
		recSound = s;
		recPos = 0;
		recording = true;
		recWaiting = recThreshold;
		if (!recWaiting) beginTake();
	}
	void beginTake() {
		Sound& S = sound[recSound];
		S.length = 0;
		std::fill(S.peaks.begin(), S.peaks.end(), 0.f);
		recPos = 0;
		S.name = "recording";
	}
	void stopRecording() {
		if (!recording) return;
		recording = false;
		if (recWaiting) { recWaiting = false; return; }
		Sound& S = sound[recSound];
		if (recNormalize && S.length > 0) {
			float m = 0.f;
			for (int i = 0; i < S.length; i++) m = std::max(m, std::max(std::fabs(S.L[i]), std::fabs(S.R[i])));
			if (m > 1e-4f) {
				float g = 0.9f / m;
				for (int i = 0; i < S.length; i++) { S.L[i] *= g; S.R[i] *= g; }
			}
			S.updatePeaks(0, S.length);
		}
		S.name = "recording";
		S.dirty = true;
		scanPos[recSound] = 0.0;
	}

	// write one 48 kHz frame from the source being recorded
	void recordFrame(float l, float r) {
		// always-listening live buffer
		if (!liveFrozen) {
			liveL[liveHead] = l;
			liveR[liveHead] = r;
			int b = liveHead / PEAK_BLOCK;
			float a = std::max(std::fabs(l), std::fabs(r));
			if (liveHead % PEAK_BLOCK == 0) livePeaks[b] = a;
			else livePeaks[b] = std::max(livePeaks[b], a);
			liveHead = (liveHead + 1) % MAX_FRAMES;
			if (liveFilled < MAX_FRAMES) liveFilled++;
		}
		if (!recording) return;
		if (recWaiting) {
			if (std::max(std::fabs(l), std::fabs(r)) > 0.05f) { recWaiting = false; beginTake(); }
			else return;
		}
		Sound& S = sound[recSound];
		if (recPos >= MAX_FRAMES) { stopRecording(); return; }
		S.L[recPos] = l;
		S.R[recPos] = r;
		int b = recPos / PEAK_BLOCK;
		float a = std::max(std::fabs(l), std::fabs(r));
		if (recPos % PEAK_BLOCK == 0) S.peaks[b] = a;
		else S.peaks[b] = std::max(S.peaks[b], a);
		recPos++;
		S.length = recPos;
	}

	// copy the last `seconds` of the live buffer into a Sound (call with audio paused)
	void grabLive(int s, int seconds) {
		Sound& S = sound[s];
		int n = std::min(liveFilled, seconds * BUF_RATE);
		for (int i = 0; i < n; i++) {
			int src = ((liveHead - n + i) % MAX_FRAMES + MAX_FRAMES) % MAX_FRAMES;
			S.L[i] = liveL[src];
			S.R[i] = liveR[src];
		}
		S.length = n;
		S.live = false;
		S.updatePeaks(0, n);
		S.name = "grabbed from live buffer";
		S.dirty = true;
		scanPos[s] = 0.0;
	}

	void setSound(int s, const std::vector<float>& L, const std::vector<float>& R, const std::string& name) {
		Sound& S = sound[s];
		int n = std::min((int) L.size(), MAX_FRAMES);
		std::copy(L.begin(), L.begin() + n, S.L.begin());
		std::copy(R.begin(), R.begin() + n, S.R.begin());
		S.length = n;
		S.updatePeaks(0, n);
		S.name = name;
		S.dirty = true;
		scanPos[s] = 0.0;
		for (Voice& v : voices) if (v.snd == s && !v.live) v.active = false;
	}

	void clearSound(int s) {
		sound[s].length = 0;
		sound[s].name = "";
		sound[s].dirty = true;
		std::fill(sound[s].peaks.begin(), sound[s].peaks.end(), 0.f);
		for (Voice& v : voices) if (v.snd == s) v.active = false;
	}

	// ---- one sample ----
	void process(float inL, float inR, bool playing) {
		float dt = 1.f / sampleRate;
		sinceClock += dt;
		if (sinceClock > 8.f) clockValid = false;

		int s = selected;
		// values for display and scheduling (no RAND)
		for (int p = 0; p < NUM_P; p++) effective[p] = value(s, p, 0.f, false);
		shownSize = grainSeconds(s, effective[P_SIZE]);
		shownRate = grainRate(s, effective[P_RATE], effective[P_SIZE]);
		shownPitch = quantize(s, effective[P_PITCH] * 24.f);

		// SCAN moves the read point through the window
		int len = soundLength(s);
		double winLen = effective[P_WRAP] * len;
		if (winLen > 1) {
			scanPos[s] += scanSpeed(effective[P_SCAN]) * (double) BUF_RATE / sampleRate;
			scanPos[s] = std::fmod(scanPos[s], winLen);
			if (scanPos[s] < 0) scanPos[s] += winLen;
		}
		else scanPos[s] = 0.0;

		// grain clock
		grainFired = false;
		if (playing && shownRate > 0.f) {
			grainPhase += shownRate * dt;
			if (grainPhase >= 1.0) {
				grainPhase -= std::floor(grainPhase);
				spawnGrain();
			}
		}

		// voices
		float dl = 0, dr = 0, sl = 0, sr = 0;
		int act = 0;
		float newestEnv = 0.f;
		int newestAge = 1 << 30;
		for (Voice& v : voices) {
			if (!v.active) continue;
			act++;
			float x = (float) v.age / v.dur;
			float env = shapeEnv(v.shape, x);
			// tiny fades so square / ramp shapes don't click
			float edge = std::min(1.f, std::min((float) v.age, (float) (v.dur - v.age)) / 48.f);
			env *= edge * v.amp;
			float l, r;
			readFrame(v, v.pos, l, r);
			if (v.toneMode) {
				float a1 = 1.f / (1.f + v.g * (v.g + v.k)), a2 = v.g * a1, a3 = v.g * a2;
				// left
				float v3 = l - v.ic2L, v1 = a1 * v.ic1L + a2 * v3, v2 = v.ic2L + a2 * v.ic1L + a3 * v3;
				v.ic1L = 2 * v1 - v.ic1L; v.ic2L = 2 * v2 - v.ic2L;
				l = v.toneMode == 1 ? v2 : l - v.k * v1 - v2;
				v3 = r - v.ic2R; v1 = a1 * v.ic1R + a2 * v3; v2 = v.ic2R + a2 * v.ic1R + a3 * v3;
				v.ic1R = 2 * v1 - v.ic1R; v.ic2R = 2 * v2 - v.ic2R;
				r = v.toneMode == 1 ? v2 : r - v.k * v1 - v2;
			}
			l *= env * v.gl;
			r *= env * v.gr;
			dl += l; dr += r;
			sl += l * v.send; sr += r * v.send;
			v.env = env;
			if (v.age < newestAge) { newestAge = v.age; newestEnv = v.amp > 0 ? env / v.amp : 0.f; }
			v.pos += v.inc;
			if (++v.age >= v.dur) v.active = false;
		}
		activeVoices = act;
		lastEnv = newestEnv;

		if (thru) {
			dl += inL; dr += inR;
			sl += inL * effective[P_BLUR]; sr += inR * effective[P_BLUR];
		}
		reverb.decay = 0.55f + 0.42f * effective[P_BLUR];
		float wl, wr;
		reverb.process(sl, sr, wl, wr);
		float ol = dl + wl * 0.6f, orr = dr + wr * 0.6f;
		// gentle soft clip so stacks of grains don't explode
		auto soft = [](float x) { return x / (1.f + std::fabs(x) * 0.25f); };
		outL = soft(ol);
		outR = soft(orr);

		inFollow = std::max(inFollow * 0.9995f, std::max(std::fabs(inL), std::fabs(inR)));
		outFollow = std::max(outFollow * 0.9995f, std::max(std::fabs(outL), std::fabs(outR)));

		// record (input, or the outputs when resampling) into the 48 kHz buffers
		float srcL = recResample ? outL : inL, srcR = recResample ? outR : inR;
		recPhase += (float) BUF_RATE / sampleRate;
		while (recPhase >= 1.f) {
			recPhase -= 1.f;
			float t = 1.f - recPhase * sampleRate / BUF_RATE;   // where this 48k frame sits between samples
			t = std::min(1.f, std::max(0.f, t));
			recordFrame(prevInL + (srcL - prevInL) * t, prevInR + (srcR - prevInR) * t);
		}
		prevInL = srcL;
		prevInR = srcR;
	}
};

} // namespace pl
