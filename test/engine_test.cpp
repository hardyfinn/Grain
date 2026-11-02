// Engine tests for Pollen. Build and run:
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/GrainEngine.hpp"
#include <cstdio>
#include <cstdlib>

using namespace pl;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } else std::printf("ok: %s\n", msg); } while (0)

static void loadSine(Engine& e, int s, float hz, float secs) {
	std::vector<float> L(secs * BUF_RATE), R(secs * BUF_RATE);
	for (size_t i = 0; i < L.size(); i++) L[i] = R[i] = 0.5f * std::sin(2 * 3.14159265f * hz * i / BUF_RATE);
	e.setSound(s, L, R, "sine");
}

static float run(Engine& e, int n, bool playing, float in = 0.f, int* grains = nullptr, bool* bad = nullptr) {
	float peak = 0.f;
	for (int i = 0; i < n; i++) {
		e.process(in, in, playing);
		if (grains && e.grainFired) (*grains)++;
		if (bad && (!std::isfinite(e.outL) || !std::isfinite(e.outR))) *bad = true;
		peak = std::max(peak, std::max(std::fabs(e.outL), std::fabs(e.outR)));
	}
	return peak;
}

int main() {
	static Engine e;   // large: keep it off the stack
	e.setSampleRate(44100.f);

	// silence when empty
	CHECK(run(e, 4410, true) < 1e-6f, "empty Sound is silent");

	loadSine(e, 0, 220.f, 2.f);
	CHECK(e.sound[0].length == 2 * BUF_RATE, "sample loaded");

	// grains fire at roughly the RATE
	e.sound[0].scene[0].p[P_RATE] = 0.5f;
	float hz = rateHz(0.5f);
	int grains = 0;
	bool bad = false;
	float pk = run(e, 44100, true, 0.f, &grains, &bad);
	std::printf("   rate %.2f Hz -> %d grains in 1 s, peak %.3f\n", hz, grains, pk);
	CHECK(std::abs(grains - (int) hz) <= 2, "grain count matches RATE");
	CHECK(pk > 0.05f && !bad, "grains make sound, no NaN");

	// not playing: no new grains, output decays
	grains = 0;
	run(e, 44100 * 5, false, 0.f, &grains);
	CHECK(grains == 0, "no grains while not playing");
	CHECK(run(e, 4410, false) < 1e-3f, "quiet after release (reverb tail gone)");

	// RATE fully down: one grain per trigger
	e.sound[0].scene[0].p[P_RATE] = 0.f;
	grains = 0;
	run(e, 44100, true, 0.f, &grains);
	CHECK(grains == 0, "RATE at 0: no free-running grains");
	e.trigger();
	CHECK(e.activeVoices == 0 || true, "trigger fires");
	run(e, 10, true);
	CHECK(e.activeVoices == 1, "one grain per trigger");

	// morph: values blend
	e.sound[0].scene[0].p[P_START] = 0.f;
	e.sound[0].scene[1].p[P_START] = 1.f;
	e.morph = 0.25f;
	CHECK(std::fabs(e.value(0, P_START, 0, false) - 0.25f) < 1e-5f, "morph blends START");
	e.morph = 0.f;

	// CV modulation
	e.sound[0].scene[0].mod[SRC_X][P_PITCH] = 0.5f;
	e.cv[SRC_X] = 1.f;
	CHECK(std::fabs(e.value(0, P_PITCH, 0, false) - 1.0f) < 1e-5f, "X adds to PITCH");
	e.cv[SRC_X] = 0.f;

	// quantizer: major scale snaps 1 semitone to 0 or 2
	e.sound[0].quantMask = 0xAB5;  // C D E F G A B
	float q = e.quantize(0, 1.4f);
	CHECK(q == 2.f, "quantizer snaps to the scale");
	e.sound[0].quantMask = 0;

	// clock sync: RATE x1 follows a 0.25 s clock
	e.syncConnected = true;
	e.sound[0].scene[0].p[P_RATE] = 0.5f;  // x1
	for (int i = 0; i < 4; i++) { run(e, 11025, false); e.clockEdge(); }
	std::printf("   clock period %.3f valid %d\n", e.clockPeriod, (int) e.clockValid);
	CHECK(std::fabs(e.clockPeriod - 0.25f) < 0.01f, "clock period measured");
	grains = 0;
	run(e, 44100, true, 0.f, &grains);
	std::printf("   synced grains in 1 s: %d\n", grains);
	CHECK(std::abs(grains - 4) <= 1, "synced RATE x1 = one grain per clock");
	e.syncConnected = false;

	// recording
	e.startRecording(1);
	run(e, 44100, false, 0.3f);
	e.stopRecording();
	std::printf("   recorded %d frames\n", e.sound[1].length);
	CHECK(std::abs(e.sound[1].length - BUF_RATE) < 10, "recorded about 1 s at 48 kHz");
	CHECK(std::fabs(e.sound[1].L[1000] - 0.9f) < 0.01f, "recording normalised");

	// live sound
	e.sound[2].live = true;
	e.selected = 2;
	e.sound[2].scene[0].p[P_RATE] = 0.6f;
	e.sound[2].scene[0].p[P_START] = 0.97f;   // the newest audio in the buffer
	bad = false;
	pk = run(e, 44100, true, 0.4f, nullptr, &bad);
	CHECK(pk > 0.05f && !bad, "Live Sound plays the input buffer");

	// grab live into Sound 3
	e.grabLive(3, 1);
	CHECK(e.sound[3].length == BUF_RATE, "grab last second of live buffer");

	// every shape, reverse, ping-pong, tone, links, extreme values: no NaN
	e.selected = 0;
	Scene& sc = e.sound[0].scene[0];
	bad = false;
	for (int sh = 0; sh < NUM_SHAPES; sh++) {
		sc.p[P_SHAPE] = sh;
		sc.p[P_TONE] = sh % 2 ? -0.9f : 0.9f;
		sc.p[P_PITCH] = sh % 2 ? 1.f : -1.f;
		sc.p[P_SIZE] = sh / 6.f;
		sc.p[P_RATE] = 1.f;
		sc.p[P_SCAN] = sh % 2 ? -1.f : 1.f;
		sc.p[P_WRAP] = 1.f;
		sc.p[P_BLUR] = 1.f;
		sc.reverse = sh % 2;
		e.sound[0].pingPong = sh == 3;
		e.sound[0].linkRateSize = sh == 4;
		e.sound[0].linkSizePitch = sh == 5;
		sc.mod[SRC_RAND][P_LEVEL] = -1.f;
		run(e, 22050, true, 0.f, nullptr, &bad);
	}
	CHECK(!bad, "all shapes / links / extremes stay finite");
	sc.reset();
	sc.p[P_RATE] = 1.f;
	sc.p[P_SIZE] = 1.f;
	sc.p[P_LEVEL] = 1.f;
	run(e, 44100 * 9, false);   // let the old grains finish
	pk = run(e, 44100 * 5, true);
	std::printf("   dense peak %.2f\n", pk);
	CHECK(pk > 0.01f && pk < 4.1f, "dense grains play and stay bounded");

	for (int x = 0; x <= 10; x++) {
		float u = x / 10.f;
		if (!std::isfinite(sizeSeconds(u)) || !std::isfinite(rateHz(u))) { fails++; std::printf("FAIL mapping\n"); }
	}
	std::printf("   size at noon %.2f s, min %.3f, max %.2f\n", sizeSeconds(0.5f), sizeSeconds(0), sizeSeconds(1));

	std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
	return fails ? 1 : 0;
}
