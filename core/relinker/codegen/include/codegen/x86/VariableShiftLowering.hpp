#ifndef CODEGEN_X86_VARIABLESHIFTLOWERING_HPP
#define CODEGEN_X86_VARIABLESHIFTLOWERING_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <optional>

namespace Codegen {

enum class VariableShiftOperation : std::uint8_t {
    Left,
    LogicalRight,
    ArithmeticRight,
};

struct VariableShiftOperands {
    VariableShiftOperation Operation;
    std::uint8_t Destination;
    std::uint8_t First;
    std::uint8_t Second;
    bool Wide;
    bool Vector256;
    std::optional<MemoryOperand> Memory;
};

[[nodiscard]] const char* VariableShiftName(VariableShiftOperation operation, bool wide);
[[nodiscard]] std::optional<VariableShiftOperands> DecodeVariableShift(const std::uint8_t* data, std::size_t length);
void EmitVariableShift(StubBodyBuilder& body, const VariableShiftOperands& operands);

}

#endif
