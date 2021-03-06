#include "stdafx.h"
#include "Emu/Memory/vm.h"
#include "TextureUtils.h"
#include "../RSXThread.h"
#include "../rsx_utils.h"


#define MAX2(a, b) ((a) > (b)) ? (a) : (b)
namespace
{
/**
* Write data, assume src pixels are packed but not mipmaplevel
*/
struct texel_rgba
{
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		for (unsigned row = 0; row < row_count; row++)
			memcpy((char*)dst + row * dst_pitch_in_block * block_size, (char*)src + row * src_pitch_in_block * block_size, width_in_block * block_size);
	}
};


/**
* Write 16 bytes pixel textures, assume src pixels are swizzled and but not mipmaplevel
*/
struct texel_16b_swizzled
{
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		u16 *castedSrc = static_cast<u16*>(src), *castedDst = static_cast<u16*>(dst);

		std::unique_ptr<u16[]> temp_swizzled(new u16[row_count * width_in_block]);
		rsx::convert_linear_swizzle<u16>(castedSrc, temp_swizzled.get(), src_pitch_in_block, row_count, true);
		for (unsigned row = 0; row < row_count; row++)
			for (int j = 0; j < width_in_block; j++)
			{
				u16 tmp = temp_swizzled[row * src_pitch_in_block + j];
				castedDst[row * dst_pitch_in_block + j] = (tmp >> 8) | (tmp << 8);
			}
	}
};

/**
* Write data, assume src pixels are swizzled and but not mipmaplevel
*/
struct texel_rgba_swizzled
{
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		u32 *castedSrc, *castedDst;
		castedSrc = (u32*)src;
		castedDst = (u32*)dst ;
		std::unique_ptr<u32[]> temp_swizzled(new u32[src_pitch_in_block * row_count]);
		rsx::convert_linear_swizzle<u32>(castedSrc, temp_swizzled.get(), src_pitch_in_block, row_count, true);
		for (unsigned row = 0; row < row_count; row++)
			memcpy((char*)dst + row * dst_pitch_in_block * block_size, (char*)temp_swizzled.get() + row * src_pitch_in_block * block_size, width_in_block * block_size);
	}
};

/**
 * Write data, assume compressed (DXTCn) format
 * Data are tightly packed
 */
struct texel_bc_format {
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		for (unsigned row = 0; row < row_count; row++)
			memcpy((char*)dst + row * dst_pitch_in_block * block_size, (char*)src + row * src_pitch_in_block * block_size, width_in_block * block_size);
	}
};

/**
* Write 16 bytes pixel textures, assume src pixels are packed but not mipmaplevel
*/
struct texel_16b_format {
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		unsigned short *castedDst = (unsigned short *)dst, *castedSrc = (unsigned short *)src;

		for (unsigned row = 0; row < row_count; row++)
			for (int j = 0; j < width_in_block; j++)
			{
				u16 tmp = castedSrc[row * src_pitch_in_block + j];
				castedDst[row * dst_pitch_in_block + j] = (tmp >> 8) | (tmp << 8);
			}
	}
};

/**
* Write 16 bytes X 4 pixel textures, assume src pixels are packed but not mipmaplevel
*/
struct texel_16bX4_format {
	template<size_t block_size>
	static void copy_mipmap_level(void *dst, void *src, size_t row_count, size_t width_in_block, size_t dst_pitch_in_block, size_t src_pitch_in_block)
	{
		unsigned short *casted_dst = (unsigned short *)dst, *casted_src = (unsigned short *)src;
		for (unsigned row = 0; row < row_count; row++)
			for (int j = 0; j < width_in_block * 4; j++)
			{
				u16 tmp = casted_src[row * src_pitch_in_block * 4 + j];
				casted_dst[row * dst_pitch_in_block * 4 + j] = (tmp >> 8) | (tmp << 8);
			}
	}
};

/**
 * Texture upload template.
 *
 * Source textures are stored as following (for power of 2 textures):
 * - For linear texture every mipmap level share rowpitch (which is the one of mipmap 0). This means that for non 0 mipmap there's padding between row.
 * - For swizzled texture row pitch is texture width X pixel/block size. There's not padding between row.
 * - There is no padding between 2 mipmap levels. This means that next mipmap level starts at offset rowpitch X row count
 * - Cubemap images are 128 bytes aligned.
 *
 * The template iterates over all depth (including cubemap) and over all mipmaps.
 * The alignment is 256 for mipmap levels and 512 for depth (TODO: make this customisable for Vulkan ?)
 * The template takes a struct with a "copy_mipmap_level" static function that copy the given mipmap level and returns the offset to add to the src buffer for next
 * mipmap level (to allow same code for packed/non packed texels)
 */
template <typename T, bool padded_row, size_t block_size_in_bytes, size_t block_edge_in_texel>
std::vector<MipmapLevelInfo> copy_texture_data(void *dst, const void *src, size_t width_in_texel, size_t height_in_texel, size_t depth, size_t mipmap_count)
{
	std::vector<MipmapLevelInfo> Result;
	size_t offsetInDst = 0, offsetInSrc = 0;
	size_t texture_height_in_block = (height_in_texel + block_edge_in_texel - 1) / block_edge_in_texel;
	size_t texture_width_in_block = (width_in_texel + block_edge_in_texel - 1) / block_edge_in_texel;
	for (unsigned depth_level = 0; depth_level < depth; depth_level++)
	{
		size_t miplevel_height_in_block = texture_height_in_block, miplevel_width_in_block = texture_width_in_block;
		for (unsigned mip_level = 0; mip_level < mipmap_count; mip_level++)
		{
			size_t dst_pitch = align(miplevel_width_in_block * block_size_in_bytes, 256) / block_size_in_bytes;

			MipmapLevelInfo currentMipmapLevelInfo = {};
			currentMipmapLevelInfo.offset = offsetInDst;
			currentMipmapLevelInfo.height = miplevel_height_in_block * block_edge_in_texel;
			currentMipmapLevelInfo.width = miplevel_width_in_block * block_edge_in_texel;
			currentMipmapLevelInfo.rowPitch = dst_pitch * block_size_in_bytes;
			Result.push_back(currentMipmapLevelInfo);

			if (!padded_row)
			{
				T::template copy_mipmap_level<block_size_in_bytes>((char*)dst + offsetInDst, (char*)src + offsetInSrc, miplevel_height_in_block, miplevel_width_in_block, dst_pitch, miplevel_width_in_block);
				offsetInSrc += miplevel_height_in_block * miplevel_width_in_block * block_size_in_bytes;
			}
			else
			{
				T::template copy_mipmap_level<block_size_in_bytes>((char*)dst + offsetInDst, (char*)src + offsetInSrc, miplevel_height_in_block, miplevel_width_in_block, dst_pitch, texture_width_in_block);
				offsetInSrc += miplevel_height_in_block * texture_width_in_block * block_size_in_bytes;
			}
			offsetInDst += align(miplevel_height_in_block * dst_pitch * block_size_in_bytes, 512);
			miplevel_height_in_block = MAX2(miplevel_height_in_block / 2, 1);
			miplevel_width_in_block = MAX2(miplevel_width_in_block / 2, 1);
		}
		offsetInSrc = align(offsetInSrc, 128);
	}
	return Result;
}

/**
 * A texture is stored as an array of blocks, where a block is a pixel for standard texture
 * but is a structure containing several pixels for compressed format
 */
size_t get_texture_block_size(u32 format)
{
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8: return 1;
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_R5G6B5: return 2;
	case CELL_GCM_TEXTURE_A8R8G8B8: return 4;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1: return 8;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23: return 16;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45: return 16;
	case CELL_GCM_TEXTURE_G8B8: return 2;
	case CELL_GCM_TEXTURE_R6G5B5:
	case CELL_GCM_TEXTURE_DEPTH24_D8:
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT: return 4;
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
	case CELL_GCM_TEXTURE_X16: return 2;
	case CELL_GCM_TEXTURE_Y16_X16: return 4;
	case CELL_GCM_TEXTURE_R5G5B5A1: return 2;
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT: return 8;
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT: return 16;
	case CELL_GCM_TEXTURE_X32_FLOAT: return 4;
	case CELL_GCM_TEXTURE_D1R5G5B5: return 2;
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_D8R8G8B8:
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8: return 4;
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
	default:
		LOG_ERROR(RSX, "Unimplemented Texture format : 0x%x", format);
		return 0;
	}
}

size_t get_texture_block_edge(u32 format)
{
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8:
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_R5G6B5:
	case CELL_GCM_TEXTURE_A8R8G8B8: return 1;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45: return 4;
	case CELL_GCM_TEXTURE_G8B8:
	case CELL_GCM_TEXTURE_R6G5B5:
	case CELL_GCM_TEXTURE_DEPTH24_D8:
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
	case CELL_GCM_TEXTURE_X16:
	case CELL_GCM_TEXTURE_Y16_X16:
	case CELL_GCM_TEXTURE_R5G5B5A1:
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
	case CELL_GCM_TEXTURE_X32_FLOAT:
	case CELL_GCM_TEXTURE_D1R5G5B5:
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_D8R8G8B8: return 1;
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8: return 2;
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
	default:
		LOG_ERROR(RSX, "Unimplemented Texture format : 0x%x", format);
		return 0;
	}
}
}


size_t get_placed_texture_storage_size(const rsx::texture &texture, size_t rowPitchAlignement)
{
	size_t w = texture.width(), h = texture.height();

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	size_t blockEdge = get_texture_block_edge(format);
	size_t blockSizeInByte = get_texture_block_size(format);

	size_t heightInBlocks = (h + blockEdge - 1) / blockEdge;
	size_t widthInBlocks = (w + blockEdge - 1) / blockEdge;

	size_t rowPitch = align(blockSizeInByte * widthInBlocks, rowPitchAlignement);

	return rowPitch * heightInBlocks * (texture.cubemap() ? 6 : 1) * 2; // * 2 for mipmap levels
}

std::vector<MipmapLevelInfo> upload_placed_texture(const rsx::texture &texture, size_t rowPitchAlignement, void* textureData)
{
	size_t w = texture.width(), h = texture.height();
	size_t depth = texture.depth();
	if (depth == 0) depth = 1;
	if (texture.cubemap()) depth *= 6;

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

	std::vector<MipmapLevelInfo> mipInfos;

	const u32 texaddr = rsx::get_address(texture.offset(), texture.location());
	auto pixels = vm::ps3::_ptr<const u8>(texaddr);
	bool is_swizzled = !(texture.format() & CELL_GCM_TEXTURE_LN);
	switch (format)
	{
	case CELL_GCM_TEXTURE_A8R8G8B8:
		if (is_swizzled)
			return copy_texture_data<texel_rgba_swizzled, false, 4, 1>(textureData, pixels, w, h, depth, texture.mipmap());
		else
			return copy_texture_data<texel_rgba, true, 4, 1>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_R5G6B5:
		if (is_swizzled)
			return copy_texture_data<texel_16b_swizzled, false, 2, 1>(textureData, pixels, w, h, depth, texture.mipmap());
		else
			return copy_texture_data<texel_16b_format, true, 2, 1>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		return copy_texture_data<texel_16bX4_format, true, 8, 1>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		if (is_swizzled)
			return copy_texture_data<texel_bc_format, false, 8, 4>(textureData, pixels, w, h, depth, texture.mipmap());
		else
			return copy_texture_data<texel_bc_format, true, 8, 4>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
		if (is_swizzled)
			return copy_texture_data<texel_bc_format, false, 16, 4>(textureData, pixels, w, h, depth, texture.mipmap());
		else
			return copy_texture_data<texel_bc_format, true, 16, 4>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		if (is_swizzled)
			return copy_texture_data<texel_bc_format, false, 16, 4>(textureData, pixels, w, h, depth, texture.mipmap());
		else
			return copy_texture_data<texel_bc_format, true, 16, 4>(textureData, pixels, w, h, depth, texture.mipmap());
	case CELL_GCM_TEXTURE_B8:
		return copy_texture_data<texel_rgba, true, 1, 1>(textureData, pixels, w, h, depth, texture.mipmap());
	default:
		return copy_texture_data<texel_rgba, true, 4, 1>(textureData, pixels, w, h, depth, texture.mipmap());
	}
}

size_t get_texture_size(const rsx::texture &texture)
{
	size_t w = texture.width(), h = texture.height();

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	// TODO: Take mipmaps into account
	switch (format)
	{
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
	default:
		LOG_ERROR(RSX, "Unimplemented Texture format : 0x%x", format);
		return 0;
	case CELL_GCM_TEXTURE_B8:
		return w * h;
	case CELL_GCM_TEXTURE_A1R5G5B5:
		return w * h * 2;
	case CELL_GCM_TEXTURE_A4R4G4B4:
		return w * h * 2;
	case CELL_GCM_TEXTURE_R5G6B5:
		return w * h * 2;
	case CELL_GCM_TEXTURE_A8R8G8B8:
		return w * h * 4;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		return w * h / 6;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
		return w * h / 4;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		return w * h / 4;
	case CELL_GCM_TEXTURE_G8B8:
		return w * h * 2;
	case CELL_GCM_TEXTURE_R6G5B5:
		return w * h * 2;
	case CELL_GCM_TEXTURE_DEPTH24_D8:
		return w * h * 4;
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
		return w * h * 4;
	case CELL_GCM_TEXTURE_DEPTH16:
		return w * h * 2;
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
		return w * h * 2;
	case CELL_GCM_TEXTURE_X16:
		return w * h * 2;
	case CELL_GCM_TEXTURE_Y16_X16:
		return w * h * 4;
	case CELL_GCM_TEXTURE_R5G5B5A1:
		return w * h * 2;
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		return w * h * 8;
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
		return w * h * 16;
	case CELL_GCM_TEXTURE_X32_FLOAT:
		return w * h * 4;
	case CELL_GCM_TEXTURE_D1R5G5B5:
		return w * h * 2;
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
		return w * h * 4;
	case CELL_GCM_TEXTURE_D8R8G8B8:
		return w * h * 4;
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
		return w * h * 4;
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
		return w * h * 4;
	}
}
