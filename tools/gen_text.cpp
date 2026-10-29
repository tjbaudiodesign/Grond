// Renders the panel text to alpha-mask PNGs so no font file has to ship with the plugin.
// Build + run (needs the Rack SDK headers for stb):
//   g++ -O2 -I $RACK_DIR/dep/include -o gen_text tools/gen_text.cpp
//   ./gen_text <font.ttf> res/text
// Output: res/text/<class>_<slug>.png, 8-bit greyscale. Class 0 = knob/jack labels (64 px),
// 1 = plate titles and header words (96 px), 2 = logo (160 px).
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_truetype.h>
#include <stb_image_write.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

static const float REF[3] = {64.f, 96.f, 160.f};

static std::string slug(const std::string& s) {
	std::string o;
	for (char c : s)
		o += isalnum((unsigned char) c) ? (char) tolower(c) : '_';
	return o;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		std::printf("usage: gen_text font.ttf outdir\n");
		return 1;
	}
	FILE* f = std::fopen(argv[1], "rb");
	if (!f) {
		std::printf("cannot open font\n");
		return 1;
	}
	std::vector<unsigned char> ttf;
	unsigned char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		ttf.insert(ttf.end(), buf, buf + n);
	std::fclose(f);
	stbtt_fontinfo font;
	if (!stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
		std::printf("bad font\n");
		return 1;
	}
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);

	struct Item { int cls; const char* text; };
	const Item items[] = {
		{2, "GROND"},
		{1, "IMPACT FORGE"}, {1, "TJB AUDIO"},
		{1, "IMPACTS"}, {1, "WHOOSHES"}, {1, "TRIGGERS + STATUS"}, {1, "TIMING + LAYERS"}, {1, "RANDOMISE"},
		{1, "DOPPLER"}, {1, "MULTIBAND"}, {1, "DISSEMINATOR"}, {1, "DISTORTION"}, {1, "SPECTRAL DELAY"}, {1, "OUTPUT"},
		{0, "PITCH"}, {0, "TIME"}, {0, "OFFSET"}, {0, "ALIGN"}, {0, "DEBRIS"}, {0, "SCATTER"}, {0, "I.LAYERS"},
		{0, "W.LAYERS"}, {0, "I.LEVEL"}, {0, "W.LEVEL"}, {0, "CHAOS"}, {0, "LEVEL"}, {0, "TIMING"}, {0, "PAN"},
		{0, "REVERSE"}, {0, "AMOUNT"}, {0, "CENTRE"}, {0, "PASS"}, {0, "WIDTH"}, {0, "DISTANCE"}, {0, "DIRECTION"},
		{0, "AUTO PEAK"}, {0, "DEPTH"}, {0, "MIX"}, {0, "LOW"}, {0, "MID"}, {0, "HIGH"}, {0, "FREQ"}, {0, "PINCH"},
		{0, "DRIVE"}, {0, "TYPE"}, {0, "TONE"}, {0, "TILT"}, {0, "FEEDBK"}, {0, "SPREAD"}, {0, "CV"},
		{0, "TRIG"}, {0, "WHSH"}, {0, "IMPT"}, {0, "DOPP"}, {0, "OUT L"}, {0, "OUT R"}, {0, "GATE"},
	};

	for (const Item& it : items) {
		float ref = REF[it.cls];
		float scale = ref / (float) (asc - desc);   // same pixel-height scaling NanoVG uses
		int pad = (int) std::ceil(ref / 16.f);
		std::string s = it.text;

		// pen positions
		std::vector<float> pen;
		float x = 0.f;
		for (size_t i = 0; i < s.size(); i++) {
			pen.push_back(x);
			int adv, lsb;
			stbtt_GetCodepointHMetrics(&font, s[i], &adv, &lsb);
			x += adv * scale;
			if (i + 1 < s.size())
				x += stbtt_GetCodepointKernAdvance(&font, s[i], s[i + 1]) * scale;
		}
		int W = (int) std::ceil(x) + 2 * pad;
		int H = (int) std::ceil(ref) + 2 * pad;
		std::vector<unsigned char> img((size_t) W * H, 0);
		float baseline = pad + asc * scale;

		for (size_t i = 0; i < s.size(); i++) {
			if (s[i] == ' ')
				continue;
			float px = pad + pen[i];
			int ix = (int) std::floor(px);
			float fx = px - ix;
			int x0, y0, x1, y1;
			stbtt_GetCodepointBitmapBoxSubpixel(&font, s[i], scale, scale, fx, 0.f, &x0, &y0, &x1, &y1);
			int gw = x1 - x0, gh = y1 - y0;
			if (gw <= 0 || gh <= 0)
				continue;
			std::vector<unsigned char> g((size_t) gw * gh, 0);
			stbtt_MakeCodepointBitmapSubpixel(&font, g.data(), gw, gh, gw, scale, scale, fx, 0.f, s[i]);
			int ox = ix + x0, oy = (int) std::floor(baseline) + y0;
			for (int yy = 0; yy < gh; yy++)
				for (int xx = 0; xx < gw; xx++) {
					int dx = ox + xx, dy = oy + yy;
					if (dx < 0 || dy < 0 || dx >= W || dy >= H)
						continue;
					int v = img[(size_t) dy * W + dx] + g[(size_t) yy * gw + xx];
					img[(size_t) dy * W + dx] = (unsigned char) std::min(v, 255);
				}
		}
		std::string path = std::string(argv[2]) + "/" + std::to_string(it.cls) + "_" + slug(s) + ".png";
		stbi_write_png(path.c_str(), W, H, 1, img.data(), W);
		std::printf("%s  %dx%d\n", path.c_str(), W, H);
	}
	return 0;
}
