#include <codegen/x86/VariableShiftLowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <array>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::uint8_t kCountRegister = 1;
constexpr std::int32_t kCountSlot = 8;
constexpr std::int32_t kValueSlot = 16;
constexpr std::int32_t kConstantSlot = 24;
constexpr std::int32_t kVectorSlot = 32;
constexpr std::int32_t kSourceSlot = 64;
constexpr std::int32_t kCountsSlot = 96;
constexpr std::int32_t kResultSlot = 128;
constexpr std::int32_t kFrame = 128 + 160;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint8_t kCompareImmediate = 0x83;
constexpr std::uint8_t kShiftByCount = 0xD3;
constexpr std::uint8_t kMoveIfAboveOrEqual = 0x43;
constexpr std::uint8_t kMoveIfAbove = 0x47;

std::uint8_t _extension(const VariableShiftOperation operation) {
    switch (operation) {
    case VariableShiftOperation::Left: return 4;
    case VariableShiftOperation::LogicalRight: return 5;
    case VariableShiftOperation::ArithmeticRight: return 7;
    }
    throw CodegenException("unknown variable shift operation");
}

std::vector<std::uint8_t> _twoByte(const std::uint8_t opcode, const bool wide, const std::uint8_t reg, const std::uint8_t rm) {
    return {Rex(wide, reg, rm), 0x0F, opcode, static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))};
}

}

const char* VariableShiftName(const VariableShiftOperation operation, const bool wide) {
    switch (operation) {
    case VariableShiftOperation::Left: return wide ? "VPSLLVQ" : "VPSLLVD";
    case VariableShiftOperation::LogicalRight: return wide ? "VPSRLVQ" : "VPSRLVD";
    case VariableShiftOperation::ArithmeticRight: return "VPSRAVD";
    }
    throw CodegenException("unknown variable shift operation");
}

std::optional<VariableShiftOperands> DecodeVariableShift(const std::uint8_t* data, const std::size_t length) {
    const auto instruction = Decode(data, length);
    if (!instruction || instruction->Map != Map0F38 || instruction->Prefix != Prefix66) return std::nullopt;
    VariableShiftOperation operation;
    if (instruction->Opcode == 0x47)
        operation = VariableShiftOperation::Left;
    else if (instruction->Opcode == 0x45)
        operation = VariableShiftOperation::LogicalRight;
    else if (instruction->Opcode == 0x46)
        operation = VariableShiftOperation::ArithmeticRight;
    else
        return std::nullopt;
    if (operation == VariableShiftOperation::ArithmeticRight && instruction->Wide)
        throw CodegenException("VPSRAVQ needs AVX-512 and is not lowered");
    const auto name = VariableShiftName(operation, instruction->Wide);
    if (instruction->End != length) throw CodegenException(std::string(name) + " has trailing bytes");
    return VariableShiftOperands{operation, instruction->Reg, instruction->Vvvv, instruction->Rm, instruction->Wide, instruction->Vector256, instruction->Memory};
}

void EmitVariableShift(StubBodyBuilder& body, const VariableShiftOperands& operands) {
    const auto name = VariableShiftName(operands.Operation, operands.Wide);
    const auto size = operands.Wide ? 8 : 4;
    const auto elements = (operands.Vector256 ? 32 : 16) / size;
    const auto bits = static_cast<std::uint8_t>(size * 8);
    const auto arithmetic = operands.Operation == VariableShiftOperation::ArithmeticRight;
    auto used = UsedRegisters({kCountRegister}, operands.Memory);
    const auto value = Scratch(used, name);
    const auto constant = Scratch(used, name);
    std::uint8_t vector = 0;
    if (operands.Memory) {
        std::array<bool, 16> taken{};
        taken[operands.Destination] = true;
        taken[operands.First] = true;
        for (std::uint8_t reg = 15;; --reg)
            if (!taken[reg]) { vector = reg; break; }
    }
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - kFrame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, kCountRegister, kCountSlot);
    StackSlot(body, 0x89, true, value, kValueSlot);
    StackSlot(body, 0x89, true, constant, kConstantSlot);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.First, 0, kSourceSlot);
    if (operands.Memory) {
        VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kVectorSlot);
        VexEncoding::Memory(body, operands.Vector256 ? MoveUnaligned256Load : MoveUnaligned128Load, vector, 0, *operands.Memory, kFrame, name);
        VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kCountsSlot);
    } else {
        VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.Second, 0, kCountsSlot);
    }
    if (arithmetic) {
        auto limit = RegisterBytes(0xC7, operands.Wide, 0, constant);
        Append32(limit, bits - 1);
        body.Raw(limit);
    } else {
        Register(body, 0x31, false, constant, constant);
    }
    for (int element = 0; element < elements; ++element) {
        const auto offset = element * size;
        StackSlot(body, 0x8B, operands.Wide, value, kSourceSlot + offset);
        StackSlot(body, 0x8B, operands.Wide, kCountRegister, kCountsSlot + offset);
        if (arithmetic) {
            body.Raw(std::array{Rex(operands.Wide, 0, kCountRegister), kCompareImmediate, std::uint8_t{0xF9}, static_cast<std::uint8_t>(bits - 1)});
            body.Raw(_twoByte(kMoveIfAbove, operands.Wide, kCountRegister, constant));
            body.Raw(std::array{Rex(operands.Wide, 0, value), kShiftByCount, static_cast<std::uint8_t>(0xC0 | (_extension(operands.Operation) << 3) | (value & 7))});
        } else {
            body.Raw(std::array{Rex(operands.Wide, 0, value), kShiftByCount, static_cast<std::uint8_t>(0xC0 | (_extension(operands.Operation) << 3) | (value & 7))});
            body.Raw(std::array{Rex(operands.Wide, 0, kCountRegister), kCompareImmediate, std::uint8_t{0xF9}, bits});
            body.Raw(_twoByte(kMoveIfAboveOrEqual, operands.Wide, value, constant));
        }
        StackSlot(body, 0x89, operands.Wide, value, kResultSlot + offset);
    }
    VexEncoding::StackOperand(body, operands.Vector256 ? MoveUnaligned256Load : MoveUnaligned128Load, operands.Destination, 0, kResultSlot);
    if (operands.Memory)
        VexEncoding::StackOperand(body, MoveUnaligned256Load, vector, 0, kVectorSlot);
    StackSlot(body, 0x8B, true, constant, kConstantSlot);
    StackSlot(body, 0x8B, true, value, kValueSlot);
    StackSlot(body, 0x8B, true, kCountRegister, kCountSlot);
    body.Raw(std::array{kPopFlags});
    StackSlot(body, 0x8D, true, Stack, kFrame - kFlagsWord);
}

}
