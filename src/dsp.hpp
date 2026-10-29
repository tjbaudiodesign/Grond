#pragma once
// Self-contained DSP for ImpactWhoosh: WAV loader, FFT, disperser, distortion,
// spectral delay. No dependency on the Rack API, and C++11 only.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace iw {

static const float PI = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

inline float lerpf(float a, float b, float t) {
	return a + (b - a) * t;
}


// ---------------------------------------------------------------- Sample

struct Sample {
	std::string path;
	std::string name;
	std::vector<float> L, R;
	float sampleRate = 44100.f;
	float peakPos = 0.5f;          // normalised position of the loudest region
	std::vector<float> peaks;      // |max| per display column, for the panel

	size_t frames() const { return L.size(); }

	static uint32_t rd32(const unsigned char* p) {
		return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
	}
	static uint16_t rd16(const unsigned char* p) {
		return (uint16_t) (p[0] | (p[1] << 8));
	}

	// Minimal RIFF/WAVE reader: PCM 8/16/24/32 and float 32/64, any channel count
	// (first two channels used). Returns false with a reason in `err` on failure.
	bool load(const std::string& filePath, const std::string& displayName,
			const std::vector<unsigned char>& buf, std::string& err) {
		if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4)) {
			err = "Not a WAV file";
			return false;
		}

		uint16_t fmtTag = 0, channels = 0, bits = 0;
		uint32_t rate = 0;
		const unsigned char* data = NULL;
		size_t dataSize = 0;

		size_t pos = 12;
		while (pos + 8 <= buf.size()) {
			const unsigned char* ck = buf.data() + pos;
			size_t sz = rd32(ck + 4);
			size_t body = pos + 8;
			if (body + sz > buf.size())
				sz = buf.size() - body;
			if (!std::memcmp(ck, "fmt ", 4) && sz >= 16) {
				fmtTag = rd16(buf.data() + body);
				channels = rd16(buf.data() + body + 2);
				rate = rd32(buf.data() + body + 4);
				bits = rd16(buf.data() + body + 14);
				if (fmtTag == 0xFFFE && sz >= 26)
					fmtTag = rd16(buf.data() + body + 24);
			}
			else if (!std::memcmp(ck, "data", 4)) {
				data = buf.data() + body;
				dataSize = sz;
				break;
			}
			pos = body + sz + (sz & 1);
		}

		if (!data || !channels || !rate || !bits) { err = "Malformed WAV"; return false; }
		bool isFloat = (fmtTag == 3);
		if (fmtTag != 1 && fmtTag != 3) { err = "Unsupported WAV encoding"; return false; }
		int bytes = bits / 8;
		if (!(bytes == 1 || bytes == 2 || bytes == 3 || bytes == 4 || (isFloat && bytes == 8))) {
			err = "Unsupported bit depth";
			return false;
		}
		if (isFloat && bytes != 4 && bytes != 8) { err = "Unsupported bit depth"; return false; }

		size_t n = dataSize / (size_t) (bytes * channels);
		const size_t maxFrames = (size_t) rate * 120;  // cap at two minutes
		if (n > maxFrames) n = maxFrames;
		if (n < 2) { err = "File is empty"; return false; }

		L.assign(n, 0.f);
		R.assign(n, 0.f);
		for (size_t i = 0; i < n; i++) {
			for (int c = 0; c < 2 && c < channels; c++) {
				const unsigned char* p = data + (i * channels + c) * bytes;
				float v = 0.f;
				if (isFloat) {
					if (bytes == 4) { float x; std::memcpy(&x, p, 4); v = x; }
					else { double x; std::memcpy(&x, p, 8); v = (float) x; }
				}
				else if (bytes == 1) v = ((int) p[0] - 128) / 128.f;
				else if (bytes == 2) v = (int16_t) rd16(p) / 32768.f;
				else if (bytes == 3) {
					int32_t x = (int32_t) ((p[0] << 8) | (p[1] << 16) | ((uint32_t) p[2] << 24));
					v = (x >> 8) / 8388608.f;
				}
				else v = (int32_t) rd32(p) / 2147483648.f;
				(c == 0 ? L : R)[i] = v;
			}
			if (channels == 1)
				R[i] = L[i];
		}

		path = filePath;
		name = displayName;
		sampleRate = (float) rate;
		analyse();
		return true;
	}

	void analyse() {
		size_t n = L.size();
		// Loudest ~23 ms block, used to centre the Doppler pass on the peak.
		const size_t block = 1024;
		float best = -1.f;
		size_t bestIdx = n / 2;
		for (size_t i = 0; i < n; i += block) {
			size_t end = std::min(n, i + block);
			float e = 0.f;
			for (size_t j = i; j < end; j++)
				e += L[j] * L[j] + R[j] * R[j];
			e /= (float) (end - i);
			if (e > best) { best = e; bestIdx = (i + end) / 2; }
		}
		peakPos = (float) bestIdx / (float) n;

		const int cols = 96;
		peaks.assign(cols, 0.f);
		float norm = 1e-6f;
		for (int c = 0; c < cols; c++) {
			size_t a = n * c / cols, b = std::max(a + 1, n * (c + 1) / cols);
			float m = 0.f;
			size_t step = std::max<size_t>(1, (b - a) / 64);
			for (size_t j = a; j < b && j < n; j += step)
				m = std::max(m, std::max(std::fabs(L[j]), std::fabs(R[j])));
			peaks[c] = m;
			norm = std::max(norm, m);
		}
		for (int c = 0; c < cols; c++)
			peaks[c] /= norm;
	}

	static float at(const std::vector<float>& v, long i) {
		return (i < 0 || i >= (long) v.size()) ? 0.f : v[i];
	}

	// 4-point Hermite interpolation
	static float read(const std::vector<float>& v, double p) {
		long i = (long) std::floor(p);
		float t = (float) (p - (double) i);
		float y0 = at(v, i - 1), y1 = at(v, i), y2 = at(v, i + 1), y3 = at(v, i + 2);
		float c0 = y1;
		float c1 = 0.5f * (y2 - y0);
		float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
		float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
		return ((c3 * t + c2) * t + c1) * t + c0;
	}
};


// ------------------------------------------------------------------- FFT

// Radix-2 complex FFT, in place, unscaled in both directions.
struct FFT {
	int n = 0;
	std::vector<int> rev;
	std::vector<float> cosT, sinT;

	void init(int size) {
		n = size;
		int logn = 0;
		while ((1 << logn) < n) logn++;
		rev.assign(n, 0);
		for (int i = 0; i < n; i++) {
			int r = 0;
			for (int b = 0; b < logn; b++)
				if (i & (1 << b))
					r |= 1 << (logn - 1 - b);
			rev[i] = r;
		}
		cosT.assign(n / 2, 0.f);
		sinT.assign(n / 2, 0.f);
		for (int k = 0; k < n / 2; k++) {
			cosT[k] = std::cos(2.f * PI * k / n);
			sinT[k] = std::sin(2.f * PI * k / n);
		}
	}

	void transform(float* re, float* im, bool inverse) const {
		for (int i = 0; i < n; i++) {
			int j = rev[i];
			if (j > i) {
				std::swap(re[i], re[j]);
				std::swap(im[i], im[j]);
			}
		}
		for (int len = 2; len <= n; len <<= 1) {
			int half = len / 2, step = n / len;
			for (int i = 0; i < n; i += len) {
				for (int k = 0; k < half; k++) {
					float wr = cosT[k * step];
					float wi = inverse ? sinT[k * step] : -sinT[k * step];
					int a = i + k, b = a + half;
					float tr = re[b] * wr - im[b] * wi;
					float ti = re[b] * wi + im[b] * wr;
					re[b] = re[a] - tr;
					im[b] = im[a] - ti;
					re[a] += tr;
					im[a] += ti;
				}
			}
		}
	}
};


// ------------------------------------------------------------- Disperser

// Cascade of 2nd-order allpass filters. All frequencies are delayed by a
// frequency dependent amount around `freq`, which turns transients into chirps.
struct Disperser {
	static const int MAX_STAGES = 32;
	float z1[MAX_STAGES][2];
	float z2[MAX_STAGES][2];
	float b0 = 1.f, b1 = 0.f;   // a2 == b0, a1 == b1, b2 == 1
	int full = 0;
	float frac = 0.f;

	Disperser() { reset(); }

	void reset() {
		std::memset(z1, 0, sizeof(z1));
		std::memset(z2, 0, sizeof(z2));
	}

	// amount 0..1 -> 0..MAX_STAGES stages (fractional part crossfades the last stage)
	void setParams(float sampleRate, float freq, float q, float amount) {
		float w0 = 2.f * PI * clampf(freq, 20.f, sampleRate * 0.45f) / sampleRate;
		float alpha = std::sin(w0) / (2.f * q);
		float a0 = 1.f + alpha;
		b0 = (1.f - alpha) / a0;
		b1 = -2.f * std::cos(w0) / a0;

		float st = clampf(amount, 0.f, 1.f) * MAX_STAGES;
		int newFull = std::min((int) st, MAX_STAGES - 1);
		frac = st - (float) newFull;
		if (st >= MAX_STAGES) frac = 1.f;
		// Clear stages that fell out of use so they don't pop when re-enabled.
		for (int i = newFull + 1; i <= full && i < MAX_STAGES; i++)
			for (int c = 0; c < 2; c++)
				z1[i][c] = z2[i][c] = 0.f;
		full = newFull;
	}

	inline float stage(int i, int c, float x) {
		float y = b0 * x + z1[i][c];
		z1[i][c] = b1 * x - b1 * y + z2[i][c];
		z2[i][c] = x - b0 * y;
		return y;
	}

	void process(float& l, float& r) {
		for (int i = 0; i < full; i++) {
			l = stage(i, 0, l);
			r = stage(i, 1, r);
		}
		if (frac > 0.f) {
			float yl = stage(full, 0, l);
			float yr = stage(full, 1, r);
			l += frac * (yl - l);
			r += frac * (yr - r);
		}
	}
};


// ------------------------------------------------------------ Distortion

struct Distortion {
	enum Type { SOFT, HARD, FOLD, CRUSH };

	float lp[2] = {0.f, 0.f};
	float dcX[2] = {0.f, 0.f}, dcY[2] = {0.f, 0.f};
	float held[2] = {0.f, 0.f};
	int holdCount = 0;

	// x is normalised to +-1 (the caller scales from/to +-5 V)
	float shape(float x, int type, float drive, float drive01, int ch, bool newHold) {
		switch (type) {
			case SOFT:
				return std::tanh(x * drive) / std::sqrt(drive);
			case HARD:
				return clampf(x * drive, -1.f, 1.f) / std::sqrt(drive);
			case FOLD: {
				// triangle-ish wavefolder via sine, keeps level bounded
				return std::sin(x * drive * 1.2f);
			}
			default: {
				float bits = lerpf(14.f, 2.f, drive01);
				float levels = std::pow(2.f, bits);
				if (newHold)
					held[ch] = std::floor(clampf(x * (1.f + drive01), -1.f, 1.f) * levels + 0.5f) / levels;
				return held[ch];
			}
		}
	}

	void process(float sampleRate, float& l, float& r, int type, float drive01, float tone01, float mix) {
		float drive = std::pow(10.f, drive01 * 36.f / 20.f);  // 0..36 dB
		float fc = 200.f * std::pow(100.f, tone01);           // 200 Hz..20 kHz
		float a = 1.f - std::exp(-2.f * PI * std::min(fc, sampleRate * 0.45f) / sampleRate);

		int hold = (type == CRUSH) ? 1 + (int) (drive01 * 15.f) : 1;
		bool newHold = (holdCount % hold) == 0;
		holdCount = (holdCount + 1) % 64;

		float in[2] = {l / 5.f, r / 5.f};
		float out[2];
		for (int c = 0; c < 2; c++) {
			float y = shape(in[c], type, drive, drive01, c, newHold);
			lp[c] += a * (y - lp[c]);
			y = lp[c];
			// DC blocker
			float dc = y - dcX[c] + 0.995f * dcY[c];
			dcX[c] = y;
			dcY[c] = dc;
			out[c] = dc * 5.f;
		}
		l += mix * (out[0] - l);
		r += mix * (out[1] - r);
	}
};


// -------------------------------------------------------- Spectral delay

// STFT with an independent delay line (in frames) and feedback per FFT bin.
class SpectralDelay {
public:
	static const int N = 1024;
	static const int HOP = 256;
	static const int BINS = N / 2 + 1;
	static const int HIST = 384;   // frames of history: ~2 s at 48 kHz

	SpectralDelay() {
		fft.init(N);
		for (int i = 0; i < N; i++)
			window[i] = 0.5f - 0.5f * std::cos(2.f * PI * i / N);
		uint32_t s = 12345u;
		for (int i = 0; i < BINS; i++) {
			s = s * 1664525u + 1013904223u;
			randTable[i] = ((s >> 8) & 0xFFFF) / 32768.f - 1.f;
		}
		for (int c = 0; c < 2; c++) {
			histRe[c].assign((size_t) HIST * BINS, 0.f);
			histIm[c].assign((size_t) HIST * BINS, 0.f);
		}
		reset();
		for (int b = 0; b < BINS; b++)
			delayFrames[b] = 1;
	}

	void reset() {
		for (int c = 0; c < 2; c++) {
			std::fill(inBuf[c], inBuf[c] + N, 0.f);
			std::fill(outBuf[c], outBuf[c] + N, 0.f);
			std::fill(histRe[c].begin(), histRe[c].end(), 0.f);
			std::fill(histIm[c].begin(), histIm[c].end(), 0.f);
		}
		inPos = outPos = hopCounter = histPos = 0;
	}

	// time in seconds (max delay), tilt -1..1, spread 0..1, feedback 0..1
	void setParams(float sampleRate, float time, float tilt, float spread, float feedback) {
		fb = clampf(feedback, 0.f, 0.97f);
		if (time == lastTime && tilt == lastTilt && spread == lastSpread && sampleRate == lastRate)
			return;
		lastTime = time; lastTilt = tilt; lastSpread = spread; lastRate = sampleRate;

		float timeFrames = time * sampleRate / HOP;
		float t = std::fabs(tilt);
		for (int b = 0; b < BINS; b++) {
			// position on a log-frequency axis spanning ~9 octaves below Nyquist
			float pos = b == 0 ? 0.f : clampf(1.f + std::log2((float) b / (N / 2)) / 9.f, 0.f, 1.f);
			float w = tilt >= 0.f ? pos : 1.f - pos;
			float mult = (1.f - t) + t * w;
			mult *= std::max(0.f, 1.f + spread * randTable[b / 4]);
			float d = std::floor(mult * timeFrames + 0.5f);
			delayFrames[b] = (int) clampf(d, 1.f, (float) (HIST - 1));
		}
	}

	// Feed one stereo sample, returns the wet signal.
	void process(float inL, float inR, float& wetL, float& wetR) {
		inBuf[0][inPos] = inL;
		inBuf[1][inPos] = inR;
		inPos = (inPos + 1) % N;

		wetL = outBuf[0][outPos];
		wetR = outBuf[1][outPos];
		outBuf[0][outPos] = 0.f;
		outBuf[1][outPos] = 0.f;
		outPos = (outPos + 1) % N;

		if (++hopCounter >= HOP) {
			hopCounter = 0;
			frame(0);
			frame(1);
			histPos = (histPos + 1) % HIST;
		}
	}

private:
	FFT fft;
	float window[N];
	float randTable[BINS];
	int delayFrames[BINS];
	float inBuf[2][N];
	float outBuf[2][N];
	std::vector<float> histRe[2], histIm[2];
	float re[N], im[N];
	int inPos = 0, outPos = 0, hopCounter = 0, histPos = 0;
	float fb = 0.f;
	float lastTime = -1.f, lastTilt = 2.f, lastSpread = -1.f, lastRate = 0.f;

	void frame(int c) {
		for (int i = 0; i < N; i++) {
			re[i] = inBuf[c][(inPos + i) % N] * window[i];
			im[i] = 0.f;
		}
		fft.transform(re, im, false);

		float* hr = histRe[c].data();
		float* hi = histIm[c].data();
		float* curR = hr + (size_t) histPos * BINS;
		float* curI = hi + (size_t) histPos * BINS;
		for (int b = 0; b < BINS; b++) {
			int idx = (histPos - delayFrames[b] + HIST) % HIST;
			float wr = hr[(size_t) idx * BINS + b];
			float wi = hi[(size_t) idx * BINS + b];
			// stored spectrum = input + feedback * delayed output
			curR[b] = re[b] + fb * wr;
			curI[b] = im[b] + fb * wi;
			re[b] = wr;
			im[b] = wi;
		}
		// Hermitian symmetry for a real output
		im[0] = 0.f;
		im[N / 2] = 0.f;
		for (int k = 1; k < N / 2; k++) {
			re[N - k] = re[k];
			im[N - k] = -im[k];
		}
		fft.transform(re, im, true);

		const float scale = 1.f / (1.5f * N);   // IFFT scale and Hann^2 overlap (N/HOP = 4)
		for (int i = 0; i < N; i++)
			outBuf[c][(outPos + i) % N] += re[i] * window[i] * scale;
	}
};


// ------------------------------------------------------------------- OTT

struct Biquad {
	float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
	float z1[2] = {0.f, 0.f}, z2[2] = {0.f, 0.f};

	void setButterworth(float fc, float sampleRate, bool high) {
		float w0 = 2.f * PI * fc / sampleRate;
		float c = std::cos(w0), alpha = std::sin(w0) / (2.f * 0.70710678f);
		float a0 = 1.f + alpha;
		if (high) {
			b0 = (1.f + c) * 0.5f / a0;
			b1 = -(1.f + c) / a0;
		}
		else {
			b0 = (1.f - c) * 0.5f / a0;
			b1 = (1.f - c) / a0;
		}
		b2 = b0;
		a1 = -2.f * c / a0;
		a2 = (1.f - alpha) / a0;
	}

	void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.f; }

	inline float process(int ch, float x) {
		float y = b0 * x + z1[ch];
		z1[ch] = b1 * x - a1 * y + z2[ch];
		z2[ch] = b2 * x - a2 * y;
		return y;
	}
};

// Linkwitz-Riley 4th order = two cascaded Butterworth sections
struct LR4 {
	Biquad a, b;
	void set(float fc, float sampleRate, bool high) {
		a.setButterworth(fc, sampleRate, high);
		b.setButterworth(fc, sampleRate, high);
	}
	void reset() { a.reset(); b.reset(); }
	inline float process(int ch, float x) { return b.process(ch, a.process(ch, x)); }
};

// Three-band upward + downward compressor ("OTT" style).
struct OTT {
	static const int BANDS = 3;
	LR4 lo1, hi1, lo2, hi2, compLo, compHi;
	float env[BANDS] = {0.f, 0.f, 0.f};
	float lastRate = 0.f, lastTime = -1.f;
	float aAtk = 0.01f, aRel = 0.001f;

	void reset() {
		lo1.reset(); hi1.reset(); lo2.reset(); hi2.reset(); compLo.reset(); compHi.reset();
		for (int i = 0; i < BANDS; i++) env[i] = 0.f;
	}

	// depth, time, mix in 0..1; gains are linear per-band makeup
	void process(float sampleRate, float& l, float& r, float depth, float time, const float* gains, float mix) {
		if (sampleRate != lastRate) {
			const float f1 = 120.f, f2 = 2500.f;
			lo1.set(f1, sampleRate, false);
			hi1.set(f1, sampleRate, true);
			lo2.set(f2, sampleRate, false);
			hi2.set(f2, sampleRate, true);
			compLo.set(f2, sampleRate, false);
			compHi.set(f2, sampleRate, true);
			lastRate = sampleRate;
			lastTime = -1.f;
		}
		if (time != lastTime) {
			float atk = 0.001f * std::pow(40.f, time);
			float rel = 0.02f * std::pow(20.f, time);
			aAtk = 1.f - std::exp(-1.f / (atk * sampleRate));
			aRel = 1.f - std::exp(-1.f / (rel * sampleRate));
			lastTime = time;
		}

		const float thHi = -18.f, thLo = -36.f;
		float down = depth * 0.8f, up = depth * 0.6f;

		float in[2] = {l / 5.f, r / 5.f};
		float band[BANDS][2];
		for (int c = 0; c < 2; c++) {
			float low = lo1.process(c, in[c]);
			float rest = hi1.process(c, in[c]);
			// phase-align the low band with the 2.5 kHz split
			band[0][c] = compLo.process(c, low) + compHi.process(c, low);
			band[1][c] = lo2.process(c, rest);
			band[2][c] = hi2.process(c, rest);
		}

		float wet[2] = {0.f, 0.f};
		for (int b = 0; b < BANDS; b++) {
			float det = std::max(std::fabs(band[b][0]), std::fabs(band[b][1]));
			env[b] += (det > env[b] ? aAtk : aRel) * (det - env[b]);
			float db = 20.f * std::log10(std::max(env[b], 1e-5f));
			float g = 0.f;
			if (db > thHi)
				g = -(db - thHi) * down;
			else if (db < thLo)
				g = std::min((thLo - db) * up, 24.f) * clampf((db + 90.f) / 20.f, 0.f, 1.f);
			float gain = std::exp(g * 0.11512925f) * gains[b];
			wet[0] += band[b][0] * gain;
			wet[1] += band[b][1] * gain;
		}
		l += mix * (wet[0] * 5.f - l);
		r += mix * (wet[1] * 5.f - r);
	}
};

} // namespace iw
