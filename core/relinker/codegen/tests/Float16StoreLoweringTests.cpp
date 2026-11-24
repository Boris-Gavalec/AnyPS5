#include "VectorExecution.hpp"
#include <codegen/x86/Float16StoreLowering.hpp>
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

Bytes site(bool vector256, std::uint8_t dst, std::uint8_t src, std::uint8_t control) {
    return {0xC4, static_cast<std::uint8_t>(((src & 8) ? 0 : 0x80) | 0x40 | ((dst & 8) ? 0 : 0x20) | 3),
        static_cast<std::uint8_t>(0x78 | (vector256 ? 4 : 0) | 1), 0x1D,
        static_cast<std::uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)), control};
}

Bytes memorySite(bool vector256, std::uint8_t src, std::uint8_t control) {
    return {0xC4, static_cast<std::uint8_t>(((src & 8) ? 0 : 0x80) | 0x60 | 3),
        static_cast<std::uint8_t>(0x78 | (vector256 ? 4 : 0) | 1), 0x1D,
        static_cast<std::uint8_t>((src & 7) << 3), control};
}

void decoding() {
    for (bool vector256 : {false, true}) {
        for (std::uint8_t dst = 0; dst < 16; ++dst) {
            for (std::uint8_t src = 0; src < 16; ++src) {
                const auto bytes = site(vector256, dst, src, 3);
                const auto operands = Codegen::DecodeFloat16Store(bytes.data(), bytes.size());
                require(operands && operands->Destination == dst && operands->Source == src && operands->Vector256 == vector256 && operands->Control == 3 && !operands->Memory, "VCVTPS2PH decoding changed an operand");
            }
        }
    }
    for (std::uint8_t control = 0; control < 8; ++control) {
        const auto bytes = site(true, 1, 2, static_cast<std::uint8_t>(control | 0xF8));
        const auto operands = Codegen::DecodeFloat16Store(bytes.data(), bytes.size());
        require(operands && operands->Control == control, "VCVTPS2PH did not ignore the reserved immediate bits");
    }
    const auto memory = memorySite(true, 9, 0);
    const auto operands = Codegen::DecodeFloat16Store(memory.data(), memory.size());
    require(operands && operands->Source == 9 && operands->Memory, "the memory form of VCVTPS2PH was not decoded");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE2, 0x7D, 0x13, 0xC1}, Bytes{0xC4, 0xE3, 0x7C, 0x1D, 0xC1, 0x00}, Bytes{0xC4, 0xE1, 0x7D, 0x1D, 0xC1, 0x00}, Bytes{0xC4, 0xE3, 0x7D, 0x1C, 0xC1, 0x00}})
        require(!Codegen::DecodeFloat16Store(bytes.data(), bytes.size()), "VCVTPH2PS, an unprefixed form, a wrong map or a neighbouring opcode was taken as VCVTPS2PH");
    for (const Bytes bytes : {Bytes{0xC4, 0xE3, 0xFD, 0x1D, 0xC1, 0x00}, Bytes{0xC4, 0xE3, 0x75, 0x1D, 0xC1, 0x00}, Bytes{0xC4, 0xE3, 0x7D, 0x1D, 0xC1}, Bytes{0xC4, 0xE3, 0x7D, 0x1D, 0xC1, 0x00, 0x90}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeFloat16Store(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "VEX.W, a non-zero VEX.vvvv, a missing immediate or trailing bytes were accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    for (bool vector256 : {false, true}) {
        const auto bytes = site(vector256, 0, 1, 0);
        require(!rosetta->Match(bytes.data(), bytes.size()), "Rosetta mode lowered VCVTPS2PH");
        require(!intel->Match(bytes.data(), bytes.size()), "Intel mode lowered VCVTPS2PH");
        const auto match = avx->Match(bytes.data(), bytes.size());
        require(match && match->InstructionName == "VCVTPS2PH" && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, "AVX mode did not lower VCVTPS2PH through a movable stub");
        const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
        require(avx->MatchSequence(sequence, {}).has_value(), "A VCVTPS2PH sequence could not move out of line");
    }
}

#if LOWERING_EXECUTION_AVAILABLE
#include <xmmintrin.h>

using VectorExecution::Machine;
using VectorExecution::Runner;
using Vector = std::array<std::uint64_t, 4>;

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitFloat16Store(body, *Codegen::DecodeFloat16Store(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

bool hostHasFloat16() {
    const Bytes probe{0xC4, 0xE3, 0x7D, 0x1D, 0xC1, 0x00};
    return LoweringExecution::HostExecutes(probe);
}

std::vector<std::uint32_t> inputs() {
    std::vector<std::uint32_t> values;
    const Bytes convert{0xC4, 0xE2, 0x7D, 0x13, 0xC1};
    const Runner runner(convert);
    for (unsigned base = 0; base < 0x10000u; base += 8) {
        auto machine = VectorExecution::Initial();
        std::array<std::uint8_t, 32> bytes{};
        for (unsigned lane = 0; lane < 8; ++lane) {
            const auto half = static_cast<std::uint16_t>(base + lane);
            std::memcpy(bytes.data() + lane * 2, &half, 2);
        }
        std::memcpy(machine.Ymm[1].data(), bytes.data(), bytes.size());
        runner.Run(machine);
        std::array<std::uint8_t, 32> result{};
        std::memcpy(result.data(), machine.Ymm[0].data(), result.size());
        for (unsigned lane = 0; lane < 8; ++lane) {
            std::uint32_t word = 0;
            std::memcpy(&word, result.data() + lane * 4, 4);
            for (const std::int32_t delta : {0, 1, -1, 0x1000, -0x1000, 0x2000}) values.push_back(static_cast<std::uint32_t>(word + static_cast<std::uint32_t>(delta)));
        }
    }
    for (const std::uint32_t special : {0u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u, 0x7FFFFFFFu, 0x477FE000u, 0x477FEFFFu, 0x477FF000u, 0x47800000u, 0xC7800000u, 0x33000000u, 0x33800000u, 0x00000001u, 0x80000001u, 0x7149F2CAu, 0x3F804000u})
        values.push_back(special);
    std::uint32_t state = 0x12345678u;
    for (unsigned index = 0; index < 60000; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        values.push_back(state);
    }
    return values;
}

void compare(const std::vector<std::uint32_t>& values, bool vector256, bool toMemory, std::uint8_t control, std::uint8_t dst, std::uint8_t src) {
    const auto instruction = toMemory ? memorySite(vector256, src, control) : site(vector256, dst, src, control);
    const Runner hardware(instruction);
    const Runner stub(lowered(instruction));
    const unsigned lanes = vector256 ? 8u : 4u;
    for (std::size_t base = 0; base + lanes <= values.size(); base += lanes) {
        auto start = VectorExecution::Initial();
        for (auto& slot : start.RedZone) slot = LoweringExecution::kSentinel;
        std::array<std::uint8_t, 32> bytes{};
        for (unsigned lane = 0; lane < 8; ++lane) {
            const auto word = values[base + lane % lanes];
            std::memcpy(bytes.data() + lane * 4, &word, 4);
        }
        std::memcpy(start.Ymm[src].data(), bytes.data(), bytes.size());
        auto host = start;
        hardware.Run(host);
        auto emulated = start;
        stub.Run(emulated);
        for (std::uint8_t reg = 0; reg < 16; ++reg) {
            if (emulated.Ymm[reg] == host.Ymm[reg]) continue;
            std::string detail;
            for (unsigned lane = 0; lane < lanes; ++lane) detail += " " + std::to_string(values[base + lane]);
            throw std::runtime_error(std::string("VCVTPS2PH ") + (vector256 ? "ymm" : "xmm") + " control " + std::to_string(control) + (toMemory ? " memory" : "") + ": ymm" + std::to_string(reg) + " differs from the host for" + detail);
        }
        if (emulated.Memory != host.Memory) {
            std::string detail;
            for (unsigned lane = 0; lane < lanes; ++lane) detail += " " + std::to_string(values[base + lane]);
            throw std::runtime_error(std::string("VCVTPS2PH ") + (vector256 ? "ymm" : "xmm") + " control " + std::to_string(control) + ": the stored halves differ from the host for" + detail);
        }
        for (const auto slot : emulated.RedZone) require(slot == LoweringExecution::kSentinel, "VCVTPS2PH overwrote the red zone");
    }
}

void execution() {
    if (!hostHasFloat16()) {
        std::cout << "this host has no F16C, the VCVTPS2PH lowering is unchecked against hardware\n";
        return;
    }
    const auto values = inputs();
    std::cout << "comparing " << values.size() << " single-precision inputs with the host\n";
    for (std::uint8_t control = 0; control < 4; ++control) {
        for (bool vector256 : {false, true}) {
            compare(values, vector256, false, control, 0, 1);
            compare(values, vector256, true, control, 0, 2);
        }
    }
    const auto saved = _mm_getcsr();
    for (unsigned rounding = 0; rounding < 4; ++rounding) {
        _mm_setcsr((saved & ~0x6000u) | (rounding << 13));
        for (bool vector256 : {false, true}) {
            compare(values, vector256, false, 4, 3, 3);
            compare(values, vector256, true, 4, 0, 9);
        }
    }
    _mm_setcsr(saved);
    for (bool vector256 : {false, true}) compare(values, vector256, false, 4, 15, 8);
}

#else

void execution() { std::cout << "VCVTPS2PH execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "VCVTPS2PH lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
