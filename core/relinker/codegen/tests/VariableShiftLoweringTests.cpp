#include "VectorExecution.hpp"
#include <codegen/x86/VariableShiftLowering.hpp>
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
using Codegen::VariableShiftOperation;

constexpr std::array kOperations{VariableShiftOperation::Left, VariableShiftOperation::LogicalRight, VariableShiftOperation::ArithmeticRight};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint8_t opcode(VariableShiftOperation operation) {
    switch (operation) {
    case VariableShiftOperation::Left: return 0x47;
    case VariableShiftOperation::LogicalRight: return 0x45;
    case VariableShiftOperation::ArithmeticRight: return 0x46;
    }
    throw std::runtime_error("unknown operation");
}

Bytes site(VariableShiftOperation operation, bool wide, bool vector256, std::uint8_t dst, std::uint8_t first, std::uint8_t second) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x40 | ((second & 8) ? 0 : 0x20) | 2),
        static_cast<std::uint8_t>((wide ? 0x80 : 0) | ((~first & 15) << 3) | (vector256 ? 4 : 0) | 1), opcode(operation),
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (second & 7))};
}

Bytes memorySite(VariableShiftOperation operation, bool wide, bool vector256, std::uint8_t dst, std::uint8_t first) {
    return {0xC4, static_cast<std::uint8_t>(((dst & 8) ? 0 : 0x80) | 0x60 | 2),
        static_cast<std::uint8_t>((wide ? 0x80 : 0) | ((~first & 15) << 3) | (vector256 ? 4 : 0) | 1), opcode(operation),
        static_cast<std::uint8_t>((dst & 7) << 3)};
}

void decoding() {
    for (const auto operation : kOperations) {
        for (bool wide : {false, true}) {
            if (wide && operation == VariableShiftOperation::ArithmeticRight) continue;
            for (bool vector256 : {false, true}) {
                for (std::uint8_t dst = 0; dst < 16; ++dst) {
                    for (const std::uint8_t first : {0, 5, 8, 15}) {
                        for (const std::uint8_t second : {0, 6, 9, 15}) {
                            const auto bytes = site(operation, wide, vector256, dst, static_cast<std::uint8_t>(first), static_cast<std::uint8_t>(second));
                            const auto operands = Codegen::DecodeVariableShift(bytes.data(), bytes.size());
                            require(operands && operands->Operation == operation && operands->Wide == wide && operands->Vector256 == vector256 && operands->Destination == dst && operands->First == first && operands->Second == second && !operands->Memory, "variable shift decoding changed an operand");
                        }
                    }
                }
            }
        }
    }
    const auto memory = memorySite(VariableShiftOperation::Left, true, true, 9, 10);
    const auto operands = Codegen::DecodeVariableShift(memory.data(), memory.size());
    require(operands && operands->Destination == 9 && operands->First == 10 && operands->Memory, "the memory form of a variable shift was not decoded");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE2, 0x7D, 0x44, 0xC1}, Bytes{0xC4, 0xE2, 0x7D, 0x48, 0xC1}, Bytes{0xC4, 0xE2, 0x7C, 0x47, 0xC1}, Bytes{0xC4, 0xE1, 0x7D, 0x47, 0xC1}, Bytes{0xC4, 0xE3, 0x7D, 0x47, 0xC1}})
        require(!Codegen::DecodeVariableShift(bytes.data(), bytes.size()), "an unrelated opcode, an unprefixed form or a wrong map was taken as a variable shift");
    for (const Bytes bytes : {Bytes{0xC4, 0xE2, 0xFD, 0x46, 0xC1}, Bytes{0xC4, 0xE2, 0x7D, 0x47, 0xC1, 0x90}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeVariableShift(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "VPSRAVQ or trailing bytes were accepted");
    }
}

void matching() {
    const auto avx = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Avx);
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    for (const auto operation : kOperations) {
        for (bool vector256 : {false, true}) {
            const auto bytes = site(operation, false, vector256, 0, 1, 2);
            const auto name = Codegen::VariableShiftName(operation, false);
            require(!rosetta->Match(bytes.data(), bytes.size()), std::string("Rosetta mode lowered ") + name);
            require(!intel->Match(bytes.data(), bytes.size()), std::string("Intel mode lowered ") + name);
            const auto match = avx->Match(bytes.data(), bytes.size());
            require(match && match->InstructionName == name && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->RelocatableWindow, std::string("AVX mode did not lower ") + name + " through a movable stub");
            const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
            require(avx->MatchSequence(sequence, {}).has_value(), std::string("A sequence could not move out of line for ") + name);
        }
    }
}

#if LOWERING_EXECUTION_AVAILABLE

using VectorExecution::Machine;
using VectorExecution::Runner;
using Vector = std::array<std::uint64_t, 4>;

Bytes lowered(const Bytes& instruction) {
    Codegen::StubBodyBuilder body;
    Codegen::EmitVariableShift(body, *Codegen::DecodeVariableShift(instruction.data(), instruction.size()));
    const auto finished = body.Finish();
    return Bytes(finished.Bytes.begin(), finished.Bytes.begin() + static_cast<std::ptrdiff_t>(finished.ReturnBranchOffset));
}

Vector model(VariableShiftOperation operation, bool wide, bool vector256, const Vector& first, const Vector& counts) {
    std::array<std::uint8_t, 32> source{};
    std::array<std::uint8_t, 32> count{};
    std::memcpy(source.data(), first.data(), source.size());
    std::memcpy(count.data(), counts.data(), count.size());
    std::array<std::uint8_t, 32> out{};
    const unsigned size = wide ? 8 : 4;
    const unsigned bits = size * 8;
    const unsigned lanes = (vector256 ? 32u : 16u) / size;
    for (unsigned element = 0; element < lanes; ++element) {
        std::uint64_t left = 0;
        std::uint64_t amount = 0;
        std::memcpy(&left, source.data() + element * size, size);
        std::memcpy(&amount, count.data() + element * size, size);
        std::uint64_t result = 0;
        if (operation == VariableShiftOperation::ArithmeticRight) {
            const auto shift = amount >= bits ? bits - 1 : amount;
            const auto signed_value = static_cast<std::int64_t>(left << (64 - bits)) >> (64 - bits);
            result = static_cast<std::uint64_t>(signed_value >> shift);
        } else if (amount < bits) {
            result = operation == VariableShiftOperation::Left ? left << amount : left >> amount;
        }
        std::memcpy(out.data() + element * size, &result, size);
    }
    Vector value{};
    std::memcpy(value.data(), out.data(), out.size());
    return value;
}

Vector counted(const Vector& base, unsigned pattern) {
    static constexpr std::array<std::uint32_t, 8> patterns{0, 1, 31, 32, 63, 64, 0xFFFFFFFFu, 0x100};
    Vector result{};
    for (std::size_t word = 0; word < result.size(); ++word) {
        const auto low = patterns[(pattern + word * 2) % patterns.size()];
        const auto high = patterns[(pattern + word * 2 + 1) % patterns.size()];
        result[word] = (static_cast<std::uint64_t>(high) << 32) | low;
    }
    if (pattern % 3 == 2) for (std::size_t word = 0; word < result.size(); ++word) result[word] ^= base[word];
    return result;
}

void compare(VariableShiftOperation operation, bool wide, bool vector256, std::uint8_t dst, std::uint8_t first, std::uint8_t second, bool fromMemory, unsigned pattern) {
    const auto instruction = fromMemory ? memorySite(operation, wide, vector256, dst, first) : site(operation, wide, vector256, dst, first, second);
    auto start = VectorExecution::Initial();
    for (std::size_t slot = 0; slot < start.RedZone.size(); ++slot) start.RedZone[slot] = LoweringExecution::kSentinel;
    const auto counts = counted(start.Ymm[first], pattern);
    if (fromMemory) start.Memory = counts; else start.Ymm[second] = counts;
    if (!fromMemory && second == first) start.Ymm[first] = counts;
    auto hardware = start;
    Runner(instruction).Run(hardware);
    auto stub = start;
    Runner(lowered(instruction)).Run(stub);
    const auto describe = std::string(Codegen::VariableShiftName(operation, wide)) + (vector256 ? " ymm " : " xmm ") + std::to_string(dst) + "," + std::to_string(first) + "," + std::to_string(second) + (fromMemory ? " memory" : "") + " pattern " + std::to_string(pattern);
    const auto wanted = model(operation, wide, vector256, start.Ymm[first], fromMemory ? start.Memory : start.Ymm[second]);
    if (hardware.Ymm[dst] != wanted) throw std::runtime_error(describe + ": the host result differs from the model");
    for (std::uint8_t reg = 0; reg < 16; ++reg) {
        const auto expected = reg == dst ? wanted : start.Ymm[reg];
        if (stub.Ymm[reg] != expected) throw std::runtime_error(describe + ": the stub left ymm" + std::to_string(reg) + " wrong");
    }
    if (stub.Memory != start.Memory) throw std::runtime_error(describe + ": the operand in memory was changed");
    for (const auto slot : stub.RedZone) require(slot == LoweringExecution::kSentinel, describe + " overwrote the red zone");
}

void execution() {
    const std::array<std::array<std::uint8_t, 3>, 7> cases{{{0, 1, 2}, {3, 3, 3}, {15, 14, 13}, {8, 0, 15}, {5, 6, 5}, {1, 2, 1}, {9, 9, 10}}};
    for (const auto operation : kOperations) {
        for (bool wide : {false, true}) {
            if (wide && operation == VariableShiftOperation::ArithmeticRight) continue;
            for (bool vector256 : {false, true}) {
                for (unsigned pattern = 0; pattern < 8; ++pattern) {
                    for (const auto& registers : cases)
                        compare(operation, wide, vector256, registers[0], registers[1], registers[2], false, pattern);
                    compare(operation, wide, vector256, 5, 6, 0, true, pattern);
                }
            }
        }
    }
}

#else

void execution() { std::cout << "variable shift execution checks require an x86-64 Linux, macOS or Windows build\n"; }

#endif

}

int main() {
    try {
        decoding();
        matching();
        execution();
        std::cout << "variable shift lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
