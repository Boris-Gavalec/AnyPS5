#include <codegen/x86/ParallelBitLowering.hpp>
#include <codegen/x86/ScalarLowering.hpp>

namespace Codegen {

using namespace ScalarLowering;

namespace {

constexpr std::int32_t kResultSlot = 8;
constexpr std::int32_t kSourceSlot = 16;
constexpr std::int32_t kMaskSlot = 24;
constexpr std::int32_t kIndexSlot = 32;
constexpr std::int32_t kStackResultSlot = 40;
constexpr std::int32_t kSpillBytes = 48;
constexpr std::int32_t kFrame = 176;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint8_t kJumpZero = 0x74;
constexpr std::uint8_t kJumpNoCarry = 0x73;
constexpr std::uint8_t kJump = 0xEB;
constexpr std::uint8_t kBitScanForward = 0xBC;
constexpr std::uint8_t kBitScanReverse = 0xBD;
constexpr std::uint8_t kBitTest = 0xA3;
constexpr std::uint8_t kBitTestSet = 0xAB;
constexpr std::uint8_t kBitTestReset = 0xB3;
constexpr std::size_t kShortBranchLimit = 127;

void _append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
}

std::vector<std::uint8_t> _twoByte(const std::uint8_t opcode, const bool wide, const std::uint8_t reg, const std::uint8_t rm) {
    return {Rex(wide, reg, rm), 0x0F, opcode, static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))};
}

std::vector<std::uint8_t> _addCarry(const bool wide, const std::uint8_t reg) {
    return {Rex(wide, 0, reg), 0x83, static_cast<std::uint8_t>(0xD0 | (reg & 7)), 0x00};
}

std::vector<std::uint8_t> _shiftRightOne(const bool wide, const std::uint8_t reg) {
    return {Rex(wide, 0, reg), 0xD1, static_cast<std::uint8_t>(0xE8 | (reg & 7))};
}

std::vector<std::uint8_t> _loop(const ParallelBitOperands& operands, const std::uint8_t result, const std::uint8_t source, const std::uint8_t mask, const std::uint8_t index) {
    const auto wide = operands.Wide;
    const auto extract = operands.Operation == ParallelBitOperation::Extract;
    auto work = _twoByte(kBitTestReset, wide, index, mask);
    if (extract) {
        _append(work, RegisterBytes(0x01, wide, result, result));
        _append(work, _twoByte(kBitTest, wide, index, source));
        _append(work, _addCarry(wide, result));
    } else {
        _append(work, _shiftRightOne(wide, source));
        const auto set = _twoByte(kBitTestSet, wide, index, result);
        work.push_back(kJumpNoCarry);
        work.push_back(static_cast<std::uint8_t>(set.size()));
        _append(work, set);
    }
    auto body = _twoByte(extract ? kBitScanReverse : kBitScanForward, wide, index, mask);
    body.push_back(kJumpZero);
    body.push_back(static_cast<std::uint8_t>(work.size() + 2));
    _append(body, work);
    body.push_back(kJump);
    if (body.size() + 1 > kShortBranchLimit)
        throw CodegenException(std::string(ParallelBitName(operands.Operation)) + " loop exceeds a short branch");
    body.push_back(static_cast<std::uint8_t>(-static_cast<std::int32_t>(body.size() + 1)));
    return body;
}

void _loadRegister(StubBodyBuilder& body, const bool wide, const std::uint8_t target, const std::uint8_t origin, const std::int32_t frame) {
    if (origin == Stack)
        StackSlot(body, 0x8D, true, target, frame);
    else
        Register(body, 0x8B, wide, target, origin);
}

}

const char* ParallelBitName(const ParallelBitOperation operation) {
    switch (operation) {
    case ParallelBitOperation::Deposit: return "PDEP";
    case ParallelBitOperation::Extract: return "PEXT";
    }
    throw CodegenException("unknown parallel bit operation");
}

std::optional<ParallelBitOperands> DecodeParallelBit(const std::uint8_t* data, const std::size_t length) {
    std::size_t vex = 0;
    while (vex < length && LegacyPrefix(data[vex])) ++vex;
    if (vex + 5 > length || data[vex] != 0xC4 || (data[vex + 1] & 0x1F) != 2 || data[vex + 3] != 0xF5) return std::nullopt;
    ParallelBitOperation operation;
    if ((data[vex + 2] & 3) == 3)
        operation = ParallelBitOperation::Deposit;
    else if ((data[vex + 2] & 3) == 2)
        operation = ParallelBitOperation::Extract;
    else
        return std::nullopt;
    const auto name = ParallelBitName(operation);
    if ((data[vex + 2] & 4) != 0) throw CodegenException(std::string(name) + " requires VEX.L=0");
    const auto extensions = static_cast<std::uint8_t>(~data[vex + 1]);
    const auto modrm = data[vex + 4];
    ParallelBitOperands operands{
        operation,
        static_cast<std::uint8_t>(((modrm >> 3) & 7) | ((extensions & 0x80) >> 4)),
        static_cast<std::uint8_t>((~data[vex + 2] >> 3) & 15),
        static_cast<std::uint8_t>((modrm & 7) | ((extensions & 0x20) >> 2)),
        (data[vex + 2] & 0x80) != 0, std::nullopt};
    std::size_t end = vex + 5;
    if ((modrm >> 6) != 3) {
        const auto rex = static_cast<std::uint8_t>(((extensions & 0x20) >> 5) | ((extensions & 0x40) >> 5));
        operands.Memory = DecodeMemoryOperand(data, length, vex + 4, rex, {data, data + vex});
        end = vex + 4 + operands.Memory->EncodedSize;
    }
    if (end != length) throw CodegenException(std::string(name) + " has trailing bytes");
    return operands;
}

void EmitParallelBit(StubBodyBuilder& body, const ParallelBitOperands& operands) {
    const auto name = ParallelBitName(operands.Operation);
    auto used = operands.Memory ? UsedRegisters({operands.Destination, operands.Source}, operands.Memory)
                                : UsedRegisters({operands.Destination, operands.Source, operands.Mask}, std::nullopt);
    const auto result = Scratch(used, name);
    const auto source = Scratch(used, name);
    const auto mask = Scratch(used, name);
    const auto index = Scratch(used, name);
    const auto frame = SelectFrame(operands.Memory, operands.Wide, kFrame, kSpillBytes);
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - frame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, result, kResultSlot);
    StackSlot(body, 0x89, true, source, kSourceSlot);
    StackSlot(body, 0x89, true, mask, kMaskSlot);
    StackSlot(body, 0x89, true, index, kIndexSlot);
    _loadRegister(body, operands.Wide, source, operands.Source, frame);
    if (operands.Memory)
        Memory(body, 0x8B, operands.Wide, *operands.Memory, mask, name, frame);
    else
        _loadRegister(body, operands.Wide, mask, operands.Mask, frame);
    Register(body, 0x31, false, result, result);
    body.Raw(_loop(operands, result, source, mask, index));
    if (operands.Destination == Stack)
        StackSlot(body, 0x89, operands.Wide, result, kStackResultSlot);
    else
        Register(body, 0x8B, operands.Wide, operands.Destination, result);
    StackSlot(body, 0x8B, true, index, kIndexSlot);
    StackSlot(body, 0x8B, true, mask, kMaskSlot);
    StackSlot(body, 0x8B, true, source, kSourceSlot);
    StackSlot(body, 0x8B, true, result, kResultSlot);
    body.Raw(std::array{kPopFlags});
    if (operands.Destination == Stack)
        StackSlot(body, 0x8B, operands.Wide, Stack, kStackResultSlot - kFlagsWord);
    else
        StackSlot(body, 0x8D, true, Stack, frame - kFlagsWord);
}

}
