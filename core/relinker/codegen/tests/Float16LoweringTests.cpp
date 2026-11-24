#include "VectorExecution.hpp"
#include <codegen/x86/Float16Lowering.hpp>
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

Bytes site(bool vector256, std::uint8_t dst, std::uint8_t src) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x40 | ((src & 8) ? 0 : 0x20) | 2),
        static_cast<std::uint8_t>(0x78 | (vector256 ? 4 : 0) | 1), 0x13,
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7))};
}

Bytes memorySite(bool vector256, std::uint8_t dst) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x60 | 2),
        static_cast<std::uint8_t>(0x78 | (vector256 ? 4 : 0) | 1), 0x13,
        static_cast<std::uint8_t>((dst & 7) << 3)};
}

void decoding() {
    for (bool vector256 : {false, true}) {
        for (std::uint8_t dst = 0; dst < 16; ++dst) {
            for (std::uint8_t src = 0; src < 16; ++src) {
                const auto bytes = site(vector256, dst, src);
                const auto operands = Codegen::DecodeFloat16(bytes.data(), bytes.size());
                require(operands && operands->Destination == dst && operands->Source == src && operands->Vector256 == vector256 && !operands->Memory, "VCVTPH2PS decoding changed an operand");
            }
        }
    }
    const auto memory = memorySite(true, 9);
    const auto operands = Codegen::DecodeFloat16(memory.data(), memory.size());
    require(operands && operands->Destination == 9 && operands->Memory, "the memory form of VCVTPH2PS was not decoded");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE3, 0x7D, 0x1D, 0xC1, 0x00}, Bytes{0xC4, 0xE2, 0x7C, 0x13, 0xC1}, Bytes{0xC4, 0xE1, 0x7D, 0x13, 0xC1}, Bytes{0xC4, 0xE2, 0x7D, 0x12, 0xC1}})
        require(!Codegen::DecodeFloat16(bytes.data(), bytes.size()), "VCVTPS2PH, an unprefixed form, a wrong map or a neighbouring opcode was taken as VCVTPH2PS");
    for (const Bytes bytes : {Bytes{0xC4, 0xE2, 0xFD, 0x13, 0xC1}, Bytes{0xC4, 0xE2, 0x75, 0x13, 0xC1}, Bytes{0xC4, 0xE2, 0x7D, 0x13, 0xC1, 0x90}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeFloat16(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "VEX.W, a non-zero VEX.vvvv or trailing bytes were accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    for (bool vector256 : {false, true}) {
        const auto bytes = site(vector256, 0, 1);
        require(!rosetta->Match(bytes.data(), bytes.size()), "Rosetta mode lowered VCVTPH2PS");
        require(!intel->Match(bytes.data(), bytes.size()), "Intel mode lowered VCVTPH2PS");
        const auto match = avx->Match(bytes.data(), bytes.size());
        require(match && match->InstructionName == "VCVTPH2PS" && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, "AVX mode did not lower VCVTPH2PS through a movable stub");
        const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
        require(avx->MatchSequence(sequence, {}).has_value(), "A VCVTPH2PS sequence could not move out of line");
    }
}

#if LOWERING_EXECUTION_AVAILABLE

using VectorExecution::Machine;
using VectorExecution::Runner;
using Vector = std::array<std::uint64_t, 4>;

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitFloat16(body, *Codegen::DecodeFloat16(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

Vector packed(const std::array<std::uint16_t, 8>& halves) {
    Vector value{};
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < halves.size(); ++index) std::memcpy(bytes.data() + index * 2, &halves[index], 2);
    std::memcpy(value.data(), bytes.data(), bytes.size());
    return value;
}

bool hostHasFloat16() {
    const Bytes probe{0xC4, 0xE2, 0x7D, 0x13, 0xC1};
    return LoweringExecution::HostExecutes(probe);
}

void exhaustive(bool vector256, bool fromMemory, std::uint8_t dst, std::uint8_t src) {
    const auto instruction = fromMemory ? memorySite(vector256, dst) : site(vector256, dst, src);
    const Runner hardware(instruction);
    const Runner stub(lowered(instruction));
    const unsigned lanes = vector256 ? 8u : 4u;
    std::array<std::uint16_t, 8> halves{};
    for (unsigned base = 0; base < 0x10000u; base += lanes) {
        for (unsigned lane = 0; lane < halves.size(); ++lane) halves[lane] = static_cast<std::uint16_t>(base + lane % lanes);
        auto start = VectorExecution::Initial();
        for (auto& slot : start.RedZone) slot = LoweringExecution::kSentinel;
        if (fromMemory) start.Memory = packed(halves); else start.Ymm[src] = packed(halves);
        auto host = start;
        hardware.Run(host);
        auto lowered = start;
        stub.Run(lowered);
        for (std::uint8_t reg = 0; reg < 16; ++reg) {
            if (lowered.Ymm[reg] == host.Ymm[reg]) continue;
            std::string detail;
            for (unsigned lane = 0; lane < lanes; ++lane) detail += " " + std::to_string(halves[lane]);
            throw std::runtime_error(std::string("VCVTPH2PS ") + (vector256 ? "ymm" : "xmm") + " " + std::to_string(dst) + "," + std::to_string(src) + (fromMemory ? " memory" : "") + ": ymm" + std::to_string(reg) + " differs from the host for halves" + detail);
        }
        if (lowered.Memory != host.Memory) throw std::runtime_error("VCVTPH2PS changed the operand in memory");
        for (const auto slot : lowered.RedZone) require(slot == LoweringExecution::kSentinel, "VCVTPH2PS overwrote the red zone");
    }
}

void execution() {
    if (!hostHasFloat16()) {
        std::cout << "this host has no F16C, the VCVTPH2PS lowering is unchecked against hardware\n";
        return;
    }
    for (bool vector256 : {false, true}) {
        exhaustive(vector256, false, 0, 1);
        exhaustive(vector256, false, 3, 3);
        exhaustive(vector256, false, 15, 8);
        exhaustive(vector256, true, 5, 0);
    }
}

#else

void execution() { std::cout << "VCVTPH2PS execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "VCVTPH2PS lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
