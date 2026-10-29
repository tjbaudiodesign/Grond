// PNG decoding for the pre-rendered panel text. Kept in its own file so stb_image does not
// clash with the stb declarations Rack's headers pull in.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

// Decodes a PNG to 8-bit greyscale. Free the result with freePngMask.
unsigned char* decodePngMask(const unsigned char* data, int len, int* w, int* h) {
	int comp = 0;
	return stbi_load_from_memory(data, len, w, h, &comp, 1);
}

void freePngMask(unsigned char* p) {
	stbi_image_free(p);
}
