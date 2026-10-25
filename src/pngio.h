// png.h - the little bit of libpng this tree needs
//
// Artemis composites its event images out of PNG tiles, so reading them is the
// bottom of the image chain: the IPT descriptor says which tile goes where, and
// this says what the tile's pixels are.  Everything comes out as 8-bit straight
// (non-premultiplied) RGBA, whatever the file held - palette, grey, RGB, RGBA,
// 16 bit - because that is what the compositor blends and what libpng writes
// back out.

#ifndef ARTEMIS_PNG_H
#define ARTEMIS_PNG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
	struct Image
	{
		int width = 0;
		int height = 0;
		bool hasAlpha = false; // the file itself carried an alpha channel
		std::vector<uint8_t> rgba; // width*height*4, straight alpha

		bool empty() const { return width <= 0 || height <= 0; }
		size_t pixels() const { return (size_t)width * (size_t)height; }
	};

	// Read a PNG from a file or from memory; on failure `error` says why.
	bool ReadPng(const std::string& path, Image& out, std::string& error);
	bool ReadPng(const uint8_t* data, size_t size, Image& out, std::string& error);

	// Write straight RGBA as an 8-bit PNG.
	bool WritePng(const std::string& path, int width, int height,
		const uint8_t* rgba, std::string& error);
	bool WritePngToMemory(std::vector<uint8_t>& out, int width, int height,
		const uint8_t* rgba, std::string& error);

	// Write an RGB (no alpha channel) PNG: a tile written this way is what the
	// compositor must copy instead of blending, so a test needs to be able to
	// make one.
	bool WritePngRgb(const std::string& path, int width, int height,
		const uint8_t* rgb, std::string& error);

	// How many distinct RGBA values the image holds: the cheap "is this a real
	// decode or a field of noise" number the tools print.
	size_t CountColors(const uint8_t* rgba, size_t pixels);
}

#endif // ARTEMIS_PNG_H