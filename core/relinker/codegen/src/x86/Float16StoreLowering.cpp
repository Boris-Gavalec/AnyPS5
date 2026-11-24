#include <codegen/x86/Float16StoreLowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <array>
#include <map>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::uint8_t kCountRegister = 1;
constexpr std::int32_t kCountSlot = 8;
constexpr std::int32_t kIndexSlot = 16;
constexpr std::int32_t kValueSlot = 24;
constexpr std::int32_t kRemainderSlot = 32;
constexpr std::int32_t kScratchSlot = 40;
constexpr std::int32_t kModeSlot = 48;
constexpr std::int32_t kSignSlot = 56;
constexpr std::int32_t kAwaySlot = 64;
constexpr std::int32_t kBaseSlot = 68;
constexpr std::int32_t kControlWordSlot = 72;
constexpr std::int32_t kVectorSlot = 80;
constexpr std::int32_t kSourceSlot = 112;
constexpr std::int32_t kResultSlot = 144;
constexpr std::int32_t kFrame = 128 + 160;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint32_t kSignMask = 0x8000;
constexpr std::uint32_t kMagnitudeMask = 0x7FFFFFFF;
constexpr std::uint32_t kInfiniteBits = 0x7F800000;
constexpr std::uint32_t kFloatMantissaMask = 0x7FFFFF;
constexpr std::uint32_t kImplicitBit = 0x800000;
constexpr std::uint32_t kHalfPayloadMask = 0x3FF;
constexpr std::uint32_t kHalfInfinite = 0x7C00;
constexpr std::uint32_t kHalfLargest = 0x7BFF;
constexpr std::uint32_t kHalfQuiet = 0x200;
constexpr std::uint8_t kMantissaShift = 13;
constexpr std::uint8_t kSignShift = 16;
constexpr std::uint8_t kFloatExponentShift = 23;
constexpr std::int32_t kFloatBias = 127;
constexpr std::int32_t kHalfBias = 15;
constexpr std::int32_t kMaximumExponent = 16;
constexpr std::int32_t kMinimumExponent = -14;
constexpr std::int32_t kWidestShift = 25;
constexpr std::uint8_t kRoundNearest = 0;
constexpr std::uint8_t kRoundDown = 1;
constexpr std::uint8_t kRoundUp = 2;
constexpr std::uint8_t kRoundZero = 3;
constexpr std::uint8_t kControlFromState = 4;
constexpr std::uint8_t kStateRoundShift = 13;
constexpr const char* kName = "VCVTPS2PH";
constexpr Form kMoveQuadLoad{Map0F, PrefixF3, false, false, 0x7E};
constexpr Form kMoveQuadStore{Map0F, Prefix66, false, false, 0xD6};
constexpr Form kMoveUnaligned128Store{Map0F, PrefixF3, false, false, 0x7F};

enum Condition : std::uint8_t {
    Equal = 0x84,
    NotEqual = 0x85,
    Below = 0x82,
    AboveOrEqual = 0x83,
    Above = 0x87,
    Less = 0x8C,
    LessOrEqual = 0x8E,
    Always = 0,
};

enum Label : int {
    Loop,
    Store,
    Finite,
    Quiet,
    HaveAway,
    NotNearest,
    DirectedUp,
    InRange,
    Denormal,
    HaveShift,
    Rounding,
    Directed,
    Increment,
    NoIncrement,
    Clamp,
    Infinite,
};

class Assembler {
public:
    void Emit(const std::vector<std::uint8_t>& more) { _bytes.insert(_bytes.end(), more.begin(), more.end()); }

    void Mark(const Label label) { _labels[label] = _bytes.size(); }

    void Jump(const Condition condition, const Label label) {
        if (condition == Always)
            _bytes.push_back(0xE9);
        else {
            _bytes.push_back(0x0F);
            _bytes.push_back(static_cast<std::uint8_t>(condition));
        }
        _fixups.push_back({_bytes.size(), label});
        Append32(_bytes, 0);
    }

    std::vector<std::uint8_t> Finish() {
        for (const auto& fixup : _fixups) {
            const auto target = _labels.find(fixup.Target);
            if (target == _labels.end()) throw CodegenException(std::string(kName) + " has an unplaced label");
            const auto displacement = static_cast<std::int64_t>(target->second) - static_cast<std::int64_t>(fixup.At + 4);
            auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
            for (unsigned shift = 0; shift < 32; shift += 8)
                _bytes[fixup.At + shift / 8] = static_cast<std::uint8_t>(value >> shift);
        }
        return _bytes;
    }

private:
    struct Fixup {
        std::size_t At;
        Label Target;
    };

    std::vector<std::uint8_t> _bytes;
    std::vector<Fixup> _fixups;
    std::map<Label, std::size_t> _labels;
};

std::vector<std::uint8_t> _immediate(const std::uint8_t extension, const std::uint8_t reg, const std::uint32_t value) {
    std::vector<std::uint8_t> bytes{Rex(false, 0, reg), 0x81, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7))};
    Append32(bytes, static_cast<std::int32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> _shiftImmediate(const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t count) {
    return {Rex(false, 0, reg), 0xC1, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7)), count};
}

std::vector<std::uint8_t> _shiftByCount(const std::uint8_t extension, const std::uint8_t reg) {
    return {Rex(false, 0, reg), 0xD3, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7))};
}

std::vector<std::uint8_t> _loadImmediate(const std::uint8_t reg, const std::uint32_t value) {
    std::vector<std::uint8_t> bytes{Rex(false, 0, reg), static_cast<std::uint8_t>(0xB8 | (reg & 7))};
    Append32(bytes, static_cast<std::int32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> _testImmediate(const std::uint8_t reg, const std::uint32_t value) {
    std::vector<std::uint8_t> bytes{Rex(false, 0, reg), 0xF7, static_cast<std::uint8_t>(0xC0 | (reg & 7))};
    Append32(bytes, static_cast<std::int32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> _unary(const std::uint8_t extension, const std::uint8_t reg) {
    return {Rex(false, 0, reg), 0xF7, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7))};
}

std::vector<std::uint8_t> _stack(const std::uint8_t opcode, const std::uint8_t reg, const std::int32_t displacement) {
    return StackBytes(opcode, false, reg, displacement);
}

std::vector<std::uint8_t> _indexed(const std::vector<std::uint8_t>& opcode, const std::uint8_t reg, const std::uint8_t index, const std::uint8_t scale, const std::int32_t displacement, const bool word = false) {
    std::vector<std::uint8_t> bytes;
    if (word) bytes.push_back(0x66);
    bytes.push_back(static_cast<std::uint8_t>(0x40 | ((reg & 8) >> 1) | ((index & 8) >> 2)));
    bytes.insert(bytes.end(), opcode.begin(), opcode.end());
    bytes.push_back(static_cast<std::uint8_t>(0x84 | ((reg & 7) << 3)));
    bytes.push_back(static_cast<std::uint8_t>((scale << 6) | ((index & 7) << 3) | 4));
    Append32(bytes, displacement);
    return bytes;
}

struct Registers {
    std::uint8_t Value;
    std::uint8_t Remainder;
    std::uint8_t Scratch;
    std::uint8_t Mode;
    std::uint8_t Sign;
    std::uint8_t Index;
};

void _away(Assembler& code, const Registers& registers) {
    const auto scratch = registers.Scratch;
    code.Emit(RegisterBytes(0x31, false, scratch, scratch));
    code.Emit(RegisterBytes(0x85, false, registers.Mode, registers.Mode));
    code.Jump(NotEqual, NotNearest);
    code.Emit(_immediate(0, scratch, 1));
    code.Jump(Always, HaveAway);
    code.Mark(NotNearest);
    code.Emit(_immediate(7, registers.Mode, kRoundZero));
    code.Jump(Equal, HaveAway);
    code.Emit(_immediate(7, registers.Mode, kRoundUp));
    code.Jump(Equal, DirectedUp);
    code.Emit(RegisterBytes(0x85, false, registers.Sign, registers.Sign));
    code.Jump(Equal, HaveAway);
    code.Emit(_immediate(0, scratch, 1));
    code.Jump(Always, HaveAway);
    code.Mark(DirectedUp);
    code.Emit(RegisterBytes(0x85, false, registers.Sign, registers.Sign));
    code.Jump(NotEqual, HaveAway);
    code.Emit(_immediate(0, scratch, 1));
    code.Mark(HaveAway);
    code.Emit(_stack(0x89, scratch, kAwaySlot));
}

std::vector<std::uint8_t> _loop(const Registers& registers, const int elements) {
    const auto value = registers.Value;
    const auto remainder = registers.Remainder;
    const auto scratch = registers.Scratch;
    const auto sign = registers.Sign;
    const auto index = registers.Index;
    Assembler code;
    code.Mark(Loop);
    code.Emit(_indexed({0x8B}, value, index, 2, kSourceSlot));
    code.Emit(RegisterBytes(0x8B, false, sign, value));
    code.Emit(_shiftImmediate(5, sign, kSignShift));
    code.Emit(_immediate(4, sign, kSignMask));
    code.Emit(_immediate(4, value, kMagnitudeMask));
    code.Jump(Equal, Store);
    code.Emit(_immediate(7, value, kInfiniteBits));
    code.Jump(Below, Finite);
    code.Emit(RegisterBytes(0x8B, false, scratch, value));
    code.Emit(_shiftImmediate(5, scratch, kMantissaShift));
    code.Emit(_immediate(4, scratch, kHalfPayloadMask));
    code.Emit(_immediate(1, scratch, kHalfInfinite));
    code.Emit(_immediate(7, value, kInfiniteBits));
    code.Jump(Equal, Quiet);
    code.Emit(_immediate(1, scratch, kHalfQuiet));
    code.Mark(Quiet);
    code.Emit(RegisterBytes(0x8B, false, value, scratch));
    code.Jump(Always, Store);

    code.Mark(Finite);
    _away(code, registers);
    code.Emit(RegisterBytes(0x8B, false, scratch, value));
    code.Emit(_shiftImmediate(5, scratch, kFloatExponentShift));
    code.Emit(_immediate(5, scratch, static_cast<std::uint32_t>(kFloatBias)));
    code.Emit(_immediate(7, scratch, static_cast<std::uint32_t>(kMaximumExponent)));
    code.Jump(Less, InRange);
    code.Jump(Always, Clamp);

    code.Mark(InRange);
    code.Emit(_immediate(4, value, kFloatMantissaMask));
    code.Emit(_immediate(1, value, kImplicitBit));
    code.Emit(_immediate(7, scratch, static_cast<std::uint32_t>(kMinimumExponent)));
    code.Jump(Less, Denormal);
    code.Emit(_immediate(0, scratch, static_cast<std::uint32_t>(kHalfBias - 1)));
    code.Emit(_shiftImmediate(4, scratch, 10));
    code.Emit(_stack(0x89, scratch, kBaseSlot));
    code.Emit(_loadImmediate(kCountRegister, kMantissaShift));
    code.Jump(Always, Rounding);

    code.Mark(Denormal);
    code.Emit(_unary(3, scratch));
    code.Emit(_immediate(5, scratch, 1));
    code.Emit(_immediate(7, scratch, static_cast<std::uint32_t>(kWidestShift)));
    code.Jump(LessOrEqual, HaveShift);
    code.Emit(_loadImmediate(scratch, kWidestShift));
    code.Mark(HaveShift);
    code.Emit(RegisterBytes(0x8B, false, kCountRegister, scratch));
    code.Emit(RegisterBytes(0x31, false, scratch, scratch));
    code.Emit(_stack(0x89, scratch, kBaseSlot));

    code.Mark(Rounding);
    code.Emit(RegisterBytes(0x8B, false, remainder, value));
    code.Emit(_shiftByCount(5, value));
    code.Emit(RegisterBytes(0x8B, false, scratch, value));
    code.Emit(_shiftByCount(4, scratch));
    code.Emit(RegisterBytes(0x29, false, scratch, remainder));
    code.Emit(RegisterBytes(0x85, false, registers.Mode, registers.Mode));
    code.Jump(NotEqual, Directed);
    code.Emit(_loadImmediate(scratch, 1));
    code.Emit(_immediate(5, kCountRegister, 1));
    code.Emit(_shiftByCount(4, scratch));
    code.Emit(RegisterBytes(0x39, false, scratch, remainder));
    code.Jump(Above, Increment);
    code.Jump(Below, NoIncrement);
    code.Emit(_testImmediate(value, 1));
    code.Jump(Equal, NoIncrement);
    code.Jump(Always, Increment);

    code.Mark(Directed);
    code.Emit(_stack(0x8B, scratch, kAwaySlot));
    code.Emit(RegisterBytes(0x85, false, scratch, scratch));
    code.Jump(Equal, NoIncrement);
    code.Emit(RegisterBytes(0x85, false, remainder, remainder));
    code.Jump(Equal, NoIncrement);
    code.Mark(Increment);
    code.Emit(_immediate(0, value, 1));
    code.Mark(NoIncrement);
    code.Emit(_stack(0x8B, scratch, kBaseSlot));
    code.Emit(RegisterBytes(0x01, false, scratch, value));
    code.Emit(_immediate(7, value, kHalfInfinite));
    code.Jump(Below, Store);

    code.Mark(Clamp);
    code.Emit(_stack(0x8B, scratch, kAwaySlot));
    code.Emit(RegisterBytes(0x85, false, scratch, scratch));
    code.Jump(NotEqual, Infinite);
    code.Emit(_loadImmediate(value, kHalfLargest));
    code.Jump(Always, Store);
    code.Mark(Infinite);
    code.Emit(_loadImmediate(value, kHalfInfinite));

    code.Mark(Store);
    code.Emit(RegisterBytes(0x09, false, sign, value));
    code.Emit(_indexed({0x89}, value, index, 1, kResultSlot, true));
    code.Emit(_immediate(0, index, 1));
    code.Emit(_immediate(7, index, static_cast<std::uint32_t>(elements)));
    code.Jump(Below, Loop);
    return code.Finish();
}

}

std::optional<Float16StoreOperands> DecodeFloat16Store(const std::uint8_t* data, const std::size_t length) {
    const auto instruction = Decode(data, length);
    if (!instruction || instruction->Map != Map0F3A || instruction->Prefix != Prefix66 || instruction->Opcode != 0x1D)
        return std::nullopt;
    if (instruction->Wide) throw CodegenException(std::string(kName) + " requires VEX.W of zero");
    if (instruction->Vvvv != 0) throw CodegenException(std::string(kName) + " requires VEX.vvvv of zero");
    if (instruction->End >= length) throw CodegenException(std::string(kName) + " is missing its immediate");
    const auto control = data[instruction->End];
    if (instruction->End + 1 != length) throw CodegenException(std::string(kName) + " has trailing bytes");
    return Float16StoreOperands{instruction->Rm, instruction->Reg, static_cast<std::uint8_t>(control & 7), instruction->Vector256, instruction->Memory};
}

void EmitFloat16Store(StubBodyBuilder& body, const Float16StoreOperands& operands) {
    const auto elements = operands.Vector256 ? 8 : 4;
    auto used = UsedRegisters({kCountRegister}, operands.Memory);
    const Registers registers{Scratch(used, kName), Scratch(used, kName), Scratch(used, kName), Scratch(used, kName), Scratch(used, kName), Scratch(used, kName)};
    std::uint8_t vector = 0;
    if (operands.Memory) {
        std::array<bool, 16> taken{};
        taken[operands.Source] = true;
        for (std::uint8_t reg = 15;; --reg)
            if (!taken[reg]) { vector = reg; break; }
    }
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - kFrame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, kCountRegister, kCountSlot);
    StackSlot(body, 0x89, true, registers.Index, kIndexSlot);
    StackSlot(body, 0x89, true, registers.Value, kValueSlot);
    StackSlot(body, 0x89, true, registers.Remainder, kRemainderSlot);
    StackSlot(body, 0x89, true, registers.Scratch, kScratchSlot);
    StackSlot(body, 0x89, true, registers.Mode, kModeSlot);
    StackSlot(body, 0x89, true, registers.Sign, kSignSlot);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.Source, 0, kSourceSlot);
    if (operands.Control >= kControlFromState) {
        body.Raw(std::array<std::uint8_t, 4>{0xC5, 0xF8, 0xAE, 0x9C});
        body.Raw(std::array<std::uint8_t, 1>{0x24});
        std::vector<std::uint8_t> displacement;
        Append32(displacement, kControlWordSlot);
        body.Raw(displacement);
        StackSlot(body, 0x8B, false, registers.Mode, kControlWordSlot);
        body.Raw(_shiftImmediate(5, registers.Mode, kStateRoundShift));
        body.Raw(_immediate(4, registers.Mode, 3));
    } else {
        body.Raw(_loadImmediate(registers.Mode, operands.Control & 3));
    }
    Register(body, 0x31, false, registers.Index, registers.Index);
    body.Raw(_loop(registers, elements));
    if (operands.Memory) {
        VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kVectorSlot);
        VexEncoding::StackOperand(body, operands.Vector256 ? MoveUnaligned128Load : kMoveQuadLoad, vector, 0, kResultSlot);
        VexEncoding::Memory(body, operands.Vector256 ? kMoveUnaligned128Store : kMoveQuadStore, vector, 0, *operands.Memory, kFrame, kName);
        VexEncoding::StackOperand(body, MoveUnaligned256Load, vector, 0, kVectorSlot);
    } else {
        VexEncoding::StackOperand(body, operands.Vector256 ? MoveUnaligned128Load : kMoveQuadLoad, operands.Destination, 0, kResultSlot);
    }
    StackSlot(body, 0x8B, true, registers.Sign, kSignSlot);
    StackSlot(body, 0x8B, true, registers.Mode, kModeSlot);
    StackSlot(body, 0x8B, true, registers.Scratch, kScratchSlot);
    StackSlot(body, 0x8B, true, registers.Remainder, kRemainderSlot);
    StackSlot(body, 0x8B, true, registers.Value, kValueSlot);
    StackSlot(body, 0x8B, true, registers.Index, kIndexSlot);
    StackSlot(body, 0x8B, true, kCountRegister, kCountSlot);
    body.Raw(std::array{kPopFlags});
    StackSlot(body, 0x8D, true, Stack, kFrame - kFlagsWord);
}

}
