#pragma once
#include <rack.hpp>

using namespace rack;

extern Plugin* pluginInstance;
extern Model* modelGrond;

// Sent from Grond to an Anvil directly on its right. Keep identical to the copy in Anvil's plugin.hpp.
struct GrondExpanderMessage {
	bool whooshPlaying = false;     // at least one whoosh voice is sounding
	uint32_t whooshStarts = 0;      // counts up each time a whoosh begins
	uint32_t whooshEnds = 0;        // counts up each time the last whoosh finishes (end of cycle)
};
