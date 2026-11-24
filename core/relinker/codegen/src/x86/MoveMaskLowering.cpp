#include <codegen/x86/MoveMaskLowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <array>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::int32_t kLowSlot = 8;
constexpr std::int32_t kHighSlot = 16;
constexpr std::int32_t kStackResultSlot = 24;
constexpr std::int32_t kVectorSlot = 32;
constexpr std::int32_t kFrame = 192;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint8_t kLanes = 16;
constexpr const char* kName = "VPMOVMSKB";
constexpr Form kMoveMask128{Map0F, Prefix66, false, false, 0xD7};

std::uint8_t _vector(const std::uint8_t source) {
    for (std::uint8_t reg = 15;; --reg)
        if (reg != source) return reg;
}

}

std::optional<MoveMaskOperands> DecodeMoveMask(const std::uint8_t* data, const std::size_t length) {
    const auto instruction = Decode(data, length);
    if (!instruction || instruction->Map != Map0F || instruction->Prefix != Prefix66 || instruction->Opcode != 0xD7 || !instruction->Vector256)
        return std::nullopt;
    if (instruction->Memory) throw CodegenException(std::string(kName) + " requires a register source");
    if (instruction->Vvvv != 0) throw CodegenException(std::string(kName) + " requires VEX.vvvv of zero");
    if (instruction->End != length) throw CodegenException(std::string(kName) + " has trailing bytes");
    return MoveMaskOperands{instruction->Reg, instruction->Rm};
}

void EmitMoveMask(StubBodyBuilder& body, const MoveMaskOperands& operands) {
    auto used = UsedRegisters({operands.Destination}, std::nullopt);
    const auto low = Scratch(used, kName);
    const auto high = Scratch(used, kName);
    const auto vector = _vector(operands.Source);
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - kFrame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, low, kLowSlot);
    StackSlot(body, 0x89, true, high, kHighSlot);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kVectorSlot);
    ExtractUpper(body, vector, operands.Source);
    VexEncoding::Register(body, kMoveMask128, low, 0, operands.Source);
    VexEncoding::Register(body, kMoveMask128, high, 0, vector);
    body.Raw(std::array{Rex(false, 0, high), std::uint8_t{0xC1}, static_cast<std::uint8_t>(0xE0 | (high & 7)), kLanes});
    Register(body, 0x09, false, high, low);
    VexEncoding::StackOperand(body, MoveUnaligned256Load, vector, 0, kVectorSlot);
    if (operands.Destination == Stack)
        StackSlot(body, 0x89, false, low, kStackResultSlot);
    else
        Register(body, 0x8B, false, operands.Destination, low);
    StackSlot(body, 0x8B, true, high, kHighSlot);
    StackSlot(body, 0x8B, true, low, kLowSlot);
    body.Raw(std::array{kPopFlags});
    if (operands.Destination == Stack)
        StackSlot(body, 0x8B, false, Stack, kStackResultSlot - kFlagsWord);
    else
        StackSlot(body, 0x8D, true, Stack, kFrame - kFlagsWord);
}

}
