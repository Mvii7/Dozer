// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/program_header.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

constexpr size_t EnvHeaderSize = 5 * 8 + 7 * 4;
constexpr u32 ComputeStage = 5;

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void PutBytes(std::vector<u8>& out, std::span<const u8> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename T>
T Get(std::span<const u8> data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

/// The GenericEnvironment::Serialize header: five u64 counts and seven u32 fields.
void PutEnvHeader(std::vector<u8>& out, u64 code_size, u32 local_memory, u32 texture_bound,
                  u32 start_address, u32 stage) {
    Put<u64>(out, code_size);
    Put<u64>(out, 0); // texture types
    Put<u64>(out, 0); // texture pixel formats
    Put<u64>(out, 0); // cbuf values
    Put<u64>(out, 0); // cbuf replacements
    Put<u32>(out, local_memory);
    Put<u32>(out, texture_bound);
    Put<u32>(out, start_address);
    Put<u32>(out, 0);                               // cached lowest
    Put<u32>(out, static_cast<u32>(code_size - 8)); // cached highest
    Put<u32>(out, 1);                               // viewport transform state
    Put<u32>(out, stage);
}

} // Anonymous namespace

std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out) {
    using Shader::Ampere::Instruction;
    if (program.empty() || program.size() % sizeof(Instruction) ||
        program.size() / sizeof(Instruction) > 65536) {
        return "Native input must contain 1..65536 complete 16-byte instructions";
    }
    std::vector<Instruction> native(program.size() / sizeof(Instruction));
    std::memcpy(native.data(), program.data(), native.size() * sizeof(Instruction));
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = false;
    for (u32 index = 0; index < native.size(); ++index) {
        const auto result = capture.Observe(native[index], index);
        if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            return "Native allocation has an out-of-range branch target";
        }
        if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            native.resize(index + 1);
            complete = true;
            break;
        }
    }
    if (!complete) {
        return "Native allocation lacks a complete runtime capture";
    }
    for (const auto& instruction : native) {
        if (instruction.Bits(0, 12) == 0x95c) {
            return "Trap guards require captured constant buffers";
        }
    }
    // As the emulator lowers a compute program, which has no header (ComputeEnvironment)
    auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) {
        return lowered.error;
    }
    const auto code = VideoCommon::MakeLoweredNativeCode(native, lowered, Shader::ProgramHeader{}, 0);
    out.code.resize(code->code.size() * sizeof(u64));
    std::memcpy(out.code.data(), code->code.data(), out.code.size());
    out.texture_buffer = code->texture_bound;
    return std::nullopt;
}

u64 StageHash(std::span<const u8> code) {
    std::vector<u64> words(code.size() / sizeof(u64));
    std::memcpy(words.data(), code.data(), words.size() * sizeof(u64));
    return VideoCommon::NativeShaderHash(words);
}

std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control) {
    if (control.size() < 108) {
        return std::nullopt;
    }
    ComputeControl result;
    for (size_t i = 0; i < 3; ++i) {
        result.workgroup[i] = Get<u32>(control, 88 + i * 4);
    }
    result.shared_memory = static_cast<u32>((u64{Get<u32>(control, 100)} + 255) / 256 * 256);
    result.local_memory = Get<u32>(control, 104);
    return result;
}

std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key) {
    std::vector<u8> out;
    out.reserve(4 + EnvHeaderSize + lowered.code.size() + 16 + key.size());
    Put<u32>(out, 1);
    // Like InitializeNativeCode: the program's own bank when it names one.
    const u32 texture_bound = lowered.texture_buffer != ~0U ? lowered.texture_buffer : 0;
    PutEnvHeader(out, lowered.code.size(), control.local_memory, texture_bound,
                 static_cast<u32>(sizeof(Shader::ProgramHeader)), ComputeStage);
    PutBytes(out, lowered.code);
    for (const u32 size : control.workgroup) {
        Put<u32>(out, size);
    }
    Put<u32>(out, control.shared_memory);
    std::vector<u8> key_bytes;
    Put<u64>(key_bytes, StageHash(lowered.code));
    Put<u32>(key_bytes, control.shared_memory);
    for (const u32 size : control.workgroup) {
        Put<u32>(key_bytes, size);
    }
    std::memcpy(key.data(), key_bytes.data(), key.size());
    PutBytes(out, key_bytes);
    return out;
}

} // namespace VideoCommon::ShaderWarm
