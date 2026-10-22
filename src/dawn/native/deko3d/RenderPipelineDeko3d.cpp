// Copyright 2018 The Dawn & Tint Authors
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

#include "src/dawn/native/deko3d/RenderPipelineDeko3d.h"

#include <algorithm>

#include "src/dawn/native/deko3d/DeviceDeko3d.h"
#include "src/dawn/native/deko3d/PipelineLayoutDeko3d.h"
#include "src/dawn/native/deko3d/UtilsDeko3d.h"

namespace dawn::native::deko3d {

namespace {

DkPrimitive ToDkPrimitive(wgpu::PrimitiveTopology topology) {
    switch (topology) {
        case wgpu::PrimitiveTopology::PointList:
            return DkPrimitive_Points;
        case wgpu::PrimitiveTopology::LineList:
            return DkPrimitive_Lines;
        case wgpu::PrimitiveTopology::LineStrip:
            return DkPrimitive_LineStrip;
        case wgpu::PrimitiveTopology::TriangleStrip:
            return DkPrimitive_TriangleStrip;
        case wgpu::PrimitiveTopology::TriangleList:
        default:
            return DkPrimitive_Triangles;
    }
}

DkBlendFactor ToDkBlendFactor(wgpu::BlendFactor factor) {
    switch (factor) {
        case wgpu::BlendFactor::Zero:
            return DkBlendFactor_Zero;
        case wgpu::BlendFactor::One:
            return DkBlendFactor_One;
        case wgpu::BlendFactor::Src:
            return DkBlendFactor_SrcColor;
        case wgpu::BlendFactor::OneMinusSrc:
            return DkBlendFactor_InvSrcColor;
        case wgpu::BlendFactor::SrcAlpha:
            return DkBlendFactor_SrcAlpha;
        case wgpu::BlendFactor::OneMinusSrcAlpha:
            return DkBlendFactor_InvSrcAlpha;
        case wgpu::BlendFactor::Dst:
            return DkBlendFactor_DstColor;
        case wgpu::BlendFactor::OneMinusDst:
            return DkBlendFactor_InvDstColor;
        case wgpu::BlendFactor::DstAlpha:
            return DkBlendFactor_DstAlpha;
        case wgpu::BlendFactor::OneMinusDstAlpha:
            return DkBlendFactor_InvDstAlpha;
        case wgpu::BlendFactor::SrcAlphaSaturated:
            return DkBlendFactor_SrcAlphaSaturate;
        case wgpu::BlendFactor::Constant:
            return DkBlendFactor_ConstColor;
        case wgpu::BlendFactor::OneMinusConstant:
            return DkBlendFactor_InvConstColor;
        case wgpu::BlendFactor::Src1:
            return DkBlendFactor_Src1Color;
        case wgpu::BlendFactor::OneMinusSrc1:
            return DkBlendFactor_InvSrc1Color;
        case wgpu::BlendFactor::Src1Alpha:
            return DkBlendFactor_Src1Alpha;
        case wgpu::BlendFactor::OneMinusSrc1Alpha:
            return DkBlendFactor_InvSrc1Alpha;
        default:
            return DkBlendFactor_One;
    }
}

DkBlendOp ToDkBlendOp(wgpu::BlendOperation operation) {
    switch (operation) {
        case wgpu::BlendOperation::Subtract:
            return DkBlendOp_Sub;
        case wgpu::BlendOperation::ReverseSubtract:
            return DkBlendOp_RevSub;
        case wgpu::BlendOperation::Min:
            return DkBlendOp_Min;
        case wgpu::BlendOperation::Max:
            return DkBlendOp_Max;
        case wgpu::BlendOperation::Add:
        default:
            return DkBlendOp_Add;
    }
}

DkStencilOp ToDkStencilOp(wgpu::StencilOperation operation) {
    switch (operation) {
        case wgpu::StencilOperation::Zero:
            return DkStencilOp_Zero;
        case wgpu::StencilOperation::Replace:
            return DkStencilOp_Replace;
        case wgpu::StencilOperation::Invert:
            return DkStencilOp_Invert;
        case wgpu::StencilOperation::IncrementClamp:
            return DkStencilOp_Incr;
        case wgpu::StencilOperation::DecrementClamp:
            return DkStencilOp_Decr;
        case wgpu::StencilOperation::IncrementWrap:
            return DkStencilOp_IncrWrap;
        case wgpu::StencilOperation::DecrementWrap:
            return DkStencilOp_DecrWrap;
        case wgpu::StencilOperation::Keep:
        default:
            return DkStencilOp_Keep;
    }
}

struct AttribFormat {
    DkVtxAttribSize size;
    DkVtxAttribType type;
    bool bgra = false;
};

AttribFormat ToDkAttribFormat(wgpu::VertexFormat format) {
    using F = wgpu::VertexFormat;
    switch (format) {
        case F::Uint8: return {DkVtxAttribSize_1x8, DkVtxAttribType_Uint};
        case F::Uint8x2: return {DkVtxAttribSize_2x8, DkVtxAttribType_Uint};
        case F::Uint8x4: return {DkVtxAttribSize_4x8, DkVtxAttribType_Uint};
        case F::Sint8: return {DkVtxAttribSize_1x8, DkVtxAttribType_Sint};
        case F::Sint8x2: return {DkVtxAttribSize_2x8, DkVtxAttribType_Sint};
        case F::Sint8x4: return {DkVtxAttribSize_4x8, DkVtxAttribType_Sint};
        case F::Unorm8: return {DkVtxAttribSize_1x8, DkVtxAttribType_Unorm};
        case F::Unorm8x2: return {DkVtxAttribSize_2x8, DkVtxAttribType_Unorm};
        case F::Unorm8x4: return {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm};
        case F::Unorm8x4BGRA: return {DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, true};
        case F::Snorm8: return {DkVtxAttribSize_1x8, DkVtxAttribType_Snorm};
        case F::Snorm8x2: return {DkVtxAttribSize_2x8, DkVtxAttribType_Snorm};
        case F::Snorm8x4: return {DkVtxAttribSize_4x8, DkVtxAttribType_Snorm};
        case F::Uint16: return {DkVtxAttribSize_1x16, DkVtxAttribType_Uint};
        case F::Uint16x2: return {DkVtxAttribSize_2x16, DkVtxAttribType_Uint};
        case F::Uint16x4: return {DkVtxAttribSize_4x16, DkVtxAttribType_Uint};
        case F::Sint16: return {DkVtxAttribSize_1x16, DkVtxAttribType_Sint};
        case F::Sint16x2: return {DkVtxAttribSize_2x16, DkVtxAttribType_Sint};
        case F::Sint16x4: return {DkVtxAttribSize_4x16, DkVtxAttribType_Sint};
        case F::Unorm16: return {DkVtxAttribSize_1x16, DkVtxAttribType_Unorm};
        case F::Unorm16x2: return {DkVtxAttribSize_2x16, DkVtxAttribType_Unorm};
        case F::Unorm16x4: return {DkVtxAttribSize_4x16, DkVtxAttribType_Unorm};
        case F::Snorm16: return {DkVtxAttribSize_1x16, DkVtxAttribType_Snorm};
        case F::Snorm16x2: return {DkVtxAttribSize_2x16, DkVtxAttribType_Snorm};
        case F::Snorm16x4: return {DkVtxAttribSize_4x16, DkVtxAttribType_Snorm};
        case F::Float16: return {DkVtxAttribSize_1x16, DkVtxAttribType_Float};
        case F::Float16x2: return {DkVtxAttribSize_2x16, DkVtxAttribType_Float};
        case F::Float16x4: return {DkVtxAttribSize_4x16, DkVtxAttribType_Float};
        case F::Float32: return {DkVtxAttribSize_1x32, DkVtxAttribType_Float};
        case F::Float32x2: return {DkVtxAttribSize_2x32, DkVtxAttribType_Float};
        case F::Float32x3: return {DkVtxAttribSize_3x32, DkVtxAttribType_Float};
        case F::Float32x4: return {DkVtxAttribSize_4x32, DkVtxAttribType_Float};
        case F::Uint32: return {DkVtxAttribSize_1x32, DkVtxAttribType_Uint};
        case F::Uint32x2: return {DkVtxAttribSize_2x32, DkVtxAttribType_Uint};
        case F::Uint32x3: return {DkVtxAttribSize_3x32, DkVtxAttribType_Uint};
        case F::Uint32x4: return {DkVtxAttribSize_4x32, DkVtxAttribType_Uint};
        case F::Sint32: return {DkVtxAttribSize_1x32, DkVtxAttribType_Sint};
        case F::Sint32x2: return {DkVtxAttribSize_2x32, DkVtxAttribType_Sint};
        case F::Sint32x3: return {DkVtxAttribSize_3x32, DkVtxAttribType_Sint};
        case F::Sint32x4: return {DkVtxAttribSize_4x32, DkVtxAttribType_Sint};
        case F::Unorm10_10_10_2: return {DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Unorm};
        default: return {DkVtxAttribSize_4x32, DkVtxAttribType_Float};
    }
}

}  // namespace

// static
Ref<RenderPipeline> RenderPipeline::CreateUninitialized(
    Device* device,
    const UnpackedPtr<RenderPipelineDescriptor>& descriptor) {
    return AcquireRef(new RenderPipeline(device, descriptor));
}

RenderPipeline::RenderPipeline(Device* device,
                               const UnpackedPtr<RenderPipelineDescriptor>& descriptor)
    : RenderPipelineBase(device, descriptor) {}

RenderPipeline::~RenderPipeline() = default;

MaybeError RenderPipeline::InitializeImpl() {
    DAWN_TRY(InitializeBase(ToBackend(GetLayout()), GetAllStages()));

    mPrimitive = ToDkPrimitive(GetPrimitiveTopology());

    dkRasterizerStateDefaults(&mRasterizerState);
    switch (GetCullMode()) {
        case wgpu::CullMode::Front:
            mRasterizerState.cullMode = DkFace_Front;
            break;
        case wgpu::CullMode::Back:
            mRasterizerState.cullMode = DkFace_Back;
            break;
        default:
            mRasterizerState.cullMode = DkFace_None;
            break;
    }
    mRasterizerState.frontFace =
        GetFrontFace() == wgpu::FrontFace::CW ? DkFrontFace_CW : DkFrontFace_CCW;
    mRasterizerState.provokingVertex = DkProvokingVertex_First;
    mRasterizerState.depthClampEnable = !HasUnclippedDepth() ? 0 : 1;
    mRasterizerState.depthBiasEnableMask = IsDepthBiasEnabled() ? DkPolygonFlag_All : 0;

    dkMultisampleStateDefaults(&mMultisampleState);
    switch (GetSampleCount()) {
        case 2:
            mMultisampleState.mode = DkMsMode_2x;
            break;
        case 4:
            mMultisampleState.mode = DkMsMode_4x;
            break;
        case 8:
            mMultisampleState.mode = DkMsMode_8x;
            break;
        default:
            break;
    }
    mMultisampleState.rasterizerMode = mMultisampleState.mode;
    mMultisampleState.alphaToCoverageEnable = IsAlphaToCoverageEnabled();

    dkColorStateDefaults(&mColorState);
    dkColorWriteStateDefaults(&mColorWriteState);
    mBlendStates.clear();
    for (auto slot : GetColorAttachmentsMask()) {
        const uint32_t index = static_cast<uint32_t>(slot);
        const ColorTargetState* target = GetColorTargetState(slot);
        if (mBlendStates.size() <= index) {
            mBlendStates.resize(index + 1);
            for (DkBlendState& state : mBlendStates) {
                dkBlendStateDefaults(&state);
            }
        }
        uint32_t mask = 0;
        if (target->writeMask & wgpu::ColorWriteMask::Red) mask |= DkColorMask_R;
        if (target->writeMask & wgpu::ColorWriteMask::Green) mask |= DkColorMask_G;
        if (target->writeMask & wgpu::ColorWriteMask::Blue) mask |= DkColorMask_B;
        if (target->writeMask & wgpu::ColorWriteMask::Alpha) mask |= DkColorMask_A;
        dkColorWriteStateSetMask(&mColorWriteState, index, mask);
        if (target->blend != nullptr) {
            dkColorStateSetBlendEnable(&mColorState, index, true);
            DkBlendState& blend = mBlendStates[index];
            dkBlendStateSetOps(&blend, ToDkBlendOp(target->blend->color.operation),
                               ToDkBlendOp(target->blend->alpha.operation));
            dkBlendStateSetFactors(&blend, ToDkBlendFactor(target->blend->color.srcFactor),
                                   ToDkBlendFactor(target->blend->color.dstFactor),
                                   ToDkBlendFactor(target->blend->alpha.srcFactor),
                                   ToDkBlendFactor(target->blend->alpha.dstFactor));
        }
    }
    for (uint32_t index = static_cast<uint32_t>(mBlendStates.size()); index < 8; ++index) {
        if (!GetColorAttachmentsMask()[ColorAttachmentIndex(static_cast<uint8_t>(index))]) {
            dkColorWriteStateSetMask(&mColorWriteState, index, 0);
        }
    }

    const DepthStencilState* ds = GetDepthStencilState();
    dkDepthStencilStateDefaults(&mDepthStencilState);
    mDepthStencilState.depthTestEnable =
        !(ds->depthCompare == wgpu::CompareFunction::Always &&
          ds->depthWriteEnabled != wgpu::OptionalBool::True);
    mDepthStencilState.depthWriteEnable = ds->depthWriteEnabled == wgpu::OptionalBool::True;
    mDepthStencilState.depthCompareOp = ToDkCompareOp(ds->depthCompare);
    mDepthStencilState.stencilTestEnable = UsesStencil();
    mDepthStencilState.stencilFrontFailOp = ToDkStencilOp(ds->stencilFront.failOp);
    mDepthStencilState.stencilFrontPassOp = ToDkStencilOp(ds->stencilFront.passOp);
    mDepthStencilState.stencilFrontDepthFailOp = ToDkStencilOp(ds->stencilFront.depthFailOp);
    mDepthStencilState.stencilFrontCompareOp = ToDkCompareOp(ds->stencilFront.compare);
    mDepthStencilState.stencilBackFailOp = ToDkStencilOp(ds->stencilBack.failOp);
    mDepthStencilState.stencilBackPassOp = ToDkStencilOp(ds->stencilBack.passOp);
    mDepthStencilState.stencilBackDepthFailOp = ToDkStencilOp(ds->stencilBack.depthFailOp);
    mDepthStencilState.stencilBackCompareOp = ToDkCompareOp(ds->stencilBack.compare);

    mVtxAttribs.clear();
    mVtxBuffers.clear();
    for (VertexBufferSlot slot : GetVertexBuffersUsed()) {
        const VertexBufferInfo& info = GetVertexBuffer(slot);
        const uint32_t index = static_cast<uint32_t>(slot);
        if (mVtxBuffers.size() <= index) {
            mVtxBuffers.resize(index + 1, DkVtxBufferState{0, 0});
        }
        mVtxBuffers[index].stride = info.arrayStride;
        mVtxBuffers[index].divisor = info.stepMode == wgpu::VertexStepMode::Instance ? 1 : 0;
    }
    for (VertexAttributeLocation location : GetAttributeLocationsUsed()) {
        const VertexAttributeInfo& attribute = GetAttribute(location);
        const uint32_t index = static_cast<uint32_t>(location);
        if (mVtxAttribs.size() <= index) {
            DkVtxAttribState unused = {};
            unused.isFixed = 1;
            unused.size = DkVtxAttribSize_4x32;
            unused.type = DkVtxAttribType_Float;
            mVtxAttribs.resize(index + 1, unused);
        }
        const AttribFormat format = ToDkAttribFormat(attribute.format);
        DkVtxAttribState& state = mVtxAttribs[index];
        state = {};
        state.bufferId = static_cast<uint32_t>(attribute.vertexBufferSlot);
        state.isFixed = 0;
        state.offset = static_cast<uint32_t>(attribute.offset);
        state.size = format.size;
        state.type = format.type;
        state.isBgra = format.bgra ? 1 : 0;
    }
    return {};
}

void RenderPipeline::ApplyNow(DkCmdBuf cmdBuf, uint32_t stencilReference) {
    const auto& shaders = GetShaders();
    DkShader const* shaderPtrs[2] = {};
    for (size_t i = 0; i < shaders.size() && i < 2; ++i) {
        shaderPtrs[i] = &shaders[i];
    }
    dkCmdBufBindShaders(cmdBuf, DkStageFlag_GraphicsMask, shaderPtrs,
                        static_cast<uint32_t>(std::min<size_t>(shaders.size(), 2)));
    dkCmdBufBindRasterizerState(cmdBuf, &mRasterizerState);
    dkCmdBufBindMultisampleState(cmdBuf, &mMultisampleState);
    dkCmdBufBindColorState(cmdBuf, &mColorState);
    dkCmdBufBindColorWriteState(cmdBuf, &mColorWriteState);
    if (!mBlendStates.empty()) {
        dkCmdBufBindBlendStates(cmdBuf, 0, mBlendStates.data(),
                                static_cast<uint32_t>(mBlendStates.size()));
    }
    dkCmdBufBindDepthStencilState(cmdBuf, &mDepthStencilState);
    if (UsesStencil()) {
        const DepthStencilState* ds = GetDepthStencilState();
        dkCmdBufSetStencil(cmdBuf, DkFace_FrontAndBack, static_cast<uint8_t>(ds->stencilWriteMask),
                           static_cast<uint8_t>(stencilReference),
                           static_cast<uint8_t>(ds->stencilReadMask));
    }
    if (IsDepthBiasEnabled()) {
        dkCmdBufSetDepthBias(cmdBuf, GetDepthBias(), GetDepthBiasClamp(), GetDepthBiasSlopeScale());
    }
    // The Maxwell sample mask register only takes 16 bits; WebGPU's default is 0xFFFFFFFF, which
    // the GPU rejects with a method error that puts the queue into an error state.
    dkCmdBufSetSampleMask(cmdBuf, GetSampleMask() & 0xFFFF);
    if (!mVtxAttribs.empty()) {
        dkCmdBufBindVtxAttribState(cmdBuf, mVtxAttribs.data(),
                                   static_cast<uint32_t>(mVtxAttribs.size()));
    }
    if (!mVtxBuffers.empty()) {
        dkCmdBufBindVtxBufferState(cmdBuf, mVtxBuffers.data(),
                                   static_cast<uint32_t>(mVtxBuffers.size()));
    }
}

}  // namespace dawn::native::deko3d
