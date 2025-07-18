// SPDX-License-Identifier: MIT

#include "gfx/process.hpp"

#include <algorithm>
#include <errno.h>
#include <inttypes.h>
#include <optional>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "error.hpp"
#include "file.hpp"
#include "helpers.hpp"
#include "itertools.hpp"

#include "gfx/main.hpp"
#include "gfx/pal_packing.hpp"
#include "gfx/pal_sorting.hpp"
#include "gfx/proto_palette.hpp"
#include "gfx/warning.hpp"

static bool isBgColorTransparent() {
	return options.bgColor.has_value() && options.bgColor->isTransparent();
}

class ImagePalette {
	std::array<std::optional<Rgba>, NB_COLOR_SLOTS> _colors;

public:
	ImagePalette() = default;

	// Registers a color in the palette.
	// If the newly inserted color "conflicts" with another one (different color, but same CGB
	// color), then the other color is returned. Otherwise, `nullptr` is returned.
	[[nodiscard]]
	Rgba const *registerColor(Rgba const &rgba) {
		std::optional<Rgba> &slot = _colors[rgba.cgbColor()];

		if (rgba.cgbColor() == Rgba::transparent && !isBgColorTransparent()) {
			options.hasTransparentPixels = true;
		}

		if (!slot.has_value()) {
			slot.emplace(rgba);
		} else if (*slot != rgba) {
			assume(slot->cgbColor() != UINT16_MAX);
			return &*slot;
		}
		return nullptr;
	}

	size_t size() const {
		return std::count_if(RANGE(_colors), [](std::optional<Rgba> const &slot) {
			return slot.has_value() && !slot->isTransparent();
		});
	}
	decltype(_colors) const &raw() const { return _colors; }

	auto begin() const { return _colors.begin(); }
	auto end() const { return _colors.end(); }
};

class Png {
	std::string const &path;
	File file{};
	png_structp png = nullptr;
	png_infop info = nullptr;

	// These are cached for speed
	uint32_t width, height;
	std::vector<Rgba> pixels;
	ImagePalette colors;
	int colorType;
	int nbColors;
	png_colorp embeddedPal = nullptr;
	int nbTransparentEntries;
	png_bytep transparencyPal = nullptr;

	[[noreturn]]
	static void handleError(png_structp png, char const *msg) {
		Png *self = reinterpret_cast<Png *>(png_get_error_ptr(png));

		fatal("Error reading input image (\"%s\"): %s", self->c_str(), msg);
	}

	static void handleWarning(png_structp png, char const *msg) {
		Png *self = reinterpret_cast<Png *>(png_get_error_ptr(png));

		warnx("In input image (\"%s\"): %s", self->c_str(), msg);
	}

	static void readData(png_structp png, png_bytep data, size_t length) {
		Png *self = reinterpret_cast<Png *>(png_get_io_ptr(png));
		std::streamsize expectedLen = length;
		std::streamsize nbBytesRead =
		    self->file->sgetn(reinterpret_cast<char *>(data), expectedLen);

		if (nbBytesRead != expectedLen) {
			fatal(
			    "Error reading input image (\"%s\"): file too short (expected at least %zd more "
			    "bytes after reading %zu)",
			    self->c_str(),
			    length - nbBytesRead,
			    static_cast<size_t>(self->file->pubseekoff(0, std::ios_base::cur))
			);
		}
	}

public:
	ImagePalette const &getColors() const { return colors; }

	int getColorType() const { return colorType; }

	std::tuple<int, png_const_colorp, int, png_bytep> getEmbeddedPal() const {
		return {nbColors, embeddedPal, nbTransparentEntries, transparencyPal};
	}

	uint32_t getWidth() const { return width; }

	uint32_t getHeight() const { return height; }

	Rgba &pixel(uint32_t x, uint32_t y) { return pixels[y * width + x]; }

	Rgba const &pixel(uint32_t x, uint32_t y) const { return pixels[y * width + x]; }

	char const *c_str() const { return file.c_str(path); }

	bool isSuitableForGrayscale() const {
		// Check that all of the grays don't fall into the same "bin"
		if (colors.size() > options.maxOpaqueColors()) { // Apply the Pigeonhole Principle
			options.verbosePrint(
			    Options::VERB_DEBUG,
			    "Too many colors for grayscale sorting (%zu > %" PRIu8 ")\n",
			    colors.size(),
			    options.maxOpaqueColors()
			);
			return false;
		}
		uint8_t bins = 0;
		for (auto const &color : colors) {
			if (!color.has_value() || color->isTransparent()) {
				continue;
			}
			if (!color->isGray()) {
				options.verbosePrint(
				    Options::VERB_DEBUG,
				    "Found non-gray color #%08x, not using grayscale sorting\n",
				    color->toCSS()
				);
				return false;
			}
			uint8_t mask = 1 << color->grayIndex();
			if (bins & mask) { // Two in the same bin!
				options.verbosePrint(
				    Options::VERB_DEBUG,
				    "Color #%08x conflicts with another one, not using grayscale sorting\n",
				    color->toCSS()
				);
				return false;
			}
			bins |= mask;
		}
		return true;
	}

	// Reads a PNG and notes all of its colors
	//
	// This code is more complicated than strictly necessary, but that's because of the API
	// being used: the "high-level" interface doesn't provide all the transformations we need,
	// so we use the "lower-level" one instead.
	// We also use that occasion to only read the PNG one line at a time, since we store all of
	// the pixel data in `pixels`, which saves on memory allocations.
	explicit Png(std::string const &filePath) : path(filePath), colors() {
		if (file.open(path, std::ios_base::in | std::ios_base::binary) == nullptr) {
			fatal("Failed to open input image (\"%s\"): %s", file.c_str(path), strerror(errno));
		}

		options.verbosePrint(Options::VERB_LOG_ACT, "Opened input file\n");

		std::array<unsigned char, 8> pngHeader;

		if (file->sgetn(reinterpret_cast<char *>(pngHeader.data()), pngHeader.size())
		        != static_cast<std::streamsize>(pngHeader.size()) // Not enough bytes?
		    || png_sig_cmp(pngHeader.data(), 0, pngHeader.size()) != 0) {
			fatal("Input file (\"%s\") is not a PNG image!", file.c_str(path));
		}

		options.verbosePrint(Options::VERB_INTERM, "PNG header signature is OK\n");

		png = png_create_read_struct(
		    PNG_LIBPNG_VER_STRING, static_cast<png_voidp>(this), handleError, handleWarning
		);
		if (!png) {
			fatal("Failed to create PNG read structure: %s", strerror(errno)); // LCOV_EXCL_LINE
		}

		info = png_create_info_struct(png);
		if (!info) {
			// LCOV_EXCL_START
			png_destroy_read_struct(&png, nullptr, nullptr);
			fatal("Failed to create PNG info structure: %s", strerror(errno));
			// LCOV_EXCL_STOP
		}

		png_set_read_fn(png, this, readData);
		png_set_sig_bytes(png, pngHeader.size());

		// Process all chunks up to but not including the image data
		png_read_info(png, info);

		int bitDepth, interlaceType; //, compressionType, filterMethod;

		png_get_IHDR(
		    png, info, &width, &height, &bitDepth, &colorType, &interlaceType, nullptr, nullptr
		);

		if (options.inputSlice.width == 0 && width % 8 != 0) {
			fatal("Image width (%" PRIu32 " pixels) is not a multiple of 8!", width);
		}
		if (options.inputSlice.height == 0 && height % 8 != 0) {
			fatal("Image height (%" PRIu32 " pixels) is not a multiple of 8!", height);
		}
		if (options.inputSlice.right() > width || options.inputSlice.bottom() > height) {
			error(
			    "Image slice ((%" PRIu16 ", %" PRIu16 ") to (%" PRIu32 ", %" PRIu32
			    ")) is outside the image bounds (%" PRIu32 "x%" PRIu32 ")!",
			    options.inputSlice.left,
			    options.inputSlice.top,
			    options.inputSlice.right(),
			    options.inputSlice.bottom(),
			    width,
			    height
			);
			if (options.inputSlice.width % 8 == 0 && options.inputSlice.height % 8 == 0) {
				fprintf(
				    stderr,
				    "note: Did you mean the slice \"%" PRIu32 ",%" PRIu32 ":%" PRId32 ",%" PRId32
				    "\"? (width and height are in tiles, not pixels!)\n",
				    options.inputSlice.left,
				    options.inputSlice.top,
				    options.inputSlice.width / 8,
				    options.inputSlice.height / 8
				);
			}
			giveUp();
		}

		pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

		auto colorTypeName = [this]() {
			switch (colorType) {
			case PNG_COLOR_TYPE_GRAY:
				return "grayscale";
			case PNG_COLOR_TYPE_GRAY_ALPHA:
				return "grayscale + alpha";
			case PNG_COLOR_TYPE_PALETTE:
				return "palette";
			case PNG_COLOR_TYPE_RGB:
				return "RGB";
			case PNG_COLOR_TYPE_RGB_ALPHA:
				return "RGB + alpha";
			default:
				fatal("Unknown color type %d", colorType);
			}
		};
		auto interlaceTypeName = [&interlaceType]() {
			switch (interlaceType) {
			case PNG_INTERLACE_NONE:
				return "not interlaced";
			case PNG_INTERLACE_ADAM7:
				return "interlaced (Adam7)";
			default:
				fatal("Unknown interlace type %d", interlaceType);
			}
		};
		options.verbosePrint(
		    Options::VERB_INTERM,
		    "Input image: %" PRIu32 "x%" PRIu32 " pixels, %dbpp %s, %s\n",
		    width,
		    height,
		    bitDepth,
		    colorTypeName(),
		    interlaceTypeName()
		);

		if (png_get_PLTE(png, info, &embeddedPal, &nbColors) != 0) {
			if (png_get_tRNS(png, info, &transparencyPal, &nbTransparentEntries, nullptr)) {
				assume(nbTransparentEntries <= nbColors);
			}

			options.verbosePrint(
			    Options::VERB_INTERM, "Embedded palette has %d colors: [", nbColors
			);
			for (int i = 0; i < nbColors; ++i) {
				auto const &color = embeddedPal[i];
				options.verbosePrint(
				    Options::VERB_INTERM,
				    "#%02x%02x%02x%02x%s",
				    color.red,
				    color.green,
				    color.blue,
				    transparencyPal && i < nbTransparentEntries ? transparencyPal[i] : 0xFF,
				    i != nbColors - 1 ? ", " : "]\n"
				);
			}
		} else {
			options.verbosePrint(Options::VERB_INTERM, "No embedded palette\n");
		}

		// Set up transformations to turn everything into RGBA888 for simplicity of handling

		// Convert grayscale to RGB
		switch (colorType & ~PNG_COLOR_MASK_ALPHA) {
		case PNG_COLOR_TYPE_GRAY:
			png_set_gray_to_rgb(png); // This also converts tRNS to alpha
			break;
		case PNG_COLOR_TYPE_PALETTE:
			png_set_palette_to_rgb(png);
			break;
		}

		if (png_get_valid(png, info, PNG_INFO_tRNS)) {
			// If we read a tRNS chunk, convert it to alpha
			png_set_tRNS_to_alpha(png);
		} else if (!(colorType & PNG_COLOR_MASK_ALPHA)) {
			// Otherwise, if we lack an alpha channel, default to full opacity
			png_set_add_alpha(png, 0xFFFF, PNG_FILLER_AFTER);
		}

		// Scale 16bpp back to 8 (we don't need all of that precision anyway)
		if (bitDepth == 16) {
			png_set_scale_16(png);
		} else if (bitDepth < 8) {
			png_set_packing(png);
		}

		// Do NOT call `png_set_interlace_handling`. We want to expand the rows ourselves.

		// Update `info` with the transformations
		png_read_update_info(png, info);
		// These shouldn't have changed
		assume(png_get_image_width(png, info) == width);
		assume(png_get_image_height(png, info) == height);
		// These should have changed, however
		assume(png_get_color_type(png, info) == PNG_COLOR_TYPE_RGBA);
		assume(png_get_bit_depth(png, info) == 8);

		// Now that metadata has been read, we can process the image data

		size_t nbRowBytes = png_get_rowbytes(png, info);
		assume(nbRowBytes != 0);
		std::vector<png_byte> row(nbRowBytes);
		// Holds known-conflicting color pairs to avoid warning about them twice.
		// We don't need to worry about transitivity, as ImagePalette slots are immutable once
		// assigned, and conflicts always occur between that and another color.
		// For the same reason, we don't need to worry about order, either.
		std::vector<std::tuple<uint32_t, uint32_t>> conflicts;
		// Holds colors whose alpha value is ambiguous
		std::vector<uint32_t> indeterminates;

		// Assign a color to the given position, and register it in the image palette as well
		auto assignColor =
		    [this, &conflicts, &indeterminates](png_uint_32 x, png_uint_32 y, Rgba &&color) {
			    if (!color.isTransparent() && !color.isOpaque()) {
				    uint32_t css = color.toCSS();
				    if (std::find(RANGE(indeterminates), css) == indeterminates.end()) {
					    error(
					        "Color #%08x is neither transparent (alpha < %u) nor opaque (alpha >= "
					        "%u) [first seen at x: %" PRIu32 ", y: %" PRIu32 "]",
					        css,
					        Rgba::transparency_threshold,
					        Rgba::opacity_threshold,
					        x,
					        y
					    );
					    indeterminates.push_back(css);
				    }
			    } else if (Rgba const *other = colors.registerColor(color); other) {
				    std::tuple conflicting{color.toCSS(), other->toCSS()};
				    // Do not report combinations twice
				    if (std::find(RANGE(conflicts), conflicting) == conflicts.end()) {
					    warnx(
					        "Fusing colors #%08x and #%08x into Game Boy color $%04x [first seen "
					        "at x: %" PRIu32 ", y: %" PRIu32 "]",
					        std::get<0>(conflicting),
					        std::get<1>(conflicting),
					        color.cgbColor(),
					        x,
					        y
					    );
					    // Do not report this combination again
					    conflicts.emplace_back(conflicting);
				    }
			    }

			    pixel(x, y) = color;
		    };

		if (interlaceType == PNG_INTERLACE_NONE) {
			for (png_uint_32 y = 0; y < height; ++y) {
				png_read_row(png, row.data(), nullptr);

				for (png_uint_32 x = 0; x < width; ++x) {
					assignColor(
					    x, y, Rgba(row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3])
					);
				}
			}
		} else {
			assume(interlaceType == PNG_INTERLACE_ADAM7);

			// For interlace to work properly, we must read the image `nbPasses` times
			for (int pass = 0; pass < PNG_INTERLACE_ADAM7_PASSES; ++pass) {
				// The interlacing pass must be skipped if its width or height is reported as zero
				if (PNG_PASS_COLS(width, pass) == 0 || PNG_PASS_ROWS(height, pass) == 0) {
					continue;
				}

				png_uint_32 xStep = 1u << PNG_PASS_COL_SHIFT(pass);
				png_uint_32 yStep = 1u << PNG_PASS_ROW_SHIFT(pass);

				for (png_uint_32 y = PNG_PASS_START_ROW(pass); y < height; y += yStep) {
					png_bytep ptr = row.data();
					png_read_row(png, ptr, nullptr);

					for (png_uint_32 x = PNG_PASS_START_COL(pass); x < width; x += xStep) {
						assignColor(x, y, Rgba(ptr[0], ptr[1], ptr[2], ptr[3]));
						ptr += 4;
					}
				}
			}
		}

		// We don't care about chunks after the image data (comments, etc.)
		png_read_end(png, nullptr);
	}

	~Png() { png_destroy_read_struct(&png, &info, nullptr); }

	class TilesVisitor {
		Png const &_png;
		bool const _columnMajor;
		uint32_t const _width, _height;
		uint32_t const _limit = _columnMajor ? _height : _width;

	public:
		TilesVisitor(Png const &png, bool columnMajor, uint32_t width, uint32_t height)
		    : _png(png), _columnMajor(columnMajor), _width(width), _height(height) {}

		class Tile {
			Png const &_png;
		public:
			uint32_t const x, y;

			Tile(Png const &png, uint32_t x_, uint32_t y_) : _png(png), x(x_), y(y_) {}

			Rgba pixel(uint32_t xOfs, uint32_t yOfs) const {
				return _png.pixel(x + xOfs, y + yOfs);
			}
		};

	private:
		struct Iterator {
			TilesVisitor const &parent;
			uint32_t const limit;
			uint32_t x, y;

			std::pair<uint32_t, uint32_t> coords() const {
				return {x + options.inputSlice.left, y + options.inputSlice.top};
			}
			Tile operator*() const {
				return {parent._png, x + options.inputSlice.left, y + options.inputSlice.top};
			}

			Iterator &operator++() {
				auto [major, minor] = parent._columnMajor ? std::tie(y, x) : std::tie(x, y);
				major += 8;
				if (major == limit) {
					minor += 8;
					major = 0;
				}
				return *this;
			}

			bool operator==(Iterator const &rhs) const { return coords() == rhs.coords(); }
		};

	public:
		Iterator begin() const { return {*this, _limit, 0, 0}; }
		Iterator end() const {
			Iterator it{*this, _limit, _width - 8, _height - 8}; // Last valid one...
			return ++it;                                         // ...now one-past-last!
		}
	};
public:
	TilesVisitor visitAsTiles() const {
		return {
		    *this,
		    options.columnMajor,
		    options.inputSlice.width ? options.inputSlice.width * 8 : width,
		    options.inputSlice.height ? options.inputSlice.height * 8 : height,
		};
	}
};

class RawTiles {
	// A tile which only contains indices into the image's global palette
	class RawTile {
		std::array<std::array<size_t, 8>, 8> _pixelIndices{};

	public:
		// Not super clean, but it's closer to matrix notation
		size_t &operator()(size_t x, size_t y) { return _pixelIndices[y][x]; }
	};

private:
	std::vector<RawTile> _tiles;

public:
	// Creates a new raw tile, and returns a reference to it so it can be filled in
	RawTile &newTile() {
		_tiles.emplace_back();
		return _tiles.back();
	}
};

struct AttrmapEntry {
	// This field can either be a proto-palette ID, or `transparent` to indicate that the
	// corresponding tile is fully transparent. If you are looking to get the palette ID for this
	// attrmap entry while correctly handling the above, use `getPalID`.
	size_t protoPaletteID; // Only this field is used when outputting "unoptimized" data
	uint8_t tileID;        // This is the ID as it will be output to the tilemap
	bool bank;
	bool yFlip;
	bool xFlip;

	static constexpr size_t transparent = static_cast<size_t>(-1);
	static constexpr size_t background = static_cast<size_t>(-2);

	bool isBackgroundTile() const { return protoPaletteID == background; }
	size_t getPalID(std::vector<size_t> const &mappings) const {
		return mappings[isBackgroundTile() || protoPaletteID == transparent ? 0 : protoPaletteID];
	}
};

static void generatePalSpec(Png const &png) {
	// Generate a palette spec from the first few colors in the embedded palette
	auto [embPalSize, embPalRGB, embPalAlphaSize, embPalAlpha] = png.getEmbeddedPal();
	if (embPalRGB == nullptr) {
		fatal("`-c embedded` was given, but the PNG does not have an embedded palette!");
	}

	// Fill in the palette spec
	options.palSpec.clear();
	options.palSpec.emplace_back(); // A single palette, with `#00000000`s (transparent)
	assume(options.palSpec.size() == 1);
	if (embPalSize > options.maxOpaqueColors()) { // Ignore extraneous colors if they are unused
		embPalSize = options.maxOpaqueColors();
	}
	for (int i = 0; i < embPalSize; ++i) {
		options.palSpec[0][i] = Rgba(
		    embPalRGB[i].red,
		    embPalRGB[i].green,
		    embPalRGB[i].blue,
		    embPalAlpha && i < embPalAlphaSize ? embPalAlpha[i] : 0xFF
		);
	}
}

static std::tuple<std::vector<size_t>, std::vector<Palette>>
    generatePalettes(std::vector<ProtoPalette> const &protoPalettes, Png const &png) {
	// Run a "pagination" problem solver
	auto [mappings, nbPalettes] = overloadAndRemove(protoPalettes);
	assume(mappings.size() == protoPalettes.size());

	// LCOV_EXCL_START
	if (options.verbosity >= Options::VERB_INTERM) {
		fprintf(
		    stderr,
		    "Proto-palette mappings: (%zu palette%s)\n",
		    nbPalettes,
		    nbPalettes != 1 ? "s" : ""
		);
		for (size_t i = 0; i < mappings.size(); ++i) {
			fprintf(stderr, "%zu -> %zu\n", i, mappings[i]);
		}
	}
	// LCOV_EXCL_STOP

	std::vector<Palette> palettes(nbPalettes);
	// If the image contains at least one transparent pixel, force transparency in the first slot of
	// all palettes
	if (options.hasTransparentPixels) {
		for (Palette &pal : palettes) {
			pal.colors[0] = Rgba::transparent;
		}
	}
	// Generate the actual palettes from the mappings
	for (size_t protoPalID = 0; protoPalID < mappings.size(); ++protoPalID) {
		auto &pal = palettes[mappings[protoPalID]];
		for (uint16_t color : protoPalettes[protoPalID]) {
			pal.addColor(color);
		}
	}

	// "Sort" colors in the generated palettes, see the man page for the flowchart
	if (options.palSpecType == Options::DMG) {
		sortGrayscale(palettes, png.getColors().raw());
	} else if (auto [embPalSize, embPalRGB, embPalAlphaSize, embPalAlpha] = png.getEmbeddedPal();
	           embPalRGB != nullptr) {
		sortIndexed(palettes, embPalSize, embPalRGB, embPalAlphaSize, embPalAlpha);
	} else if (png.isSuitableForGrayscale()) {
		sortGrayscale(palettes, png.getColors().raw());
	} else {
		sortRgb(palettes);
	}
	return {mappings, palettes};
}

static std::tuple<std::vector<size_t>, std::vector<Palette>>
    makePalsAsSpecified(std::vector<ProtoPalette> const &protoPalettes) {
	// Convert the palette spec to actual palettes
	std::vector<Palette> palettes(options.palSpec.size());
	for (auto [spec, pal] : zip(options.palSpec, palettes)) {
		for (size_t i = 0; i < options.nbColorsPerPal; ++i) {
			// If the spec has a gap, there's no need to copy anything.
			if (spec[i].has_value() && !spec[i]->isTransparent()) {
				pal[i] = spec[i]->cgbColor();
			}
		}
	}

	auto listColors = [](auto const &list) {
		static char buf[sizeof(", $XXXX, $XXXX, $XXXX, $XXXX")];
		char *ptr = buf;
		for (uint16_t cgbColor : list) {
			ptr += snprintf(ptr, sizeof(", $XXXX"), ", $%04x", cgbColor);
		}
		return &buf[literal_strlen(", ")];
	};

	// Iterate through proto-palettes, and try mapping them to the specified palettes
	std::vector<size_t> mappings(protoPalettes.size());
	bool bad = false;
	for (size_t i = 0; i < protoPalettes.size(); ++i) {
		ProtoPalette const &protoPal = protoPalettes[i];
		// Find the palette...
		auto iter = std::find_if(RANGE(palettes), [&protoPal](Palette const &pal) {
			// ...which contains all colors in this proto-pal
			return std::all_of(RANGE(protoPal), [&pal](uint16_t color) {
				return std::find(RANGE(pal), color) != pal.end();
			});
		});

		if (iter == palettes.end()) {
			assume(!protoPal.empty());
			error("Failed to fit tile colors [%s] in specified palettes", listColors(protoPal));
			bad = true;
		}
		mappings[i] = iter - palettes.begin(); // Bogus value, but whatever
	}
	if (bad) {
		fprintf(
		    stderr,
		    "note: The following palette%s specified:\n",
		    palettes.size() == 1 ? " was" : "s were"
		);
		for (Palette const &pal : palettes) {
			fprintf(stderr, "        [%s]\n", listColors(pal));
		}
		giveUp();
	}

	return {mappings, palettes};
}

static void outputPalettes(std::vector<Palette> const &palettes) {
	// LCOV_EXCL_START
	if (options.verbosity >= Options::VERB_INTERM) {
		for (auto &&palette : palettes) {
			fputs("{ ", stderr);
			for (uint16_t colorIndex : palette) {
				fprintf(stderr, "%04" PRIx16 ", ", colorIndex);
			}
			fputs("}\n", stderr);
		}
	}
	// LCOV_EXCL_STOP

	if (palettes.size() > options.nbPalettes) {
		// If the palette generation is wrong, other (dependee) operations are likely to be
		// nonsensical, so fatal-error outright
		fatal(
		    "Generated %zu palettes, over the maximum of %" PRIu16,
		    palettes.size(),
		    options.nbPalettes
		);
	}

	if (!options.palettes.empty()) {
		File output;
		if (!output.open(options.palettes, std::ios_base::out | std::ios_base::binary)) {
			// LCOV_EXCL_START
			fatal("Failed to create \"%s\": %s", output.c_str(options.palettes), strerror(errno));
			// LCOV_EXCL_STOP
		}

		for (Palette const &palette : palettes) {
			for (uint8_t i = 0; i < options.nbColorsPerPal; ++i) {
				// Will output `UINT16_MAX` for unused slots
				uint16_t color = palette.colors[i];
				output->sputc(color & 0xFF);
				output->sputc(color >> 8);
			}
		}
	}
}

static void hashBitplanes(uint16_t bitplanes, uint16_t &hash) {
	hash ^= bitplanes;
	if (options.allowMirroringX) {
		// Count the line itself as mirrored, which ensures the same hash as the tile's horizontal
		// flip; vertical mirroring is already taken care of because the symmetric line will be
		// XOR'd the same way. (This can trivially create some collisions, but real-world tile data
		// generally doesn't trigger them.)
		hash ^= flipTable[bitplanes >> 8] << 8 | flipTable[bitplanes & 0xFF];
	}
}

class TileData {
	// Importantly, `TileData` is **always** 2bpp.
	// If the active bit depth is 1bpp, all tiles are processed as 2bpp nonetheless, but emitted as
	// 1bpp. This massively simplifies internal processing, since bit depth is always identical
	// outside of I/O / serialization boundaries.
	std::array<uint8_t, 16> _data;
	// The hash is a bit lax: it's the XOR of all lines, and every other nibble is identical
	// if horizontal mirroring is in effect. It should still be a reasonable tie-breaker in
	// non-pathological cases.
	uint16_t _hash;
public:
	// This is an index within the "global" pool; no bank info is encoded here
	// It's marked as `mutable` so that it can be modified even on a `const` object;
	// this is necessary because the `set` in which it's inserted refuses any modification for fear
	// of altering the element's hash, but the tile ID is not part of it.
	mutable uint16_t tileID;

	static uint16_t
	    rowBitplanes(Png::TilesVisitor::Tile const &tile, Palette const &palette, uint32_t y) {
		uint16_t row = 0;
		for (uint32_t x = 0; x < 8; ++x) {
			row <<= 1;
			uint8_t index = palette.indexOf(tile.pixel(x, y).cgbColor());
			assume(index < palette.size()); // The color should be in the palette
			if (index & 1) {
				row |= 1;
			}
			if (index & 2) {
				row |= 0x100;
			}
		}
		return row;
	}

	TileData(std::array<uint8_t, 16> &&raw) : _data(raw), _hash(0) {
		for (uint8_t y = 0; y < 8; ++y) {
			uint16_t bitplanes = _data[y * 2] | _data[y * 2 + 1] << 8;
			hashBitplanes(bitplanes, _hash);
		}
	}

	TileData(Png::TilesVisitor::Tile const &tile, Palette const &palette) : _hash(0) {
		size_t writeIndex = 0;
		for (uint32_t y = 0; y < 8; ++y) {
			uint16_t bitplanes = rowBitplanes(tile, palette, y);
			hashBitplanes(bitplanes, _hash);

			_data[writeIndex++] = bitplanes & 0xFF;
			_data[writeIndex++] = bitplanes >> 8;
		}
	}

	auto const &data() const { return _data; }
	uint16_t hash() const { return _hash; }

	enum MatchType {
		NOPE,
		EXACT,
		HFLIP,
		VFLIP,
		VHFLIP,
	};
	MatchType tryMatching(TileData const &other) const {
		// Check for strict equality first, as that can typically be optimized, and it allows
		// hoisting the mirroring check out of the loop
		if (_data == other._data) {
			return MatchType::EXACT;
		}

		// Check if we have horizontal mirroring, which scans the array forward again
		if (options.allowMirroringX
		    && std::equal(RANGE(_data), other._data.begin(), [](uint8_t lhs, uint8_t rhs) {
			       return lhs == flipTable[rhs];
		       })) {
			return MatchType::HFLIP;
		}

		// The remaining possibilities for matching all require vertical mirroring
		if (!options.allowMirroringY) {
			return MatchType::NOPE;
		}

		// Check if we have vertical or vertical+horizontal mirroring, for which we have to read
		// bitplane *pairs*  backwards
		bool hasVFlip = true, hasVHFlip = true;
		for (uint8_t i = 0; i < _data.size(); ++i) {
			// Flip the bottom bit to get the corresponding row's bitplane 0/1
			// (This works because the array size is even)
			uint8_t lhs = _data[i], rhs = other._data[(15 - i) ^ 1];
			if (lhs != rhs) {
				hasVFlip = false;
			}
			if (lhs != flipTable[rhs]) {
				hasVHFlip = false;
			}
			if (!hasVFlip && !hasVHFlip) {
				return MatchType::NOPE; // If both have been eliminated, all hope is lost!
			}
		}

		// If we have both (i.e. we have symmetry), default to vflip only
		if (hasVFlip) {
			return MatchType::VFLIP;
		}

		// If we allow both and have both, then use both
		if (options.allowMirroringX && hasVHFlip) {
			return MatchType::VHFLIP;
		}

		return MatchType::NOPE;
	}
	bool operator==(TileData const &rhs) const { return tryMatching(rhs) != MatchType::NOPE; }
};

template<>
struct std::hash<TileData> {
	std::size_t operator()(TileData const &tile) const { return tile.hash(); }
};

static void outputUnoptimizedTileData(
    Png const &png,
    std::vector<AttrmapEntry> const &attrmap,
    std::vector<Palette> const &palettes,
    std::vector<size_t> const &mappings
) {
	File output;
	if (!output.open(options.output, std::ios_base::out | std::ios_base::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", output.c_str(options.output), strerror(errno));
		// LCOV_EXCL_STOP
	}

	uint16_t widthTiles = options.inputSlice.width ? options.inputSlice.width : png.getWidth() / 8;
	uint16_t heightTiles =
	    options.inputSlice.height ? options.inputSlice.height : png.getHeight() / 8;
	uint64_t remainingTiles = widthTiles * heightTiles;
	if (remainingTiles <= options.trim) {
		return;
	}
	remainingTiles -= options.trim;

	for (auto [tile, attr] : zip(png.visitAsTiles(), attrmap)) {
		// Do not emit fully-background tiles.
		if (!attr.isBackgroundTile()) {
			// If the tile is fully transparent, this defaults to palette 0.
			Palette const &palette = palettes[attr.getPalID(mappings)];
			for (uint32_t y = 0; y < 8; ++y) {
				uint16_t bitplanes = TileData::rowBitplanes(tile, palette, y);
				output->sputc(bitplanes & 0xFF);
				if (options.bitDepth == 2) {
					output->sputc(bitplanes >> 8);
				}
			}
		}

		--remainingTiles;
		if (remainingTiles == 0) {
			break;
		}
	}
	assume(remainingTiles == 0);
}

static void outputUnoptimizedMaps(
    std::vector<AttrmapEntry> const &attrmap, std::vector<size_t> const &mappings
) {
	std::optional<File> tilemapOutput, attrmapOutput, palmapOutput;
	auto autoOpenPath = [](std::string const &path, std::optional<File> &file) {
		if (!path.empty()) {
			file.emplace();
			if (!file->open(path, std::ios_base::out | std::ios_base::binary)) {
				// LCOV_EXCL_START
				fatal("Failed to create \"%s\": %s", file->c_str(options.tilemap), strerror(errno));
				// LCOV_EXCL_STOP
			}
		}
	};
	autoOpenPath(options.tilemap, tilemapOutput);
	autoOpenPath(options.attrmap, attrmapOutput);
	autoOpenPath(options.palmap, palmapOutput);

	uint8_t tileID = 0;
	uint8_t bank = 0;
	for (auto attr : attrmap) {
		if (tileID == options.maxNbTiles[bank]) {
			assume(bank == 0);
			bank = 1;
			tileID = 0;
		}

		if (tilemapOutput.has_value()) {
			(*tilemapOutput)
			    ->sputc((attr.isBackgroundTile() ? 0 : tileID) + options.baseTileIDs[bank]);
		}
		uint8_t palID = attr.getPalID(mappings) + options.basePalID;
		if (attrmapOutput.has_value()) {
			(*attrmapOutput)->sputc((palID & 0b111) | bank << 3); // The other flags are all 0
		}
		if (palmapOutput.has_value()) {
			(*palmapOutput)->sputc(palID);
		}

		// Background tiles are skipped in the tile data, so they should be skipped in the maps too.
		if (!attr.isBackgroundTile()) {
			++tileID;
		}
	}
}

struct UniqueTiles {
	std::unordered_set<TileData> tileset;
	std::vector<TileData const *> tiles;

	UniqueTiles() = default;
	// Copies are likely to break pointers, so we really don't want those.
	// Copy elision should be relied on to be more sure that refs won't be invalidated, too!
	UniqueTiles(UniqueTiles const &) = delete;
	UniqueTiles(UniqueTiles &&) = default;

	// Adds a tile to the collection, and returns its ID
	std::tuple<uint16_t, TileData::MatchType> addTile(TileData newTile) {
		auto [tileData, inserted] = tileset.insert(newTile);

		TileData::MatchType matchType = TileData::NOPE;
		if (inserted) {
			// Give the new tile the next available unique ID
			tileData->tileID = static_cast<uint16_t>(tiles.size());
			// Pointers are never invalidated!
			tiles.emplace_back(&*tileData);
		} else {
			matchType = tileData->tryMatching(newTile);
		}
		return {tileData->tileID, matchType};
	}

	auto size() const { return tiles.size(); }

	auto begin() const { return tiles.begin(); }
	auto end() const { return tiles.end(); }
};

// Generate tile data while deduplicating unique tiles (via mirroring if enabled)
// Additionally, while we have the info handy, convert from the 16-bit "global" tile IDs to
// 8-bit tile IDs + the bank bit; this will save the work when we output the data later (potentially
// twice)
static UniqueTiles dedupTiles(
    Png const &png,
    std::vector<AttrmapEntry> &attrmap,
    std::vector<Palette> const &palettes,
    std::vector<size_t> const &mappings
) {
	// Iterate throughout the image, generating tile data as we go
	// (We don't need the full tile data to be able to dedup tiles, but we don't lose anything
	// by caching the full tile data anyway, so we might as well.)
	UniqueTiles tiles;

	if (!options.inputTileset.empty()) {
		File inputTileset;
		if (!inputTileset.open(options.inputTileset, std::ios::in | std::ios::binary)) {
			fatal("Failed to open \"%s\": %s", options.inputTileset.c_str(), strerror(errno));
		}

		std::array<uint8_t, 16> tile;
		size_t const tileSize = options.bitDepth * 8;
		for (;;) {
			// It's okay to cast between character types.
			size_t len = inputTileset->sgetn(reinterpret_cast<char *>(tile.data()), tileSize);
			if (len == 0) { // EOF!
				break;
			} else if (len != tileSize) {
				fatal(
				    "\"%s\" does not contain a multiple of %zu bytes; is it actually tile data?",
				    options.inputTileset.c_str(),
				    tileSize
				);
			} else if (len == 8) {
				// Expand the tile data to 2bpp.
				for (size_t i = 8; i--;) {
					tile[i * 2 + 1] = 0;
					tile[i * 2] = tile[i];
				}
			}

			auto [tileID, matchType] = tiles.addTile(std::move(tile));

			if (matchType != TileData::NOPE) {
				error(
				    "The input tileset's tile #%hu was deduplicated; please check that your "
				    "deduplication flags (`-u`, `-m`) are consistent with what was used to "
				    "generate the input tileset",
				    tileID
				);
			}
		}
	}

	bool inputWithoutOutput = !options.inputTileset.empty() && options.output.empty();
	for (auto [tile, attr] : zip(png.visitAsTiles(), attrmap)) {
		if (attr.isBackgroundTile()) {
			attr.xFlip = false;
			attr.yFlip = false;
			attr.bank = 0;
			attr.tileID = 0;
		} else {
			auto [tileID, matchType] =
			    tiles.addTile({tile, palettes[mappings[attr.protoPaletteID]]});

			if (inputWithoutOutput && matchType == TileData::NOPE) {
				error(
				    "Tile at (%" PRIu32 ", %" PRIu32
				    ") is not within the input tileset, and `-o` was not given!",
				    tile.x,
				    tile.y
				);
			}

			attr.xFlip = matchType == TileData::HFLIP || matchType == TileData::VHFLIP;
			attr.yFlip = matchType == TileData::VFLIP || matchType == TileData::VHFLIP;
			attr.bank = tileID >= options.maxNbTiles[0];
			attr.tileID = (attr.bank ? tileID - options.maxNbTiles[0] : tileID)
			              + options.baseTileIDs[attr.bank];
		}
	}

	// Copy elision should prevent the contained `unordered_set` from being re-constructed
	return tiles;
}

static void outputTileData(UniqueTiles const &tiles) {
	File output;
	if (!output.open(options.output, std::ios_base::out | std::ios_base::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", output.c_str(options.output), strerror(errno));
		// LCOV_EXCL_STOP
	}

	uint16_t tileID = 0;
	for (auto iter = tiles.begin(), end = tiles.end() - options.trim; iter != end; ++iter) {
		TileData const *tile = *iter;
		assume(tile->tileID == tileID);
		++tileID;
		if (options.bitDepth == 2) {
			output->sputn(reinterpret_cast<char const *>(tile->data().data()), 16);
		} else {
			assume(options.bitDepth == 1);
			for (size_t y = 0; y < 8; ++y) {
				output->sputc(tile->data()[y * 2]);
			}
		}
	}
}

static void outputTilemap(std::vector<AttrmapEntry> const &attrmap) {
	File output;
	if (!output.open(options.tilemap, std::ios_base::out | std::ios_base::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", output.c_str(options.tilemap), strerror(errno));
		// LCOV_EXCL_STOP
	}

	for (AttrmapEntry const &entry : attrmap) {
		output->sputc(entry.tileID); // The tile ID has already been converted
	}
}

static void
    outputAttrmap(std::vector<AttrmapEntry> const &attrmap, std::vector<size_t> const &mappings) {
	File output;
	if (!output.open(options.attrmap, std::ios_base::out | std::ios_base::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", output.c_str(options.attrmap), strerror(errno));
		// LCOV_EXCL_STOP
	}

	for (AttrmapEntry const &entry : attrmap) {
		uint8_t attr = entry.xFlip << 5 | entry.yFlip << 6;
		attr |= entry.bank << 3;
		attr |= (entry.getPalID(mappings) + options.basePalID) & 0b111;
		output->sputc(attr);
	}
}

static void
    outputPalmap(std::vector<AttrmapEntry> const &attrmap, std::vector<size_t> const &mappings) {
	File output;
	if (!output.open(options.palmap, std::ios_base::out | std::ios_base::binary)) {
		// LCOV_EXCL_START
		fatal("Failed to create \"%s\": %s", output.c_str(options.palmap), strerror(errno));
		// LCOV_EXCL_STOP
	}

	for (AttrmapEntry const &entry : attrmap) {
		output->sputc(entry.getPalID(mappings) + options.basePalID);
	}
}

void processPalettes() {
	options.verbosePrint(Options::VERB_CFG, "Using libpng %s\n", png_get_libpng_ver(nullptr));

	std::vector<ProtoPalette> protoPalettes;
	std::vector<Palette> palettes;
	std::tie(std::ignore, palettes) = makePalsAsSpecified(protoPalettes);

	outputPalettes(palettes);
}

void process() {
	options.verbosePrint(Options::VERB_CFG, "Using libpng %s\n", png_get_libpng_ver(nullptr));

	options.verbosePrint(Options::VERB_LOG_ACT, "Reading tiles...\n");
	Png png(options.input); // This also sets `hasTransparentPixels` as a side effect
	ImagePalette const &colors = png.getColors();

	// Now, we have all the image's colors in `colors`
	// The next step is to order the palette

	// LCOV_EXCL_START
	if (options.verbosity >= Options::VERB_INTERM) {
		fputs("Image colors: [ ", stderr);
		for (auto const &slot : colors) {
			if (!slot.has_value()) {
				continue;
			}
			fprintf(stderr, "#%08x, ", slot->toCSS());
		}
		fputs("]\n", stderr);
	}
	// LCOV_EXCL_STOP

	if (options.palSpecType == Options::DMG) {
		if (options.hasTransparentPixels) {
			fatal(
			    "Image contains transparent pixels, not compatible with a DMG palette specification"
			);
		}
		if (!png.isSuitableForGrayscale()) {
			fatal("Image contains too many or non-gray colors, not compatible with a DMG palette "
			      "specification");
		}
	}

	// Now, iterate through the tiles, generating proto-palettes as we go
	// We do this unconditionally because this performs the image validation (which we want to
	// perform even if no output is requested), and because it's necessary to generate any
	// output (with the exception of an un-duplicated tilemap, but that's an acceptable loss.)
	std::vector<ProtoPalette> protoPalettes;
	std::vector<AttrmapEntry> attrmap{};

	for (auto tile : png.visitAsTiles()) {
		AttrmapEntry &attrs = attrmap.emplace_back();

		// Count the unique non-transparent colors for packing
		std::unordered_set<uint16_t> tileColors;
		for (uint32_t y = 0; y < 8; ++y) {
			for (uint32_t x = 0; x < 8; ++x) {
				if (Rgba color = tile.pixel(x, y);
				    !color.isTransparent() || !options.hasTransparentPixels) {
					tileColors.insert(color.cgbColor());
				}
			}
		}

		if (tileColors.size() > options.maxOpaqueColors()) {
			fatal(
			    "Tile at (%" PRIu32 ", %" PRIu32 ") has %zu colors, more than %" PRIu8 "!",
			    tile.x,
			    tile.y,
			    tileColors.size(),
			    options.maxOpaqueColors()
			);
		}

		if (tileColors.empty()) {
			// "Empty" proto-palettes screw with the packing process, so discard those
			assume(!isBgColorTransparent());
			attrs.protoPaletteID = AttrmapEntry::transparent;
			continue;
		}

		ProtoPalette protoPalette;
		for (uint16_t cgbColor : tileColors) {
			protoPalette.add(cgbColor);
		}

		if (options.bgColor.has_value()
		    && std::find(RANGE(tileColors), options.bgColor->cgbColor()) != tileColors.end()) {
			if (tileColors.size() == 1) {
				// The tile contains just the background color, skip it.
				attrs.protoPaletteID = AttrmapEntry::background;
				continue;
			}
			fatal(
			    "Tile (%" PRIu32 ", %" PRIu32 ") contains the background color (#%08x)!",
			    tile.x,
			    tile.y,
			    options.bgColor->toCSS()
			);
		}

		// Insert the proto-palette, making sure to avoid overlaps
		for (size_t n = 0; n < protoPalettes.size(); ++n) {
			switch (protoPalette.compare(protoPalettes[n])) {
			case ProtoPalette::WE_BIGGER:
				protoPalettes[n] = protoPalette; // Override them
				// Remove any other proto-palettes that we encompass
				// (Example [(0, 1), (0, 2)], inserting (0, 1, 2))
				[[fallthrough]];

			case ProtoPalette::THEY_BIGGER:
				// Do nothing, they already contain us
				attrs.protoPaletteID = n;
				goto continue_visiting_tiles; // Can't `continue` from within a nested loop

			case ProtoPalette::NEITHER:
				break; // Keep going
			}
		}

		attrs.protoPaletteID = protoPalettes.size();
		if (protoPalettes.size() == AttrmapEntry::background) { // Check for overflow
			fatal(
			    "Reached %zu proto-palettes... sorry, this image is too much for me to handle :(",
			    AttrmapEntry::transparent
			);
		}
		protoPalettes.push_back(protoPalette);
continue_visiting_tiles:;
	}

	options.verbosePrint(
	    Options::VERB_INTERM,
	    "Image contains %zu proto-palette%s\n",
	    protoPalettes.size(),
	    protoPalettes.size() != 1 ? "s" : ""
	);
	// LCOV_EXCL_START
	if (options.verbosity >= Options::VERB_INTERM) {
		for (auto const &protoPal : protoPalettes) {
			fputs("[ ", stderr);
			for (uint16_t color : protoPal) {
				fprintf(stderr, "$%04x, ", color);
			}
			fputs("]\n", stderr);
		}
	}
	// LCOV_EXCL_STOP

	if (options.palSpecType == Options::EMBEDDED) {
		generatePalSpec(png);
	}
	auto [mappings, palettes] =
	    options.palSpecType == Options::NO_SPEC || options.palSpecType == Options::DMG
	        ? generatePalettes(protoPalettes, png)
	        : makePalsAsSpecified(protoPalettes);
	outputPalettes(palettes);

	// If deduplication is not happening, we just need to output the tile data and/or maps as-is
	if (!options.allowDedup) {
		uint32_t const nbTilesH = png.getHeight() / 8, nbTilesW = png.getWidth() / 8;

		// Check the tile count
		if (nbTilesW * nbTilesH > options.maxNbTiles[0] + options.maxNbTiles[1]) {
			fatal(
			    "Image contains %" PRIu32 " tiles, exceeding the limit of %" PRIu16 " + %" PRIu16,
			    nbTilesW * nbTilesH,
			    options.maxNbTiles[0],
			    options.maxNbTiles[1]
			);
		}

		// I currently cannot figure out useful semantics for this combination of flags.
		if (!options.inputTileset.empty()) {
			fatal("Input tilesets are not supported without `-u`\nPlease consider explaining your "
			      "use case to RGBDS' developers!");
		}

		if (!options.output.empty()) {
			options.verbosePrint(Options::VERB_LOG_ACT, "Generating unoptimized tile data...\n");
			outputUnoptimizedTileData(png, attrmap, palettes, mappings);
		}

		if (!options.tilemap.empty() || !options.attrmap.empty() || !options.palmap.empty()) {
			options.verbosePrint(
			    Options::VERB_LOG_ACT,
			    "Generating unoptimized tilemap and/or attrmap and/or palmap...\n"
			);
			outputUnoptimizedMaps(attrmap, mappings);
		}
	} else {
		// All of these require the deduplication process to be performed to be output
		options.verbosePrint(Options::VERB_LOG_ACT, "Deduplicating tiles...\n");
		UniqueTiles tiles = dedupTiles(png, attrmap, palettes, mappings);

		if (tiles.size() > options.maxNbTiles[0] + options.maxNbTiles[1]) {
			fatal(
			    "Image contains %zu tiles, exceeding the limit of %" PRIu16 " + %" PRIu16,
			    tiles.size(),
			    options.maxNbTiles[0],
			    options.maxNbTiles[1]
			);
		}

		if (!options.output.empty()) {
			options.verbosePrint(Options::VERB_LOG_ACT, "Generating optimized tile data...\n");
			outputTileData(tiles);
		}

		if (!options.tilemap.empty()) {
			options.verbosePrint(Options::VERB_LOG_ACT, "Generating optimized tilemap...\n");
			outputTilemap(attrmap);
		}

		if (!options.attrmap.empty()) {
			options.verbosePrint(Options::VERB_LOG_ACT, "Generating optimized attrmap...\n");
			outputAttrmap(attrmap, mappings);
		}

		if (!options.palmap.empty()) {
			options.verbosePrint(Options::VERB_LOG_ACT, "Generating optimized palmap...\n");
			outputPalmap(attrmap, mappings);
		}
	}
}
