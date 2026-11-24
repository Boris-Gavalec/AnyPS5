#include "VectorExecution.hpp"
#include <codegen/x86/GatherLowering.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/CodegenException.hpp>
#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using LoweringExecution::Bytes;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Form {
    std::uint8_t Opcode;
    bool Wide;
    const char* Name;
    std::uint8_t IndexSize;
    std::uint8_t ElementSize;
};

const std::array<Form, 8> kForms{{
    {0x90, false, "VPGATHERDD", 4, 4},
    {0x90, true, "VPGATHERDQ", 4, 8},
    {0x91, false, "VPGATHERQD", 8, 4},
    {0x91, true, "VPGATHERQQ", 8, 8},
    {0x92, false, "VGATHERDPS", 4, 4},
    {0x92, true, "VGATHERDPD", 4, 8},
    {0x93, false, "VGATHERQPS", 8, 4},
    {0x93, true, "VGATHERQPD", 8, 8},
}};

Bytes site(const Form& form, bool vector256, std::uint8_t dst, std::uint8_t mask, std::uint8_t indexRegister, std::uint8_t base, std::uint8_t scale) {
    return {0xC4,
        static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | ((indexRegister & 8) ? 0 : 0x40) | ((base & 8) ? 0 : 0x20) | 2),
        static_cast<std::uint8_t>((form.Wide ? 0x80 : 0) | ((~mask & 15) << 3) | (vector256 ? 4 : 0) | 1), form.Opcode,
        static_cast<std::uint8_t>((dst & 7) << 3 | 4),
        static_cast<std::uint8_t>((scale << 6) | ((indexRegister & 7) << 3) | (base & 7))};
}

int elements(const Form& form, bool vector256) {
    const int width = vector256 ? 32 : 16;
    return width / (form.IndexSize > form.ElementSize ? form.IndexSize : form.ElementSize);
}

void decoding() {
    for (const auto& form : kForms) {
        for (bool vector256 : {false, true}) {
            const auto bytes = site(form, vector256, 0, 2, 1, 0, 2);
            const auto operands = Codegen::DecodeGather(bytes.data(), bytes.size());
            require(operands && std::string(operands->Name) == form.Name && operands->Destination == 0 && operands->Mask == 2 && operands->Index == 1 && operands->Base == 0 && operands->HasBase && operands->Scale == 2 && operands->Displacement == 0 && operands->IndexSize == form.IndexSize && operands->ElementSize == form.ElementSize && operands->Elements == elements(form, vector256), std::string(form.Name) + " decoding changed an operand");
        }
    }
    const auto extended = site(kForms[0], true, 9, 10, 11, 12, 3);
    const auto operands = Codegen::DecodeGather(extended.data(), extended.size());
    require(operands && operands->Destination == 9 && operands->Mask == 10 && operands->Index == 11 && operands->Base == 12 && operands->Scale == 3, "a gather lost a register extension");
    const Bytes displaced{0xC4, 0xE2, 0x6D, 0x90, 0x44, 0x88, 0x10};
    const auto byteDisplacement = Codegen::DecodeGather(displaced.data(), displaced.size());
    require(byteDisplacement && byteDisplacement->Displacement == 0x10 && byteDisplacement->HasBase, "a gather lost its 8-bit displacement");
    const Bytes absolute{0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00};
    const auto noBase = Codegen::DecodeGather(absolute.data(), absolute.size());
    require(noBase && !noBase->HasBase && noBase->Displacement == 0x1000, "a gather without a base register was not decoded");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE2, 0x6D, 0x8E, 0x04, 0x88}, Bytes{0xC4, 0xE2, 0x6C, 0x90, 0x04, 0x88}, Bytes{0xC4, 0xE1, 0x6D, 0x90, 0x04, 0x88}, Bytes{0xC4, 0xE2, 0x6D, 0x94, 0x04, 0x88}})
        require(!Codegen::DecodeGather(bytes.data(), bytes.size()), "a masked store, an unprefixed form, a wrong map or a neighbouring opcode was taken as a gather");
    for (const Bytes bytes : {Bytes{0xC4, 0xE2, 0x6D, 0x90, 0xC1}, Bytes{0xC4, 0xE2, 0x6D, 0x90, 0x01}, Bytes{0xC4, 0xE2, 0x7D, 0x90, 0x04, 0x88}, Bytes{0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x88, 0x90}, Bytes{0x67, 0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x88}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeGather(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "a register index, a missing SIB, overlapping registers, trailing bytes or a 32-bit address size were accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    for (const auto& form : kForms) {
        const auto bytes = site(form, true, 0, 2, 1, 0, 2);
        require(!rosetta->Match(bytes.data(), bytes.size()), std::string("Rosetta mode lowered ") + form.Name);
        require(!intel->Match(bytes.data(), bytes.size()), std::string("Intel mode lowered ") + form.Name);
        const auto match = avx->Match(bytes.data(), bytes.size());
        require(match && std::string(match->InstructionName) == form.Name && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, std::string("AVX mode did not lower ") + form.Name + " through a movable stub");
        const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
        require(avx->MatchSequence(sequence, {}).has_value(), std::string("A sequence could not move out of line for ") + form.Name);
    }
}

#if LOWERING_EXECUTION_AVAILABLE

using VectorExecution::Machine;
using VectorExecution::Runner;
using Vector = std::array<std::uint64_t, 4>;

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitGather(body, *Codegen::DecodeGather(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

bool hostGathers() {
    const Bytes probe{0xC5, 0xEC, 0x57, 0xD2, 0xC5, 0xF4, 0x57, 0xC9, 0x48, 0x89, 0xF8, 0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x88};
    return LoweringExecution::HostExecutes(probe);
}

Vector indexVector(std::uint8_t indexSize, int count, unsigned pattern) {
    std::array<std::uint8_t, 32> bytes{};
    for (int element = 0; element < count; ++element) {
        const std::int64_t value = static_cast<std::int64_t>((element * 7 + pattern) % 8) - (pattern % 3 == 2 ? 2 : 0);
        if (indexSize == 4) {
            const auto narrow = static_cast<std::int32_t>(value);
            std::memcpy(bytes.data() + element * 4, &narrow, 4);
        } else {
            std::memcpy(bytes.data() + element * 8, &value, 8);
        }
    }
    Vector out{};
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

Vector maskVector(std::uint8_t elementSize, int count, unsigned pattern) {
    std::array<std::uint8_t, 32> bytes{};
    for (int element = 0; element < count; ++element) {
        const bool set = pattern == 0 ? true : pattern == 1 ? false : ((element + pattern) % 2) == 0;
        const std::uint64_t value = set ? 0x8000000000000000ull : 0x4000000000000000ull;
        if (elementSize == 4) {
            const auto narrow = static_cast<std::uint32_t>(value >> 32);
            std::memcpy(bytes.data() + element * 4, &narrow, 4);
        } else {
            std::memcpy(bytes.data() + element * 8, &value, 8);
        }
    }
    Vector out{};
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

void compare(const Form& form, bool vector256, std::uint8_t scale, unsigned pattern, unsigned maskPattern) {
    const auto instruction = site(form, vector256, 0, 2, 1, 0, scale);
    const Runner hardware(instruction);
    const Runner stub(lowered(instruction));
    const auto count = elements(form, vector256);
    auto start = VectorExecution::Initial();
    for (auto& slot : start.RedZone) slot = LoweringExecution::kSentinel;
    start.Ymm[1] = indexVector(form.IndexSize, count, pattern);
    start.Ymm[2] = maskVector(form.ElementSize, count, maskPattern);
    auto host = start;
    hardware.Run(host);
    auto emulated = start;
    stub.Run(emulated);
    const auto describe = std::string(form.Name) + (vector256 ? " ymm" : " xmm") + " scale " + std::to_string(1 << scale) + " index pattern " + std::to_string(pattern) + " mask pattern " + std::to_string(maskPattern);
    for (std::uint8_t reg = 0; reg < 16; ++reg)
        if (emulated.Ymm[reg] != host.Ymm[reg])
            throw std::runtime_error(describe + ": ymm" + std::to_string(reg) + " differs from the host");
    if (emulated.Memory != host.Memory) throw std::runtime_error(describe + ": the gathered memory was changed");
    for (const auto slot : emulated.RedZone) require(slot == LoweringExecution::kSentinel, describe + " overwrote the red zone");
}

void execution() {
    if (!hostGathers()) {
        std::cout << "this host has no AVX2 gathers, the lowering is unchecked against hardware\n";
        return;
    }
    for (const auto& form : kForms) {
        for (bool vector256 : {false, true}) {
            for (const std::uint8_t scale : {0, 1, 2, 3}) {
                for (unsigned pattern = 0; pattern < 4; ++pattern)
                    for (unsigned maskPattern = 0; maskPattern < 4; ++maskPattern)
                        compare(form, vector256, scale, pattern, maskPattern);
            }
        }
    }
}

#else

void execution() { std::cout << "gather execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "gather lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
