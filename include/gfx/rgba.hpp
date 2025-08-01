// SPDX-License-Identifier: MIT

#ifndef RGBDS_GFX_RGBA_HPP
#define RGBDS_GFX_RGBA_HPP

#include <stdint.h>

struct Rgba {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t alpha;

	constexpr Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	    : red(r), green(g), blue(b), alpha(a) {}
	// Constructs the color from a "packed" RGBA representation (0xRRGGBBAA)
	explicit constexpr Rgba(uint32_t rgba = 0)
	    : red(rgba >> 24), green(rgba >> 16), blue(rgba >> 8), alpha(rgba) {}

	static constexpr Rgba fromCGBColor(uint16_t color) {
		constexpr auto _5to8 = [](uint8_t channel) -> uint8_t {
			channel &= 0b11111; // For caller's convenience
			return channel << 3 | channel >> 2;
		};
		return {
		    _5to8(color),
		    _5to8(color >> 5),
		    _5to8(color >> 10),
		    static_cast<uint8_t>(color & 0x8000 ? 0x00 : 0xFF),
		};
	}

	// Returns this RGBA as a 32-bit number that can be printed in hex (`%08x`) to yield its CSS
	// representation
	uint32_t toCSS() const {
		constexpr auto shl = [](uint8_t val, unsigned shift) {
			return static_cast<uint32_t>(val) << shift;
		};
		return shl(red, 24) | shl(green, 16) | shl(blue, 8) | shl(alpha, 0);
	}

	bool operator==(Rgba const &rhs) const { return toCSS() == rhs.toCSS(); }

	// CGB colors are RGB555, so we use bit 15 to signify that the color is transparent instead
	// Since the rest of the bits don't matter then, we return 0x8000 exactly.
	static constexpr uint16_t transparent = 0b1'00000'00000'00000;

	static constexpr uint8_t transparency_threshold = 0x10;
	bool isTransparent() const { return alpha < transparency_threshold; }
	static constexpr uint8_t opacity_threshold = 0xF0;
	bool isOpaque() const { return alpha >= opacity_threshold; }
	// Computes the equivalent CGB color, respects the color curve depending on options
	uint16_t cgbColor() const;

	bool isGray() const { return red == green && green == blue; }
	uint8_t grayIndex() const;
};

#endif // RGBDS_GFX_RGBA_HPP
