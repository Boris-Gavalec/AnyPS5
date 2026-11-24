#include "LoweringExecution.hpp"
#include <codegen/x86/ParallelBitLowering.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/CodegenException.hpp>
#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using LoweringExecution::Bytes;
using Codegen::ParallelBitOperation;

constexpr std::array kOperations{ParallelBitOperation::Deposit, ParallelBitOperation::Extract};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint8_t prefix(ParallelBitOperation operation) {
    return operation == ParallelBitOperation::Deposit ? 3 : 2;
}

Bytes instruction(ParallelBitOperation operation, bool wide, std::uint8_t dst, std::uint8_t source, std::uint8_t mask) {
    return {0xC4, static_cast<std::uint8_t>(0xE2 ^ ((dst & 8) << 4) ^ ((mask & 8) << 2)),
        static_cast<std::uint8_t>((wide ? 0x80 : 0) | ((~source & 15) << 3) | prefix(operation)), 0xF5,
        static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (mask & 7))};
}

Bytes memorySite(ParallelBitOperation operation, bool wide, const Bytes& tail) {
    Bytes site{0xC4, 0xE2, static_cast<std::uint8_t>((wide ? 0x80 : 0) | 0x40 | prefix(operation)), 0xF5};
    site.insert(site.end(), tail.begin(), tail.end());
    return site;
}

std::uint64_t model(ParallelBitOperation operation, bool wide, std::uint64_t source, std::uint64_t mask) {
    const unsigned bits = wide ? 64 : 32;
    const auto width = wide ? ~std::uint64_t{0} : 0xFFFFFFFFull;
    source &= width;
    mask &= width;
    std::uint64_t result = 0;
    unsigned packed = 0;
    for (unsigned bit = 0; bit < bits; ++bit) {
        if (((mask >> bit) & 1) == 0) continue;
        if (operation == ParallelBitOperation::Extract)
            result |= ((source >> bit) & 1) << packed;
        else
            result |= ((source >> packed) & 1) << bit;
        ++packed;
    }
    return result;
}

void decoding() {
    for (const auto operation : kOperations) {
        for (bool wide : {false, true}) {
            for (std::uint8_t dst = 0; dst < 16; ++dst) {
                for (std::uint8_t source = 0; source < 16; ++source) {
                    for (std::uint8_t mask = 0; mask < 16; ++mask) {
                        const auto bytes = instruction(operation, wide, dst, source, mask);
                        const auto operands = Codegen::DecodeParallelBit(bytes.data(), bytes.size());
                        require(operands && operands->Operation == operation && operands->Wide == wide && operands->Destination == dst && operands->Source == source && operands->Mask == mask && !operands->Memory, "parallel bit register decoding changed an operand");
                    }
                }
            }
        }
    }
    const Bytes memory{0x64, 0x67, 0xC4, 0x02, 0xAB, 0xF5, 0x8C, 0xAC, 0x10, 0, 0, 0};
    const auto operands = Codegen::DecodeParallelBit(memory.data(), memory.size());
    require(operands && operands->Operation == ParallelBitOperation::Deposit && operands->Destination == 9 && operands->Source == 10 && operands->Memory && operands->Memory->RexIndexBase == 3 && operands->Memory->Prefixes == Bytes{0x64, 0x67}, "parallel bit memory extensions or prefixes were lost");
    for (const Bytes bytes : {Bytes{0x90}, Bytes{0xC4, 0xE2, 0xF3}, Bytes{0xC4, 0xE1, 0xF3, 0xF5, 0xC0}, Bytes{0xC4, 0xE2, 0xF0, 0xF5, 0xC0}, Bytes{0xC4, 0xE2, 0xF1, 0xF5, 0xC0}, Bytes{0xC4, 0xE2, 0xF3, 0xF6, 0xC0}, Bytes{0xC4, 0xE2, 0xF3, 0xF7, 0xC0}, Bytes{0xC4, 0xE2, 0xF2, 0xF2, 0xC0}})
        require(!Codegen::DecodeParallelBit(bytes.data(), bytes.size()), "Unrelated, truncated, BZHI, SHLX, MULX, ANDN or SARX opcode matched a parallel bit instruction");
    for (const Bytes bytes : {Bytes{0xC4, 0xE2, 0xF7, 0xF5, 0xC0}, Bytes{0xC4, 0xE2, 0xF3, 0xF5, 0xC0, 0x90}, Bytes{0xC4, 0xE2, 0xF3, 0xF5, 0x84}}) {
        bool rejected = false;
        try { (void)Codegen::DecodeParallelBit(bytes.data(), bytes.size()); }
        catch (const Codegen::CodegenException&) { rejected = true; }
        require(rejected, "Malformed parallel bit instruction was accepted");
    }
    const Bytes overflow{0xC4, 0xE2, 0xF3, 0xF5, 0x8C, 0x24, 0xFF, 0xFF, 0xFF, 0x7F};
    bool rejected = false;
    try {
        Codegen::StubBodyBuilder body;
        Codegen::EmitParallelBit(body, *Codegen::DecodeParallelBit(overflow.data(), overflow.size()));
    } catch (const Codegen::CodegenException&) { rejected = true; }
    require(rejected, "An overflowing stack displacement was silently wrapped");
}

void matching() {
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher();
    for (const auto target : {Codegen::Amd64OnlyTarget::Rosetta, Codegen::Amd64OnlyTarget::Avx}) {
        const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher(target);
        for (const auto operation : kOperations) {
            const auto bytes = instruction(operation, true, 0, 6, 2);
            const auto name = Codegen::ParallelBitName(operation);
            require(!intel->Match(bytes.data(), bytes.size()), std::string("Intel mode rewrote ") + name);
            const auto match = matcher->Match(bytes.data(), bytes.size());
            require(match && match->InstructionName == name && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && !match->Optional, std::string("A trampoline was not required for ") + name);
            const std::array<std::span<const std::uint8_t>, 2> sequence{bytes, bytes};
            require(matcher->MatchSequence(sequence, {}).has_value(), std::string("A sequence could not move out of line for ") + name);
        }
    }
}

#if LOWERING_EXECUTION_AVAILABLE

using LoweringExecution::Execution;
using LoweringExecution::State;

Execution executable(const Bytes& site, std::uint64_t memoryValue) {
    const auto operands = Codegen::DecodeParallelBit(site.data(), site.size());
    require(operands.has_value(), "Execution site was not a parallel bit instruction");
    return Execution(site, memoryValue, operands->Memory && operands->Memory->RipRelative, [](Codegen::StubBodyBuilder& body, const Bytes& relocated) {
        Codegen::EmitParallelBit(body, *Codegen::DecodeParallelBit(relocated.data(), relocated.size()));
    });
}

std::string describe(const Codegen::ParallelBitOperands& operands, std::uint64_t source, std::uint64_t mask) {
    return std::string(Codegen::ParallelBitName(operands.Operation)) + (operands.Wide ? " 64" : " 32") + " dst " + std::to_string(operands.Destination) + " source " + std::to_string(operands.Source) + " mask " + std::to_string(operands.Mask) + (operands.Memory ? " memory" : "") + " (source value " + std::to_string(source) + ", mask value " + std::to_string(mask) + ")";
}

void check(const State& state, const Codegen::ParallelBitOperands& operands, std::uint64_t source, std::uint64_t mask) {
    const auto wanted = model(operands.Operation, operands.Wide, source, mask);
    for (std::size_t reg = 0; reg < 16; ++reg) {
        const auto value = reg == operands.Destination ? wanted : state.Input[reg];
        if (state.Output[reg] != value)
            throw std::runtime_error(describe(operands, source, mask) + ": register " + std::to_string(reg) + " is " + std::to_string(state.Output[reg]) + ", wanted " + std::to_string(value));
    }
    if ((state.Flags & LoweringExecution::kFlagMask) != (state.InputFlags & LoweringExecution::kFlagMask))
        throw std::runtime_error(describe(operands, source, mask) + ": flags are " + std::to_string(state.Flags & LoweringExecution::kFlagMask) + ", wanted " + std::to_string(state.InputFlags & LoweringExecution::kFlagMask));
    for (const auto value : state.RedZone) require(value == LoweringExecution::kSentinel, describe(operands, source, mask) + " overwrote the caller's red zone");
}

void run(const Execution& execution, const Codegen::ParallelBitOperands& operands, State state, std::uint64_t memoryValue) {
    for (const auto flags : {LoweringExecution::kBaseFlags | LoweringExecution::kFlagMask, LoweringExecution::kBaseFlags}) {
        state.InputFlags = flags;
        auto trial = state;
        execution.Run(trial);
        check(trial, operands, trial.Input[operands.Source], operands.Memory ? memoryValue : trial.Input[operands.Mask]);
    }
}

void hardware() {
    for (const auto operation : kOperations) {
        const auto probe = instruction(operation, true, 0, 1, 2);
        if (!LoweringExecution::HostExecutes(probe)) {
            std::cout << Codegen::ParallelBitName(operation) << " is not available on this host, the model is unchecked\n";
            continue;
        }
        for (bool wide : {false, true}) {
            for (const auto pair : std::array<std::pair<std::uint8_t, std::uint8_t>, 4>{{{6, 2}, {11, 11}, {1, 6}, {9, 3}}}) {
                const auto site = instruction(operation, wide, 0, pair.first, pair.second);
                const auto operands = *Codegen::DecodeParallelBit(site.data(), site.size());
                const Execution native(site, 0, false, [&](Codegen::StubBodyBuilder& body, const Bytes& relocated) { body.Raw(relocated); });
                for (const auto source : std::array<std::uint64_t, 5>{0, 1, 0xFFFFFFFFFFFFFFFFull, 0xABCD123456789000ull, 0x8000000080000000ull}) {
                    for (const auto mask : std::array<std::uint64_t, 5>{0, 1, 0xFFFFFFFFFFFFFFFFull, 0xAAAAAAAAAAAAAAAAull, 0x123456789ABCDEF0ull}) {
                        auto state = LoweringExecution::InitialState();
                        state.Input[pair.first] = source;
                        state.Input[pair.second] = mask;
                        auto trial = state;
                        native.Run(trial);
                        const auto wanted = model(operation, wide, trial.Input[pair.first], trial.Input[pair.second]);
                        if (trial.Output[operands.Destination] != wanted)
                            throw std::runtime_error(describe(operands, source, mask) + ": the host produced " + std::to_string(trial.Output[operands.Destination]) + ", the model " + std::to_string(wanted));
                        if ((trial.Flags & LoweringExecution::kFlagMask) != (trial.InputFlags & LoweringExecution::kFlagMask))
                            throw std::runtime_error(describe(operands, source, mask) + ": the host changed the flags");
                    }
                }
            }
        }
    }
}

void execution() {
    constexpr std::array<std::uint64_t, 6> sources{0, 1, 0x80000000ull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xABCD123456789000ull};
    constexpr std::array<std::uint64_t, 8> masks{0, 1, 0xFFFFFFFFFFFFFFFFull, 0xAAAAAAAAAAAAAAAAull, 0x5555ull, 0x8000000000000000ull, 0xFF00FF00FF00FF00ull, 0x123456789ABCDEF0ull};
    for (const auto operation : kOperations) {
        for (bool wide : {false, true}) {
            for (std::uint8_t dst = 0; dst < 16; ++dst) {
                for (const auto pair : std::array<std::pair<std::uint8_t, std::uint8_t>, 9>{{{6, 2}, {dst, 10}, {11, dst}, {dst, dst}, {4, 9}, {9, 4}, {1, 6}, {6, 1}, {1, 1}}}) {
                    const auto site = instruction(operation, wide, dst, pair.first, pair.second);
                    const auto operands = *Codegen::DecodeParallelBit(site.data(), site.size());
                    const auto execution = executable(site, 0);
                    for (const auto source : sources) {
                        for (const auto mask : masks) {
                            auto state = LoweringExecution::InitialState();
                            state.Input[pair.first] = source;
                            state.Input[pair.second] = mask;
                            run(execution, operands, state, 0);
                        }
                    }
                }
            }
        }
    }
    for (const auto operation : kOperations) {
        for (bool wide : {false, true}) {
            for (const Bytes tail : {Bytes{0x06}, Bytes{0x46, 0x10}, Bytes{0x84, 0x24, 0x40, 0, 0, 0}, Bytes{0x04, 0xCA}, Bytes{0x85, 0x10, 0, 0, 0}, Bytes{0x05, 0, 0, 0, 0}}) {
                for (const auto mask : std::array<std::uint64_t, 3>{0, 0xFEDCBA9876543210ull, 0xFFFFFFFF00000000ull}) {
                    const auto site = memorySite(operation, wide, tail);
                    const auto operands = *Codegen::DecodeParallelBit(site.data(), site.size());
                    const auto execution = executable(site, mask);
                    auto state = LoweringExecution::InitialState();
                    state.Input[6] = reinterpret_cast<std::uint64_t>(execution.Memory()) - (tail[0] == 0x46 ? 16 : 0);
                    state.Input[2] = reinterpret_cast<std::uint64_t>(execution.Memory()) - 3 * 8;
                    state.Input[1] = 3;
                    state.Input[5] = reinterpret_cast<std::uint64_t>(execution.Memory()) - 16;
                    state.Output[0] = mask;
                    run(execution, operands, state, mask);
                }
            }
            for (const std::uint8_t dst : std::array<std::uint8_t, 2>{0, 4}) {
                for (const std::int32_t displacement : {-192, -184, -183, -176, -168, -144, -136}) {
                    Bytes tail{static_cast<std::uint8_t>(0x84 | (dst << 3)), 0x24};
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        tail.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(displacement) >> shift));
                    constexpr std::uint64_t mask = 0xFEDCBA9876543210ull;
                    const auto site = memorySite(operation, wide, tail);
                    const auto operands = *Codegen::DecodeParallelBit(site.data(), site.size());
                    const Execution execution(site, mask, false, [&](Codegen::StubBodyBuilder& body, const Bytes&) {
                        for (int part = 0; part < 2; ++part) {
                            Bytes store{0xC7, 0x84, 0x24};
                            const auto slot = static_cast<std::uint32_t>(displacement + part * 4);
                            const auto word = static_cast<std::uint32_t>(mask >> (part * 32));
                            for (unsigned shift = 0; shift < 32; shift += 8) store.push_back(static_cast<std::uint8_t>(slot >> shift));
                            for (unsigned shift = 0; shift < 32; shift += 8) store.push_back(static_cast<std::uint8_t>(word >> shift));
                            body.Raw(store);
                        }
                        Codegen::EmitParallelBit(body, operands);
                    });
                    auto state = LoweringExecution::InitialState();
                    run(execution, operands, state, mask);
                }
            }
        }
    }
}
#else
void hardware() { std::cout << "parallel bit host checks require an x86-64 Linux, macOS or Windows build\n"; }
void execution() { std::cout << "parallel bit execution checks require an x86-64 Linux, macOS or Windows build\n"; }
#endif

}

int main() {
    try {
        decoding();
        matching();
        hardware();
        execution();
        std::cout << "parallel bit lowering checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
