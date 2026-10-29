#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "plugin.hpp"
#include "dsp.hpp"
#include <osdialog.h>
#include <atomic>
#include <memory>

static const int SLOTS_PER_TYPE = 10;
static const int NUM_SLOTS = SLOTS_PER_TYPE * 2;   // 0..5 impacts, 6..11 whooshes
static const int NUM_VOICES = 32;
static const int MAX_LAYERS = 4;

enum SlotType { SLOT_IMPACT = 0, SLOT_WHOOSH = 1 };
enum FireMode { FIRE_EVENT = 0, FIRE_WHOOSH = 1, FIRE_IMPACT = 2 };

static inline int slotType(int slot) { return slot < SLOTS_PER_TYPE ? SLOT_IMPACT : SLOT_WHOOSH; }


struct Grond : Module {
	enum ParamId {
		// timing / layers
		GAIN_PARAM,
		PITCH_PARAM,
		TIME_PARAM,
		ALIGN_PARAM,
		OFFSET_PARAM,
		DEBRIS_PARAM,
		SCATTER_PARAM,
		WHOOSH_LVL_PARAM,
		IMPACT_LVL_PARAM,
		WHOOSH_LAYERS_PARAM,
		IMPACT_LAYERS_PARAM,
		TRIG_BTN_PARAM,
		WHOOSH_BTN_PARAM,
		IMPACT_BTN_PARAM,
		// randomisation
		CHAOS_PARAM,
		R_PITCH_PARAM,
		R_GAIN_PARAM,
		R_TIME_PARAM,
		R_PAN_PARAM,
		R_REV_PARAM,
		// doppler
		D_AMT_PARAM,
		D_CENTER_PARAM,
		D_PASS_PARAM,
		D_WIDTH_PARAM,
		D_DIST_PARAM,
		D_DIR_PARAM,
		D_AUTO_PARAM,
		// disperser
		DISP_AMT_PARAM,
		DISP_FREQ_PARAM,
		DISP_PINCH_PARAM,
		DISP_MIX_PARAM,
		// distortion
		DIST_DRIVE_PARAM,
		DIST_TYPE_PARAM,
		DIST_TONE_PARAM,
		DIST_MIX_PARAM,
		// spectral delay
		SPEC_TIME_PARAM,
		SPEC_TILT_PARAM,
		SPEC_FB_PARAM,
		SPEC_SPREAD_PARAM,
		SPEC_MIX_PARAM,
		// OTT
		OTT_DEPTH_PARAM,
		OTT_TIME_PARAM,
		OTT_MIX_PARAM,
		OTT_LOW_PARAM,
		OTT_MID_PARAM,
		OTT_HIGH_PARAM,
		NUM_PARAMS
	};
	enum InputId {
		TRIG_INPUT,
		WHOOSH_INPUT,
		IMPACT_INPUT,
		PITCH_INPUT,
		RAND_INPUT,
		DOPP_INPUT,
		TIME_CV_INPUT,
		OFFSET_CV_INPUT,
		SCATTER_CV_INPUT,
		DRIVE_CV_INPUT,
		TONE_CV_INPUT,
		DISTMIX_CV_INPUT,
		STIME_CV_INPUT,
		STILT_CV_INPUT,
		SFB_CV_INPUT,
		SSPREAD_CV_INPUT,
		SMIX_CV_INPUT,
		NUM_INPUTS
	};
	enum OutputId {
		L_OUTPUT,
		R_OUTPUT,
		GATE_OUTPUT,
		NUM_OUTPUTS
	};

	struct Slot {
		std::shared_ptr<iw::Sample> sample;   // accessed with std::atomic_load/store
		float flash = 0.f;
	};

	struct Voice {
		bool active = false;
		bool started = false;
		bool gate = false;        // pulse the gate output when this voice starts
		bool reverse = false;
		bool doppler = false;
		std::shared_ptr<iw::Sample> smp;
		int slot = 0;
		double delay = 0.0;       // samples until the voice starts
		double pos = 0.0;         // read position in source frames
		float rate = 1.f;         // source frames per output sample
		float gain = 1.f;
		float pan = 0.f;
		float mach = 0.f, center = 0.5f, pass = 0.3f, width = 1.f, att = 0.f, dir = 1.f;
		float lpL = 0.f, lpR = 0.f;
		float head = 0.f;         // normalised source position, read by the UI for the play head
		int age = 0;              // samples since allocation
		int playAge = 0;          // samples since start
	};

	Slot slots[NUM_SLOTS];
	Voice voices[NUM_VOICES];
	int lastPick[2] = {-1, -1};
	double lastFire[3] = {-100.0, -100.0, -100.0};   // wall-clock time of the last trigger per mode, for the hammer animation

	dsp::SchmittTrigger trigEvent, trigWhoosh, trigImpact;
	dsp::SchmittTrigger btnEvent, btnWhoosh, btnImpact;
	dsp::PulseGenerator impactPulse;

	iw::Disperser disperser;
	iw::Distortion distortion;
	iw::SpectralDelay spectral;
	iw::OTT ott;
	bool dispActive = false, specActive = false, ottActive = false;
	int fxCounter = 0;

	bool dopplerOnImpacts = false;
	std::string lastDir;

	Grond() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);

		configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Output level", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " st");
		configParam(TIME_PARAM, 0.f, 4.f, 0.6f, "Impact time (when Align is manual)", " s");
		configSwitch(ALIGN_PARAM, 0.f, 2.f, 1.f, "Impact align", {"Manual time", "Whoosh peak", "Whoosh end"});
		configParam(OFFSET_PARAM, -0.5f, 0.5f, 0.f, "Impact offset", " ms", 0.f, 1000.f);
		configParam(DEBRIS_PARAM, 0.f, 8.f, 0.f, "Debris hits");
		paramQuantities[DEBRIS_PARAM]->snapEnabled = true;
		configParam(SCATTER_PARAM, 0.05f, 3.f, 0.6f, "Debris scatter", " s");
		configParam(WHOOSH_LVL_PARAM, 0.f, 1.5f, 0.8f, "Whoosh level", "%", 0.f, 100.f);
		configParam(IMPACT_LVL_PARAM, 0.f, 1.5f, 1.f, "Impact level", "%", 0.f, 100.f);
		configParam(WHOOSH_LAYERS_PARAM, 1.f, MAX_LAYERS, 1.f, "Whoosh layers (samples played at once)");
		paramQuantities[WHOOSH_LAYERS_PARAM]->snapEnabled = true;
		configParam(IMPACT_LAYERS_PARAM, 1.f, MAX_LAYERS, 1.f, "Impact layers (samples played at once)");
		paramQuantities[IMPACT_LAYERS_PARAM]->snapEnabled = true;
		configButton(TRIG_BTN_PARAM, "Trigger whoosh + impact");
		configButton(WHOOSH_BTN_PARAM, "Trigger whoosh only");
		configButton(IMPACT_BTN_PARAM, "Trigger impact only");

		configParam(CHAOS_PARAM, 0.f, 1.f, 0.5f, "Chaos (scales all randomisation)", "%", 0.f, 100.f);
		configParam(R_PITCH_PARAM, 0.f, 12.f, 2.f, "Random pitch", " st");
		configParam(R_GAIN_PARAM, 0.f, 12.f, 3.f, "Random level", " dB");
		configParam(R_TIME_PARAM, 0.f, 0.5f, 0.03f, "Random timing", " ms", 0.f, 1000.f);
		configParam(R_PAN_PARAM, 0.f, 1.f, 0.3f, "Random pan", "%", 0.f, 100.f);
		configParam(R_REV_PARAM, 0.f, 1.f, 0.f, "Reverse probability", "%", 0.f, 100.f);

		configParam(D_AMT_PARAM, 0.f, 1.f, 0.5f, "Doppler amount", "%", 0.f, 100.f);
		configParam(D_CENTER_PARAM, 0.f, 1.f, 0.5f, "Pass-by position (offset)", "%", 0.f, 100.f);
		configParam(D_PASS_PARAM, 0.f, 1.f, 0.5f, "Pass sharpness", "%", 0.f, 100.f);
		configParam(D_WIDTH_PARAM, 0.f, 1.f, 0.7f, "Stereo width of pass", "%", 0.f, 100.f);
		configParam(D_DIST_PARAM, 0.f, 1.f, 0.4f, "Distance (level + air loss)", "%", 0.f, 100.f);
		configSwitch(D_DIR_PARAM, 0.f, 2.f, 0.f, "Direction", {"Left to right", "Right to left", "Random"});
		configSwitch(D_AUTO_PARAM, 0.f, 1.f, 1.f, "Pass-by centre", {"Manual", "Auto (loudest part)"});

		configParam(DISP_AMT_PARAM, 0.f, 1.f, 0.f, "Disseminator amount", "%", 0.f, 100.f);
		configParam(DISP_FREQ_PARAM, 0.f, 1.f, 0.5f, "Disseminator frequency", " Hz", 1000.f, 20.f);
		configParam(DISP_PINCH_PARAM, 0.f, 1.f, 0.4f, "Disseminator pinch", "%", 0.f, 100.f);
		configParam(DISP_MIX_PARAM, 0.f, 1.f, 1.f, "Disseminator mix", "%", 0.f, 100.f);

		configParam(DIST_DRIVE_PARAM, 0.f, 1.f, 0.3f, "Drive", "%", 0.f, 100.f);
		configSwitch(DIST_TYPE_PARAM, 0.f, 3.f, 0.f, "Distortion type", {"Soft", "Hard", "Fold", "Crush"});
		configParam(DIST_TONE_PARAM, 0.f, 1.f, 1.f, "Tone", "%", 0.f, 100.f);
		configParam(DIST_MIX_PARAM, 0.f, 1.f, 0.f, "Distortion mix", "%", 0.f, 100.f);

		configParam(SPEC_TIME_PARAM, 0.f, 2.f, 0.3f, "Spectral delay time", " s");
		configParam(SPEC_TILT_PARAM, -1.f, 1.f, 0.f, "Tilt (-: lows later, +: highs later)", "%", 0.f, 100.f);
		configParam(SPEC_FB_PARAM, 0.f, 0.95f, 0.4f, "Feedback", "%", 0.f, 100.f);
		configParam(SPEC_SPREAD_PARAM, 0.f, 1.f, 0.f, "Spread (per-band time scatter)", "%", 0.f, 100.f);
		configParam(SPEC_MIX_PARAM, 0.f, 1.f, 0.f, "Spectral delay mix", "%", 0.f, 100.f);

		configParam(OTT_DEPTH_PARAM, 0.f, 1.f, 0.5f, "Multiband depth (up + down compression)", "%", 0.f, 100.f);
		configParam(OTT_TIME_PARAM, 0.f, 1.f, 0.4f, "Multiband time (attack / release)", "%", 0.f, 100.f);
		configParam(OTT_MIX_PARAM, 0.f, 1.f, 0.f, "Multiband mix", "%", 0.f, 100.f);
		configParam(OTT_LOW_PARAM, -12.f, 12.f, 0.f, "Multiband low band gain (<120 Hz)", " dB");
		configParam(OTT_MID_PARAM, -12.f, 12.f, 0.f, "Multiband mid band gain", " dB");
		configParam(OTT_HIGH_PARAM, -12.f, 12.f, 0.f, "Multiband high band gain (>2.5 kHz)", " dB");

		configInput(TRIG_INPUT, "Trigger: whoosh then impact");
		configInput(WHOOSH_INPUT, "Trigger: whoosh only");
		configInput(IMPACT_INPUT, "Trigger: impact only");
		configInput(PITCH_INPUT, "Pitch (1V/oct)");
		configInput(RAND_INPUT, "Chaos CV (0-10V)");
		configInput(DOPP_INPUT, "Doppler amount CV (0-10V)");
		configInput(TIME_CV_INPUT, "Impact time CV (10V = full range)");
		configInput(OFFSET_CV_INPUT, "Impact offset CV (10V = full range)");
		configInput(SCATTER_CV_INPUT, "Debris scatter CV (10V = full range)");
		configInput(DRIVE_CV_INPUT, "Drive CV (10V = full range)");
		configInput(TONE_CV_INPUT, "Tone CV (10V = full range)");
		configInput(DISTMIX_CV_INPUT, "Distortion mix CV (10V = full range)");
		configInput(STIME_CV_INPUT, "Spectral delay time CV (10V = full range)");
		configInput(STILT_CV_INPUT, "Spectral delay tilt CV (10V = full range)");
		configInput(SFB_CV_INPUT, "Spectral delay feedback CV (10V = full range)");
		configInput(SSPREAD_CV_INPUT, "Spectral delay spread CV (10V = full range)");
		configInput(SMIX_CV_INPUT, "Spectral delay mix CV (10V = full range)");
		configOutput(L_OUTPUT, "Left / mono");
		configOutput(R_OUTPUT, "Right");
		configOutput(GATE_OUTPUT, "Impact gate");
	}

	// ----------------------------------------------------------- samples

	bool loadSlot(int i, const std::string& path) {
		std::shared_ptr<iw::Sample> s = std::make_shared<iw::Sample>();
		std::string err;
		std::vector<unsigned char> bytes;
		try {
			std::vector<uint8_t> data = system::readFile(path);
			bytes.assign(data.begin(), data.end());
		}
		catch (Exception& e) {
			WARN("Grond: could not read %s: %s", path.c_str(), e.what());
			return false;
		}
		if (!s->load(path, system::getStem(path), bytes, err)) {
			WARN("Grond: could not load %s: %s", path.c_str(), err.c_str());
			return false;
		}
		std::atomic_store(&slots[i].sample, s);
		lastDir = system::getDirectory(path);
		return true;
	}

	void clearSlot(int i) {
		std::atomic_store(&slots[i].sample, std::shared_ptr<iw::Sample>());
	}

	// Replaces the slots of one section with the WAVs found next to the given file.
	void loadFolder(const std::string& anyFileInFolder, int type) {
		int first = type == SLOT_IMPACT ? 0 : SLOTS_PER_TYPE;
		std::string dir = system::getDirectory(anyFileInFolder);
		std::vector<std::string> entries = system::getEntries(dir);
		std::sort(entries.begin(), entries.end());
		int n = 0;
		for (size_t k = 0; k < entries.size() && n < SLOTS_PER_TYPE; k++) {
			if (string::lowercase(system::getExtension(entries[k])) != ".wav")
				continue;
			if (loadSlot(first + n, entries[k]))
				n++;
		}
		for (int i = n; i < SLOTS_PER_TYPE; i++)
			clearSlot(first + i);
	}

	int firstEmptySlot(int type) {
		int first = type == SLOT_IMPACT ? 0 : SLOTS_PER_TYPE;
		for (int i = first; i < first + SLOTS_PER_TYPE; i++)
			if (!std::atomic_load(&slots[i].sample))
				return i;
		return -1;
	}

	int loadedCount() {
		int n = 0;
		for (int i = 0; i < NUM_SLOTS; i++)
			if (std::atomic_load(&slots[i].sample))
				n++;
		return n;
	}

	int activeVoices() {
		int n = 0;
		for (int i = 0; i < NUM_VOICES; i++)
			if (voices[i].active)
				n++;
		return n;
	}

	// ----------------------------------------------------------- engine

	static float rbi() { return random::uniform() * 2.f - 1.f; }

	// Parameter value plus CV: 10 V sweeps the parameter's whole range.
	float mod(int paramId, int inputId) {
		float v = params[paramId].getValue();
		if (inputs[inputId].isConnected()) {
			ParamQuantity* pq = paramQuantities[paramId];
			v += inputs[inputId].getVoltage() * 0.1f * (pq->getMaxValue() - pq->getMinValue());
			v = iw::clampf(v, pq->getMinValue(), pq->getMaxValue());
		}
		return v;
	}

	// Picks up to `want` distinct loaded slots of a type, in random order.
	int pickSlots(int type, int want, int* out) {
		int first = type == SLOT_IMPACT ? 0 : SLOTS_PER_TYPE;
		int cand[SLOTS_PER_TYPE];
		int n = 0;
		for (int i = first; i < first + SLOTS_PER_TYPE; i++)
			if (std::atomic_load(&slots[i].sample))
				cand[n++] = i;
		if (n == 0)
			return 0;
		for (int i = n - 1; i > 0; i--) {
			int j = std::min((int) (random::uniform() * (i + 1)), i);
			std::swap(cand[i], cand[j]);
		}
		if (n > 1 && cand[0] == lastPick[type])
			std::swap(cand[0], cand[1]);
		lastPick[type] = cand[0];
		int k = std::min(want, n);
		for (int i = 0; i < k; i++)
			out[i] = cand[i];
		return k;
	}

	Voice* allocVoice() {
		int best = 0, bestAge = -1;
		for (int i = 0; i < NUM_VOICES; i++) {
			if (!voices[i].active)
				return &voices[i];
			if (voices[i].age > bestAge) {
				bestAge = voices[i].age;
				best = i;
			}
		}
		return &voices[best];
	}

	// Returns the nominal duration in seconds; peakSec receives the time of the loudest part.
	float startVoice(int slot, float delaySec, float semitones, float gain, float pan,
			bool reverse, bool doppler, bool gate, float sr, float* peakSec = NULL) {
		std::shared_ptr<iw::Sample> sp = std::atomic_load(&slots[slot].sample);
		if (!sp)
			return 0.f;
		Voice* v = allocVoice();
		v->smp = sp;
		v->slot = slot;
		v->active = true;
		v->started = false;
		v->gate = gate;
		v->reverse = reverse;
		v->doppler = doppler;
		v->delay = (double) std::max(0.f, delaySec) * sr;
		v->rate = std::pow(2.f, semitones / 12.f) * sp->sampleRate / sr;
		v->pos = reverse ? (double) sp->frames() - 1.0 : 0.0;
		v->gain = gain;
		v->pan = pan;
		v->lpL = v->lpR = 0.f;
		v->age = 0;
		v->playAge = 0;

		if (doppler) {
			float amt = iw::clampf(params[D_AMT_PARAM].getValue() + inputs[DOPP_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			v->mach = amt * 0.6f;
			float c = (params[D_AUTO_PARAM].getValue() > 0.5f) ? sp->peakPos : 0.5f;
			c += params[D_CENTER_PARAM].getValue() - 0.5f;
			v->center = reverse ? 1.f - c : c;
			v->pass = 2.f * std::pow(0.015f, params[D_PASS_PARAM].getValue());   // 2.0 .. 0.03
			v->width = params[D_WIDTH_PARAM].getValue();
			v->att = params[D_DIST_PARAM].getValue();
			int dir = (int) std::round(params[D_DIR_PARAM].getValue());
			v->dir = dir == 0 ? 1.f : (dir == 1 ? -1.f : (random::uniform() < 0.5f ? -1.f : 1.f));
		}
		float dur = (float) ((double) sp->frames() / v->rate / sr);
		if (peakSec)
			*peakSec = dur * (reverse ? 1.f - sp->peakPos : sp->peakPos);
		return dur;
	}

	void fire(int mode, float sr) {
		lastFire[mode] = system::getTime();
		float chaos = iw::clampf(params[CHAOS_PARAM].getValue() + inputs[RAND_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float rPitch = params[R_PITCH_PARAM].getValue() * chaos;
		float rGain = params[R_GAIN_PARAM].getValue() * chaos;
		float rTime = params[R_TIME_PARAM].getValue() * chaos;
		float rPan = params[R_PAN_PARAM].getValue() * chaos;
		float rRev = params[R_REV_PARAM].getValue() * chaos;

		float baseSt = params[PITCH_PARAM].getValue() + inputs[PITCH_INPUT].getVoltage() * 12.f;
		float master = params[GAIN_PARAM].getValue();

		bool didWhoosh = false;
		float whooshDur = 0.f, whooshPeak = 0.f;

		if (mode == FIRE_EVENT || mode == FIRE_WHOOSH) {
			int picks[MAX_LAYERS];
			int k = pickSlots(SLOT_WHOOSH, (int) std::round(params[WHOOSH_LAYERS_PARAM].getValue()), picks);
			float layerScale = k > 0 ? 1.f / std::sqrt((float) k) : 1.f;
			for (int j = 0; j < k; j++) {
				bool rev = random::uniform() < rRev;
				float st = baseSt + rbi() * rPitch;
				float g = master * params[WHOOSH_LVL_PARAM].getValue() * layerScale * std::pow(10.f, rbi() * rGain / 20.f);
				float pan = rbi() * rPan;
				float peak = 0.f;
				float dur = startVoice(picks[j], 0.f, st, g, pan, rev, true, false, sr, &peak);
				if (dur > whooshDur) {
					whooshDur = dur;
					whooshPeak = peak;
				}
				didWhoosh = true;
			}
		}

		if (mode == FIRE_EVENT || mode == FIRE_IMPACT) {
			int picks[MAX_LAYERS];
			int k = pickSlots(SLOT_IMPACT, (int) std::round(params[IMPACT_LAYERS_PARAM].getValue()), picks);
			if (k > 0) {
				float delay;
				if (mode == FIRE_EVENT) {
					int align = didWhoosh ? (int) std::round(params[ALIGN_PARAM].getValue()) : 0;
					float base = align == 0 ? mod(TIME_PARAM, TIME_CV_INPUT) : (align == 1 ? whooshPeak : whooshDur);
					delay = base + mod(OFFSET_PARAM, OFFSET_CV_INPUT) + rbi() * rTime;
				}
				else {
					delay = random::uniform() * rTime;
				}
				delay = std::max(0.f, delay);

				float layerScale = 1.f / std::sqrt((float) k);
				float mainGain = 0.f;
				for (int j = 0; j < k; j++) {
					float st = baseSt + rbi() * rPitch;
					float g = master * params[IMPACT_LVL_PARAM].getValue() * layerScale * std::pow(10.f, rbi() * rGain / 20.f);
					float pan = rbi() * rPan;
					float jitter = j == 0 ? 0.f : rbi() * rTime * 0.15f;
					startVoice(picks[j], delay + std::max(0.f, jitter), st, g, pan, false, dopplerOnImpacts, j == 0, sr);
					if (j == 0)
						mainGain = g;
				}

				// debris: quieter, higher, scattered after the main hit
				int debris = (int) std::round(params[DEBRIS_PARAM].getValue());
				float scatter = mod(SCATTER_PARAM, SCATTER_CV_INPUT);
				for (int i = 1; i <= debris; i++) {
					int ds;
					if (pickSlots(SLOT_IMPACT, 1, &ds) < 1)
						break;
					float dd = delay + 0.02f + std::pow(random::uniform(), 1.5f) * scatter;
					float dst = baseSt + random::uniform() * (3.f + 9.f * chaos) + rbi() * rPitch;
					float dg = mainGain * 0.6f * std::pow(0.82f, (float) i) * (0.5f + 0.5f * random::uniform());
					float dp = rbi() * 0.8f;
					startVoice(ds, dd, dst, dg, dp, false, false, false, sr);
				}
			}
		}
	}

	// Renders one sample of a voice. Returns false while the voice is still waiting to start.
	bool renderVoice(Voice& v, float sr, float& outL, float& outR) {
		v.age++;
		if (v.delay >= 1.0) {
			v.delay -= 1.0;
			return false;
		}
		if (!v.started) {
			v.started = true;
			slots[v.slot].flash = 1.f;
			if (v.gate)
				impactPulse.trigger(1e-3f);
		}

		const iw::Sample& s = *v.smp;
		double len = (double) s.frames();

		float ratio = 1.f, pan = v.pan, dist = 1.f;
		float cutoff = 0.f;
		if (v.doppler) {
			float u = (float) (v.pos / len);
			if (v.reverse)
				u = 1.f - u;
			float x = (u - v.center) * 4.f;
			float rd = std::sqrt(x * x + v.pass * v.pass);
			ratio = 1.f / (1.f + v.mach * (x / rd));           // approaching: >1, receding: <1
			const float da = 0.5f;
			float ra = std::sqrt(x * x + da * da);
			dist = std::pow(da / ra, v.att * 2.f);
			pan = iw::clampf(v.pan + v.dir * v.width * (x / ra), -1.f, 1.f);
			if (v.att > 0.f)
				cutoff = std::max(400.f, 20000.f * std::pow(dist, 2.f * v.att));
		}

		float l = iw::Sample::read(s.L, v.pos);
		float r = iw::Sample::read(s.R, v.pos);

		if (cutoff > 0.f && cutoff < 19000.f) {
			float a = 1.f - std::exp(-2.f * iw::PI * cutoff / sr);
			v.lpL += a * (l - v.lpL);
			v.lpR += a * (r - v.lpR);
			l = v.lpL;
			r = v.lpR;
		}
		else {
			v.lpL = l;
			v.lpR = r;
		}

		float env = dist * v.gain;
		if (v.playAge < 16)
			env *= (float) v.playAge / 16.f;
		double remain = v.reverse ? v.pos : len - 1.0 - v.pos;
		double fadeLen = 0.004 * s.sampleRate;
		if (remain < fadeLen)
			env *= (float) (remain / fadeLen);
		v.playAge++;

		float ang = (pan + 1.f) * iw::PI * 0.25f;
		outL = l * env * std::cos(ang) * 1.41421356f;
		outR = r * env * std::sin(ang) * 1.41421356f;

		v.head = (float) (v.pos / len);
		double step = (double) v.rate * ratio;
		v.pos += v.reverse ? -step : step;
		if (v.pos < 0.0 || v.pos >= len - 1.0) {
			v.active = false;
			v.smp.reset();
		}
		return true;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;

		if (trigEvent.process(inputs[TRIG_INPUT].getVoltage()) | btnEvent.process(params[TRIG_BTN_PARAM].getValue()))
			fire(FIRE_EVENT, sr);
		if (trigWhoosh.process(inputs[WHOOSH_INPUT].getVoltage()) | btnWhoosh.process(params[WHOOSH_BTN_PARAM].getValue()))
			fire(FIRE_WHOOSH, sr);
		if (trigImpact.process(inputs[IMPACT_INPUT].getVoltage()) | btnImpact.process(params[IMPACT_BTN_PARAM].getValue()))
			fire(FIRE_IMPACT, sr);

		float L = 0.f, R = 0.f;
		for (int i = 0; i < NUM_VOICES; i++) {
			Voice& v = voices[i];
			if (!v.active)
				continue;
			float l, r;
			if (renderVoice(v, sr, l, r)) {
				L += l;
				R += r;
			}
		}
		L *= 5.f;
		R *= 5.f;

		float flashDecay = std::exp(-args.sampleTime / 0.35f);
		for (int i = 0; i < NUM_SLOTS; i++)
			slots[i].flash *= flashDecay;

		// ---- effects: disperser -> distortion -> spectral delay -> OTT
		float dispMix = params[DISP_MIX_PARAM].getValue();
		float dispAmt = params[DISP_AMT_PARAM].getValue();
		float distMix = mod(DIST_MIX_PARAM, DISTMIX_CV_INPUT);
		float specMix = mod(SPEC_MIX_PARAM, SMIX_CV_INPUT);
		float ottMix = params[OTT_MIX_PARAM].getValue();

		if ((fxCounter++ & 15) == 0) {
			float freq = 20.f * std::pow(1000.f, params[DISP_FREQ_PARAM].getValue());
			float q = 0.25f * std::pow(16.f, params[DISP_PINCH_PARAM].getValue());
			disperser.setParams(sr, freq, q, dispAmt);
			spectral.setParams(sr, mod(SPEC_TIME_PARAM, STIME_CV_INPUT), mod(SPEC_TILT_PARAM, STILT_CV_INPUT),
				mod(SPEC_SPREAD_PARAM, SSPREAD_CV_INPUT), mod(SPEC_FB_PARAM, SFB_CV_INPUT));
		}

		bool dispOn = dispAmt > 0.f && dispMix > 0.001f;
		if (dispOn) {
			float wl = L, wr = R;
			disperser.process(wl, wr);
			L += dispMix * (wl - L);
			R += dispMix * (wr - R);
		}
		else if (dispActive) {
			disperser.reset();
		}
		dispActive = dispOn;

		if (distMix > 0.001f) {
			distortion.process(sr, L, R, (int) std::round(params[DIST_TYPE_PARAM].getValue()),
				mod(DIST_DRIVE_PARAM, DRIVE_CV_INPUT), mod(DIST_TONE_PARAM, TONE_CV_INPUT), distMix);
		}

		bool specOn = specMix > 0.001f;
		if (specOn) {
			float wl, wr;
			spectral.process(L, R, wl, wr);
			L += specMix * (wl - L);
			R += specMix * (wr - R);
		}
		else if (specActive) {
			spectral.reset();
		}
		specActive = specOn;

		bool ottOn = ottMix > 0.001f;
		if (ottOn) {
			float gains[3] = {
				std::pow(10.f, params[OTT_LOW_PARAM].getValue() / 20.f),
				std::pow(10.f, params[OTT_MID_PARAM].getValue() / 20.f),
				std::pow(10.f, params[OTT_HIGH_PARAM].getValue() / 20.f)
			};
			ott.process(sr, L, R, params[OTT_DEPTH_PARAM].getValue(), params[OTT_TIME_PARAM].getValue(), gains, ottMix);
		}
		else if (ottActive) {
			ott.reset();
		}
		ottActive = ottOn;

		L = iw::clampf(L, -20.f, 20.f);
		R = iw::clampf(R, -20.f, 20.f);
		if (!outputs[R_OUTPUT].isConnected())
			L = 0.5f * (L + R);

		outputs[L_OUTPUT].setVoltage(L);
		outputs[R_OUTPUT].setVoltage(R);
		outputs[GATE_OUTPUT].setVoltage(impactPulse.process(args.sampleTime) ? 10.f : 0.f);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (int i = 0; i < NUM_VOICES; i++)
			voices[i] = Voice();
		for (int i = 0; i < NUM_SLOTS; i++)
			clearSlot(i);
		disperser.reset();
		spectral.reset();
		ott.reset();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_t* arr = json_array();
		for (int i = 0; i < NUM_SLOTS; i++) {
			std::shared_ptr<iw::Sample> s = std::atomic_load(&slots[i].sample);
			json_array_append_new(arr, json_string(s ? s->path.c_str() : ""));
		}
		json_object_set_new(rootJ, "slotPaths", arr);
		json_object_set_new(rootJ, "dopplerOnImpacts", json_boolean(dopplerOnImpacts));
		json_object_set_new(rootJ, "lastDir", json_string(lastDir.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* arr = json_object_get(rootJ, "slotPaths");
		for (int i = 0; arr && i < NUM_SLOTS && i < (int) json_array_size(arr); i++) {
			json_t* p = json_array_get(arr, i);
			std::string path = p && json_is_string(p) ? json_string_value(p) : "";
			if (!path.empty() && system::isFile(path))
				loadSlot(i, path);
			else
				clearSlot(i);
		}
		json_t* d = json_object_get(rootJ, "dopplerOnImpacts");
		if (d)
			dopplerOnImpacts = json_is_true(d);
		json_t* ld = json_object_get(rootJ, "lastDir");
		if (ld)
			lastDir = json_string_value(ld);
	}
};


// ------------------------------------------------------- Nordic / Viking theme

static const NVGcolor C_BONE = nvgRGB(0xea, 0xdf, 0xc4);
static const NVGcolor C_GOLD = nvgRGB(0xe6, 0xb8, 0x5a);
static const NVGcolor C_BRONZE = nvgRGB(0x9a, 0x74, 0x2c);
static const NVGcolor C_EMBER = nvgRGB(0xff, 0x9a, 0x3c);
static const NVGcolor C_FROST = nvgRGB(0x8f, 0xd3, 0xf0);
// section accents (muted metals and dyes)
static const NVGcolor A_COPPER = nvgRGB(0xb4, 0x54, 0x22);
static const NVGcolor A_STEEL = nvgRGB(0x4f, 0x80, 0x9c);
static const NVGcolor A_BRONZE = nvgRGB(0x9a, 0x78, 0x2e);
static const NVGcolor A_MOSS = nvgRGB(0x5f, 0x7e, 0x3a);
static const NVGcolor A_BLOOD = nvgRGB(0x92, 0x2f, 0x2a);
static const NVGcolor A_IRON = nvgRGB(0x62, 0x68, 0x6e);

static NVGcolor alpha(NVGcolor c, float a) {
	c.a = a;
	return c;
}

static NVGcolor mixColor(NVGcolor a, NVGcolor b, float t) {
	return nvgRGBAf(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

static float hash1(float n);

enum FontId { F_LABEL, F_MONO, F_SEG, F_TITLE };

static std::shared_ptr<window::Font> getFont(int f) {
	if (f == F_SEG)
		return APP->window->loadFont(asset::system("res/fonts/DSEG7ClassicMini-Bold.ttf"));
	if (f == F_MONO)
		return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	return APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
}

// The title lettering is pre-rendered into alpha-mask PNGs (res/text, made by tools/gen_text.cpp), so no font
// file ships with the plugin. Each mask is tinted at load time and cached as a mipmapped NanoVG image.
// Classes: 0 = knob/jack labels, 1 = plate titles and header words, 2 = logo.
static const float GLYPH_REF[3] = {64.f, 96.f, 160.f};

unsigned char* decodePngMask(const unsigned char* data, int len, int* w, int* h);   // pngmask.cpp
void freePngMask(unsigned char* p);

struct GlyphMask {
	std::vector<unsigned char> px;
	int w = 0, h = 0;
};

static GlyphMask* glyphMask(int cls, const std::string& s) {
	static std::map<std::string, GlyphMask> masks;
	std::string slug;
	for (char c : s)
		slug += std::isalnum((unsigned char) c) ? (char) std::tolower((unsigned char) c) : '_';
	std::string key = std::to_string(cls) + "_" + slug;
	auto it = masks.find(key);
	if (it != masks.end())
		return it->second.w > 0 ? &it->second : NULL;
	GlyphMask& m = masks[key];
	try {
		std::vector<uint8_t> data = system::readFile(asset::plugin(pluginInstance, "res/text/" + key + ".png"));
		int w = 0, h = 0;
		unsigned char* p = decodePngMask(data.data(), (int) data.size(), &w, &h);
		if (p) {
			m.px.assign(p, p + (size_t) w * h);
			m.w = w;
			m.h = h;
			freePngMask(p);
		}
	}
	catch (...) {
	}
	return m.w > 0 ? &m : NULL;
}

static void boxBlur(std::vector<float>& a, int w, int h, int r) {
	std::vector<float> t(a.size());
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				float s = 0.f;
				for (int k = -r; k <= r; k++)
					s += a[(size_t) y * w + std::min(std::max(x + k, 0), w - 1)];
				t[(size_t) y * w + x] = s / (2 * r + 1);
			}
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				float s = 0.f;
				for (int k = -r; k <= r; k++)
					s += t[(size_t) std::min(std::max(y + k, 0), h - 1) * w + x];
				a[(size_t) y * w + x] = s / (2 * r + 1);
			}
	}
}

// Draws pre-rendered text vertically centred on y. Returns the advance width in px, or -1 if the text has no image.
static float drawGlyphText(NVGcontext* vg, float x, float y, const std::string& s, float size, int align, int cls,
		NVGcolor top, NVGcolor bot, int blur = 0, bool dryRun = false) {
	GlyphMask* m = glyphMask(cls, s);
	if (!m)
		return -1.f;
	float ref = GLYPH_REF[cls];
	float pad = std::ceil(ref / 16.f);
	float k = size / ref;
	float adv = (m->w - 2.f * pad) * k;
	if (dryRun)
		return adv;

	static std::map<std::string, int> images;
	char tag[160];
	std::snprintf(tag, sizeof(tag), "%d_%s_%.3f%.3f%.3f%.3f_%.3f%.3f%.3f%.3f_%d", cls, s.c_str(), top.r, top.g, top.b, top.a,
		bot.r, bot.g, bot.b, bot.a, blur);
	int img;
	auto it = images.find(tag);
	if (it != images.end()) {
		img = it->second;
	}
	else {
		int n = m->w * m->h;
		std::vector<float> a(n);
		for (int i = 0; i < n; i++)
			a[i] = m->px[i] / 255.f;
		if (blur > 0)
			boxBlur(a, m->w, m->h, blur);
		std::vector<unsigned char> rgba((size_t) n * 4);
		for (int yy = 0; yy < m->h; yy++) {
			float t = iw::clampf((yy - pad) / (m->h - 2.f * pad), 0.f, 1.f);
			NVGcolor c = mixColor(top, bot, t);
			for (int xx = 0; xx < m->w; xx++) {
				size_t i = (size_t) yy * m->w + xx;
				rgba[i * 4 + 0] = (unsigned char) (c.r * 255.f + 0.5f);
				rgba[i * 4 + 1] = (unsigned char) (c.g * 255.f + 0.5f);
				rgba[i * 4 + 2] = (unsigned char) (c.b * 255.f + 0.5f);
				rgba[i * 4 + 3] = (unsigned char) (iw::clampf(a[i] * c.a, 0.f, 1.f) * 255.f + 0.5f);
			}
		}
		img = nvgCreateImageRGBA(vg, m->w, m->h, NVG_IMAGE_GENERATE_MIPMAPS, rgba.data());
		images[tag] = img;
	}
	if (img <= 0)
		return -1.f;

	float x0 = x - pad * k;
	if (align & NVG_ALIGN_CENTER)
		x0 = x - adv / 2.f - pad * k;
	else if (align & NVG_ALIGN_RIGHT)
		x0 = x - adv - pad * k;
	float y0 = y - m->h * k / 2.f;
	nvgBeginPath(vg);
	nvgRect(vg, x0, y0, m->w * k, m->h * k);
	nvgFillPaint(vg, nvgImagePattern(vg, x0, y0, m->w * k, m->h * k, 0.f, img, 1.f));
	nvgFill(vg);
	return adv;
}

static void drawText(NVGcontext* vg, float x, float y, const std::string& s, float size, NVGcolor col, int align,
		int font = F_LABEL) {
	if (font == F_TITLE && (align & NVG_ALIGN_MIDDLE)
		&& drawGlyphText(vg, x, y, s, size, align, size < 9.5f ? 0 : 1, col, col) >= 0.f)
		return;
	std::shared_ptr<window::Font> f = getFont(font);
	if (!f)
		return;
	nvgFontFaceId(vg, f->handle);
	nvgFontSize(vg, size);
	nvgFillColor(vg, col);
	nvgTextAlign(vg, align);
	nvgText(vg, x, y, s.c_str(), NULL);
}

static std::string pickWav(const std::string& dir) {
	osdialog_filters* filters = osdialog_filters_parse("WAV audio:wav,WAV");
	char* p = osdialog_file(OSDIALOG_OPEN, dir.empty() ? NULL : dir.c_str(), NULL, filters);
	osdialog_filters_free(filters);
	std::string s = p ? p : "";
	free(p);
	return s;
}

// Old, weathered boards: faded and stained wood, deep grain, knots, cracks and scratches.
struct PanelDecor : TransparentWidget {
	static void soft(NVGcontext* vg, float x, float y, float r, NVGcolor c) {
		nvgBeginPath(vg);
		nvgCircle(vg, x, y, r);
		nvgFillPaint(vg, nvgRadialGradient(vg, x, y, r * 0.15f, r, c, nvgRGBAf(c.r, c.g, c.b, 0.f)));
		nvgFill(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;

		// base: dry, greying brown
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, w, h);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0x2c, 0x22, 0x18), nvgRGB(0x17, 0x10, 0x0b)));
		nvgFill(vg);

		// five planks, each with its own tint and age
		const int planks = 5;
		float pw = w / planks;
		for (int p = 0; p < planks; p++) {
			float x0 = p * pw;
			float tone = hash1(p * 3.17f + 0.5f);
			nvgBeginPath(vg);
			nvgRect(vg, x0, 0.f, pw, h);
			nvgFillColor(vg, tone > 0.5f ? nvgRGBAf(0.55f, 0.42f, 0.28f, 0.08f + 0.08f * tone) : nvgRGBAf(0.f, 0.f, 0.f, 0.10f + 0.08f * tone));
			nvgFill(vg);

			// long wavy grain lines
			int lines = 70;
			for (int i = 0; i < lines; i++) {
				float bx = x0 + hash1(p * 91.f + i * 1.9f) * pw;
				float amp = 1.5f + 6.f * hash1(p * 17.f + i * 4.3f);
				float freq = 0.004f + 0.012f * hash1(p * 29.f + i * 2.1f);
				float ph = hash1(p * 7.f + i * 8.7f) * 6.28f;
				nvgBeginPath(vg);
				for (float y = 0.f; y <= h; y += 14.f) {
					float x = bx + amp * std::sin(y * freq + ph) + 3.f * std::sin(y * 0.003f + p);
					if (y == 0.f)
						nvgMoveTo(vg, x, y);
					else
						nvgLineTo(vg, x, y);
				}
				float k = hash1(p * 5.f + i * 6.1f);
				nvgStrokeWidth(vg, 0.5f + 1.6f * hash1(p * 3.f + i * 9.7f));
				nvgStrokeColor(vg, k > 0.38f ? nvgRGBAf(0.f, 0.f, 0.f, 0.10f + 0.16f * k) : nvgRGBAf(0.8f, 0.62f, 0.4f, 0.04f + 0.07f * k));
				nvgStroke(vg);
			}
		}

		// faded, dry patches and dark water stains
		for (int i = 0; i < 12; i++)
			soft(vg, hash1(i * 4.1f) * w, hash1(i * 6.3f) * h, 60.f + 140.f * hash1(i * 2.9f), nvgRGBAf(0.62f, 0.55f, 0.46f, 0.10f));
		for (int i = 0; i < 16; i++)
			soft(vg, hash1(i * 8.3f + 1.f) * w, hash1(i * 5.1f + 2.f) * h, 40.f + 120.f * hash1(i * 3.3f + 1.f), nvgRGBAf(0.f, 0.f, 0.f, 0.20f));
		// tide marks: pale rings around a few stains
		for (int i = 0; i < 5; i++) {
			float x = hash1(i * 12.7f + 3.f) * w, y = hash1(i * 7.9f + 4.f) * h, r = 30.f + 60.f * hash1(i * 5.5f);
			nvgBeginPath(vg);
			nvgEllipse(vg, x, y, r, r * 0.7f);
			nvgStrokeWidth(vg, 1.4f);
			nvgStrokeColor(vg, nvgRGBAf(0.75f, 0.66f, 0.5f, 0.10f));
			nvgStroke(vg);
		}

		// knots
		for (int i = 0; i < 7; i++) {
			float x = hash1(i * 15.7f + 9.f) * w, y = hash1(i * 9.3f + 7.f) * h;
			float r = 5.f + 7.f * hash1(i * 3.1f + 2.f);
			for (int ring = 5; ring >= 1; ring--) {
				nvgBeginPath(vg);
				nvgEllipse(vg, x, y, r * ring / 3.f, r * ring / 3.f * 1.7f);
				nvgFillColor(vg, nvgRGBAf(0.05f, 0.03f, 0.01f, 0.10f + 0.04f * (6 - ring)));
				nvgFill(vg);
				nvgStrokeWidth(vg, 0.7f);
				nvgStrokeColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.22f));
				nvgStroke(vg);
			}
		}

		// plank seams: dark gaps, uneven edges, lit lips
		for (int k = 1; k < planks; k++) {
			float x = std::floor(pw * k);
			nvgBeginPath(vg);
			for (float y = 0.f; y <= h + 6.f; y += 6.f) {
				float j = 1.2f * (hash1(k * 31.f + y * 0.37f) - 0.5f);
				if (y == 0.f)
					nvgMoveTo(vg, x + j, y);
				else
					nvgLineTo(vg, x + j, y);
			}
			nvgStrokeWidth(vg, 3.f);
			nvgStrokeColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.85f));
			nvgStroke(vg);
			nvgBeginPath(vg);
			nvgRect(vg, x + 2.f, 0.f, 1.f, h);
			nvgFillColor(vg, nvgRGBAf(0.9f, 0.75f, 0.55f, 0.10f));
			nvgFill(vg);
		}

		// cracks that run with the grain, with a few splits
		for (int i = 0; i < 9; i++) {
			float x = hash1(i * 21.1f + 5.f) * w, y = hash1(i * 13.7f + 1.f) * h;
			float len = 50.f + 150.f * hash1(i * 4.9f + 3.f);
			nvgBeginPath(vg);
			nvgMoveTo(vg, x, y);
			float cx = x, cy = y;
			for (float s = 0.f; s < len; s += 7.f) {
				cx += 2.f * (hash1(i * 100.f + s) - 0.5f);
				cy += 7.f;
				nvgLineTo(vg, cx, cy);
				if (hash1(i * 55.f + s * 1.7f) > 0.93f) {
					nvgMoveTo(vg, cx, cy);
					nvgLineTo(vg, cx + 6.f * (hash1(i + s) - 0.5f) * 3.f, cy + 5.f);
					nvgMoveTo(vg, cx, cy);
				}
			}
			nvgStrokeWidth(vg, 1.3f);
			nvgStrokeColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.75f));
			nvgStroke(vg);
			nvgStrokeWidth(vg, 0.5f);
			nvgStrokeColor(vg, nvgRGBAf(0.9f, 0.75f, 0.55f, 0.12f));
			nvgStroke(vg);
		}

		// scratches and gouges
		nvgBeginPath(vg);
		for (int i = 0; i < 160; i++) {
			float x = hash1(i * 2.7f + 11.f) * w, y = hash1(i * 6.1f + 12.f) * h;
			float a = (hash1(i * 4.4f + 13.f) - 0.5f) * 1.2f + 1.57f * (hash1(i * 9.9f) > 0.7f ? 0.f : 1.f);
			float l = 8.f + 40.f * hash1(i * 8.8f + 14.f);
			nvgMoveTo(vg, x, y);
			nvgLineTo(vg, x + std::cos(a) * l, y + std::sin(a) * l);
		}
		nvgStrokeWidth(vg, 0.6f);
		nvgStrokeColor(vg, nvgRGBAf(0.85f, 0.72f, 0.55f, 0.10f));
		nvgStroke(vg);

		// worm holes and pits
		nvgBeginPath(vg);
		for (int i = 0; i < 260; i++)
			nvgCircle(vg, hash1(i * 5.3f + 21.f) * w, hash1(i * 3.9f + 22.f) * h, 0.5f + 0.9f * hash1(i * 7.7f));
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
		nvgFill(vg);

		// worn, lighter edges where hands and time have rubbed the boards
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, w, h);
		nvgFillPaint(vg, nvgBoxGradient(vg, 6.f, 6.f, w - 12.f, h - 12.f, 30.f, 24.f, nvgRGBAf(0.f, 0.f, 0.f, 0.f), nvgRGBAf(0.7f, 0.6f, 0.45f, 0.10f)));
		nvgFill(vg);
	}
};


// Header: carved gold logo, rune row, braid and the TJB AUDIO mark.
struct HeaderDecor : TransparentWidget {
	HeaderDecor(float widthMm) {
		box.pos = Vec(0.f, 0.f);
		box.size = Vec(mm2px(widthMm), mm2px(10.f));
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		float x = mm2px(5.f), cy = h * 0.55f;
		const int L = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
		const float gs = 19.f;
		float gw = drawGlyphText(vg, x + 1.5f, cy + 2.f, "GROND", gs, L, 2, nvgRGBAf(0.f, 0.f, 0.f, 0.8f), nvgRGBAf(0.f, 0.f, 0.f, 0.8f), 10);
		float xr;
		if (gw >= 0.f) {
			drawGlyphText(vg, x + 0.8f, cy + 1.1f, "GROND", gs, L, 2, nvgRGB(0x3a, 0x22, 0x08), nvgRGB(0x3a, 0x22, 0x08));
			drawGlyphText(vg, x, cy, "GROND", gs, L, 2, nvgRGB(0xff, 0xe6, 0x9a), nvgRGB(0xa8, 0x72, 0x1c));
			xr = x + gw + mm2px(4.f);
		}
		else {
			// no text image available: fall back to Rack's bold UI font
			std::shared_ptr<window::Font> f = getFont(F_TITLE);
			if (!f)
				return;
			nvgFontFaceId(vg, f->handle);
			nvgTextAlign(vg, L);
			nvgFontSize(vg, gs);
			nvgFillColor(vg, nvgRGB(0xe0, 0xb8, 0x60));
			nvgText(vg, x, cy, "GROND", NULL);
			float b[4];
			nvgTextBounds(vg, 0.f, 0.f, "GROND", NULL, b);
			xr = x + (b[2] - b[0]) + mm2px(4.f);
		}

		float xt = xr;
		drawText(vg, xt, cy + 0.5f, "IMPACT FORGE", 11.f, alpha(C_BONE, 0.75f), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, F_TITLE);

		// TJB AUDIO + ember lamp
		float lx = w - mm2px(5.f);
		nvgBeginPath(vg);
		nvgCircle(vg, lx, cy, 7.f);
		nvgFillPaint(vg, nvgRadialGradient(vg, lx, cy, 0.f, 7.f, nvgRGBAf(1.f, 0.5f, 0.1f, 0.5f), nvgRGBAf(1.f, 0.5f, 0.1f, 0.f)));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, lx, cy, 3.f);
		nvgFillPaint(vg, nvgRadialGradient(vg, lx - 0.8f, cy - 0.8f, 0.2f, 3.2f, nvgRGB(0xff, 0xf0, 0xb0), nvgRGB(0xd8, 0x50, 0x10)));
		nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x4a, 0x1a, 0x08));
		nvgStrokeWidth(vg, 0.8f);
		nvgStroke(vg);
		drawText(vg, lx - 11.f, cy + 1.f, "TJB AUDIO", 13.f, nvgRGBAf(0.f, 0.f, 0.f, 0.7f), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, F_TITLE);
		drawText(vg, lx - 11.f, cy, "TJB AUDIO", 13.f, C_GOLD, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, F_TITLE);
	}
};


struct PanelLabel : TransparentWidget {
	std::string text;
	float size;
	NVGcolor color;
	int align;

	PanelLabel(Vec posMm, const std::string& t, float s = 6.3f, NVGcolor c = C_BONE,
			int a = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE) {
		text = t;
		size = s;
		color = c;
		align = a;
		box.size = Vec(mm2px(40.f), 12.f);
		float ox = (a & NVG_ALIGN_LEFT) ? 0.f : ((a & NVG_ALIGN_RIGHT) ? box.size.x : box.size.x / 2.f);
		box.pos = mm2px(posMm).minus(Vec(ox, box.size.y / 2.f));
	}

	void draw(const DrawArgs& args) override {
		float x = (align & NVG_ALIGN_LEFT) ? 0.f : ((align & NVG_ALIGN_RIGHT) ? box.size.x : box.size.x / 2.f);
		float y = box.size.y / 2.f;
		drawText(args.vg, x, y + 0.9f, text, size * 1.4f, nvgRGBAf(0.f, 0.f, 0.f, 0.65f), align, F_TITLE);   // carved shadow
		drawText(args.vg, x, y, text, size * 1.4f, color, align, F_TITLE);
	}
};


// Iron plate with riveted corners and a bronze-and-dye title bar edged with wolf teeth.
struct GroupBox : TransparentWidget {
	std::string title;
	NVGcolor accent;

	GroupBox(Vec posMm, Vec sizeMm, const std::string& t, NVGcolor c) {
		box.pos = mm2px(posMm);
		box.size = mm2px(sizeMm);
		title = t;
		accent = c;
	}

	static void rivet(NVGcontext* vg, float x, float y) {
		nvgBeginPath(vg);
		nvgCircle(vg, x, y, 1.9f);
		nvgFillPaint(vg, nvgRadialGradient(vg, x - 0.5f, y - 0.6f, 0.2f, 2.2f, nvgRGB(0xe8, 0xd0, 0x90), nvgRGB(0x5a, 0x3f, 0x14)));
		nvgFill(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y, r = 3.f, hdr = mm2px(5.4f);

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 1.f, 2.5f, w - 1.f, h - 1.f, r);
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
		nvgFill(vg);

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.5f, 0.5f, w - 1.f, h - 1.f, r);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0x3d, 0x41, 0x44), nvgRGB(0x1f, 0x22, 0x25)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 1.4f);
		nvgStrokeColor(vg, C_BRONZE);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 2.2f, 2.2f, w - 4.4f, h - 4.4f, r - 1.f);
		nvgStrokeWidth(vg, 0.6f);
		nvgStrokeColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.6f));
		nvgStroke(vg);

		// title bar
		nvgBeginPath(vg);
		nvgRoundedRectVarying(vg, 0.5f, 0.5f, w - 1.f, hdr, r, r, 0.f, 0.f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, hdr, mixColor(accent, C_BONE, 0.22f), mixColor(accent, nvgRGB(0, 0, 0), 0.45f)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 1.f);
		nvgStrokeColor(vg, mixColor(accent, nvgRGB(0, 0, 0), 0.6f));
		nvgStroke(vg);

		// wolf teeth along the bar's lower edge
		nvgBeginPath(vg);
		for (float x = 6.f; x + 6.f < w - 4.f; x += 6.f) {
			nvgMoveTo(vg, x, hdr);
			nvgLineTo(vg, x + 3.f, hdr + 3.2f);
			nvgLineTo(vg, x + 6.f, hdr);
		}
		nvgFillColor(vg, mixColor(accent, nvgRGB(0, 0, 0), 0.5f));
		nvgFill(vg);

		drawText(vg, 9.f, hdr / 2.f + 1.f, title, 10.5f, nvgRGBAf(0.f, 0.f, 0.f, 0.6f), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, F_TITLE);
		drawText(vg, 9.f, hdr / 2.f, title, 10.5f, C_BONE, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, F_TITLE);

		rivet(vg, 5.f, h - 5.f);
		rivet(vg, w - 5.f, h - 5.f);
	}
};


// Forged knob: iron boss in a bronze rim, ember value ring.
struct TermKnob : app::Knob {
	TermKnob(float diameterMm = 8.5f) {
		box.size = mm2px(Vec(diameterMm, diameterMm));
		minAngle = -0.8f * iw::PI;
		maxAngle = 0.8f * iw::PI;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = box.size.x / 2.f - 1.f;
		float rb = r - 3.2f;
		ParamQuantity* pq = getParamQuantity();
		float t = pq ? pq->getScaledValue() : 0.5f;
		float t0 = 0.f;
		if (pq && pq->getMinValue() < 0.f && pq->getMaxValue() > 0.f)
			t0 = (0.f - pq->getMinValue()) / (pq->getMaxValue() - pq->getMinValue());
		float aMin = minAngle - iw::PI / 2.f, aMax = maxAngle - iw::PI / 2.f;
		float aT = aMin + (aMax - aMin) * t;
		float aZ = aMin + (aMax - aMin) * t0;

		// carved groove (the lit value arc and pointer are drawn on the light layer)
		nvgBeginPath(vg);
		nvgArc(vg, cx, cy, r - 0.8f, aMin, aMax, NVG_CW);
		nvgStrokeWidth(vg, 2.6f);
		nvgStrokeColor(vg, nvgRGB(0x10, 0x09, 0x04));
		nvgStroke(vg);

		// bronze rim and iron boss
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy + 0.8f, rb + 1.4f);
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, rb + 1.1f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, cy - rb, 0.f, cy + rb, nvgRGB(0xe0, 0xb8, 0x68), nvgRGB(0x5c, 0x3e, 0x12)));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, rb);
		nvgFillPaint(vg, nvgRadialGradient(vg, cx - rb * 0.3f, cy - rb * 0.4f, rb * 0.1f, rb * 1.3f, nvgRGB(0x82, 0x86, 0x8a), nvgRGB(0x1c, 0x1e, 0x20)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 0.7f);
		nvgStrokeColor(vg, nvgRGB(0x0c, 0x0d, 0x0e));
		nvgStroke(vg);


		nvgBeginPath(vg);
		nvgEllipse(vg, cx - rb * 0.1f, cy - rb * 0.5f, rb * 0.55f, rb * 0.26f);
		nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.12f));
		nvgFill(vg);
	}
	// Value arc and pointer glow in the dark: drawn on the light layer, above Rack's room-brightness dimming.
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		NVGcontext* vg = args.vg;
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = box.size.x / 2.f - 1.f;
		float rb = r - 3.2f;
		ParamQuantity* pq = getParamQuantity();
		float t = pq ? pq->getScaledValue() : 0.5f;
		float t0 = 0.f;
		if (pq && pq->getMinValue() < 0.f && pq->getMaxValue() > 0.f)
			t0 = (0.f - pq->getMinValue()) / (pq->getMaxValue() - pq->getMinValue());
		float aMin = minAngle - iw::PI / 2.f, aMax = maxAngle - iw::PI / 2.f;
		float aT = aMin + (aMax - aMin) * t;
		float aZ = aMin + (aMax - aMin) * t0;

		if (std::fabs(aT - aZ) > 0.01f) {
			nvgBeginPath(vg);
			if (aT >= aZ)
				nvgArc(vg, cx, cy, r - 0.8f, aZ, aT, NVG_CW);
			else
				nvgArc(vg, cx, cy, r - 0.8f, aT, aZ, NVG_CW);
			nvgStrokeWidth(vg, 5.f);
			nvgStrokeColor(vg, nvgRGBAf(1.f, 0.55f, 0.1f, 0.22f));
			nvgStroke(vg);
			nvgStrokeWidth(vg, 2.2f);
			nvgStrokeColor(vg, C_EMBER);
			nvgStroke(vg);
		}

		if (pq && pq->snapEnabled) {
			std::string s = pq->getDisplayValueString();
			if (s.size() > 3)
				s.resize(3);
			for (size_t i = 0; i < s.size(); i++)
				s[i] = (char) std::toupper((unsigned char) s[i]);
			drawText(vg, cx, cy + 0.5f, s, 8.f, C_GOLD, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		}
		else {
			nvgBeginPath(vg);
			nvgMoveTo(vg, cx + std::cos(aT) * rb * 0.2f, cy + std::sin(aT) * rb * 0.2f);
			nvgLineTo(vg, cx + std::cos(aT) * (rb - 1.2f), cy + std::sin(aT) * (rb - 1.2f));
			nvgStrokeWidth(vg, 2.f);
			nvgLineCap(vg, NVG_ROUND);
			nvgStrokeColor(vg, C_BONE);
			nvgStroke(vg);
		}
	}
};


// Leather-and-bronze plaque switch / button.
struct TermSwitch : app::Switch {
	std::vector<std::string> shorts;

	TermSwitch() {
		shorts.push_back("?");
		box.size = mm2px(Vec(11.f, 4.8f));
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		ParamQuantity* pq = getParamQuantity();
		int idx = pq ? (int) std::round(pq->getValue() - pq->getMinValue()) : 0;
		idx = std::max(0, std::min(idx, (int) shorts.size() - 1));
		bool lit = momentary && pq && pq->getValue() > 0.5f;

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.5f, 1.6f, w - 1.f, h - 1.f, 2.f);
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
		nvgFill(vg);

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.5f, 0.5f, w - 1.f, h - 1.f, 2.f);
		if (lit)
			nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0xff, 0xc8, 0x70), nvgRGB(0xc4, 0x52, 0x14)));
		else
			nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0x6a, 0x47, 0x28), nvgRGB(0x2c, 0x1b, 0x0d)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 1.1f);
		nvgStrokeColor(vg, C_BRONZE);
		nvgStroke(vg);

		nvgBeginPath(vg);
		nvgRect(vg, 2.f, 1.5f, w - 4.f, 1.f);
		nvgFillColor(vg, nvgRGBAf(1.f, 0.85f, 0.6f, lit ? 0.5f : 0.18f));
		nvgFill(vg);

		drawText(vg, w / 2.f, h / 2.f + 0.5f, shorts[idx], 6.8f, lit ? nvgRGB(0x2a, 0x10, 0x04) : C_GOLD,
			NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	}
};


// Bronze-ringed socket. Frost-blue glow for inputs, ember for outputs.
struct TermPort : app::PortWidget {
	TermPort() {
		box.size = mm2px(Vec(7.f, 7.f));
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = box.size.x / 2.f - 0.8f;
		bool out = type == engine::Port::OUTPUT;
		engine::Port* p = module ? getPort() : NULL;
		bool connected = p && p->isConnected();
		NVGcolor led = out ? C_EMBER : C_FROST;

		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy + 0.9f, r + 0.4f);
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.5f));
		nvgFill(vg);

		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, r);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, cy - r, 0.f, cy + r, nvgRGB(0xe8, 0xc4, 0x78), nvgRGB(0x5a, 0x3c, 0x10)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 0.8f);
		nvgStrokeColor(vg, nvgRGB(0x2a, 0x1a, 0x06));
		nvgStroke(vg);

		nvgBeginPath(vg);
		nvgCircle(vg, cx, cy, r * 0.62f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, cy - r, 0.f, cy + r, nvgRGB(0x03, 0x03, 0x04), nvgRGB(0x26, 0x22, 0x1e)));
		nvgFill(vg);

		if (connected) {
			nvgBeginPath(vg);
			nvgCircle(vg, cx, cy, r * 0.62f);
			nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, 0.f, r * 0.62f, alpha(led, 0.95f), alpha(led, 0.2f)));
			nvgFill(vg);
		}
	}
};

struct TermPortSmall : TermPort {
	TermPortSmall() {
		box.size = mm2px(Vec(5.4f, 5.4f));
	}
};


// Runestone sample slot: dark slate in a bronze frame, with waveform and play heads.
struct SlotDisplay : TransparentWidget {
	Grond* module = NULL;
	int index = 0;

	SlotDisplay(Vec topLeftMm, Vec sizeMm, Grond* m, int i) {
		module = m;
		index = i;
		box.pos = mm2px(topLeftMm);
		box.size = mm2px(sizeMm);
	}

	// The bezel is part of the dimmed panel; the screen itself is drawn on the light layer so it stays lit
	// when Rack's room brightness is turned down.
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.f, 0.f, w, h, 2.f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0xd2, 0xa8, 0x5c), nvgRGB(0x4e, 0x34, 0x10)));
		nvgFill(vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		bool whoosh = slotType(index) == SLOT_WHOOSH;
		NVGcolor col = whoosh ? C_FROST : nvgRGB(0xee, 0x72, 0x34);

		float in = 1.2f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, in, in, w - 2.f * in, h - 2.f * in, 1.4f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, in, 0.f, h - in, nvgRGB(0x0b, 0x0e, 0x10), nvgRGB(0x1c, 0x21, 0x24)));
		nvgFill(vg);

		std::shared_ptr<iw::Sample> s;
		float flash = 0.f;
		if (module) {
			s = std::atomic_load(&module->slots[index].sample);
			flash = module->slots[index].flash;
		}

		nvgScissor(vg, in, in, w - 2.f * in, h - 2.f * in);
		if (s && !s->peaks.empty()) {
			int cols = (int) s->peaks.size();
			nvgBeginPath(vg);
			for (int c = 0; c < cols; c++) {
				float x = w * (c + 0.5f) / cols;
				float hh = std::max(0.5f, s->peaks[c] * h * 0.42f);
				nvgMoveTo(vg, x, h * 0.5f - hh);
				nvgLineTo(vg, x, h * 0.5f + hh);
			}
			nvgStrokeWidth(vg, std::max(1.f, w / cols * 0.6f));
			nvgStrokeColor(vg, alpha(col, 0.5f));
			nvgStroke(vg);
		}

		if (flash > 0.01f) {
			nvgBeginPath(vg);
			nvgRect(vg, 0.f, 0.f, w, h);
			nvgFillColor(vg, alpha(col, 0.32f * flash));
			nvgFill(vg);
		}

		// play heads: one per voice currently sounding from this slot
		if (module) {
			for (int v = 0; v < NUM_VOICES; v++) {
				const Grond::Voice& vo = module->voices[v];
				if (!vo.active || !vo.started || vo.slot != index)
					continue;
				float x = iw::clampf(vo.head, 0.f, 1.f) * (w - 2.f * in) + in;
				nvgBeginPath(vg);
				nvgMoveTo(vg, x, in);
				nvgLineTo(vg, x, h - in);
				nvgStrokeWidth(vg, 4.f);
				nvgStrokeColor(vg, nvgRGBAf(1.f, 0.85f, 0.4f, 0.25f));
				nvgStroke(vg);
				nvgStrokeWidth(vg, 1.4f);
				nvgStrokeColor(vg, nvgRGB(0xff, 0xf2, 0xc0));
				nvgStroke(vg);
			}
		}

		std::string id = string::f("%02d ", (index % SLOTS_PER_TYPE) + 1);
		if (s)
			drawText(vg, 5.f, h / 2.f, id + s->name, 7.2f, C_BONE, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		else
			drawText(vg, 5.f, h / 2.f, id + "- empty -", 7.2f, nvgRGB(0x6a, 0x62, 0x50), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgResetScissor(vg);

		// type mark: a small rune-coloured gem at the left edge
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 2.f, 1.2f, h - 4.f);
		nvgFillColor(vg, col);
		nvgFill(vg);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		Grond* m = module;
		int i = index;

		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("%s slot %d", slotType(i) == SLOT_WHOOSH ? "Whoosh" : "Impact",
			(i % SLOTS_PER_TYPE) + 1)));
		menu->addChild(createMenuItem("Load WAV...", "", [=]() {
			std::string path = pickWav(m->lastDir);
			if (!path.empty())
				m->loadSlot(i, path);
		}));
		menu->addChild(createMenuItem("Clear", "", [=]() { m->clearSlot(i); }));
		e.consume(this);
	}
};


// A war-hammer that strikes an enchanted anvil whenever any trigger fires.
struct ForgeDisplay : TransparentWidget {
	Grond* module = NULL;

	ForgeDisplay(Vec topLeftMm, Vec sizeMm, Grond* m) {
		module = m;
		box.pos = mm2px(topLeftMm);
		box.size = mm2px(sizeMm);
	}

	static float smooth(float x) {
		x = iw::clampf(x, 0.f, 1.f);
		return x * x * (3.f - 2.f * x);
	}

	static void star(NVGcontext* vg, float x, float y, float s, NVGcolor c) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, x - s, y); nvgLineTo(vg, x + s, y);
		nvgMoveTo(vg, x, y - s); nvgLineTo(vg, x, y + s);
		nvgStrokeWidth(vg, 0.9f);
		nvgStrokeColor(vg, c);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgCircle(vg, x, y, s * 0.35f);
		nvgFillColor(vg, c);
		nvgFill(vg);
	}

	// London-pattern anvil: long horn on the left, flat face, stepped heel on the right,
	// curved waist and a flared foot. Coordinates are fractions of the widget box.
	static void anvilPath(NVGcontext* vg, float W, float H) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, 0.00f * W, 0.50f * H);                                              // horn tip
		nvgBezierTo(vg, 0.10f * W, 0.46f * H, 0.20f * W, 0.42f * H, 0.28f * W, 0.42f * H);  // horn top curve
		nvgLineTo(vg, 0.80f * W, 0.42f * H);                                              // face
		nvgLineTo(vg, 0.88f * W, 0.42f * H);                                              // heel top
		nvgLineTo(vg, 0.88f * W, 0.52f * H);
		nvgLineTo(vg, 0.70f * W, 0.52f * H);                                              // under the heel
		nvgBezierTo(vg, 0.66f * W, 0.55f * H, 0.62f * W, 0.58f * H, 0.62f * W, 0.66f * H);  // waist, right
		nvgBezierTo(vg, 0.62f * W, 0.74f * H, 0.70f * W, 0.78f * H, 0.78f * W, 0.84f * H);  // foot flare, right
		nvgLineTo(vg, 0.78f * W, 1.00f * H);
		nvgLineTo(vg, 0.22f * W, 1.00f * H);
		nvgLineTo(vg, 0.22f * W, 0.84f * H);
		nvgBezierTo(vg, 0.30f * W, 0.78f * H, 0.38f * W, 0.74f * H, 0.38f * W, 0.66f * H);  // foot flare, left
		nvgBezierTo(vg, 0.38f * W, 0.58f * H, 0.34f * W, 0.55f * H, 0.28f * W, 0.54f * H);  // waist, left
		nvgBezierTo(vg, 0.18f * W, 0.54f * H, 0.08f * W, 0.53f * H, 0.00f * W, 0.50f * H);  // horn underside
		nvgClosePath(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float W = box.size.x, H = box.size.y;
		float u = W / 35.f;   // drawing unit
		double now = system::getTime();
		float t = 10.f;
		if (module) {
			double last = std::max(module->lastFire[0], std::max(module->lastFire[1], module->lastFire[2]));
			t = (float) (now - last);
		}
		float yFace = 0.42f * H;
		float ix = 0.44f * W, iy = yFace;   // where the hammer lands

		// forge glow beneath the anvil, breathing slowly
		float breathe = 0.5f + 0.5f * std::sin((float) now * 2.f);
		float glow = 0.16f + 0.08f * breathe + (t >= 0.f && t < 0.5f ? 0.5f * (1.f - t / 0.5f) : 0.f);
		nvgBeginPath(vg);
		nvgEllipse(vg, W * 0.5f, H * 1.0f, W * 0.62f, H * 0.38f);
		nvgFillPaint(vg, nvgRadialGradient(vg, W * 0.5f, H * 1.0f, 1.f, W * 0.6f, nvgRGBAf(1.f, 0.5f, 0.1f, glow), nvgRGBAf(1.f, 0.4f, 0.05f, 0.f)));
		nvgFill(vg);

		// hammer angle (handle direction from +x, y down)
		const float rest = 204.f * iw::PI / 180.f, hit = 168.f * iw::PI / 180.f, bounce = 190.f * iw::PI / 180.f;
		float ang = rest;
		if (t >= 0.f && t < 0.07f)
			ang = rest + (hit - rest) * (t / 0.07f) * (t / 0.07f);
		else if (t < 0.20f)
			ang = hit + (bounce - hit) * smooth((t - 0.07f) / 0.13f);
		else if (t < 0.75f)
			ang = bounce + (rest - bounce) * smooth((t - 0.20f) / 0.55f);

		// anvil body: dark steel, lit from above
		anvilPath(vg, W, H);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.42f * H, 0.f, H, nvgRGB(0x8e, 0x93, 0x99), nvgRGB(0x1a, 0x1c, 0x1f)));
		nvgFill(vg);
		// subtle vertical shading so the waist reads as round
		nvgSave(vg);
		anvilPath(vg, W, H);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.25f * W, 0.f, 0.80f * W, 0.f, nvgRGBAf(0.f, 0.f, 0.f, 0.35f), nvgRGBAf(0.f, 0.f, 0.f, 0.f)));
		nvgFill(vg);
		nvgRestore(vg);
		anvilPath(vg, W, H);
		nvgStrokeWidth(vg, 0.9f);
		nvgStrokeColor(vg, nvgRGB(0x07, 0x07, 0x08));
		nvgStroke(vg);

		// hardy hole on the heel
		nvgBeginPath(vg);
		nvgRect(vg, 0.72f * W, yFace, 0.07f * W, 0.04f * H);
		nvgFillColor(vg, nvgRGB(0x05, 0x05, 0x06));
		nvgFill(vg);

		// worked steel face with an enchanted frost-blue edge that flares on impact
		float flare = (t >= 0.07f && t < 0.6f) ? 1.f - (t - 0.07f) / 0.53f : 0.f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, 0.28f * W, yFace + 0.4f);
		nvgLineTo(vg, 0.86f * W, yFace + 0.4f);
		nvgStrokeWidth(vg, 0.9f + 1.4f * flare);
		nvgStrokeColor(vg, nvgRGBAf(0.55f, 0.83f, 0.95f, 0.30f + 0.65f * flare));
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgMoveTo(vg, 0.02f * W, 0.495f * H);
		nvgBezierTo(vg, 0.10f * W, 0.455f * H, 0.20f * W, 0.43f * H, 0.28f * W, 0.43f * H);
		nvgStrokeWidth(vg, 0.7f);
		nvgStrokeColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.35f));
		nvgStroke(vg);

		// war-hammer: leather-wrapped haft, bronze bands, iron head. Its striking face meets the anvil at the hit angle.
		float len = 15.f * u, headHalf = 5.f * u;
		float cosH = std::cos(hit), sinH = std::sin(hit);
		float px = ix - len * cosH;
		float py = (yFace - headHalf) - len * sinH;
		nvgSave(vg);
		nvgTranslate(vg, px, py);
		nvgRotate(vg, ang);
		nvgBeginPath(vg);
		nvgMoveTo(vg, -3.f * u, 0.f);
		nvgLineTo(vg, len, 0.f);
		nvgStrokeWidth(vg, 3.4f * u);
		nvgLineCap(vg, NVG_ROUND);
		nvgStrokeColor(vg, nvgRGB(0x1a, 0x10, 0x06));
		nvgStroke(vg);
		nvgStrokeWidth(vg, 2.2f * u);
		nvgStrokeColor(vg, nvgRGB(0x7a, 0x52, 0x2a));
		nvgStroke(vg);
		nvgBeginPath(vg);
		for (int i = 0; i < 3; i++) {
			float bx = (1.f + i * 3.f) * u;
			nvgMoveTo(vg, bx, -1.2f * u);
			nvgLineTo(vg, bx + 1.f * u, 1.2f * u);
		}
		nvgStrokeWidth(vg, 0.8f * u);
		nvgStrokeColor(vg, nvgRGB(0x2c, 0x1a, 0x0a));
		nvgStroke(vg);
		float heat = (t >= 0.07f && t < 0.45f) ? 1.f - (t - 0.07f) / 0.38f : 0.f;
		nvgBeginPath(vg);
		nvgRect(vg, len - 2.4f * u, -headHalf, 5.f * u, 2.f * headHalf);
		nvgFillPaint(vg, nvgLinearGradient(vg, len - 2.4f * u, 0.f, len + 2.6f * u, 0.f, nvgRGB(0xc8, 0xcc, 0xd0), nvgRGB(0x3a, 0x3d, 0x40)));
		nvgFill(vg);
		nvgStrokeWidth(vg, 0.9f);
		nvgStrokeColor(vg, mixColor(nvgRGB(0x0a, 0x0a, 0x0b), nvgRGB(0xff, 0x9a, 0x3c), heat));
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgRect(vg, len - 2.4f * u, -headHalf, 1.2f * u, 2.f * headHalf);
		nvgRect(vg, len + 1.4f * u, -headHalf, 1.2f * u, 2.f * headHalf);
		nvgFillColor(vg, nvgRGB(0xb0, 0x84, 0x3c));
		nvgFill(vg);
		nvgRestore(vg);

		if (t < 0.07f || t > 1.4f)
			return;

		// arcane shock rings
		for (int r = 0; r < 2; r++) {
			float tr = t - 0.07f - r * 0.06f;
			if (tr < 0.f || tr > 0.4f)
				continue;
			float k = tr / 0.4f;
			nvgBeginPath(vg);
			nvgEllipse(vg, ix, iy, (2.f + 14.f * k) * u, (2.f + 14.f * k) * 0.45f * u);
			nvgStrokeWidth(vg, 1.3f - 0.7f * k);
			nvgStrokeColor(vg, r ? nvgRGBAf(1.f, 0.8f, 0.35f, 0.8f * (1.f - k)) : nvgRGBAf(0.55f, 0.83f, 0.95f, 0.9f * (1.f - k)));
			nvgStroke(vg);
		}

		// impact flash
		if (t < 0.18f) {
			float k = 1.f - (t - 0.07f) / 0.11f;
			float fr = (6.f * k + 2.f) * u;
			nvgBeginPath(vg);
			nvgCircle(vg, ix, iy, fr);
			nvgFillPaint(vg, nvgRadialGradient(vg, ix, iy, 0.f, fr, nvgRGBAf(1.f, 0.93f, 0.6f, 0.9f * k), nvgRGBAf(1.f, 0.5f, 0.1f, 0.f)));
			nvgFill(vg);
		}

		float ts = t - 0.07f;
		// star-shaped sparks, alternating gold and frost blue
		for (int i = 0; i < 10; i++) {
			float a = (-165.f + 150.f * hash1(i * 3.7f + 1.f)) * iw::PI / 180.f;
			float v = (46.f + 70.f * hash1(i * 9.1f + 2.f)) * u;
			float life = 0.3f + 0.35f * hash1(i * 5.3f + 3.f);
			if (ts > life)
				continue;
			float sx = ix + std::cos(a) * v * ts;
			float sy = iy + std::sin(a) * v * ts + 0.5f * 210.f * u * ts * ts;
			float k = 1.f - ts / life;
			NVGcolor c = (i % 2) ? nvgRGBAf(1.f, 0.82f, 0.38f, k) : nvgRGBAf(0.6f, 0.86f, 1.f, k);
			star(vg, sx, sy, (0.8f + 1.2f * k) * u, c);
		}
		// slow embers drifting upward like fireflies
		for (int i = 0; i < 6; i++) {
			float life = 0.8f + 0.5f * hash1(i * 7.1f + 5.f);
			if (ts > life)
				continue;
			float k = 1.f - ts / life;
			float ex = ix + (hash1(i * 3.3f + 6.f) - 0.5f) * 12.f * u + 3.f * u * std::sin(ts * 7.f + i);
			float ey = iy - 3.f * u - 14.f * u * ts * (0.6f + hash1(i * 2.2f + 7.f));
			nvgBeginPath(vg);
			nvgCircle(vg, ex, ey, (0.5f + 0.8f * k) * u);
			nvgFillColor(vg, nvgRGBAf(1.f, 0.62f, 0.2f, 0.8f * k));
			nvgFill(vg);
		}
	}
};


// Iron plate with ember 7-segment readouts.
struct StatusDisplay : TransparentWidget {
	Grond* module = NULL;

	StatusDisplay(Vec topLeftMm, Vec sizeMm, Grond* m) {
		module = m;
		box.pos = mm2px(topLeftMm);
		box.size = mm2px(sizeMm);
	}

	void row(NVGcontext* vg, float y, const char* name, const std::string& value) {
		float w = box.size.x;
		drawText(vg, 6.f, y, name, 6.2f, alpha(C_BONE, 0.75f), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		drawText(vg, w - 6.f, y, "88", 9.f, nvgRGBAf(1.f, 0.6f, 0.2f, 0.08f), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, F_SEG);
		drawText(vg, w - 6.f, y, value, 9.f, nvgRGB(0xff, 0xb0, 0x4a), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, F_SEG);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.f, 0.f, w, h, 2.5f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0xd2, 0xa8, 0x5c), nvgRGB(0x4e, 0x34, 0x10)));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 1.3f, 1.3f, w - 2.6f, h - 2.6f, 1.6f);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0.f, 0.f, 0.f, h, nvgRGB(0x0b, 0x0e, 0x10), nvgRGB(0x1c, 0x21, 0x24)));
		nvgFill(vg);

		int voices = module ? module->activeVoices() : 0;
		int loaded = module ? module->loadedCount() : 0;
		int il = module ? (int) std::round(module->params[Grond::IMPACT_LAYERS_PARAM].getValue()) : 1;
		int wl = module ? (int) std::round(module->params[Grond::WHOOSH_LAYERS_PARAM].getValue()) : 1;
		row(vg, h * 0.2f, "VOICES", string::f("%02d", voices));
		row(vg, h * 0.5f, "SAMPLES", string::f("%02d", loaded));
		row(vg, h * 0.8f, "LAYERS I-W", string::f("%d-%d", il, wl));
	}
};


// Candle-lit vignette and bronze trim around the whole panel.
struct GlassOverlay : TransparentWidget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, w, h);
		nvgFillPaint(vg, nvgBoxGradient(vg, 0.f, 0.f, w, h, 40.f, 110.f, nvgRGBAf(0.f, 0.f, 0.f, 0.f), nvgRGBAf(0.f, 0.f, 0.f, 0.4f)));
		nvgFill(vg);
	}
};


// Two-strand rope braid running along one edge from (x0,y0) to (x1,y1).
static void drawBraid(NVGcontext* vg, float x0, float y0, float x1, float y1, float amp) {
	float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
	float dx = (x1 - x0) / len, dy = (y1 - y0) / len;
	float nx = -dy, ny = dx;
	const float k = 0.42f;      // radians per pixel
	const float step = 1.2f;

	// strand 0 = gold, strand 1 = bronze; phase pi apart
	auto strand = [&](int sIdx, float s0, float s1) {
		float phase = sIdx ? iw::PI : 0.f;
		nvgBeginPath(vg);
		for (float s = s0; s <= s1; s += step) {
			float off = amp * std::sin(k * s + phase);
			float px = x0 + dx * s + nx * off, py = y0 + dy * s + ny * off;
			if (s == s0)
				nvgMoveTo(vg, px, py);
			else
				nvgLineTo(vg, px, py);
		}
		nvgLineCap(vg, NVG_BUTT);
		nvgStrokeWidth(vg, 3.8f);
		nvgStrokeColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.75f));
		nvgStroke(vg);
		nvgStrokeWidth(vg, 2.4f);
		nvgStrokeColor(vg, sIdx ? nvgRGB(0x6e, 0x50, 0x28) : nvgRGB(0x9c, 0x84, 0x48));
		nvgStroke(vg);
		nvgStrokeWidth(vg, 0.8f);
		nvgStrokeColor(vg, sIdx ? nvgRGB(0x92, 0x72, 0x3c) : nvgRGB(0xc4, 0xae, 0x72));
		nvgStroke(vg);
	};

	strand(0, 0.f, len);
	strand(1, 0.f, len);
	// strand 0 passes over strand 1 at every second crossing
	for (int n = 1; n * iw::PI / k < len; n += 2) {
		float c = n * iw::PI / k;
		strand(0, std::max(0.f, c - 2.2f / k * 0.5f * 1.0f), std::min(len, c + 2.2f / k * 0.5f * 1.0f));
	}
}

// Braided frame around the module edge plus corner studs.
struct BraidBorder : TransparentWidget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		float m = mm2px(1.15f), amp = 1.6f;

		// dark channel the braid sits in
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, w, h);
		nvgRect(vg, m * 2.f, m * 2.f, w - m * 4.f, h - m * 4.f);
		nvgPathWinding(vg, NVG_HOLE);
		nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.55f));
		nvgFill(vg);

		drawBraid(vg, m * 2.f, m, w - m * 2.f, m, amp);
		drawBraid(vg, m * 2.f, h - m, w - m * 2.f, h - m, amp);
		drawBraid(vg, m, m * 2.f, m, h - m * 2.f, amp);
		drawBraid(vg, w - m, m * 2.f, w - m, h - m * 2.f, amp);

		const float cx[4] = {m, w - m, m, w - m};
		const float cy[4] = {m, m, h - m, h - m};
		for (int i = 0; i < 4; i++) {
			nvgBeginPath(vg);
			nvgCircle(vg, cx[i], cy[i], m * 1.05f);
			nvgFillPaint(vg, nvgRadialGradient(vg, cx[i] - 1.f, cy[i] - 1.f, 0.5f, m * 1.3f, nvgRGB(0xc4, 0xa8, 0x62), nvgRGB(0x4a, 0x32, 0x10)));
			nvgFill(vg);
			nvgStrokeWidth(vg, 0.8f);
			nvgStrokeColor(vg, nvgRGB(0x20, 0x12, 0x04));
			nvgStroke(vg);
		}
	}
};

static float hash1(float n) {
	float s = std::sin(n * 127.1f) * 43758.5453f;
	return s - std::floor(s);
}

// Dirt, dark patina and green verdigris scattered over the braid channel.
struct TarnishOverlay : TransparentWidget {
	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x, h = box.size.y;
		float m = mm2px(1.15f), band = m * 2.f;

		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, w, h);
		nvgRect(vg, band, band, w - 2.f * band, h - 2.f * band);
		nvgPathWinding(vg, NVG_HOLE);
		nvgFillColor(vg, nvgRGBAf(0.08f, 0.06f, 0.02f, 0.22f));
		nvgFill(vg);

		float perim = 2.f * (w + h);
		for (int i = 0; i < 1100; i++) {
			float u = hash1(i * 1.31f) * perim;
			float across = hash1(i * 7.77f) * band;
			float x, y;
			if (u < w) { x = u; y = across; }
			else if (u < w + h) { x = w - across; y = u - w; }
			else if (u < 2.f * w + h) { x = u - w - h; y = h - across; }
			else { x = across; y = u - 2.f * w - h; }
			float r = 0.4f + hash1(i * 3.3f) * 1.5f;
			float k = hash1(i * 5.9f);
			NVGcolor c;
			if (k < 0.45f)
				c = nvgRGBAf(0.10f, 0.06f, 0.02f, 0.40f);            // dark grime
			else if (k < 0.8f)
				c = nvgRGBAf(0.24f, 0.46f, 0.34f, 0.32f);            // verdigris
			else
				c = nvgRGBAf(0.72f, 0.66f, 0.48f, 0.12f);            // dust
			nvgBeginPath(vg);
			nvgCircle(vg, x, y, r);
			nvgFillColor(vg, c);
			nvgFill(vg);
		}
	}
};

// Wood and braid never change, so render them once into a framebuffer.
struct StaticBackdrop : FramebufferWidget {
	StaticBackdrop(Vec size) {
		box.size = size;
		PanelDecor* decor = new PanelDecor();
		decor->box.size = size;
		addChild(decor);
		BraidBorder* braid = new BraidBorder();
		braid->box.size = size;
		addChild(braid);
		TarnishOverlay* tarnish = new TarnishOverlay();
		tarnish->box.size = size;
		addChild(tarnish);
	}
};


struct GrondWidget : ModuleWidget {
	void label(float x, float y, const std::string& t, float size = 6.3f) {
		addChild(new PanelLabel(Vec(x, y), t, size));
	}

	void knob(float x, float y, int id, const std::string& name, float dia = 8.5f) {
		TermKnob* k = createParam<TermKnob>(Vec(), module, id);
		k->box.size = mm2px(Vec(dia, dia));
		k->box.pos = mm2px(Vec(x, y)).minus(k->box.size.div(2.f));
		addParam(k);
		label(x, y + 6.9f, name);
	}

	TermSwitch* termSwitch(float x, float y, int id, const std::vector<std::string>& shorts, float w, bool momentary) {
		TermSwitch* s = createParam<TermSwitch>(Vec(), module, id);
		s->shorts = shorts;
		s->momentary = momentary;
		s->box.size = mm2px(Vec(w, 4.8f));
		s->box.pos = mm2px(Vec(x, y)).minus(s->box.size.div(2.f));
		addParam(s);
		return s;
	}

	void sw(float x, float y, int id, const std::string& name, const std::vector<std::string>& shorts, float w = 11.f) {
		termSwitch(x, y, id, shorts, w, false);
		label(x, y + 5.2f, name);
	}

	// small CV input with a "CV" caption below it
	void cv(float x, float y, int inputId) {
		addInput(createInputCentered<TermPortSmall>(mm2px(Vec(x, y)), module, inputId));
		label(x, y + 3.9f, "CV", 4.8f);
	}

	GrondWidget(Grond* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Grond.svg")));
		typedef Grond M;

		addChild(new StaticBackdrop(box.size));
		addChild(new HeaderDecor(284.48f));

		// section plates
		addChild(new GroupBox(Vec(3.f, 11.f), Vec(90.f, 40.f), "IMPACTS", A_COPPER));
		addChild(new GroupBox(Vec(96.f, 11.f), Vec(90.f, 40.f), "WHOOSHES", A_STEEL));
		addChild(new GroupBox(Vec(189.f, 11.f), Vec(92.f, 40.f), "TRIGGERS + STATUS", A_BRONZE));
		addChild(new GroupBox(Vec(3.f, 54.f), Vec(96.f, 38.f), "TIMING + LAYERS", A_MOSS));
		addChild(new GroupBox(Vec(102.f, 54.f), Vec(56.f, 38.f), "RANDOMISE", A_BLOOD));
		addChild(new GroupBox(Vec(161.f, 54.f), Vec(70.f, 38.f), "DOPPLER", A_STEEL));
		addChild(new GroupBox(Vec(234.f, 54.f), Vec(47.f, 38.f), "MULTIBAND", A_COPPER));
		addChild(new GroupBox(Vec(3.f, 95.f), Vec(58.f, 31.f), "DISSEMINATOR", A_STEEL));
		addChild(new GroupBox(Vec(63.f, 95.f), Vec(58.f, 31.f), "DISTORTION", A_BLOOD));
		addChild(new GroupBox(Vec(123.f, 95.f), Vec(70.f, 31.f), "SPECTRAL DELAY", A_MOSS));
		addChild(new GroupBox(Vec(196.f, 95.f), Vec(85.f, 31.f), "OUTPUT", A_IRON));

		// sample slots: 2 columns x 5 rows per section
		for (int i = 0; i < NUM_SLOTS; i++) {
			int local = i % SLOTS_PER_TYPE;
			float x0 = (i < SLOTS_PER_TYPE) ? 5.f : 98.f;
			float x = x0 + (local % 2) * 44.f;
			float y = 17.f + (local / 2) * 6.4f;
			addChild(new SlotDisplay(Vec(x, y), Vec(42.f, 5.9f), module, i));
		}

		// triggers and CV inputs
		const float jx[6] = {198.f, 212.f, 226.f, 240.f, 254.f, 268.f};
		const char* jn[6] = {"TRIG", "WHSH", "IMPT", "PITCH", "CHAOS", "DOPP"};
		for (int c = 0; c < 6; c++) {
			label(jx[c], 28.6f, jn[c]);
			addInput(createInputCentered<TermPort>(mm2px(Vec(jx[c], 22.2f)), module, c));
		}
		const int btn[3] = {M::TRIG_BTN_PARAM, M::WHOOSH_BTN_PARAM, M::IMPACT_BTN_PARAM};
		for (int c = 0; c < 3; c++)
			termSwitch(jx[c], 36.f, btn[c], std::vector<std::string>(1, "FIRE"), 11.f, true);
		addChild(new StatusDisplay(Vec(235.f, 31.f), Vec(43.f, 17.f), module));
		addChild(new ForgeDisplay(Vec(212.f - 4.8f, 41.2f), Vec(9.6f, 7.6f), module));

		// timing + layers
		const float tx[5] = {13.f, 31.f, 49.f, 67.f, 85.f};
		knob(tx[0], 66.f, M::PITCH_PARAM, "PITCH");
		knob(tx[1], 66.f, M::TIME_PARAM, "TIME");
		cv(tx[1] + 8.4f, 65.f, M::TIME_CV_INPUT);
		knob(tx[2], 66.f, M::OFFSET_PARAM, "OFFSET");
		cv(tx[2] + 8.4f, 65.f, M::OFFSET_CV_INPUT);
		sw(tx[3], 64.f, M::ALIGN_PARAM, "ALIGN", {"MAN", "PEAK", "END"});
		knob(tx[4], 66.f, M::DEBRIS_PARAM, "DEBRIS");
		knob(tx[0], 81.f, M::SCATTER_PARAM, "SCATTER");
		cv(tx[0] + 8.4f, 80.f, M::SCATTER_CV_INPUT);
		knob(tx[1], 81.f, M::IMPACT_LAYERS_PARAM, "I.LAYERS");
		knob(tx[2], 81.f, M::WHOOSH_LAYERS_PARAM, "W.LAYERS");
		knob(tx[3], 81.f, M::IMPACT_LVL_PARAM, "I.LEVEL");
		knob(tx[4], 81.f, M::WHOOSH_LVL_PARAM, "W.LEVEL");

		// randomise
		const float rx[3] = {113.f, 130.f, 147.f};
		knob(rx[0], 66.f, M::CHAOS_PARAM, "CHAOS", 10.f);
		knob(rx[1], 66.f, M::R_PITCH_PARAM, "PITCH");
		knob(rx[2], 66.f, M::R_GAIN_PARAM, "LEVEL");
		knob(rx[0], 81.f, M::R_TIME_PARAM, "TIMING");
		knob(rx[1], 81.f, M::R_PAN_PARAM, "PAN");
		knob(rx[2], 81.f, M::R_REV_PARAM, "REVERSE");

		// doppler
		const float dx[4] = {171.f, 188.f, 205.f, 222.f};
		knob(dx[0], 66.f, M::D_AMT_PARAM, "AMOUNT");
		knob(dx[1], 66.f, M::D_CENTER_PARAM, "CENTRE");
		knob(dx[2], 66.f, M::D_PASS_PARAM, "PASS");
		knob(dx[3], 66.f, M::D_WIDTH_PARAM, "WIDTH");
		knob(dx[0], 81.f, M::D_DIST_PARAM, "DISTANCE");
		sw(190.f, 79.f, M::D_DIR_PARAM, "DIRECTION", {"L>R", "R>L", "RND"});
		sw(216.f, 79.f, M::D_AUTO_PARAM, "AUTO PEAK", {"MAN", "AUTO"});

		// OTT
		const float ox[3] = {243.f, 257.5f, 272.f};
		knob(ox[0], 66.f, M::OTT_DEPTH_PARAM, "DEPTH");
		knob(ox[1], 66.f, M::OTT_TIME_PARAM, "TIME");
		knob(ox[2], 66.f, M::OTT_MIX_PARAM, "MIX");
		knob(ox[0], 81.f, M::OTT_LOW_PARAM, "LOW");
		knob(ox[1], 81.f, M::OTT_MID_PARAM, "MID");
		knob(ox[2], 81.f, M::OTT_HIGH_PARAM, "HIGH");

		// effects
		const float ky = 105.5f, cvy = 120.3f;
		const float ex[4] = {11.f, 25.f, 39.f, 53.f};
		const float xx[4] = {71.f, 85.f, 99.f, 113.f};
		const float sx[5] = {131.f, 145.f, 159.f, 173.f, 187.f};
		knob(ex[0], ky, M::DISP_AMT_PARAM, "AMOUNT");
		knob(ex[1], ky, M::DISP_FREQ_PARAM, "FREQ");
		knob(ex[2], ky, M::DISP_PINCH_PARAM, "PINCH");
		knob(ex[3], ky, M::DISP_MIX_PARAM, "MIX");
		knob(xx[0], ky, M::DIST_DRIVE_PARAM, "DRIVE");
		knob(xx[1], ky, M::DIST_TYPE_PARAM, "TYPE");
		knob(xx[2], ky, M::DIST_TONE_PARAM, "TONE");
		knob(xx[3], ky, M::DIST_MIX_PARAM, "MIX");
		cv(xx[0], cvy, M::DRIVE_CV_INPUT);
		cv(xx[2], cvy, M::TONE_CV_INPUT);
		cv(xx[3], cvy, M::DISTMIX_CV_INPUT);
		knob(sx[0], ky, M::SPEC_TIME_PARAM, "TIME");
		knob(sx[1], ky, M::SPEC_TILT_PARAM, "TILT");
		knob(sx[2], ky, M::SPEC_FB_PARAM, "FEEDBK");
		knob(sx[3], ky, M::SPEC_SPREAD_PARAM, "SPREAD");
		knob(sx[4], ky, M::SPEC_MIX_PARAM, "MIX");
		cv(sx[0], cvy, M::STIME_CV_INPUT);
		cv(sx[1], cvy, M::STILT_CV_INPUT);
		cv(sx[2], cvy, M::SFB_CV_INPUT);
		cv(sx[3], cvy, M::SSPREAD_CV_INPUT);
		cv(sx[4], cvy, M::SMIX_CV_INPUT);

		// output
		knob(210.f, ky + 1.f, M::GAIN_PARAM, "LEVEL", 10.f);
		const float outx[3] = {235.f, 252.f, 269.f};
		const char* on[3] = {"OUT L", "OUT R", "GATE"};
		for (int c = 0; c < 3; c++) {
			label(outx[c], 114.f, on[c]);
			addOutput(createOutputCentered<TermPort>(mm2px(Vec(outx[c], 106.f)), module, c));
		}

		GlassOverlay* glass = new GlassOverlay();
		glass->box.size = box.size;
		addChild(glass);
	}

	void appendContextMenu(ui::Menu* menu) override {
		Grond* m = dynamic_cast<Grond*>(module);
		if (!m)
			return;
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuItem("Load WAV folder into IMPACTS (pick any file in it)", "", [=]() {
			std::string path = pickWav(m->lastDir);
			if (!path.empty())
				m->loadFolder(path, SLOT_IMPACT);
		}));
		menu->addChild(createMenuItem("Load WAV folder into WHOOSHES (pick any file in it)", "", [=]() {
			std::string path = pickWav(m->lastDir);
			if (!path.empty())
				m->loadFolder(path, SLOT_WHOOSH);
		}));
		menu->addChild(createMenuItem("Clear all samples", "", [=]() {
			for (int i = 0; i < NUM_SLOTS; i++)
				m->clearSlot(i);
		}));
		menu->addChild(createBoolPtrMenuItem("Apply doppler to impacts too", "", &m->dopplerOnImpacts));
	}

	// Dropped WAVs go into the free slots of the section they are dropped on.
	void onPathDrop(const PathDropEvent& e) override {
		Grond* m = dynamic_cast<Grond*>(module);
		if (!m)
			return;
		int type = (e.pos.x > mm2px(94.5f) && e.pos.x < mm2px(187.f)) ? SLOT_WHOOSH : SLOT_IMPACT;
		for (size_t k = 0; k < e.paths.size(); k++) {
			if (string::lowercase(system::getExtension(e.paths[k])) != ".wav")
				continue;
			int i = m->firstEmptySlot(type);
			if (i < 0)
				break;
			m->loadSlot(i, e.paths[k]);
		}
		e.consume(this);
	}
};


Model* modelGrond = createModel<Grond, GrondWidget>("TJB_Grond");
