#ifndef CODEGEN_X86_FLOAT16STORELOWERING_HPP
#define CODEGEN_X86_FLOAT16STORELOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>

namespace Codegen {

struct Float16StoreOperands {
    std::uint8_t Destination;
    std::uint8_t Source;
    std::uint8_t Control;
    bool Vector256;
    std::optional<MemoryOperand> Memory;
};

[[nodiscard]] std::optional<Float16StoreOperands> DecodeFloat16Store(const std::uint8_t* data, std::size_t length);
void EmitFloat16Store(StubBodyBuilder& body, const Float16StoreOperands& operands);

}

#endif
