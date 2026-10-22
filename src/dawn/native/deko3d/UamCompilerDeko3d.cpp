// Copyright 2017 The Dawn & Tint Authors
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

// Compiled with uam's include paths and defines (see src/dawn/native/CMakeLists.txt); keep this the
// only translation unit that sees mesa headers.
#include "src/dawn/native/deko3d/UamCompilerDeko3d.h"

#include <mutex>
#include <vector>

#include "compiler_iface.h"

namespace dawn::native::deko3d {

bool CompileGlslToDksh(SingleShaderStage stage,
                       const std::string& glsl,
                       std::string* dksh,
                       std::string* error) {
    static std::mutex sMutex;
    std::lock_guard<std::mutex> lock(sMutex);

    pipeline_stage uamStage = pipeline_stage_vertex;
    switch (stage) {
        case SingleShaderStage::Vertex:
            uamStage = pipeline_stage_vertex;
            break;
        case SingleShaderStage::Fragment:
            uamStage = pipeline_stage_fragment;
            break;
        case SingleShaderStage::Compute:
            uamStage = pipeline_stage_compute;
            break;
    }

    DekoCompiler compiler(uamStage);
    if (!compiler.CompileGlsl(glsl.c_str())) {
        *error = "uam rejected the shader (details were written to stderr)";
        return false;
    }
    std::vector<uint8_t> bytes;
    compiler.OutputDkshToMemory(bytes);
    dksh->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

}  // namespace dawn::native::deko3d
