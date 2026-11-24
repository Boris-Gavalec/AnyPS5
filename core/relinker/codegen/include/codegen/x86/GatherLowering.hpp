#ifndef CODEGEN_X86_GATHERLOWERING_HPP
#define CODEGEN_X86_GATHERLOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>
#include <vector>

namespace Codegen {

struct GatherOperands {
    const char* Name;
    std::uint8_t Destination;
    std::uint8_t Mask;
    std::uint8_t Index;
    std::uint8_t Base;
    bool HasBase;
    std::uint8_t Scale;
    std::int32_t Displacement;
    std::uint8_t IndexSize;
    std::uint8_t ElementSize;
    int Elements;
    std::vector<std::uint8_t> Prefixes;
};

[[nodiscard]] std::optional<GatherOperands> DecodeGather(const std::uint8_t* data, std::size_t length);
void EmitGather(StubBodyBuilder& body, const GatherOperands& operands);

}

#endif
