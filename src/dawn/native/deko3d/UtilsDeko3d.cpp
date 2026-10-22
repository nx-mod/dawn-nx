// Copyright 2026 The Dawn & Tint Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/dawn/native/deko3d/UtilsDeko3d.h"


namespace dawn::native::deko3d {

DkImageFormat ToDkImageFormat(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::R8Unorm:
            return DkImageFormat_R8_Unorm;
        case wgpu::TextureFormat::R8Snorm:
            return DkImageFormat_R8_Snorm;
        case wgpu::TextureFormat::R8Uint:
            return DkImageFormat_R8_Uint;
        case wgpu::TextureFormat::R8Sint:
            return DkImageFormat_R8_Sint;
        case wgpu::TextureFormat::R16Unorm:
            return DkImageFormat_R16_Unorm;
        case wgpu::TextureFormat::R16Snorm:
            return DkImageFormat_R16_Snorm;
        case wgpu::TextureFormat::R16Uint:
            return DkImageFormat_R16_Uint;
        case wgpu::TextureFormat::R16Sint:
            return DkImageFormat_R16_Sint;
        case wgpu::TextureFormat::R16Float:
            return DkImageFormat_R16_Float;
        case wgpu::TextureFormat::RG8Unorm:
            return DkImageFormat_RG8_Unorm;
        case wgpu::TextureFormat::RG8Snorm:
            return DkImageFormat_RG8_Snorm;
        case wgpu::TextureFormat::RG8Uint:
            return DkImageFormat_RG8_Uint;
        case wgpu::TextureFormat::RG8Sint:
            return DkImageFormat_RG8_Sint;
        case wgpu::TextureFormat::R32Float:
            return DkImageFormat_R32_Float;
        case wgpu::TextureFormat::R32Uint:
            return DkImageFormat_R32_Uint;
        case wgpu::TextureFormat::R32Sint:
            return DkImageFormat_R32_Sint;
        case wgpu::TextureFormat::RG16Unorm:
            return DkImageFormat_RG16_Unorm;
        case wgpu::TextureFormat::RG16Snorm:
            return DkImageFormat_RG16_Snorm;
        case wgpu::TextureFormat::RG16Uint:
            return DkImageFormat_RG16_Uint;
        case wgpu::TextureFormat::RG16Sint:
            return DkImageFormat_RG16_Sint;
        case wgpu::TextureFormat::RG16Float:
            return DkImageFormat_RG16_Float;
        case wgpu::TextureFormat::RGBA8Unorm:
            return DkImageFormat_RGBA8_Unorm;
        case wgpu::TextureFormat::RGBA8UnormSrgb:
            return DkImageFormat_RGBA8_Unorm_sRGB;
        case wgpu::TextureFormat::RGBA8Snorm:
            return DkImageFormat_RGBA8_Snorm;
        case wgpu::TextureFormat::RGBA8Uint:
            return DkImageFormat_RGBA8_Uint;
        case wgpu::TextureFormat::RGBA8Sint:
            return DkImageFormat_RGBA8_Sint;
        case wgpu::TextureFormat::BGRA8Unorm:
            return DkImageFormat_BGRA8_Unorm;
        case wgpu::TextureFormat::BGRA8UnormSrgb:
            return DkImageFormat_BGRA8_Unorm_sRGB;
        case wgpu::TextureFormat::RGB10A2Uint:
            return DkImageFormat_RGB10A2_Uint;
        case wgpu::TextureFormat::RGB10A2Unorm:
            return DkImageFormat_RGB10A2_Unorm;
        case wgpu::TextureFormat::RG11B10Ufloat:
            return DkImageFormat_RG11B10_Float;
        case wgpu::TextureFormat::RGB9E5Ufloat:
            return DkImageFormat_E5BGR9_Float;
        case wgpu::TextureFormat::RG32Float:
            return DkImageFormat_RG32_Float;
        case wgpu::TextureFormat::RG32Uint:
            return DkImageFormat_RG32_Uint;
        case wgpu::TextureFormat::RG32Sint:
            return DkImageFormat_RG32_Sint;
        case wgpu::TextureFormat::RGBA16Unorm:
            return DkImageFormat_RGBA16_Unorm;
        case wgpu::TextureFormat::RGBA16Snorm:
            return DkImageFormat_RGBA16_Snorm;
        case wgpu::TextureFormat::RGBA16Uint:
            return DkImageFormat_RGBA16_Uint;
        case wgpu::TextureFormat::RGBA16Sint:
            return DkImageFormat_RGBA16_Sint;
        case wgpu::TextureFormat::RGBA16Float:
            return DkImageFormat_RGBA16_Float;
        case wgpu::TextureFormat::RGBA32Float:
            return DkImageFormat_RGBA32_Float;
        case wgpu::TextureFormat::RGBA32Uint:
            return DkImageFormat_RGBA32_Uint;
        case wgpu::TextureFormat::RGBA32Sint:
            return DkImageFormat_RGBA32_Sint;
        case wgpu::TextureFormat::Stencil8:
            return DkImageFormat_S8;
        case wgpu::TextureFormat::Depth16Unorm:
            return DkImageFormat_Z16;
        case wgpu::TextureFormat::Depth24Plus:
            return DkImageFormat_Z24X8;
        case wgpu::TextureFormat::Depth24PlusStencil8:
            return DkImageFormat_Z24S8;
        case wgpu::TextureFormat::Depth32Float:
            return DkImageFormat_ZF32;
        case wgpu::TextureFormat::Depth32FloatStencil8:
            return DkImageFormat_ZF32_X24S8;
        case wgpu::TextureFormat::BC1RGBAUnorm:
            return DkImageFormat_RGBA_BC1;
        case wgpu::TextureFormat::BC1RGBAUnormSrgb:
            return DkImageFormat_RGBA_BC1_sRGB;
        case wgpu::TextureFormat::BC2RGBAUnorm:
            return DkImageFormat_RGBA_BC2;
        case wgpu::TextureFormat::BC2RGBAUnormSrgb:
            return DkImageFormat_RGBA_BC2_sRGB;
        case wgpu::TextureFormat::BC3RGBAUnorm:
            return DkImageFormat_RGBA_BC3;
        case wgpu::TextureFormat::BC3RGBAUnormSrgb:
            return DkImageFormat_RGBA_BC3_sRGB;
        case wgpu::TextureFormat::BC4RUnorm:
            return DkImageFormat_R_BC4_Unorm;
        case wgpu::TextureFormat::BC4RSnorm:
            return DkImageFormat_R_BC4_Snorm;
        case wgpu::TextureFormat::BC5RGUnorm:
            return DkImageFormat_RG_BC5_Unorm;
        case wgpu::TextureFormat::BC5RGSnorm:
            return DkImageFormat_RG_BC5_Snorm;
        case wgpu::TextureFormat::BC6HRGBUfloat:
            return DkImageFormat_RGBA_BC6H_UF16_Float;
        case wgpu::TextureFormat::BC6HRGBFloat:
            return DkImageFormat_RGBA_BC6H_SF16_Float;
        case wgpu::TextureFormat::BC7RGBAUnorm:
            return DkImageFormat_RGBA_BC7_Unorm;
        case wgpu::TextureFormat::BC7RGBAUnormSrgb:
            return DkImageFormat_RGBA_BC7_Unorm_sRGB;
        case wgpu::TextureFormat::ETC2RGB8Unorm:
            return DkImageFormat_RGB_ETC2;
        case wgpu::TextureFormat::ETC2RGB8UnormSrgb:
            return DkImageFormat_RGB_ETC2_sRGB;
        case wgpu::TextureFormat::ETC2RGB8A1Unorm:
            return DkImageFormat_RGB_PTA_ETC2;
        case wgpu::TextureFormat::ETC2RGB8A1UnormSrgb:
            return DkImageFormat_RGB_PTA_ETC2_sRGB;
        case wgpu::TextureFormat::ETC2RGBA8Unorm:
            return DkImageFormat_RGBA_ETC2;
        case wgpu::TextureFormat::ETC2RGBA8UnormSrgb:
            return DkImageFormat_RGBA_ETC2_sRGB;
        case wgpu::TextureFormat::EACR11Unorm:
            return DkImageFormat_R_ETC2_Unorm;
        case wgpu::TextureFormat::EACR11Snorm:
            return DkImageFormat_R_ETC2_Snorm;
        case wgpu::TextureFormat::EACRG11Unorm:
            return DkImageFormat_RG_ETC2_Unorm;
        case wgpu::TextureFormat::EACRG11Snorm:
            return DkImageFormat_RG_ETC2_Snorm;
        case wgpu::TextureFormat::ASTC4x4Unorm:
            return DkImageFormat_RGBA_ASTC_4x4;
        case wgpu::TextureFormat::ASTC4x4UnormSrgb:
            return DkImageFormat_RGBA_ASTC_4x4_sRGB;
        default:
            return DkImageFormat_None;
    }
}

DkFilter ToDkFilter(wgpu::FilterMode mode) {
    return mode == wgpu::FilterMode::Linear ? DkFilter_Linear : DkFilter_Nearest;
}

DkMipFilter ToDkMipFilter(wgpu::MipmapFilterMode mode) {
    return mode == wgpu::MipmapFilterMode::Linear ? DkMipFilter_Linear : DkMipFilter_Nearest;
}

DkWrapMode ToDkWrapMode(wgpu::AddressMode mode) {
    switch (mode) {
        case wgpu::AddressMode::Repeat:
            return DkWrapMode_Repeat;
        case wgpu::AddressMode::MirrorRepeat:
            return DkWrapMode_MirroredRepeat;
        case wgpu::AddressMode::ClampToEdge:
        default:
            return DkWrapMode_ClampToEdge;
    }
}

DkCompareOp ToDkCompareOp(wgpu::CompareFunction function) {
    switch (function) {
        case wgpu::CompareFunction::Never:
            return DkCompareOp_Never;
        case wgpu::CompareFunction::Less:
            return DkCompareOp_Less;
        case wgpu::CompareFunction::LessEqual:
            return DkCompareOp_Lequal;
        case wgpu::CompareFunction::Greater:
            return DkCompareOp_Greater;
        case wgpu::CompareFunction::GreaterEqual:
            return DkCompareOp_Gequal;
        case wgpu::CompareFunction::Equal:
            return DkCompareOp_Equal;
        case wgpu::CompareFunction::NotEqual:
            return DkCompareOp_NotEqual;
        case wgpu::CompareFunction::Always:
        case wgpu::CompareFunction::Undefined:
        default:
            return DkCompareOp_Always;
    }
}

}  // namespace dawn::native::deko3d
