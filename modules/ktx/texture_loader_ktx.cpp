/**************************************************************************/
/*  texture_loader_ktx.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "texture_loader_ktx.h"

#include "core/io/file_access.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_mmap.h"
#include "core/object/class_db.h"
#include "core/os/load_thread_pool.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"

#include <ktx.h>
#include <vk_format.h>

GODOT_GCC_WARNING_PUSH
GODOT_GCC_WARNING_IGNORE("-Wenum-conversion")
GODOT_GCC_WARNING_IGNORE("-Wshadow")
GODOT_GCC_WARNING_IGNORE("-Wunused-value")
#include <transcoder/basisu_transcoder.h>
GODOT_GCC_WARNING_POP

#include <atomic>
#include <cstring>

namespace {

constexpr uint8_t kKtx2Magic[12] = {
	0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

// Direct zero-stream KTX2 BasisU (UASTC / ETC1S / UASTC HDR) transcoder path.
// Preserves native GPU block compression for both sRGB and Linear KTX2 textures
// and parallelizes large multi-mip transcode across LoadThreadPool (<= 3 threads).
Ref<Image> try_load_basis_ktx2_direct(const uint8_t *p_data, size_t p_size, Error *r_error) {
	if (!p_data || p_size < 80 || std::memcmp(p_data, kKtx2Magic, 12) != 0) {
		return Ref<Image>();
	}
	uint32_t vk_format = 0;
	std::memcpy(&vk_format, p_data + 12, 4);
	if (vk_format != VK_FORMAT_UNDEFINED) {
		return Ref<Image>();
	}
	if (p_size > UINT32_MAX) {
		return Ref<Image>();
	}

	basist::ktx2_transcoder transcoder;
	if (!transcoder.init(p_data, static_cast<uint32_t>(p_size))) {
		return Ref<Image>();
	}
	if (transcoder.get_faces() != 1 || transcoder.get_layers() != 0 || transcoder.is_video()) {
		return Ref<Image>();
	}

	const uint32_t width = transcoder.get_width();
	const uint32_t height = transcoder.get_height();
	if (width == 0 || height == 0) {
		return Ref<Image>();
	}

	RS *rs = RS::get_singleton();
	const bool astc_supported = rs && rs->has_os_feature("astc");
	const bool astc_hdr_supported = rs && rs->has_os_feature("astc_hdr");
	const bool bptc_supported = rs && rs->has_os_feature("bptc");
	const bool rgtc_supported = rs && rs->has_os_feature("rgtc");
	const bool s3tc_supported = rs && rs->has_os_feature("s3tc");
	const bool etc2_supported = rs && rs->has_os_feature("etc2");

	const bool block_aligned = (width % 4 == 0) && (height % 4 == 0);
	const bool is_srgb = (transcoder.get_dfd_transfer_func() == basist::KTX2_KHR_DF_TRANSFER_SRGB);
	const bool has_alpha = transcoder.get_has_alpha();

	basist::transcoder_texture_format basisu_format = basist::transcoder_texture_format::cTFRGBA32;
	Image::Format image_format = Image::FORMAT_RGBA8;

	if (transcoder.is_hdr()) {
		if (!transcoder.is_hdr_4x4()) {
			return Ref<Image>();
		}
		if (block_aligned && bptc_supported) {
			basisu_format = basist::transcoder_texture_format::cTFBC6H;
			image_format = Image::FORMAT_BPTC_RGBFU;
		} else if (block_aligned && astc_hdr_supported) {
			basisu_format = basist::transcoder_texture_format::cTFASTC_HDR_4x4_RGBA;
			image_format = Image::FORMAT_ASTC_4x4_HDR;
		} else {
			basisu_format = basist::transcoder_texture_format::cTFRGB_9E5;
			image_format = Image::FORMAT_RGBE9995;
		}
	} else if (block_aligned) {
		const basist::ktx2_df_channel_id ch0 = transcoder.get_dfd_channel_id0();
		const basist::ktx2_df_channel_id ch1 = transcoder.get_dfd_channel_id1();
		const uint32_t samples = transcoder.get_dfd_total_samples();

		// Detect true 1-channel or 2-channel linear data maps when authored as R / RG.
		const bool is_single_r = !is_srgb && !has_alpha && samples == 1 &&
				(ch0 == basist::KTX2_DF_CHANNEL_ETC1S_RRR || ch0 == basist::KTX2_DF_CHANNEL_UASTC_RRR);
		const bool is_dual_rg = !is_srgb &&
				((samples == 1 && (ch0 == basist::KTX2_DF_CHANNEL_UASTC_RG || ch0 == basist::KTX2_DF_CHANNEL_UASTC_RRRG)) ||
						(samples == 2 && ch0 == basist::KTX2_DF_CHANNEL_ETC1S_RRR && ch1 == basist::KTX2_DF_CHANNEL_ETC1S_GGG));

		if (is_single_r && (rgtc_supported || (!astc_supported && etc2_supported))) {
			if (rgtc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC4_R;
				image_format = Image::FORMAT_RGTC_R;
			} else {
				basisu_format = basist::transcoder_texture_format::cTFETC2_EAC_R11;
				image_format = Image::FORMAT_ETC2_R11;
			}
		} else if (is_dual_rg && (rgtc_supported || (!astc_supported && etc2_supported))) {
			if (rgtc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC5_RG;
				image_format = Image::FORMAT_RGTC_RG;
			} else {
				basisu_format = basist::transcoder_texture_format::cTFETC2_EAC_RG11;
				image_format = Image::FORMAT_ETC2_RG11;
			}
		} else if (has_alpha) {
			if (astc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
				image_format = Image::FORMAT_ASTC_4x4;
			} else if (bptc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC7_RGBA;
				image_format = Image::FORMAT_BPTC_RGBA;
			} else if (s3tc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC3_RGBA;
				image_format = Image::FORMAT_DXT5;
			} else if (etc2_supported) {
				basisu_format = basist::transcoder_texture_format::cTFETC2_RGBA;
				image_format = Image::FORMAT_ETC2_RGBA8;
			}
		} else {
			if (astc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
				image_format = Image::FORMAT_ASTC_4x4;
			} else if (bptc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC7_M6_OPAQUE_ONLY;
				image_format = Image::FORMAT_BPTC_RGBA;
			} else if (s3tc_supported) {
				basisu_format = basist::transcoder_texture_format::cTFBC1_RGB;
				image_format = Image::FORMAT_DXT1;
			} else if (etc2_supported) {
				basisu_format = basist::transcoder_texture_format::cTFETC1_RGB;
				image_format = Image::FORMAT_ETC2_RGB8;
			}
		}
	}

	if (!transcoder.start_transcoding()) {
		return Ref<Image>();
	}

	const uint32_t expected_full_levels = static_cast<uint32_t>(Image::get_image_required_mipmaps(width, height, image_format)) + 1u;
	const uint32_t authored_levels = transcoder.get_levels();
	const bool use_mipmaps = authored_levels >= expected_full_levels && expected_full_levels > 1u;
	const uint32_t levels_to_load = use_mipmaps ? expected_full_levels : 1u;

	Vector<uint8_t> out_data;
	const int64_t out_size = Image::get_image_data_size(width, height, image_format, use_mipmaps);
	if (out_data.resize(out_size) != OK) {
		return Ref<Image>();
	}
	uint8_t *dst = out_data.ptrw();
	const bool compressed_out = Image::is_format_compressed(image_format);

	auto transcode_one_level = [&](uint32_t p_level, basist::ktx2_transcoder_state *p_state) -> bool {
		basist::ktx2_image_level_info level_info;
		if (!transcoder.get_image_level_info(level_info, p_level, 0, 0)) {
			return false;
		}
		const uint32_t count = compressed_out ? level_info.m_total_blocks : (level_info.m_orig_width * level_info.m_orig_height);
		const int64_t ofs = Image::get_image_mipmap_offset(width, height, image_format, p_level);
		return transcoder.transcode_image_level(
				p_level, 0, 0, dst + ofs, count, basisu_format,
				basist::cDecodeFlagsHighQuality, 0, 0, -1, -1, p_state);
	};

	std::atomic<bool> failed{ false };
	if (levels_to_load > 1 && width * height >= 512u * 512u) {
		LoadThreadPool::get_singleton()->parallel_for(static_cast<int>(levels_to_load), [&](int p_idx) {
			if (failed.load(std::memory_order_relaxed)) {
				return;
			}
			basist::ktx2_transcoder_state state;
			state.clear();
			if (!transcode_one_level(static_cast<uint32_t>(p_idx), &state)) {
				failed.store(true, std::memory_order_relaxed);
			}
		});
	} else {
		for (uint32_t i = 0; i < levels_to_load; i++) {
			if (!transcode_one_level(i, nullptr)) {
				failed.store(true, std::memory_order_relaxed);
				break;
			}
		}
	}

	if (failed.load(std::memory_order_relaxed)) {
		return Ref<Image>();
	}

	Ref<Image> img = Image::create_from_data(width, height, use_mipmaps, image_format, out_data);
	if (img.is_valid() && r_error) {
		*r_error = OK;
	}
	return img;
}

} // namespace

KTX_error_code ktx_read(ktxStream *stream, void *dst, const ktx_size_t count) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	(*f)->get_buffer(reinterpret_cast<uint8_t *>(dst), count);
	return KTX_SUCCESS;
}

KTX_error_code ktx_skip(ktxStream *stream, const ktx_size_t count) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	(*f)->seek((*f)->get_position() + count);
	return KTX_SUCCESS;
}

KTX_error_code ktx_write(ktxStream *stream, const void *src, const ktx_size_t size, const ktx_size_t count) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	(*f)->store_buffer(reinterpret_cast<const uint8_t *>(src), size * count);
	return KTX_SUCCESS;
}

KTX_error_code ktx_getpos(ktxStream *stream, ktx_off_t *const offset) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	*offset = (*f)->get_position();
	return KTX_SUCCESS;
}

KTX_error_code ktx_setpos(ktxStream *stream, const ktx_off_t offset) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	(*f)->seek(offset);
	return KTX_SUCCESS;
}

KTX_error_code ktx_getsize(ktxStream *stream, ktx_size_t *const size) {
	Ref<FileAccess> *f = reinterpret_cast<Ref<FileAccess> *>(stream->data.custom_ptr.address);
	*size = (*f)->get_length();
	return KTX_SUCCESS;
}

void ktx_destruct(ktxStream *stream) {
	(void)stream;
}

static Ref<Image> load_from_ktx_texture(ktxTexture *ktx_texture, Error *r_error) {
	if (ktx_texture->numDimensions != 2) {
		ktxTexture_Destroy(ktx_texture);
		ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported non-2D KTX texture file.");
	}

	if (ktx_texture->isCubemap || ktx_texture->numFaces != 1) {
		ktxTexture_Destroy(ktx_texture);
		ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported cube map KTX texture file.");
	}

	if (ktx_texture->isArray) {
		ktxTexture_Destroy(ktx_texture);
		ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported array KTX texture file.");
	}

	uint32_t width = ktx_texture->baseWidth;
	uint32_t height = ktx_texture->baseHeight;
	uint32_t mipmaps = ktx_texture->numLevels;
	Image::Format format;

	switch (ktx_texture->classId) {
		case ktxTexture1_c:
			switch (((ktxTexture1 *)ktx_texture)->glInternalformat) {
				case GL_LUMINANCE:
					format = Image::FORMAT_L8;
					break;
				case GL_LUMINANCE_ALPHA:
					format = Image::FORMAT_LA8;
					break;
				case GL_SRGB8:
				case GL_RGB8:
					format = Image::FORMAT_RGB8;
					break;
				case GL_SRGB8_ALPHA8:
				case GL_RGBA8:
					format = Image::FORMAT_RGBA8;
					break;
				case GL_R8:
				case GL_R8UI:
					format = Image::FORMAT_R8;
					break;
				case GL_RG8:
					format = Image::FORMAT_RG8;
					break;
				case GL_RGBA4:
					format = Image::FORMAT_RGBA4444;
					break;
				case GL_RGB565:
					format = Image::FORMAT_RGB565;
					break;
				case GL_R32F:
					format = Image::FORMAT_RF;
					break;
				case GL_RG32F:
					format = Image::FORMAT_RGF;
					break;
				case GL_RGB32F:
					format = Image::FORMAT_RGBF;
					break;
				case GL_RGBA32F:
					format = Image::FORMAT_RGBAF;
					break;
				case GL_R16F:
					format = Image::FORMAT_RH;
					break;
				case GL_RG16F:
					format = Image::FORMAT_RGH;
					break;
				case GL_RGB16F:
					format = Image::FORMAT_RGBH;
					break;
				case GL_RGBA16F:
					format = Image::FORMAT_RGBAH;
					break;
				case GL_RGB9_E5:
					format = Image::FORMAT_RGBE9995;
					break;
				case GL_R16:
					format = Image::FORMAT_R16;
					break;
				case GL_RG16:
					format = Image::FORMAT_RG16;
					break;
				case GL_RGB16:
					format = Image::FORMAT_RGB16;
					break;
				case GL_RGBA16:
					format = Image::FORMAT_RGBA16;
					break;
				case GL_R16UI:
					format = Image::FORMAT_R16I;
					break;
				case GL_RG16UI:
					format = Image::FORMAT_RG16I;
					break;
				case GL_RGB16UI:
					format = Image::FORMAT_RGB16I;
					break;
				case GL_RGBA16UI:
					format = Image::FORMAT_RGBA16I;
					break;
				case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
				case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
					format = Image::FORMAT_DXT1;
					break;
				case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
					format = Image::FORMAT_DXT3;
					break;
				case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
					format = Image::FORMAT_DXT5;
					break;
				case GL_COMPRESSED_RED_RGTC1:
					format = Image::FORMAT_RGTC_R;
					break;
				case GL_COMPRESSED_RG_RGTC2:
					format = Image::FORMAT_RGTC_RG;
					break;
				case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT:
					format = Image::FORMAT_BPTC_RGBFU;
					break;
				case GL_COMPRESSED_RGBA_BPTC_UNORM:
					format = Image::FORMAT_BPTC_RGBA;
					break;
				case GL_ETC1_RGB8_OES:
					format = Image::FORMAT_ETC;
					break;
				case GL_COMPRESSED_R11_EAC:
					format = Image::FORMAT_ETC2_R11;
					break;
				case GL_COMPRESSED_RG11_EAC:
					format = Image::FORMAT_ETC2_RG11;
					break;
				case GL_COMPRESSED_RGB8_ETC2:
					format = Image::FORMAT_ETC2_RGB8;
					break;
				case GL_COMPRESSED_RGBA8_ETC2_EAC:
					format = Image::FORMAT_ETC2_RGBA8;
					break;
				case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
				case GL_COMPRESSED_RGBA_ASTC_4x4_KHR:
					format = Image::FORMAT_ASTC_4x4;
					break;
				case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR:
				case GL_COMPRESSED_RGBA_ASTC_8x8_KHR:
					format = Image::FORMAT_ASTC_8x8;
					break;
				default:
					ktxTexture_Destroy(ktx_texture);
					ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported format " + itos(((ktxTexture1 *)ktx_texture)->glInternalformat) + " of KTX1 texture file.");
			}
			break;
		case ktxTexture2_c: {
			ktxTexture2 *ktx_texture2 = reinterpret_cast<ktxTexture2 *>(ktx_texture);
			if (ktx_texture2->vkFormat == VK_FORMAT_UNDEFINED) {
				if (!ktxTexture2_NeedsTranscoding(ktx_texture2)) {
					ktxTexture_Destroy(ktx_texture);
					ERR_FAIL_V_MSG(Ref<Image>(), "Invalid VK_FORMAT_UNDEFINED of KTX2 texture file.");
				}
				const bool block_aligned = (width % 4 == 0) && (height % 4 == 0);
				const bool is_srgb = (ktxTexture2_GetOETF_e(ktx_texture2) == KHR_DF_TRANSFER_SRGB);
				RS *rs = RS::get_singleton();
				const bool has_astc = block_aligned && rs && rs->has_os_feature("astc");
				const bool has_bptc = block_aligned && rs && rs->has_os_feature("bptc");
				const bool has_rgtc = block_aligned && rs && rs->has_os_feature("rgtc");
				const bool has_s3tc = block_aligned && rs && rs->has_os_feature("s3tc");
				const bool has_etc2 = block_aligned && rs && rs->has_os_feature("etc2");

				ktx_transcode_fmt_e ktxfmt = KTX_TTF_RGBA32;
				switch (ktxTexture2_GetNumComponents(ktx_texture2)) {
					case 1: {
						if (!is_srgb && has_rgtc) {
							ktxfmt = KTX_TTF_BC4_R;
						} else if (!is_srgb && !has_astc && has_etc2) {
							ktxfmt = KTX_TTF_ETC2_EAC_R11;
						} else if (has_astc) {
							ktxfmt = KTX_TTF_ASTC_4x4_RGBA;
						} else if (has_bptc) {
							ktxfmt = KTX_TTF_BC7_RGBA;
						} else if (has_s3tc) {
							ktxfmt = KTX_TTF_BC1_RGB;
						} else if (has_etc2) {
							ktxfmt = KTX_TTF_ETC1_RGB;
						}
						break;
					}
					case 2: {
						if (!is_srgb && has_rgtc) {
							ktxfmt = KTX_TTF_BC5_RG;
						} else if (!is_srgb && !has_astc && has_etc2) {
							ktxfmt = KTX_TTF_ETC2_EAC_RG11;
						} else if (has_astc) {
							ktxfmt = KTX_TTF_ASTC_4x4_RGBA;
						} else if (has_bptc) {
							ktxfmt = KTX_TTF_BC7_RGBA;
						} else if (has_s3tc) {
							ktxfmt = KTX_TTF_BC3_RGBA;
						} else if (has_etc2) {
							ktxfmt = KTX_TTF_ETC2_RGBA;
						}
						break;
					}
					case 3: {
						if (has_astc) {
							ktxfmt = KTX_TTF_ASTC_4x4_RGBA;
						} else if (has_bptc) {
							ktxfmt = KTX_TTF_BC7_RGBA;
						} else if (has_s3tc) {
							ktxfmt = KTX_TTF_BC1_RGB;
						} else if (has_etc2) {
							ktxfmt = KTX_TTF_ETC1_RGB;
						}
						break;
					}
					case 4: {
						if (has_astc) {
							ktxfmt = KTX_TTF_ASTC_4x4_RGBA;
						} else if (has_bptc) {
							ktxfmt = KTX_TTF_BC7_RGBA;
						} else if (has_s3tc) {
							ktxfmt = KTX_TTF_BC3_RGBA;
						} else if (has_etc2) {
							ktxfmt = KTX_TTF_ETC2_RGBA;
						}
						break;
					}
					default: {
						ktxTexture_Destroy(ktx_texture);
						ERR_FAIL_V_MSG(Ref<Image>(), "Invalid components of KTX2 texture file.");
					}
				}
				KTX_error_code result = ktxTexture2_TranscodeBasis(ktx_texture2, ktxfmt, 0);
				if (result != KTX_SUCCESS) {
					ktxTexture_Destroy(ktx_texture);
					ERR_FAIL_V_MSG(Ref<Image>(), "Failed to transcode KTX2 texture file.");
				}
			}
			switch (ktx_texture2->vkFormat) {
				case VK_FORMAT_R8_UNORM:
				case VK_FORMAT_R8_SRGB:
				case VK_FORMAT_R8_UINT:
					format = Image::FORMAT_R8;
					break;
				case VK_FORMAT_R8G8_UNORM:
				case VK_FORMAT_R8G8_SRGB:
				case VK_FORMAT_R8G8_UINT:
					format = Image::FORMAT_RG8;
					break;
				case VK_FORMAT_R8G8B8_UNORM:
				case VK_FORMAT_R8G8B8_SRGB:
				case VK_FORMAT_R8G8B8_UINT:
					format = Image::FORMAT_RGB8;
					break;
				case VK_FORMAT_R8G8B8A8_UNORM:
				case VK_FORMAT_R8G8B8A8_SRGB:
				case VK_FORMAT_R8G8B8A8_UINT:
					format = Image::FORMAT_RGBA8;
					break;
				case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
					format = Image::FORMAT_RGBA4444;
					break;
				case VK_FORMAT_R5G6B5_UNORM_PACK16:
					format = Image::FORMAT_RGB565;
					break;
				case VK_FORMAT_R32_SFLOAT:
					format = Image::FORMAT_RF;
					break;
				case VK_FORMAT_R32G32_SFLOAT:
					format = Image::FORMAT_RGF;
					break;
				case VK_FORMAT_R32G32B32_SFLOAT:
					format = Image::FORMAT_RGBF;
					break;
				case VK_FORMAT_R32G32B32A32_SFLOAT:
					format = Image::FORMAT_RGBAF;
					break;
				case VK_FORMAT_R16_SFLOAT:
					format = Image::FORMAT_RH;
					break;
				case VK_FORMAT_R16G16_SFLOAT:
					format = Image::FORMAT_RGH;
					break;
				case VK_FORMAT_R16G16B16_SFLOAT:
					format = Image::FORMAT_RGBH;
					break;
				case VK_FORMAT_R16G16B16A16_SFLOAT:
					format = Image::FORMAT_RGBAH;
					break;
				case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
					format = Image::FORMAT_RGBE9995;
					break;
				case VK_FORMAT_R16_UNORM:
					format = Image::FORMAT_R16;
					break;
				case VK_FORMAT_R16G16_UNORM:
					format = Image::FORMAT_RG16;
					break;
				case VK_FORMAT_R16G16B16_UNORM:
					format = Image::FORMAT_RGB16;
					break;
				case VK_FORMAT_R16G16B16A16_UNORM:
					format = Image::FORMAT_RGBA16;
					break;
				case VK_FORMAT_R16_UINT:
					format = Image::FORMAT_R16I;
					break;
				case VK_FORMAT_R16G16_UINT:
					format = Image::FORMAT_RG16I;
					break;
				case VK_FORMAT_R16G16B16_UINT:
					format = Image::FORMAT_RGB16I;
					break;
				case VK_FORMAT_R16G16B16A16_UINT:
					format = Image::FORMAT_RGBA16I;
					break;
				case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
				case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
				case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
				case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
					format = Image::FORMAT_DXT1;
					break;
				case VK_FORMAT_BC2_UNORM_BLOCK:
				case VK_FORMAT_BC2_SRGB_BLOCK:
					format = Image::FORMAT_DXT3;
					break;
				case VK_FORMAT_BC3_UNORM_BLOCK:
				case VK_FORMAT_BC3_SRGB_BLOCK:
					format = Image::FORMAT_DXT5;
					break;
				case VK_FORMAT_BC4_UNORM_BLOCK:
					format = Image::FORMAT_RGTC_R;
					break;
				case VK_FORMAT_BC5_UNORM_BLOCK:
					format = Image::FORMAT_RGTC_RG;
					break;
				case VK_FORMAT_BC6H_UFLOAT_BLOCK:
					format = Image::FORMAT_BPTC_RGBFU;
					break;
				case VK_FORMAT_BC6H_SFLOAT_BLOCK:
					format = Image::FORMAT_BPTC_RGBF;
					break;
				case VK_FORMAT_BC7_UNORM_BLOCK:
				case VK_FORMAT_BC7_SRGB_BLOCK:
					format = Image::FORMAT_BPTC_RGBA;
					break;
				case VK_FORMAT_EAC_R11_UNORM_BLOCK:
					format = Image::FORMAT_ETC2_R11;
					break;
				case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
					format = Image::FORMAT_ETC2_RG11;
					break;
				case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
				case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
					format = Image::FORMAT_ETC2_RGB8;
					break;
				case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
				case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
					format = Image::FORMAT_ETC2_RGBA8;
					break;
				case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
				case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
					format = Image::FORMAT_ASTC_4x4;
					break;
				case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK:
					format = Image::FORMAT_ASTC_4x4_HDR;
					break;
				case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
				case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
					format = Image::FORMAT_ASTC_8x8;
					break;
				case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK:
					format = Image::FORMAT_ASTC_8x8_HDR;
					break;
				default:
					ktxTexture_Destroy(ktx_texture);
					ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported format " + itos(((ktxTexture2 *)ktx_texture)->vkFormat) + " of KTX2 texture file.");
			}
			break;
		}
		default:
			ktxTexture_Destroy(ktx_texture);
			ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported version KTX texture file.");
	}

	const uint32_t expected_full_levels = static_cast<uint32_t>(Image::get_image_required_mipmaps(width, height, format)) + 1u;
	if (mipmaps > 1 && mipmaps < expected_full_levels) {
		mipmaps = 1;
	}

	int pixel_size = Image::get_format_pixel_size(format);
	int pixel_rshift = Image::get_format_pixel_rshift(format);
	int block = Image::get_format_block_size(format);
	int minw, minh;
	Image::get_format_min_pixel_size(format, minw, minh);
	int w = width;
	int h = height;
	for (uint32_t i = 0; i < mipmaps; ++i) {
		ktx_size_t mip_size = ktxTexture_GetImageSize(ktx_texture, i);
		size_t bw = w % block != 0 ? w + (block - w % block) : w;
		size_t bh = h % block != 0 ? h + (block - h % block) : h;
		size_t s = bw * bh;
		s *= pixel_size;
		s >>= pixel_rshift;
		if (mip_size != static_cast<ktx_size_t>(s)) {
			if (!i) {
				ktxTexture_Destroy(ktx_texture);
				ERR_FAIL_V_MSG(Ref<Image>(), "Unsupported padded KTX texture file.");
			}
			mipmaps = 1;
			break;
		}
		w = MAX(minw, w >> 1);
		h = MAX(minh, h >> 1);
	}

	Vector<uint8_t> src_data;
	int64_t total_bytes = 0;
	for (uint32_t i = 0; i < mipmaps; ++i) {
		total_bytes += static_cast<int64_t>(ktxTexture_GetImageSize(ktx_texture, i));
	}
	src_data.resize(total_bytes);
	uint8_t *dst_ptr = src_data.ptrw();
	int64_t write_pos = 0;

	for (uint32_t i = 0; i < mipmaps; ++i) {
		ktx_size_t mip_size = ktxTexture_GetImageSize(ktx_texture, i);
		ktx_size_t offset;
		if (ktxTexture_GetImageOffset(ktx_texture, i, 0, 0, &offset) != KTX_SUCCESS) {
			ktxTexture_Destroy(ktx_texture);
			ERR_FAIL_V_MSG(Ref<Image>(), "Invalid KTX texture file.");
		}
		memcpy(dst_ptr + write_pos, ktxTexture_GetData(ktx_texture) + offset, mip_size);
		write_pos += static_cast<int64_t>(mip_size);
	}

	Ref<Image> img = memnew(Image(width, height, mipmaps > 1, format, src_data));
	if (r_error) {
		*r_error = OK;
	}

	ktxTexture_Destroy(ktx_texture);
	return img;
}

static Ref<Image> load_from_memory(const uint8_t *p_data, size_t p_size, Error *r_error) {
	if (r_error) {
		*r_error = ERR_FILE_CORRUPT;
	}
	if (!p_data || p_size == 0) {
		return Ref<Image>();
	}
	Ref<Image> direct = try_load_basis_ktx2_direct(p_data, p_size, r_error);
	if (direct.is_valid()) {
		return direct;
	}
	ktxTexture *ktx_texture = nullptr;
	KTX_error_code result = ktxTexture_CreateFromMemory(
			p_data, p_size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx_texture);
	if (result != KTX_SUCCESS || !ktx_texture) {
		ERR_FAIL_V_MSG(Ref<Image>(), "Invalid or unsupported KTX texture file.");
	}
	return load_from_ktx_texture(ktx_texture, r_error);
}

Ref<Resource> ResourceFormatKTX::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	if (r_error) {
		*r_error = ERR_CANT_OPEN;
	}

	FileAccessMMapView view;
	Error err = view.open(p_path);
	ERR_FAIL_COND_V_MSG(err != OK || !view.is_valid(), Ref<Resource>(), "Unable to open KTX texture file '" + p_path + "'.");

	Ref<Image> img = load_from_memory(view.ptr(), view.size(), r_error);
	if (img.is_null()) {
		return Ref<Resource>();
	}
	Ref<ImageTexture> texture = ImageTexture::create_from_image(img);
	return texture;
}

static Ref<Image> _ktx_mem_loader_func(const uint8_t *p_ktx, int p_size) {
	ERR_FAIL_COND_V(!p_ktx || p_size <= 0, Ref<Image>());
	Error err = OK;
	Ref<Image> img = load_from_memory(p_ktx, static_cast<size_t>(p_size), &err);
	ERR_FAIL_COND_V(err != OK, Ref<Image>());
	return img;
}

void ResourceFormatKTX::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("ktx");
	p_extensions->push_back("ktx2");
}

bool ResourceFormatKTX::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "Texture2D");
}

String ResourceFormatKTX::get_resource_type(const String &p_path) const {
	if (p_path.has_extension("ktx") || p_path.has_extension("ktx2")) {
		return "ImageTexture";
	}
	return "";
}

ResourceFormatKTX::ResourceFormatKTX() {
	Image::_ktx_mem_loader_func = _ktx_mem_loader_func;
}
