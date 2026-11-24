#include "VectorExecution.hpp"
#include <codegen/x86/MoveMaskLowering.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/CodegenException.hpp>
#include <array>
#include <cstring>
#include <span>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using LoweringExecution::Bytes;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Bytes twoByteSite(bool wide256, std::uint8_t dst, std::uint8_t src) {
    return {0xC5, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x78 | (wide256 ? 4 : 0) | 1), 0xD7,
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7))};
}

Bytes site(bool wide256, std::uint8_t dst, std::uint8_t src) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x40 | ((src & 8) ? 0 : 0x20) | 1),
        static_cast<std::uint8_t>(0x78 | (wide256 ? 4 : 0) | 1), 0xD7,
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7))};
}

void decoding() {
    for (std::uint8_t dst = 0; dst < 16; ++dst) {
        for (std::uint8_t src = 0; src < 16; ++src) {
            const auto bytes = site(true, dst, src);
            const auto operands = Codegen::DecodeMoveMask(bytes.data(), bytes.size());
            require(operands && operands->Destination == dst && operands->Source == src, "VPMOVMSKB decoding changed an operand");
            const auto narrow = site(false, dst, src);
            require(!Codegen::DecodeMoveMask(narrow.data(), narrow.size()), "the 128-bit VPMOVMSKB, which AVX has, was lowered");
            if (src >= 8) continue;
            const auto compact = twoByteSite(true, dst, src);
            const auto twoByte = Codegen::DecodeMoveMask(compact.data(), compact.size());
            require(twoByte && twoByte->Destination == dst && twoByte->Source == src, "the two-byte VEX form of VPMOVMSKB was not decoded");
            const auto compactNarrow = twoByteSite(false, dst, src);
            require(!Codegen::DecodeMoveMask(compactNarrow.data(), compactNarrow.size()), "the 128-bit two-byte VPMOVMSKB was lowered");
        }
    }
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE1, 0x7D, 0xD6, 0xC1}, Bytes{0xC4, 0xE1, 0x7C, 0xD7, 0xC1}, Bytes{0xC4, 0xE1, 0x7F, 0xD7, 0xC1}, Bytes{0xC4, 0xE2, 0x7D, 0xD7, 0xC1}})
        require(!Codegen::DecodeMoveMask(bytes.data(), bytes.size()), "an unrelated, 128-bit, unprefixed or wrong-map instruction was taken as VPMOVMSKB");
    for (const Bytes bytes : {Bytes{0xC4, 0xE1, 0x7D, 0xD7, 0x01}, Bytes{0xC4, 0xE1, 0x7D, 0xD7, 0xC1, 0x90}, Bytes{0xC4, 0xE1, 0x6D, 0xD7, 0xC1}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeMoveMask(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "A memory source, trailing bytes or a non-zero VEX.vvvv was accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    const auto bytes = site(true, 0, 1);
    require(!rosetta->Match(bytes.data(), bytes.size()), "Rosetta mode lowered the 256-bit VPMOVMSKB");
    require(!intel->Match(bytes.data(), bytes.size()), "Intel mode lowered the 256-bit VPMOVMSKB");
    const auto match = avx->Match(bytes.data(), bytes.size());
    require(match && match->InstructionName == "VPMOVMSKB" && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, "AVX mode did not lower VPMOVMSKB through a movable stub");
    const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
    require(avx->MatchSequence(sequence, {}).has_value(), "A VPMOVMSKB sequence could not move out of line");
}

#if LOWERING_EXECUTION_AVAILABLE

using VectorExecution::Machine;
using VectorExecution::Runner;

constexpr std::array<std::uint8_t, 15> kTracked{0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr std::int32_t kHarnessSlots = -512;
constexpr std::int32_t kSlots = -128;
constexpr std::uint32_t kFlagState = 0x8D5;

std::uint64_t sentinel(std::uint8_t reg) {
    return 0x1111111100000000ull + reg * 0x01020304ull;
}

void append32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

Bytes framed(const Bytes& inner) {
    Bytes body;
    const auto stack = [&body](std::uint8_t opcode, std::uint8_t reg, std::int32_t displacement) {
        body.push_back(static_cast<std::uint8_t>(0x48 | ((reg & 8) >> 1)));
        body.insert(body.end(), {opcode, static_cast<std::uint8_t>(0x84 | ((reg & 7) << 3)), 0x24});
        append32(body, static_cast<std::uint32_t>(displacement));
    };
    for (std::size_t slot = 0; slot < kTracked.size(); ++slot)
        stack(0x89, kTracked[slot], kHarnessSlots - static_cast<std::int32_t>(slot) * 8);
    for (const auto reg : kTracked) {
        body.push_back(static_cast<std::uint8_t>(0x48 | ((reg & 8) >> 3)));
        body.push_back(static_cast<std::uint8_t>(0xB8 | (reg & 7)));
        const auto value = sentinel(reg);
        for (unsigned shift = 0; shift < 64; shift += 8) body.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    body.push_back(0x68);
    append32(body, kFlagState);
    body.push_back(0x9D);
    body.insert(body.end(), inner.begin(), inner.end());
    for (std::size_t slot = 0; slot < kTracked.size(); ++slot)
        stack(0x89, kTracked[slot], kSlots + static_cast<std::int32_t>(slot) * 8);
    body.insert(body.end(), {0x9C, 0x58});
    stack(0x89, 0, kSlots + static_cast<std::int32_t>(kTracked.size()) * 8);
    for (std::size_t slot = 0; slot < kTracked.size(); ++slot)
        stack(0x8B, kTracked[slot], kHarnessSlots - static_cast<std::int32_t>(slot) * 8);
    return body;
}

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitMoveMask(body, *Codegen::DecodeMoveMask(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

std::uint64_t model(const std::array<std::uint64_t, 4>& source) {
    std::array<std::uint8_t, 32> bytes{};
    std::memcpy(bytes.data(), source.data(), bytes.size());
    std::uint64_t mask = 0;
    for (unsigned index = 0; index < bytes.size(); ++index)
        if ((bytes[index] & 0x80) != 0) mask |= std::uint64_t{1} << index;
    return mask;
}

void execution() {
    for (std::uint8_t dst = 0; dst < 16; ++dst) {
        if (dst == 4) continue;
        for (const std::uint8_t src : {0, 1, 7, 8, 15}) {
            for (const auto& instruction : src < 8 ? std::vector<Bytes>{site(true, dst, static_cast<std::uint8_t>(src)), twoByteSite(true, dst, static_cast<std::uint8_t>(src))} : std::vector<Bytes>{site(true, dst, static_cast<std::uint8_t>(src))}) {
            auto machine = VectorExecution::Initial();
            for (std::size_t slot = 0; slot < machine.RedZone.size(); ++slot) machine.RedZone[slot] = LoweringExecution::kSentinel;
            const auto start = machine;
            auto hardware = start;
            Runner(framed(instruction)).Run(hardware);
            auto stub = start;
            Runner(framed(lowered(instruction))).Run(stub);
            const auto describe = std::string("VPMOVMSKB ") + std::to_string(dst) + "," + std::to_string(src);
            const auto wanted = model(start.Ymm[src]);
            for (std::size_t slot = 0; slot < kTracked.size(); ++slot) {
                const auto reg = kTracked[slot];
                const auto expected = reg == dst ? wanted : sentinel(reg);
                if (hardware.RedZone[slot] != expected)
                    throw std::runtime_error(describe + ": the host left register " + std::to_string(reg) + " as " + std::to_string(hardware.RedZone[slot]) + ", the model wants " + std::to_string(expected));
                if (stub.RedZone[slot] != expected)
                    throw std::runtime_error(describe + ": the stub left register " + std::to_string(reg) + " as " + std::to_string(stub.RedZone[slot]) + ", wanted " + std::to_string(expected));
            }
            if ((stub.RedZone[kTracked.size()] & LoweringExecution::kFlagMask) != (hardware.RedZone[kTracked.size()] & LoweringExecution::kFlagMask))
                throw std::runtime_error(describe + ": the stub changed the flags the instruction leaves alone");
            for (std::uint8_t reg = 0; reg < 16; ++reg) {
                if (hardware.Ymm[reg] != start.Ymm[reg])
                    throw std::runtime_error(describe + ": the host changed ymm" + std::to_string(reg));
                if (stub.Ymm[reg] != start.Ymm[reg])
                    throw std::runtime_error(describe + ": the stub changed ymm" + std::to_string(reg));
            }
            }
        }
    }
}

#else

void execution() { std::cout << "VPMOVMSKB execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "VPMOVMSKB lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
