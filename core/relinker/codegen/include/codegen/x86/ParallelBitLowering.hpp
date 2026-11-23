#ifndef CODEGEN_X86_PARALLELBITLOWERING_HPP
#define CODEGEN_X86_PARALLELBITLOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>

namespace Codegen {

enum class ParallelBitOperation : std::uint8_t {
    Deposit,
    Extract,
};

struct ParallelBitOperands {
    ParallelBitOperation Operation;
    std::uint8_t Destination;
    std::uint8_t Source;
    std::uint8_t Mask;
    bool Wide;
    std::optional<MemoryOperand> Memory;
};

[[nodiscard]] const char* ParallelBitName(ParallelBitOperation operation);
[[nodiscard]] std::optional<ParallelBitOperands> DecodeParallelBit(const std::uint8_t* data, std::size_t length);
void EmitParallelBit(StubBodyBuilder& body, const ParallelBitOperands& operands);

}

#endif
