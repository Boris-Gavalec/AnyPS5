#ifndef CODEGEN_X86_FLOAT16LOWERING_HPP
#define CODEGEN_X86_FLOAT16LOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>

namespace Codegen {

struct Float16Operands {
    std::uint8_t Destination;
    std::uint8_t Source;
    bool Vector256;
    std::optional<MemoryOperand> Memory;
};

[[nodiscard]] std::optional<Float16Operands> DecodeFloat16(const std::uint8_t* data, std::size_t length);
void EmitFloat16(StubBodyBuilder& body, const Float16Operands& operands);

}

#endif
