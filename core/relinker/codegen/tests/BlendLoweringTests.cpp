#include "VectorExecution.hpp"
#include <codegen/x86/BlendLowering.hpp>
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

Bytes site(bool wide256, std::uint8_t dst, std::uint8_t first, std::uint8_t second, std::uint8_t mask) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x40 | ((second & 8) ? 0 : 0x20) | 3),
        static_cast<std::uint8_t>(((~first & 15) << 3) | (wide256 ? 4 : 0) | 1), 0x4C,
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (second & 7)),
        static_cast<std::uint8_t>((mask & 15) << 4)};
}

Bytes memorySite(bool wide256, std::uint8_t dst, std::uint8_t first, std::uint8_t mask) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x60 | 3),
        static_cast<std::uint8_t>(((~first & 15) << 3) | (wide256 ? 4 : 0) | 1), 0x4C,
        static_cast<std::uint8_t>((dst & 7) << 3),
        static_cast<std::uint8_t>((mask & 15) << 4)};
}

void decoding() {
    for (std::uint8_t dst = 0; dst < 16; ++dst) {
        for (std::uint8_t first = 0; first < 16; ++first) {
            for (const std::uint8_t second : {0, 3, 8, 15}) {
                for (const std::uint8_t mask : {0, 7, 9, 15}) {
                    const auto bytes = site(true, dst, first, static_cast<std::uint8_t>(second), static_cast<std::uint8_t>(mask));
                    const auto operands = Codegen::DecodeBlend(bytes.data(), bytes.size());
                    require(operands && operands->Destination == dst && operands->First == first && operands->Second == second && operands->Mask == mask && !operands->Memory, "VPBLENDVB decoding changed an operand");
                    const auto narrow = site(false, dst, first, static_cast<std::uint8_t>(second), static_cast<std::uint8_t>(mask));
                    require(!Codegen::DecodeBlend(narrow.data(), narrow.size()), "the 128-bit VPBLENDVB, which AVX has, was lowered");
                }
            }
        }
    }
    const auto memory = memorySite(true, 9, 10, 11);
    const auto operands = Codegen::DecodeBlend(memory.data(), memory.size());
    require(operands && operands->Destination == 9 && operands->First == 10 && operands->Mask == 11 && operands->Memory, "the memory form of VPBLENDVB was not decoded");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE3, 0x7D, 0x4A, 0xC1, 0x20}, Bytes{0xC4, 0xE3, 0x7D, 0x4B, 0xC1, 0x20}, Bytes{0xC4, 0xE3, 0x7C, 0x4C, 0xC1, 0x20}, Bytes{0xC4, 0xE2, 0x7D, 0x4C, 0xC1, 0x20}})
        require(!Codegen::DecodeBlend(bytes.data(), bytes.size()), "VBLENDVPS, VBLENDVPD, an unprefixed or a wrong-map instruction was taken as VPBLENDVB");
    for (const Bytes bytes : {Bytes{0xC4, 0xE3, 0xFD, 0x4C, 0xC1, 0x20}, Bytes{0xC4, 0xE3, 0x7D, 0x4C, 0xC1, 0x21}, Bytes{0xC4, 0xE3, 0x7D, 0x4C, 0xC1}, Bytes{0xC4, 0xE3, 0x7D, 0x4C, 0xC1, 0x20, 0x90}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeBlend(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "VEX.W, a non-zero selector remainder, a missing selector or trailing bytes were accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    const auto bytes = site(true, 0, 1, 2, 3);
    require(!rosetta->Match(bytes.data(), bytes.size()), "Rosetta mode lowered the 256-bit VPBLENDVB");
    require(!intel->Match(bytes.data(), bytes.size()), "Intel mode lowered the 256-bit VPBLENDVB");
    const auto match = avx->Match(bytes.data(), bytes.size());
    require(match && match->InstructionName == "VPBLENDVB" && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, "AVX mode did not lower VPBLENDVB through a movable stub");
    const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
    require(avx->MatchSequence(sequence, {}).has_value(), "A VPBLENDVB sequence could not move out of line");
}

#if LOWERING_EXECUTION_AVAILABLE

using VectorExecution::Machine;
using VectorExecution::Runner;
using Vector = std::array<std::uint64_t, 4>;

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitBlend(body, *Codegen::DecodeBlend(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

Vector model(const Vector& first, const Vector& second, const Vector& mask) {
    std::array<std::uint8_t, 32> a{};
    std::array<std::uint8_t, 32> b{};
    std::array<std::uint8_t, 32> m{};
    std::memcpy(a.data(), first.data(), a.size());
    std::memcpy(b.data(), second.data(), b.size());
    std::memcpy(m.data(), mask.data(), m.size());
    std::array<std::uint8_t, 32> out{};
    for (std::size_t index = 0; index < out.size(); ++index) out[index] = (m[index] & 0x80) != 0 ? b[index] : a[index];
    Vector result{};
    std::memcpy(result.data(), out.data(), out.size());
    return result;
}

void compare(const Bytes& instruction, std::uint8_t dst, const std::string& describe, bool fromMemory, std::uint8_t first, std::uint8_t second, std::uint8_t mask) {
    auto start = VectorExecution::Initial();
    for (std::size_t slot = 0; slot < start.RedZone.size(); ++slot) start.RedZone[slot] = LoweringExecution::kSentinel;
    auto hardware = start;
    Runner(instruction).Run(hardware);
    auto stub = start;
    Runner(lowered(instruction)).Run(stub);
    const auto wanted = model(start.Ymm[first], fromMemory ? start.Memory : start.Ymm[second], start.Ymm[mask]);
    if (hardware.Ymm[dst] != wanted) throw std::runtime_error(describe + ": the host result differs from the model");
    for (std::uint8_t reg = 0; reg < 16; ++reg) {
        const auto expected = reg == dst ? wanted : start.Ymm[reg];
        if (stub.Ymm[reg] != expected) throw std::runtime_error(describe + ": the stub left ymm" + std::to_string(reg) + " wrong");
        if (hardware.Ymm[reg] != expected) throw std::runtime_error(describe + ": the host left ymm" + std::to_string(reg) + " wrong");
    }
    if (stub.Memory != start.Memory || hardware.Memory != start.Memory) throw std::runtime_error(describe + ": the operand in memory was changed");
    for (const auto slot : stub.RedZone) require(slot == LoweringExecution::kSentinel, describe + " overwrote the red zone");
}

void execution() {
    const std::array<std::array<std::uint8_t, 4>, 10> cases{{
        {0, 1, 2, 3}, {3, 3, 3, 3}, {15, 14, 13, 12}, {8, 0, 15, 7}, {5, 5, 6, 6},
        {1, 2, 1, 2}, {11, 11, 12, 11}, {0, 15, 0, 15}, {9, 8, 10, 9}, {2, 1, 0, 2}}};
    for (const auto& operands : cases) {
        const auto instruction = site(true, operands[0], operands[1], operands[2], operands[3]);
        compare(instruction, operands[0], "VPBLENDVB " + std::to_string(operands[0]) + "," + std::to_string(operands[1]) + "," + std::to_string(operands[2]) + "," + std::to_string(operands[3]), false, operands[1], operands[2], operands[3]);
    }
    for (const auto& operands : cases) {
        const auto instruction = memorySite(true, operands[0], operands[1], operands[3]);
        compare(instruction, operands[0], "VPBLENDVB from memory " + std::to_string(operands[0]) + "," + std::to_string(operands[1]) + "," + std::to_string(operands[3]), true, operands[1], 0, operands[3]);
    }
}

#else

void execution() { std::cout << "VPBLENDVB execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "VPBLENDVB lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
