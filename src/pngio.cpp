// png.cpp - libpng behind the small interface in png.h
//
// The 1.6 "png_image" API does the format juggling: ask for RGBA and it expands
// palette, grey and bit depths for us, and it reports failures by message rather
// than by longjmp, which keeps this file free of setjmp.

#include "pngio.h"

#include <algorithm>
#include <cstring>

#include <png.h>

namespace artemis
{
	static std::string ImageMessage(const png_image& image)
	{
		return image.message[0] ? std::string(image.message) : std::string("libpng failed");
	}

	bool ReadPng(const uint8_t* data, size_t size, Image& out, std::string& error)
	{
		if(data == nullptr || size == 0)
		{
			error = "empty PNG";
			return false;
		}
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		if(!png_image_begin_read_from_memory(&image, data, size))
		{
			error = ImageMessage(image);
			return false;
		}
		out.hasAlpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
		image.format = PNG_FORMAT_RGBA;
		out.width = (int)image.width;
		out.height = (int)image.height;
		out.rgba.assign(PNG_IMAGE_SIZE(image), 0);
		const bool ok = png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) != 0;
		if(!ok)
		{
			error = ImageMessage(image);
			out = Image{};
		}
		png_image_free(&image);
		return ok;
	}

	bool ReadPng(const std::string& path, Image& out, std::string& error)
	{
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		if(!png_image_begin_read_from_file(&image, path.c_str()))
		{
			error = ImageMessage(image);
			return false;
		}
		out.hasAlpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
		image.format = PNG_FORMAT_RGBA;
		out.width = (int)image.width;
		out.height = (int)image.height;
		out.rgba.assign(PNG_IMAGE_SIZE(image), 0);
		const bool ok = png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) != 0;
		if(!ok)
		{
			error = ImageMessage(image);
			out = Image{};
		}
		png_image_free(&image);
		return ok;
	}

	bool WritePngToMemory(std::vector<uint8_t>& out, int width, int height,
		const uint8_t* rgba, std::string& error)
	{
		if(width <= 0 || height <= 0 || rgba == nullptr)
		{
			error = "nothing to write";
			return false;
		}
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		image.width = (png_uint_32)width;
		image.height = (png_uint_32)height;
		image.format = PNG_FORMAT_RGBA;
		png_alloc_size_t size = 0;
		// a first call with a null buffer asks for the size
		if(!png_image_write_to_memory(&image, nullptr, &size, 0, rgba, 0, nullptr))
		{
			error = ImageMessage(image);
			return false;
		}
		out.assign(size, 0);
		if(!png_image_write_to_memory(&image, out.data(), &size, 0, rgba, 0, nullptr))
		{
			error = ImageMessage(image);
			return false;
		}
		out.resize(size);
		return true;
	}

	bool WritePng(const std::string& path, int width, int height,
		const uint8_t* rgba, std::string& error)
	{
		if(width <= 0 || height <= 0 || rgba == nullptr)
		{
			error = "nothing to write";
			return false;
		}
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		image.width = (png_uint_32)width;
		image.height = (png_uint_32)height;
		image.format = PNG_FORMAT_RGBA;
		if(!png_image_write_to_file(&image, path.c_str(), 0, rgba, 0, nullptr))
		{
			error = ImageMessage(image);
			return false;
		}
		return true;
	}

	bool WritePngRgb(const std::string& path, int width, int height,
		const uint8_t* rgb, std::string& error)
	{
		if(width <= 0 || height <= 0 || rgb == nullptr)
		{
			error = "nothing to write";
			return false;
		}
		png_image image;
		memset(&image, 0, sizeof(image));
		image.version = PNG_IMAGE_VERSION;
		image.width = (png_uint_32)width;
		image.height = (png_uint_32)height;
		image.format = PNG_FORMAT_RGB;
		if(!png_image_write_to_file(&image, path.c_str(), 0, rgb, 0, nullptr))
		{
			error = ImageMessage(image);
			return false;
		}
		return true;
	}

	size_t CountColors(const uint8_t* rgba, size_t pixels)
	{
		if(rgba == nullptr || pixels == 0)
			return 0;
		// Sort rather than hash: the keys here always carry a constant low byte
		// (alpha), and libc++'s unordered_set grown from a small reserve ends up
		// with power-of-two bucket counts, where those keys collapse - a 1920x1080
		// canvas took 46 seconds in the set and 50 milliseconds in the sort.
		std::vector<uint32_t> values(pixels);
		for(size_t i = 0; i < pixels; i++)
		{
			const uint8_t* p = rgba + i * 4;
			values[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
				| (uint32_t)p[2] << 8 | p[3];
		}
		std::sort(values.begin(), values.end());
		return (size_t)(std::unique(values.begin(), values.end()) - values.begin());
	}
}