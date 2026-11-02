// Pollen.cpp
// A clear-interface stereo granular sampler, made for learning a hardware
// granular module away from the case. 4 Sounds, 2 Scenes each, a MORPH fader
// between the Scenes, X / Y / Z / RAND modulation, recording and a live
// input buffer. The display shows the audio, the grains as they fire, the
// grain envelope, the morph between Scenes, and a plain-words readout.
// The sound engine is in GrainEngine.hpp; this file connects it to the panel.
#include "plugin.hpp"
#include "GrainEngine.hpp"
#include "Wav.hpp"
#include <osdialog.h>
#include <atomic>
#include <thread>
#include <chrono>

using namespace pl;

struct Pollen;

// Quantizer scales (bit 0 = the sample's own pitch)
static const int QUANT_MASKS[8] = {0, 0xFFF, 0xAB5, 0x5AD, 0x295, 0x4A9, 0x081, 0x001};
static const char* QUANT_NAMES[8] = {"Off (free pitch)", "Semitones", "Major", "Minor", "Major pentatonic", "Minor pentatonic", "Octaves and fifths", "Octaves only"};

enum OutSource { OS_MORPH, OS_TRIGGER, OS_ENV, OS_PITCH, OS_IN_FOLLOW, OS_OUT_FOLLOW, OS_RANDOM, NUM_OS };
static const char* OUT_SOURCE_NAMES[NUM_OS] = {
	"Morph position (0-5 V)", "Grain trigger (5 V pulse per grain)", "Grain envelope (0-5 V)",
	"Grain pitch (1 V/oct, 0 V = original)", "Input envelope follower (0-5 V)",
	"Output envelope follower (0-5 V)", "Random per grain (+-5 V)"};

struct Pollen : Module {
	enum ParamId {
		START_PARAM, WRAP_PARAM, SCAN_PARAM, RATE_PARAM, SIZE_PARAM, PITCH_PARAM,
		SHAPE_PARAM, LEVEL_PARAM, TONE_PARAM, BLUR_PARAM,
		MORPH_PARAM, MORPH_ATT_PARAM,
		ENUMS(SOUND_BUTTON, 4),
		SCENE_A_BUTTON, SCENE_B_BUTTON, LATCH_BUTTON, REVERSE_BUTTON,
		LINK_RS_BUTTON, LINK_SP_BUTTON,
		ENUMS(MOD_BUTTON, 4),
		SAMPLE_BUTTON, THRU_BUTTON,
		PARAMS_LEN
	};
	enum InputId {
		IN_L_INPUT, IN_R_INPUT, GATE_INPUT, NEXT_INPUT, SELECT_INPUT, SYNC_INPUT,
		MORPH_INPUT, X_INPUT, Y_INPUT, Z_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, A_OUTPUT, B_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(SOUND_LIGHT, 4),
		SCENE_A_LIGHT, SCENE_B_LIGHT, LATCH_LIGHT, REVERSE_LIGHT,
		LINK_RS_LIGHT, LINK_SP_LIGHT,
		ENUMS(MOD_LIGHT, 4),
		SAMPLE_LIGHT, THRU_LIGHT,
		GRAIN_LIGHT,
		LIGHTS_LEN
	};

	Engine eng;
	int modEdit = -1;                 // -1 = knobs set values, 0..3 = knobs set X/Y/Z/RAND depth
	float lastKnob[NUM_P];
	bool knobsDirty = true;
	int outSource[2] = {OS_ENV, OS_TRIGGER};
	bool nextResetsScan = false;
	bool keepWithPatch = true;
	float inputGain = 1.f;

	dsp::SchmittTrigger gateTrig, nextTrig, syncTrig;
	dsp::BooleanTrigger btn[PARAMS_LEN];
	dsp::PulseGenerator trigPulse, grainLightPulse;
	dsp::ClockDivider ctrlDiv, lightDiv;
	int lastSelectIdx = -1;
	float randHold = 0.f;
	double blink = 0.0;

	std::atomic<bool> pauseReq{false}, paused{false};

	std::string message;
	float messageTime = 0.f;
	void say(const std::string& m) { message = m; messageTime = 2.5f; }

	Pollen();

	Scene& editedScene() { return eng.sound[eng.selected].scene[eng.editScene]; }

	// Knobs show either the edited Scene's values or, in a MOD edit mode,
	// that source's depths (-100%..+100%, noon = none).
	float knobFromState(int p) {
		Scene& sc = editedScene();
		if (modEdit < 0) return sc.p[p];
		return P_MIN[p] + (sc.mod[modEdit][p] + 1.f) * 0.5f * (P_MAX[p] - P_MIN[p]);
	}
	void loadKnobs() {
		paramQuantities[SHAPE_PARAM]->snapEnabled = modEdit < 0;
		for (int p = 0; p < NUM_P; p++) {
			float v = knobFromState(p);
			params[START_PARAM + p].setValue(v);
			lastKnob[p] = v;
		}
		knobsDirty = false;
	}
	void readKnobs() {
		if (knobsDirty) loadKnobs();
		Scene& sc = editedScene();
		for (int p = 0; p < NUM_P; p++) {
			float v = params[START_PARAM + p].getValue();
			if (v == lastKnob[p]) continue;
			lastKnob[p] = v;
			if (modEdit < 0) sc.p[p] = v;
			else {
				float d = 2.f * (v - P_MIN[p]) / (P_MAX[p] - P_MIN[p]) - 1.f;
				if (std::fabs(d) < 0.02f) d = 0.f;   // easy to find "none" at noon
				sc.mod[modEdit][p] = d;
			}
		}
	}

	void select(int s) {
		s = clamp(s, 0, NUM_SOUNDS - 1);
		if (s != eng.selected) {
			eng.selected = s;
			knobsDirty = true;
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override { eng.setSampleRate(e.sampleRate); }

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		pauseAudio();
		for (int s = 0; s < NUM_SOUNDS; s++) {
			eng.sound[s].resetSettings();
			eng.clearSound(s);
		}
		resumeAudio();
		eng.selected = 0;
		eng.editScene = 0;
		eng.latch = false;
		eng.thru = false;
		modEdit = -1;
		knobsDirty = true;
	}

	void pauseAudio() {
		pauseReq = true;
		for (int i = 0; i < 300 && !paused; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	void resumeAudio() { pauseReq = false; }

	void process(const ProcessArgs& args) override {
		if (pauseReq) {
			paused = true;
			for (int o = 0; o < OUTPUTS_LEN; o++) outputs[o].setVoltage(0.f);
			return;
		}
		paused = false;

		// ---- buttons ----
		auto pressed = [&](int id) { return btn[id].process(params[id].getValue() > 0.f); };
		bool soundHeld = false;
		for (int i = 0; i < 4; i++) {
			bool down = params[SOUND_BUTTON + i].getValue() > 0.f;
			if (pressed(SOUND_BUTTON + i)) {
				select(i);
				if (eng.soundEmpty(i)) say(string::f("Sound %d is empty: right-click > Load WAV, or press SAMPLE", i + 1));
				eng.trigger();
			}
			if (down && eng.selected == i) soundHeld = true;
		}
		if (pressed(SCENE_A_BUTTON)) { eng.editScene = 0; knobsDirty = true; }
		if (pressed(SCENE_B_BUTTON)) { eng.editScene = 1; knobsDirty = true; }
		if (pressed(LATCH_BUTTON)) {
			eng.latch = !eng.latch;
			if (eng.latch) eng.trigger();
		}
		if (pressed(REVERSE_BUTTON)) editedScene().reverse = !editedScene().reverse;
		if (pressed(LINK_RS_BUTTON)) {
			Sound& S = eng.sound[eng.selected];
			S.linkRateSize = !S.linkRateSize;
			say(S.linkRateSize ? "RATE-SIZE linked: RATE now sets how many grains overlap (1x to 8x)" : "RATE-SIZE unlinked: RATE sets grains per second");
		}
		if (pressed(LINK_SP_BUTTON)) {
			Sound& S = eng.sound[eng.selected];
			S.linkSizePitch = !S.linkSizePitch;
			say(S.linkSizePitch ? "SIZE-PITCH linked: pitching up shortens grains, same slice of audio" : "SIZE-PITCH unlinked: grain length stays put when you change pitch");
		}
		for (int i = 0; i < 4; i++) {
			if (pressed(MOD_BUTTON + i)) {
				modEdit = modEdit == i ? -1 : i;
				knobsDirty = true;
			}
		}
		if (pressed(SAMPLE_BUTTON)) {
			if (eng.recording) { eng.stopRecording(); say(string::f("Recorded %.1f s into Sound %d", eng.sound[eng.recSound].length / (float) BUF_RATE, eng.recSound + 1)); }
			else {
				bool wasLive = eng.sound[eng.selected].live;
				eng.startRecording(eng.selected);
				if (wasLive) say(eng.liveFrozen ? "Live buffer frozen" : "Live buffer running");
				else say(string::f("Recording into Sound %d: press SAMPLE again to stop", eng.selected + 1));
			}
		}
		if (pressed(THRU_BUTTON)) eng.thru = !eng.thru;

		// ---- gates ----
		bool gateRise = gateTrig.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 2.f);
		bool gateHigh = inputs[GATE_INPUT].isConnected() && gateTrig.isHigh();
		if (nextTrig.process(inputs[NEXT_INPUT].getVoltage(), 0.1f, 2.f)) {
			if (nextResetsScan) eng.resetScan();
			else {
				for (int k = 1; k <= NUM_SOUNDS; k++) {
					int s = (eng.selected + k) % NUM_SOUNDS;
					if (!eng.soundEmpty(s)) { select(s); break; }
				}
				if (eng.latch || gateHigh) eng.trigger();
			}
		}
		if (inputs[SELECT_INPUT].isConnected()) {
			float v = std::fabs(inputs[SELECT_INPUT].getVoltage());
			int idx = clamp((int) (v / 5.f * NUM_SOUNDS), 0, NUM_SOUNDS - 1);
			if (idx != lastSelectIdx) { lastSelectIdx = idx; select(idx); }
		}
		else lastSelectIdx = -1;
		if (gateRise) eng.trigger();
		eng.syncConnected = inputs[SYNC_INPUT].isConnected();
		if (syncTrig.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 2.f)) eng.clockEdge();

		// ---- knobs and CV (at a lower rate) ----
		if (ctrlDiv.process()) {
			readKnobs();
			float m = params[MORPH_PARAM].getValue() + params[MORPH_ATT_PARAM].getValue() * inputs[MORPH_INPUT].getVoltage() / 5.f;
			eng.morph = clamp(m, 0.f, 1.f);
			for (int i = 0; i < 3; i++) eng.cv[i] = clamp(inputs[X_INPUT + i].getVoltage() / 5.f, -2.f, 2.f);
		}

		// ---- audio ----
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f * inputGain;
		float inR = (inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inputs[IN_L_INPUT].getVoltage()) / 5.f * inputGain;
		eng.held = soundHeld || gateHigh;
		eng.process(inL, inR, eng.latch || eng.held);
		outputs[OUT_L_OUTPUT].setVoltage(eng.outL * 5.f);
		outputs[OUT_R_OUTPUT].setVoltage(eng.outR * 5.f);

		if (eng.grainFired) {
			trigPulse.trigger(1e-3f);
			grainLightPulse.trigger(0.03f);
			randHold = eng.lastRand;
		}
		bool trig = trigPulse.process(args.sampleTime);
		for (int o = 0; o < 2; o++) {
			float v = 0.f;
			switch (outSource[o]) {
				case OS_MORPH: v = eng.morph * 5.f; break;
				case OS_TRIGGER: v = trig ? 5.f : 0.f; break;
				case OS_ENV: v = eng.lastEnv * 5.f; break;
				case OS_PITCH: v = eng.lastPitch / 12.f; break;
				case OS_IN_FOLLOW: v = std::min(5.f, eng.inFollow * 5.f); break;
				case OS_OUT_FOLLOW: v = std::min(5.f, eng.outFollow * 5.f); break;
				case OS_RANDOM: v = randHold * 5.f; break;
			}
			outputs[A_OUTPUT + o].setVoltage(v);
		}
		if (messageTime > 0.f) messageTime -= args.sampleTime;

		// ---- lights ----
		if (lightDiv.process()) {
			float dt = args.sampleTime * lightDiv.getDivision();
			blink += dt * 3.0;
			bool on = std::fmod(blink, 1.0) < 0.5;
			for (int i = 0; i < 4; i++)
				lights[SOUND_LIGHT + i].setBrightness(eng.selected == i ? 1.f : eng.soundEmpty(i) ? 0.f : 0.18f);
			lights[SCENE_A_LIGHT].setBrightness(eng.editScene == 0 ? 1.f : 0.f);
			lights[SCENE_B_LIGHT].setBrightness(eng.editScene == 1 ? 1.f : 0.f);
			lights[LATCH_LIGHT].setBrightness(eng.latch ? 1.f : 0.f);
			lights[REVERSE_LIGHT].setBrightness(editedScene().reverse ? 1.f : 0.f);
			lights[LINK_RS_LIGHT].setBrightness(eng.sound[eng.selected].linkRateSize ? 1.f : 0.f);
			lights[LINK_SP_LIGHT].setBrightness(eng.sound[eng.selected].linkSizePitch ? 1.f : 0.f);
			Scene& sc = editedScene();
			for (int i = 0; i < 4; i++) {
				bool any = false;
				for (int p = 0; p < NUM_P; p++) if (sc.mod[i][p] != 0.f) any = true;
				lights[MOD_LIGHT + i].setBrightness(modEdit == i ? (on ? 1.f : 0.5f) : any ? 0.2f : 0.f);
			}
			float sl = 0.f;
			if (eng.recording) sl = eng.recWaiting ? (on ? 1.f : 0.f) : 1.f;
			else if (eng.sound[eng.selected].live && eng.liveFrozen) sl = 0.35f;
			lights[SAMPLE_LIGHT].setBrightness(sl);
			lights[THRU_LIGHT].setBrightness(eng.thru ? 1.f : 0.f);
			lights[GRAIN_LIGHT].setBrightness(grainLightPulse.process(dt) ? 1.f : 0.f);
		}
	}

	// ---- files ----
	std::string loadSound(int s, const std::string& path) {
		WavData wd;
		std::string err = readWav(path, wd, MAX_FRAMES);
		if (!err.empty()) return err;
		pauseAudio();
		eng.setSound(s, wd.L, wd.R, system::getStem(path));
		eng.sound[s].live = false;
		resumeAudio();
		return "";
	}
	std::string saveSound(int s, const std::string& path) {
		Sound& S = eng.sound[s];
		return writeWav(path, S.L.data(), S.R.data(), S.length, nullptr, 0);
	}
	std::string soundFile(const std::string& dir, int s) { return system::join(dir, string::f("sound%d.wav", s + 1)); }

	void onAdd(const AddEvent& e) override {
		std::string dir = getPatchStorageDirectory();
		for (int s = 0; s < NUM_SOUNDS; s++) {
			std::string p = soundFile(dir, s);
			if (!system::isFile(p)) continue;
			WavData wd;
			if (readWav(p, wd, MAX_FRAMES).empty()) {
				std::string name = eng.sound[s].name;
				eng.setSound(s, wd.L, wd.R, name.empty() ? "saved sound" : name);
				eng.sound[s].dirty = false;
			}
		}
		knobsDirty = true;
	}

	void onSave(const SaveEvent& e) override {
		std::string dir = createPatchStorageDirectory();
		for (int s = 0; s < NUM_SOUNDS; s++) {
			Sound& S = eng.sound[s];
			std::string p = soundFile(dir, s);
			if (!keepWithPatch || S.length == 0) {
				if (system::isFile(p)) system::remove(p);
				continue;
			}
			if (S.dirty || !system::isFile(p)) {
				saveSound(s, p);
				S.dirty = false;
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "selected", json_integer(eng.selected));
		json_object_set_new(root, "editScene", json_integer(eng.editScene));
		json_object_set_new(root, "latch", json_boolean(eng.latch));
		json_object_set_new(root, "thru", json_boolean(eng.thru));
		json_object_set_new(root, "syncOn", json_boolean(eng.syncOn));
		json_object_set_new(root, "gateResetsScan", json_boolean(eng.gateResetsScan));
		json_object_set_new(root, "nextResetsScan", json_boolean(nextResetsScan));
		json_object_set_new(root, "recThreshold", json_boolean(eng.recThreshold));
		json_object_set_new(root, "recNormalize", json_boolean(eng.recNormalize));
		json_object_set_new(root, "recResample", json_boolean(eng.recResample));
		json_object_set_new(root, "liveSeconds", json_integer(eng.liveSeconds));
		json_object_set_new(root, "outA", json_integer(outSource[0]));
		json_object_set_new(root, "outB", json_integer(outSource[1]));
		json_object_set_new(root, "keepWithPatch", json_boolean(keepWithPatch));
		json_object_set_new(root, "inputGain", json_real(inputGain));
		json_t* sounds = json_array();
		for (int s = 0; s < NUM_SOUNDS; s++) {
			Sound& S = eng.sound[s];
			json_t* js = json_object();
			json_object_set_new(js, "name", json_string(S.name.c_str()));
			json_object_set_new(js, "live", json_boolean(S.live));
			json_object_set_new(js, "pingPong", json_boolean(S.pingPong));
			json_object_set_new(js, "linkRateSize", json_boolean(S.linkRateSize));
			json_object_set_new(js, "linkSizePitch", json_boolean(S.linkSizePitch));
			json_object_set_new(js, "quantMask", json_integer(S.quantMask));
			json_t* scenes = json_array();
			for (int c = 0; c < 2; c++) {
				Scene& sc = S.scene[c];
				json_t* jc = json_object();
				json_t* jp = json_array();
				for (int p = 0; p < NUM_P; p++) json_array_append_new(jp, json_real(sc.p[p]));
				json_object_set_new(jc, "values", jp);
				json_t* jm = json_array();
				for (int src = 0; src < NUM_SRC; src++)
					for (int p = 0; p < NUM_P; p++) json_array_append_new(jm, json_real(sc.mod[src][p]));
				json_object_set_new(jc, "mod", jm);
				json_object_set_new(jc, "reverse", json_boolean(sc.reverse));
				json_array_append_new(scenes, jc);
			}
			json_object_set_new(js, "scenes", scenes);
			json_array_append_new(sounds, js);
		}
		json_object_set_new(root, "sounds", sounds);
		return root;
	}

	void dataFromJson(json_t* root) override {
		auto geti = [&](json_t* o, const char* k, int def) { json_t* j = json_object_get(o, k); return j ? (int) json_integer_value(j) : def; };
		auto getb = [&](json_t* o, const char* k, bool def) { json_t* j = json_object_get(o, k); return j ? json_boolean_value(j) : def; };
		eng.selected = clamp(geti(root, "selected", 0), 0, NUM_SOUNDS - 1);
		eng.editScene = clamp(geti(root, "editScene", 0), 0, 1);
		eng.latch = getb(root, "latch", false);
		eng.thru = getb(root, "thru", false);
		eng.syncOn = getb(root, "syncOn", true);
		eng.gateResetsScan = getb(root, "gateResetsScan", true);
		nextResetsScan = getb(root, "nextResetsScan", false);
		eng.recThreshold = getb(root, "recThreshold", false);
		eng.recNormalize = getb(root, "recNormalize", true);
		eng.recResample = getb(root, "recResample", false);
		eng.liveSeconds = clamp(geti(root, "liveSeconds", 16), 8, 32);
		outSource[0] = clamp(geti(root, "outA", OS_ENV), 0, NUM_OS - 1);
		outSource[1] = clamp(geti(root, "outB", OS_TRIGGER), 0, NUM_OS - 1);
		keepWithPatch = getb(root, "keepWithPatch", true);
		if (json_t* g = json_object_get(root, "inputGain")) inputGain = clamp((float) json_number_value(g), 0.5f, 2.5f);
		json_t* sounds = json_object_get(root, "sounds");
		for (int s = 0; s < NUM_SOUNDS && sounds && s < (int) json_array_size(sounds); s++) {
			json_t* js = json_array_get(sounds, s);
			Sound& S = eng.sound[s];
			if (json_t* n = json_object_get(js, "name")) S.name = json_string_value(n);
			S.live = getb(js, "live", false);
			S.pingPong = getb(js, "pingPong", false);
			S.linkRateSize = getb(js, "linkRateSize", false);
			S.linkSizePitch = getb(js, "linkSizePitch", false);
			S.quantMask = geti(js, "quantMask", 0) & 0xFFF;
			json_t* scenes = json_object_get(js, "scenes");
			for (int c = 0; c < 2 && scenes && c < (int) json_array_size(scenes); c++) {
				json_t* jc = json_array_get(scenes, c);
				Scene& sc = S.scene[c];
				json_t* jp = json_object_get(jc, "values");
				for (int p = 0; p < NUM_P && jp && p < (int) json_array_size(jp); p++)
					sc.p[p] = clamp((float) json_number_value(json_array_get(jp, p)), P_MIN[p], P_MAX[p]);
				json_t* jm = json_object_get(jc, "mod");
				for (int i = 0; jm && i < NUM_SRC * NUM_P && i < (int) json_array_size(jm); i++)
					sc.mod[i / NUM_P][i % NUM_P] = clamp((float) json_number_value(json_array_get(jm, i)), -1.f, 1.f);
				sc.reverse = getb(jc, "reverse", false);
			}
		}
		modEdit = -1;
		knobsDirty = true;
	}
};

// ---- knob read-outs in real units ------------------------------------------
struct PollenQuantity : ParamQuantity {
	int p = 0;
	std::string getDisplayValueString() override {
		Pollen* m = dynamic_cast<Pollen*>(module);
		float v = getValue();
		if (m && m->modEdit >= 0) {
			static const char* SRC[4] = {"X", "Y", "Z", "RAND"};
			float d = 2.f * (v - P_MIN[p]) / (P_MAX[p] - P_MIN[p]) - 1.f;
			if (std::fabs(d) < 0.02f) return string::f("%s depth: none", SRC[m->modEdit]);
			return string::f("%s depth: %+.0f%%", SRC[m->modEdit], d * 100.f);
		}
		Engine* e = m ? &m->eng : nullptr;
		int s = e ? e->selected : 0;
		bool synced = e && e->syncOn && e->syncConnected && e->clockValid;
		switch (p) {
			case P_START: {
				if (e && e->soundLength(s) > 0) return string::f("%.0f%% (%.2f s into the audio)", v * 100.f, v * e->soundLength(s) / BUF_RATE);
				return string::f("%.0f%%", v * 100.f);
			}
			case P_WRAP: {
				if (v < 0.005f) return "0% (grains all start at START)";
				if (e && e->soundLength(s) > 0) return string::f("%.0f%% (window %.2f s)", v * 100.f, v * e->soundLength(s) / BUF_RATE);
				return string::f("%.0f%%", v * 100.f);
			}
			case P_SCAN: {
				float sp = scanSpeed(v);
				if (sp == 0.f) return "still (noon)";
				return string::f("%s x%.2f real time", sp < 0 ? "backward" : "forward", std::fabs(sp));
			}
			case P_RATE: {
				if (synced) return string::f("clock %s", CLOCK_STEP_NAMES[clockStep(v)]);
				if (v < RATE_OFF) return "0: one grain per trigger";
				if (e && e->sound[s].linkRateSize) return string::f("%.1fx overlap (linked to SIZE)", rateMult(v));
				return string::f("%.2f grains per second", rateHz(v));
			}
			case P_SIZE: {
				float sec = e ? e->grainSeconds(s, v) : sizeSeconds(v);
				return sec < 1.f ? string::f("%.0f ms", sec * 1000.f) : string::f("%.2f s", sec);
			}
			case P_PITCH: return string::f("%+.1f semitones", v * 24.f);
			case P_SHAPE: return SHAPE_NAMES[clamp((int) std::lround(v), 0, NUM_SHAPES - 1)];
			case P_LEVEL: return string::f("%.0f%%", v * 100.f);
			case P_TONE: {
				float fc = toneCutoff(v);
				if (fc <= 0.f) return "flat (noon)";
				return string::f("%s %.0f Hz", v < 0 ? "low-pass" : "high-pass", fc);
			}
			case P_BLUR: return string::f("%.0f%% to the reverb", v * 100.f);
		}
		return ParamQuantity::getDisplayValueString();
	}
	void setDisplayValueString(std::string s) override {
		Pollen* m = dynamic_cast<Pollen*>(module);
		if (m && m->modEdit >= 0) return;
		ParamQuantity::setDisplayValueString(s);
	}
};

static const char* KNOB_HELP[NUM_P] = {
	"START: where the grain window begins in the audio",
	"WRAP: how wide the window is that grains pick from",
	"SCAN: moves the start point through the window (noon = still)",
	"RATE: how often grains fire (fully left = one per trigger)",
	"SIZE: how long each grain lasts",
	"PITCH: playback speed of each grain (+-2 octaves)",
	"SHAPE: the grain envelope",
	"LEVEL: grain volume",
	"TONE: per-grain filter (left = low-pass, right = high-pass)",
	"BLUR: reverb send and decay",
};

Pollen::Pollen() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
	for (int p = 0; p < NUM_P; p++) {
		PollenQuantity* q = configParam<PollenQuantity>(START_PARAM + p, P_MIN[p], P_MAX[p], P_DEF[p], KNOB_HELP[p]);
		q->p = p;
		lastKnob[p] = P_DEF[p];
	}
	paramQuantities[SHAPE_PARAM]->snapEnabled = true;
	configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "MORPH: Scene A <-> Scene B", "% B", 0.f, 100.f);
	configParam(MORPH_ATT_PARAM, -1.f, 1.f, 1.f, "MORPH CV amount", "%", 0.f, 100.f);
	for (int i = 0; i < 4; i++)
		configButton(SOUND_BUTTON + i, string::f("SOUND %d: select and play (hold to play when LATCH is off)", i + 1));
	configButton(SCENE_A_BUTTON, "SCENE A: knobs edit Scene A");
	configButton(SCENE_B_BUTTON, "SCENE B: knobs edit Scene B");
	configButton(LATCH_BUTTON, "LATCH: keep playing without holding");
	configButton(REVERSE_BUTTON, "REVERSE: grains play backwards (this Scene)");
	configButton(LINK_RS_BUTTON, "LINK RATE-SIZE: RATE becomes overlap amount");
	configButton(LINK_SP_BUTTON, "LINK SIZE-PITCH: grains keep the same slice when pitched");
	const char* src[4] = {"X", "Y", "Z", "RAND"};
	for (int i = 0; i < 4; i++)
		configButton(MOD_BUTTON + i, string::f("%s DEPTH: press, then turn knobs to set how much %s moves them (hardware: hold %s + turn)", src[i], src[i], src[i]));
	configButton(SAMPLE_BUTTON, "SAMPLE: record into this Sound (Live Sound: freeze / unfreeze)");
	configButton(THRU_BUTTON, "THRU: hear the input (through BLUR)");
	configInput(IN_L_INPUT, "Audio left (mono)");
	configInput(IN_R_INPUT, "Audio right");
	configInput(GATE_INPUT, "Gate: plays the Sound while high, fires a grain on each rise");
	configInput(NEXT_INPUT, "Next: go to the next Sound (or reset SCAN, see menu)");
	configInput(SELECT_INPUT, "Select: 0-5 V picks Sound 1-4");
	configInput(SYNC_INPUT, "Sync clock: RATE becomes a clock divider / multiplier");
	configInput(MORPH_INPUT, "Morph CV (+-5 V)");
	configInput(X_INPUT, "X modulation (+-5 V)");
	configInput(Y_INPUT, "Y modulation (+-5 V)");
	configInput(Z_INPUT, "Z modulation (+-5 V)");
	configOutput(OUT_L_OUTPUT, "Audio left");
	configOutput(OUT_R_OUTPUT, "Audio right");
	configOutput(A_OUTPUT, "CV out A (choose the source in the menu)");
	configOutput(B_OUTPUT, "CV out B (choose the source in the menu)");
	configBypass(IN_L_INPUT, OUT_L_OUTPUT);
	configBypass(IN_R_INPUT, OUT_R_OUTPUT);
	ctrlDiv.setDivision(32);
	lightDiv.setDivision(128);
	eng.rng.seed(random::u32());
}

// ---- display ---------------------------------------------------------------
static const NVGcolor C_BG = nvgRGB(0x12, 0x14, 0x10);
static const NVGcolor C_BOX = nvgRGB(0x1C, 0x1F, 0x19);
static const NVGcolor C_INK = nvgRGB(0xF2, 0xEC, 0xD8);
static const NVGcolor C_SOFT = nvgRGB(0x8E, 0x8C, 0x7C);
static const NVGcolor C_POLLEN = nvgRGB(0xF5, 0xC5, 0x2A);
static const NVGcolor C_A = nvgRGB(0x7F, 0xC8, 0xF0);
static const NVGcolor C_B = nvgRGB(0xF0, 0x8E, 0x6A);
static const NVGcolor C_REC = nvgRGB(0xF0, 0x40, 0x40);
static const NVGcolor C_WAVE = nvgRGB(0x9C, 0xA4, 0x8C);
static const NVGcolor C_GREEN = nvgRGB(0x60, 0xD8, 0x70);
static const NVGcolor C_PURPLE = nvgRGB(0xB8, 0x7C, 0xF0);
static const NVGcolor C_MOD[4] = {nvgRGB(0x6C, 0xE0, 0xD0), nvgRGB(0xE8, 0x7C, 0xD8), nvgRGB(0xA0, 0xE0, 0x60), nvgRGB(0xF0, 0xF0, 0xF0)};

struct PollenDisplay : Widget {
	Pollen* module = nullptr;

	void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(vg);
		nvgRect(vg, x, y, w, h);
		nvgFillColor(vg, c);
		nvgFill(vg);
	}
	void box(NVGcontext* vg, float x, float y, float w, float h) {
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x, y, w, h, 2.f);
		nvgFillColor(vg, C_BOX);
		nvgFill(vg);
	}
	void line(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor c, float w) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, x0, y0);
		nvgLineTo(vg, x1, y1);
		nvgStrokeColor(vg, c);
		nvgStrokeWidth(vg, w);
		nvgStroke(vg);
	}
	void text(NVGcontext* vg, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(vg, size);
		nvgTextAlign(vg, align);
		nvgFillColor(vg, c);
		nvgText(vg, x, y, s.c_str(), NULL);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(vg, C_BG);
		nvgFill(vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) nvgFontFaceId(vg, font->handle);
		float pad = mm2px(1.5f);
		float fs = mm2px(2.4f), fsS = mm2px(1.95f), fsT = mm2px(1.7f);
		if (!module) {
			text(vg, W / 2, H / 2, "POLLEN  granular sampler", C_POLLEN, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, fs * 1.4f);
			return;
		}
		Pollen* m = module;
		Engine& e = m->eng;
		int s = e.selected;
		Sound& S = e.sound[s];
		int len = e.soundLength(s);

		// ---------- header ----------
		float hy = pad + fs * 0.5f;
		text(vg, pad, hy, string::f("SOUND %d", s + 1), C_POLLEN, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fs);
		std::string info;
		if (S.live) info = string::f("LIVE input, last %d s%s", e.liveSeconds, e.liveFrozen ? "  (FROZEN)" : "");
		else if (len > 0) info = string::f("%s  %.2f s", S.name.empty() ? "untitled" : S.name.c_str(), len / (float) BUF_RATE);
		else info = "empty";
		text(vg, pad + mm2px(17.f), hy, info, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsS);
		std::string right = string::f("editing SCENE %c", e.editScene ? 'B' : 'A');
		if (e.latch) right += "  LATCH";
		if (e.thru) right += "  THRU";
		if (e.syncConnected) right += e.syncOn ? (e.clockValid ? string::f("  SYNC %.0f bpm", 60.f / e.clockPeriod) : "  SYNC waiting") : "  SYNC off";
		text(vg, W - pad, hy, right, e.editScene ? C_B : C_A, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, fsS);

		// ---------- view 1: the audio, the window and the grains ----------
		float wx = pad, wy = pad + fs + mm2px(1.0f), ww = W - 2 * pad, wh = mm2px(17.5f);
		box(vg, wx, wy, ww, wh);
		if (len > 0) {
			// waveform
			int cols = (int) ww;
			bool live = S.live;
			int head = e.liveHead;
			nvgBeginPath(vg);
			for (int i = 0; i < cols; i++) {
				int f0 = (int) ((double) len * i / cols), f1 = std::max(f0 + 1, (int) ((double) len * (i + 1) / cols));
				float pk = 0.f;
				for (int f = f0; f < f1; f += PEAK_BLOCK) {
					int idx = live ? (((head - len + f) % MAX_FRAMES + MAX_FRAMES) % MAX_FRAMES) / PEAK_BLOCK : f / PEAK_BLOCK;
					pk = std::max(pk, live ? e.livePeaks[idx] : S.peaks[idx]);
				}
				float h = std::min(1.f, pk) * wh * 0.46f;
				nvgRect(vg, wx + i, wy + wh / 2 - h, 1.f, std::max(0.5f, 2 * h));
			}
			nvgFillColor(vg, C_WAVE);
			nvgFill(vg);

			// the window START..START+WRAP (wraps round the end)
			float st = e.effective[P_START], wr = e.effective[P_WRAP];
			auto shade = [&](float a, float b) { rect(vg, wx + a * ww, wy, std::max(1.f, (b - a) * ww), wh, nvgRGBA(0xF5, 0xC5, 0x2A, 40)); };
			if (st + wr <= 1.f) shade(st, st + wr);
			else { shade(st, 1.f); shade(0.f, st + wr - 1.f); }
			line(vg, wx + st * ww, wy, wx + st * ww, wy + wh, C_POLLEN, 1.2f);
			text(vg, wx + st * ww + 2, wy + 1, "START", C_POLLEN, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, fsT);
			// scan head
			if (wr * len > 1) {
				float sp = std::fmod((float) (st + e.scanPos[s] / len), 1.f);
				line(vg, wx + sp * ww, wy + wh * 0.15f, wx + sp * ww, wy + wh, nvgRGBA(0xF5, 0xC5, 0x2A, 160), 1.f);
			}
			// grains: a blip at each grain's read position, height = its envelope
			for (int v = 0; v < NUM_VOICES; v++) {
				const Engine::Voice& vo = e.voices[v];
				if (!vo.active || vo.snd != s) continue;
				double p = vo.pos;
				if (vo.live) p = std::fmod(p - (head - len) + 4.0 * MAX_FRAMES, (double) MAX_FRAMES);
				p = std::fmod(p, (double) len);
				if (p < 0) p += len;
				float x = wx + (float) (p / len) * ww;
				float env = std::min(1.f, vo.amp > 0 ? vo.env / vo.amp : 0.f);
				float h = wh * (0.15f + 0.8f * env);
				nvgBeginPath(vg);
				nvgCircle(vg, x, wy + wh - h, 1.6f + 1.6f * env);
				nvgFillColor(vg, nvgRGBA(255, 236, 160, (int) (90 + 160 * env)));
				nvgFill(vg);
				line(vg, x, wy + wh, x, wy + wh - h, nvgRGBA(255, 236, 160, (int) (40 + 80 * env)), 0.8f);
			}
			if (e.recording && !e.recWaiting && e.recSound == s) {
				float x = wx + std::min(1.f, (float) e.recPos / std::max(1, len)) * ww;
				line(vg, x, wy - 1, x, wy + wh + 1, C_REC, 1.5f);
			}
			text(vg, wx + ww - 2, wy + 1, string::f("%d grains", e.activeVoices), C_SOFT, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, fsT);
		}
		else {
			std::string hint = e.recording ? (e.recWaiting ? "waiting for signal..." : "recording...") : "empty: right-click > Load WAV, or patch audio in and press SAMPLE";
			text(vg, wx + ww / 2, wy + wh / 2, hint, e.recording ? C_REC : C_SOFT, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, fsS);
		}

		// ---------- bottom row ----------
		float by = wy + wh + mm2px(1.2f), bh = H - by - pad;
		float ex = pad, ew = mm2px(34.f);
		float mx = ex + ew + mm2px(1.2f), mw = mm2px(56.f);
		float rx = mx + mw + mm2px(1.2f), rw = W - pad - rx;

		// ---------- view 2: grain envelope ----------
		box(vg, ex, by, ew, bh);
		{
			int sa = clamp((int) std::lround(S.scene[0].p[P_SHAPE]), 0, NUM_SHAPES - 1);
			int sb = clamp((int) std::lround(S.scene[1].p[P_SHAPE]), 0, NUM_SHAPES - 1);
			float gx = ex + mm2px(1.5f), gy = by + mm2px(3.2f), gw = ew - mm2px(3.f), gh = bh - mm2px(5.4f);
			auto curve = [&](int shape, NVGcolor c, float wdt) {
				nvgBeginPath(vg);
				for (int i = 0; i <= 60; i++) {
					float x = i / 60.f;
					float y = std::max(-0.25f, shapeEnv(shape, x));
					float px = gx + x * gw, py = gy + gh * (1.f - y) * 0.8f;
					if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
				}
				nvgStrokeColor(vg, c);
				nvgStrokeWidth(vg, wdt);
				nvgStroke(vg);
			};
			if (sb != sa) curve(sb, nvgTransRGBA(C_B, 150), 1.f);
			curve(sa, nvgTransRGBA(C_A, sb != sa ? 150 : 230), 1.2f);
			// newest grain's progress
			int newest = -1, minAge = 1 << 30;
			for (int v = 0; v < NUM_VOICES; v++)
				if (e.voices[v].active && e.voices[v].age < minAge) { minAge = e.voices[v].age; newest = v; }
			if (newest >= 0) {
				const Engine::Voice& vo = e.voices[newest];
				float x = (float) vo.age / vo.dur;
				float y = std::max(-0.25f, shapeEnv(vo.shape, x));
				nvgBeginPath(vg);
				nvgCircle(vg, gx + x * gw, gy + gh * (1.f - y) * 0.8f, 2.4f);
				nvgFillColor(vg, C_POLLEN);
				nvgFill(vg);
			}
			std::string t = sa == sb ? std::string("SHAPE ") + SHAPE_NAMES[sa] : std::string(SHAPE_NAMES[sa]) + " / " + SHAPE_NAMES[sb];
			text(vg, ex + mm2px(1.f), by + mm2px(0.6f), t, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, fsT);
			bool rev = S.scene[e.editScene].reverse;
			std::string dir = S.pingPong ? "ping-pong" : rev ? "< reverse" : "forward >";
			text(vg, ex + ew - mm2px(1.f), by + bh - mm2px(0.5f), dir, rev || S.pingPong ? C_B : C_SOFT, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM, fsT);
			text(vg, ex + mm2px(1.f), by + bh - mm2px(0.5f), e.shownSize < 1.f ? string::f("%.0f ms", e.shownSize * 1000) : string::f("%.2f s", e.shownSize), C_SOFT, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, fsT);
		}

		// ---------- view 3: morph between Scene A and Scene B ----------
		box(vg, mx, by, mw, bh);
		{
			text(vg, mx + mm2px(1.f), by + mm2px(0.6f), "A", C_A, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, fsS);
			text(vg, mx + mw - mm2px(1.f), by + mm2px(0.6f), "B", C_B, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, fsS);
			float bx0 = mx + mm2px(4.f), bx1 = mx + mw - mm2px(4.f), byy = by + mm2px(1.7f);
			line(vg, bx0, byy, bx1, byy, C_SOFT, 1.f);
			float mxp = bx0 + (bx1 - bx0) * e.morph;
			nvgBeginPath(vg);
			nvgCircle(vg, mxp, byy, 2.6f);
			nvgFillColor(vg, C_POLLEN);
			nvgFill(vg);
			text(vg, (bx0 + bx1) / 2, byy + mm2px(1.0f), string::f("MORPH %.0f%%", e.morph * 100), C_SOFT, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, fsT);
			// one column per parameter: A mark, B mark, and the value you hear now
			float cy0 = by + mm2px(5.6f), ch = bh - mm2px(8.6f);
			float cw = (mw - mm2px(2.f)) / NUM_P;
			static const char* SHORT[NUM_P] = {"ST", "WR", "SC", "RT", "SZ", "PI", "SH", "LV", "TN", "BL"};
			for (int p = 0; p < NUM_P; p++) {
				float cx = mx + mm2px(1.f) + cw * (p + 0.5f);
				float range = P_MAX[p] - P_MIN[p];
				auto ny = [&](float v) { return cy0 + ch * (1.f - clamp((v - P_MIN[p]) / range, 0.f, 1.f)); };
				rect(vg, cx - 1.f, cy0, 2.f, ch, nvgRGB(0x2A, 0x2E, 0x26));
				float now = p == P_PITCH ? clamp(e.effective[p], -1.f, 1.f) : e.effective[p];
				float base = (p == P_SCAN || p == P_PITCH || p == P_TONE) ? 0.f : P_MIN[p];
				float y0 = ny(base), y1 = ny(now);
				rect(vg, cx - 1.5f, std::min(y0, y1), 3.f, std::max(1.f, std::fabs(y1 - y0)), nvgTransRGBA(C_POLLEN, 200));
				line(vg, cx - 3.5f, ny(S.scene[0].p[p]), cx - 0.5f, ny(S.scene[0].p[p]), C_A, 1.4f);
				line(vg, cx + 0.5f, ny(S.scene[1].p[p]), cx + 3.5f, ny(S.scene[1].p[p]), C_B, 1.4f);
				text(vg, cx, by + bh - mm2px(0.4f), SHORT[p], m->modEdit >= 0 && S.scene[e.editScene].mod[m->modEdit][p] != 0.f ? C_MOD[m->modEdit] : C_SOFT, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM, fsT * 0.9f);
			}
		}

		// ---------- view 4: plain-words readout ----------
		box(vg, rx, by, rw, bh);
		{
			float lx = rx + mm2px(1.2f), ly = by + mm2px(0.7f), lh = mm2px(2.45f);
			int row = 0;
			auto put = [&](const std::string& t, NVGcolor c) { text(vg, lx, ly + lh * row++, t, c, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, fsT); };
			static const char* SRC[4] = {"X", "Y", "Z", "RAND"};
			if (m->modEdit >= 0) {
				int src = m->modEdit;
				put(string::f("EDITING %s DEPTH (Scene %c)", SRC[src], e.editScene ? 'B' : 'A'), C_MOD[src]);
				put("Turn any knob: noon = none,", C_INK);
				put("right = more, left = inverted.", C_INK);
				put(src == SRC_RAND ? "RAND: new random value every grain" : string::f("%s input now %+.2f V", SRC[src], e.cv[src] * 5.f), C_SOFT);
				put(string::f("hardware: hold %s and turn knobs", SRC[src]), C_SOFT);
				// depth bars
				float bx = rx + rw - mm2px(22.f), bw = mm2px(20.f);
				for (int p = 0; p < NUM_P; p++) {
					float d = S.scene[e.editScene].mod[src][p];
					float yy = by + mm2px(1.0f) + p * (bh - mm2px(2.f)) / NUM_P;
					float cxm = bx + bw / 2;
					rect(vg, bx, yy, bw, 1.f, nvgRGB(0x2A, 0x2E, 0x26));
					if (d != 0.f) rect(vg, d > 0 ? cxm : cxm + d * bw / 2, yy - 1.f, std::fabs(d) * bw / 2, 3.f, C_MOD[src]);
				}
			}
			else {
				// RATE
				bool synced = e.syncOn && e.syncConnected && e.clockValid;
				std::string rs;
				if (synced) rs = string::f("RATE clock %s", CLOCK_STEP_NAMES[clockStep(e.effective[P_RATE])]);
				else if (e.shownRate <= 0.f) rs = "RATE 0: one grain per trigger";
				else rs = string::f("RATE %.1f grains/s", e.shownRate);
				if (S.linkRateSize) rs += string::f("  (%.1fx overlap)", e.shownRate * e.shownSize);
				put(rs, C_INK);
				// PITCH with the hardware LED colour
				float semi = e.shownPitch;
				bool onOct = std::fabs(std::fmod(std::fabs(semi), 12.f)) < 0.05f;
				NVGcolor led = S.quantMask ? C_PURPLE : onOct ? C_GREEN : C_REC;
				std::string ledName = S.quantMask ? "purple: quantized" : onOct ? "green: octave" : "red: between";
				put(string::f("PITCH %+.1f st   LED %s", semi, ledName.c_str()), led);
				// TONE, BLUR, LEVEL
				float fc = toneCutoff(e.effective[P_TONE]);
				std::string ts = fc <= 0.f ? "TONE flat" : string::f("TONE %s %.0f Hz", e.effective[P_TONE] < 0 ? "LP" : "HP", fc);
				put(ts + string::f("  BLUR %.0f%%  LEVEL %.0f%%", e.effective[P_BLUR] * 100, e.effective[P_LEVEL] * 100), C_INK);
				// SCAN
				float sp = scanSpeed(e.effective[P_SCAN]);
				put(sp == 0.f ? "SCAN still: grains repeat the same spot" : string::f("SCAN %s x%.2f", sp < 0 ? "backward" : "forward", std::fabs(sp)), C_INK);
				// links and play state
				std::string ls = "LINKS:";
				ls += S.linkRateSize ? " RATE-SIZE" : "";
				ls += S.linkSizePitch ? " SIZE-PITCH" : "";
				if (!S.linkRateSize && !S.linkSizePitch) ls += " none";
				ls += e.latch ? "   playing (latched)" : e.held ? "   playing (held)" : "   stopped";
				put(ls, C_SOFT);
			}
			if (m->messageTime > 0.f) {
				rect(vg, rx + 1, by + bh - lh - mm2px(0.6f), rw - 2, lh + mm2px(0.4f), C_BOX);
				text(vg, lx, by + bh - mm2px(0.5f), m->message, C_POLLEN, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, fsT);
			}
		}
		Widget::drawLayer(args, layer);
	}
};

// ---- panel -----------------------------------------------------------------
// Positions in mm, matching res/make_panel.py
struct PollenWidget : ModuleWidget {
	PollenWidget(Pollen* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Pollen.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PollenDisplay* d = createWidget<PollenDisplay>(mm2px(Vec(4.5, 13.0)));
		d->box.size = mm2px(Vec(173.9, 45.0));
		d->module = module;
		addChild(d);

		// SOUND section
		const float sx[4] = {8.0, 17.3, 26.6, 35.9};
		for (int i = 0; i < 4; i++)
			addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(sx[i], 69.0)), module, Pollen::SOUND_BUTTON + i, Pollen::SOUND_LIGHT + i));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(8.0, 82.0)), module, Pollen::LATCH_BUTTON, Pollen::LATCH_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(17.3, 82.0)), module, Pollen::SAMPLE_BUTTON, Pollen::SAMPLE_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(26.6, 82.0)), module, Pollen::THRU_BUTTON, Pollen::THRU_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<BlueLight>>(mm2px(Vec(8.0, 97.0)), module, Pollen::SCENE_A_BUTTON, Pollen::SCENE_A_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(19.0, 97.0)), module, Pollen::SCENE_B_BUTTON, Pollen::SCENE_B_LIGHT));
		addParam(createParamCentered<VCVSlider>(mm2px(Vec(37.5, 98.5)), module, Pollen::MORPH_PARAM));

		// WHERE and GRAIN knobs
		const float row1 = 73.0, row2 = 99.0;
		const float wx[3] = {52.0, 68.0, 84.0};
		const float gx[3] = {103.0, 127.0, 151.0};
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(wx[0], row1)), module, Pollen::START_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(wx[1], row1)), module, Pollen::WRAP_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(wx[2], row1)), module, Pollen::SCAN_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(gx[0], row1)), module, Pollen::RATE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(gx[1], row1)), module, Pollen::SIZE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(gx[2], row1)), module, Pollen::PITCH_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(115.0, row1)), module, Pollen::LINK_RS_BUTTON, Pollen::LINK_RS_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(139.0, row1)), module, Pollen::LINK_SP_BUTTON, Pollen::LINK_SP_LIGHT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(wx[0], row2)), module, Pollen::SHAPE_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(wx[1], row2)), module, Pollen::REVERSE_BUTTON, Pollen::REVERSE_LIGHT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(wx[2], row2)), module, Pollen::LEVEL_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(gx[0], row2)), module, Pollen::TONE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(gx[1], row2)), module, Pollen::BLUR_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(gx[2], row2)), module, Pollen::MORPH_ATT_PARAM));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(136.0, 60.9)), module, Pollen::GRAIN_LIGHT));

		// MOD depth buttons
		for (int i = 0; i < 4; i++)
			addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(173.0, 69.0 + 9.5 * i)), module, Pollen::MOD_BUTTON + i, Pollen::MOD_LIGHT + i));

		// jacks
		const float jy = 118.0;
		const int ins[10] = {Pollen::IN_L_INPUT, Pollen::IN_R_INPUT, Pollen::GATE_INPUT, Pollen::NEXT_INPUT, Pollen::SELECT_INPUT,
		                     Pollen::SYNC_INPUT, Pollen::MORPH_INPUT, Pollen::X_INPUT, Pollen::Y_INPUT, Pollen::Z_INPUT};
		for (int i = 0; i < 10; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.0 + 11.0 * i, jy)), module, ins[i]));
		const int outs[4] = {Pollen::OUT_L_OUTPUT, Pollen::OUT_R_OUTPUT, Pollen::A_OUTPUT, Pollen::B_OUTPUT};
		for (int i = 0; i < 4; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(129.0 + 11.5 * i, jy)), module, outs[i]));
	}

	void appendContextMenu(Menu* menu) override {
		Pollen* m = getModule<Pollen>();
		Engine* e = &m->eng;
		int s = e->selected;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(string::f("SOUND %d", s + 1)));
		menu->addChild(createMenuItem("Load WAV...", "", [=]() {
			osdialog_filters* filters = osdialog_filters_parse("WAV:wav,WAV");
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
			osdialog_filters_free(filters);
			if (!path) return;
			std::string err = m->loadSound(s, path);
			std::free(path);
			if (!err.empty()) osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, err.c_str());
			else m->say(string::f("Loaded into Sound %d (%.1f s)", s + 1, e->sound[s].length / (float) BUF_RATE));
		}));
		menu->addChild(createMenuItem("Save as WAV...", "", [=]() {
			if (e->sound[s].length == 0) { m->say("This Sound is empty"); return; }
			osdialog_filters* filters = osdialog_filters_parse("WAV:wav");
			std::string def = string::f("pollen-sound%d.wav", s + 1);
			char* path = osdialog_file(OSDIALOG_SAVE, NULL, def.c_str(), filters);
			osdialog_filters_free(filters);
			if (!path) return;
			std::string p = path;
			std::free(path);
			if (system::getExtension(p) != ".wav") p += ".wav";
			std::string err = m->saveSound(s, p);
			if (!err.empty()) osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, err.c_str());
			else m->say("Saved");
		}));
		menu->addChild(createSubmenuItem("Grab the last seconds of input", "", [=](Menu* sub) {
			const int secs[4] = {4, 8, 16, 32};
			for (int i = 0; i < 4; i++) {
				sub->addChild(createMenuItem(string::f("Last %d s", secs[i]), "", [=]() {
					m->pauseAudio();
					e->grabLive(s, secs[i]);
					m->resumeAudio();
					m->say(string::f("Grabbed %.1f s of input into Sound %d", e->sound[s].length / (float) BUF_RATE, s + 1));
				}));
			}
		}));
		menu->addChild(createMenuItem("Clear audio", "", [=]() {
			m->pauseAudio();
			e->clearSound(s);
			m->resumeAudio();
		}));
		menu->addChild(createBoolMenuItem("Live: grains from the input buffer", "hw: hold SOUND + SAMPLE",
			[=]() { return e->sound[s].live; }, [=](bool v) { e->sound[s].live = v; }));
		menu->addChild(createBoolMenuItem("Ping-pong grains", "hw: long-press REVERSE",
			[=]() { return e->sound[s].pingPong; }, [=](bool v) { e->sound[s].pingPong = v; }));
		menu->addChild(createSubmenuItem("Pitch quantizer", "", [=](Menu* sub) {
			for (int i = 0; i < 8; i++)
				sub->addChild(createCheckMenuItem(QUANT_NAMES[i], "", [=]() { return e->sound[s].quantMask == QUANT_MASKS[i]; }, [=]() { e->sound[s].quantMask = QUANT_MASKS[i]; }));
		}));
		menu->addChild(createSubmenuItem("Scenes", "", [=](Menu* sub) {
			sub->addChild(createMenuItem("Copy Scene A to B", "", [=]() { e->sound[s].scene[1] = e->sound[s].scene[0]; m->knobsDirty = true; }));
			sub->addChild(createMenuItem("Copy Scene B to A", "", [=]() { e->sound[s].scene[0] = e->sound[s].scene[1]; m->knobsDirty = true; }));
			sub->addChild(createMenuItem(string::f("Reset Scene %c", e->editScene ? 'B' : 'A'), "", [=]() { e->sound[s].scene[e->editScene].reset(); m->knobsDirty = true; }));
			sub->addChild(createMenuItem(string::f("Clear all modulation depths in Scene %c", e->editScene ? 'B' : 'A'), "", [=]() {
				std::memset(e->sound[s].scene[e->editScene].mod, 0, sizeof(e->sound[s].scene[e->editScene].mod));
				m->knobsDirty = true;
			}));
		}));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Recording"));
		menu->addChild(createBoolPtrMenuItem("Wait for signal before recording", "threshold", &e->recThreshold));
		menu->addChild(createBoolPtrMenuItem("Normalise recordings", "", &e->recNormalize));
		menu->addChild(createIndexSubmenuItem("Record from", {"Inputs", "Outputs (resample)"},
			[=]() { return e->recResample ? 1 : 0; }, [=](int i) { e->recResample = i == 1; }));
		menu->addChild(createIndexSubmenuItem("Live buffer length", {"8 s", "16 s", "24 s", "32 s"},
			[=]() { return clamp(e->liveSeconds / 8 - 1, 0, 3); }, [=](int i) { e->liveSeconds = (i + 1) * 8; }));
		menu->addChild(createIndexSubmenuItem("Input gain", {"x0.5", "x1 (modular level)", "x1.5", "x2.5 (quiet sources)"},
			[=]() { return m->inputGain < 0.75f ? 0 : m->inputGain < 1.25f ? 1 : m->inputGain < 2.f ? 2 : 3; },
			[=](int i) { const float g[4] = {0.5f, 1.f, 1.5f, 2.5f}; m->inputGain = g[i]; }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Timing and outputs"));
		menu->addChild(createBoolPtrMenuItem("RATE follows the SYNC clock", "", &e->syncOn));
		menu->addChild(createBoolPtrMenuItem("GATE restarts SCAN", "", &e->gateResetsScan));
		menu->addChild(createIndexPtrSubmenuItem("NEXT input", {"Next Sound", "Reset SCAN"}, &m->nextResetsScan));
		menu->addChild(createIndexSubmenuItem("Output A", std::vector<std::string>(OUT_SOURCE_NAMES, OUT_SOURCE_NAMES + NUM_OS),
			[=]() { return m->outSource[0]; }, [=](int i) { m->outSource[0] = i; }));
		menu->addChild(createIndexSubmenuItem("Output B", std::vector<std::string>(OUT_SOURCE_NAMES, OUT_SOURCE_NAMES + NUM_OS),
			[=]() { return m->outSource[1]; }, [=](int i) { m->outSource[1] = i; }));
		menu->addChild(createBoolPtrMenuItem("Keep Sounds with the patch", "", &m->keepWithPatch));

		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("On the hardware...", "", [=](Menu* sub) {
			const char* lines[] = {
				"MOD DEPTH buttons = hold X / Y / Z / RAND and turn a knob",
				"Live (menu) = hold SOUND, press SAMPLE",
				"Ping-pong (menu) = long-press REVERSE",
				"Pitch quantizer (menu) = PITCH's ALT page",
				"RATE follows SYNC (menu) = sync toggle on the ALT page",
				"Output A / B source (menu) = System Settings",
				"Hardware has 8 Sounds and Presets; Pollen has 4 Sounds",
				"PITCH LED: green on octaves, red between, purple quantized",
			};
			for (const char* l : lines) sub->addChild(createMenuLabel(l));
		}));
	}
};

Model* modelPollen = createModel<Pollen, PollenWidget>("Pollen");
