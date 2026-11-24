#include <codegen/x86/BlendLowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <array>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::int32_t kSaveSize = 128;
constexpr std::int32_t kFrame = 128 + kSaveSize;
constexpr const char* kName = "VPBLENDVB";
constexpr Form kBlend128{Map0F3A, Prefix66, false, false, 0x4C};

std::uint8_t _selector(const std::uint8_t mask) {
    return static_cast<std::uint8_t>((mask & 15) << 4);
}

}

std::optional<BlendOperands> DecodeBlend(const std::uint8_t* data, const std::size_t length) {
    const auto instruction = Decode(data, length);
    if (!instruction || instruction->Map != Map0F3A || instruction->Prefix != Prefix66 || instruction->Opcode != 0x4C || !instruction->Vector256)
        return std::nullopt;
    if (instruction->Wide) throw CodegenException(std::string(kName) + " requires VEX.W of zero");
    if (instruction->End >= length) throw CodegenException(std::string(kName) + " is missing its register selector");
    const auto selector = data[instruction->End];
    if ((selector & 15) != 0) throw CodegenException(std::string(kName) + " has a non-zero selector remainder");
    if (instruction->End + 1 != length) throw CodegenException(std::string(kName) + " has trailing bytes");
    return BlendOperands{instruction->Reg, instruction->Vvvv, instruction->Rm, static_cast<std::uint8_t>(selector >> 4), instruction->Memory};
}

void EmitBlend(StubBodyBuilder& body, const BlendOperands& operands) {
    std::array<bool, 16> used{};
    used[operands.Destination] = true;
    used[operands.First] = true;
    used[operands.Mask] = true;
    if (!operands.Memory) used[operands.Second] = true;
    std::array<std::uint8_t, 4> scratch{};
    std::size_t taken = 0;
    for (std::uint8_t reg = 15; taken < scratch.size(); --reg)
        if (!used[reg]) scratch[taken++] = reg;
    const auto firstHigh = scratch[0];
    const auto secondHigh = scratch[1];
    const auto maskHigh = scratch[2];
    const auto loaded = scratch[3];
    StackSlot(body, 0x8D, true, Stack, -kFrame);
    for (std::size_t index = 0; index < scratch.size(); ++index)
        VexEncoding::StackOperand(body, MoveUnaligned256Store, scratch[index], 0, static_cast<std::int32_t>(index * 32));
    auto second = operands.Second;
    if (operands.Memory) {
        VexEncoding::Memory(body, MoveUnaligned256Load, loaded, 0, *operands.Memory, kFrame, kName);
        second = loaded;
    }
    ExtractUpper(body, firstHigh, operands.First);
    ExtractUpper(body, secondHigh, second);
    ExtractUpper(body, maskHigh, operands.Mask);
    VexEncoding::Register(body, kBlend128, firstHigh, firstHigh, secondHigh, _selector(maskHigh));
    VexEncoding::Register(body, kBlend128, operands.Destination, operands.First, second, _selector(operands.Mask));
    InsertUpper(body, operands.Destination, operands.Destination, firstHigh);
    for (std::size_t index = 0; index < scratch.size(); ++index)
        VexEncoding::StackOperand(body, MoveUnaligned256Load, scratch[index], 0, static_cast<std::int32_t>(index * 32));
    StackSlot(body, 0x8D, true, Stack, kFrame);
}

}
