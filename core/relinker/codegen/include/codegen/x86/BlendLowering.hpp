#ifndef CODEGEN_X86_BLENDLOWERING_HPP
#define CODEGEN_X86_BLENDLOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>

namespace Codegen {

struct BlendOperands {
    std::uint8_t Destination;
    std::uint8_t First;
    std::uint8_t Second;
    std::uint8_t Mask;
    std::optional<MemoryOperand> Memory;
};

[[nodiscard]] std::optional<BlendOperands> DecodeBlend(const std::uint8_t* data, std::size_t length);
void EmitBlend(StubBodyBuilder& body, const BlendOperands& operands);

}

#endif
