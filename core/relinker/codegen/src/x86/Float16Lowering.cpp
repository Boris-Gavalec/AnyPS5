#include <codegen/x86/Float16Lowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <array>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::uint8_t kShiftRegister = 1;
constexpr std::int32_t kShiftSlot = 8;
constexpr std::int32_t kMagnitudeSlot = 16;
constexpr std::int32_t kSignSlot = 24;
constexpr std::int32_t kScratchSlot = 32;
constexpr std::int32_t kIndexSlot = 40;
constexpr std::int32_t kVectorSlot = 64;
constexpr std::int32_t kSourceSlot = 96;
constexpr std::int32_t kResultSlot = 128;
constexpr std::int32_t kFrame = 128 + 160;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint8_t kJumpEqual = 0x74;
constexpr std::uint8_t kJumpZero = 0x74;
constexpr std::uint8_t kJump = 0xEB;
constexpr std::uint8_t kJumpBelowLong = 0x82;
constexpr std::uint32_t kSignBit = 0x8000;
constexpr std::uint32_t kExponentMask = 0x7C00;
constexpr std::uint32_t kMagnitudeMask = 0x7FFF;
constexpr std::uint32_t kNormalBias = 0x38000000;
constexpr std::uint32_t kInfiniteBias = 0x70000000;
constexpr std::uint32_t kMantissaMask = 0x007FFFFF;
constexpr std::uint32_t kHalfPayloadMask = 0x03FF;
constexpr std::uint32_t kQuietBit = 0x00400000;
constexpr std::uint8_t kMantissaShift = 13;
constexpr std::uint8_t kSignShift = 16;
constexpr std::uint8_t kFloatMantissaBits = 23;
constexpr std::uint32_t kDenormalExponent = 103;
constexpr const char* kName = "VCVTPH2PS";
constexpr Form kMoveQuadLoad{Map0F, PrefixF3, false, false, 0x7E};

void _append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
}

std::vector<std::uint8_t> _immediate(const std::uint8_t extension, const std::uint8_t reg, const std::uint32_t value) {
    std::vector<std::uint8_t> bytes{Rex(false, 0, reg), 0x81, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7))};
    Append32(bytes, static_cast<std::int32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> _shiftImmediate(const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t count) {
    return {Rex(false, 0, reg), 0xC1, static_cast<std::uint8_t>(0xC0 | (extension << 3) | (reg & 7)), count};
}

std::vector<std::uint8_t> _loadImmediate(const std::uint8_t reg, const std::uint32_t value) {
    std::vector<std::uint8_t> bytes{Rex(false, 0, reg), static_cast<std::uint8_t>(0xB8 | (reg & 7))};
    Append32(bytes, static_cast<std::int32_t>(value));
    return bytes;
}

std::vector<std::uint8_t> _registerPair(const std::uint8_t opcode, const std::uint8_t reg, const std::uint8_t rm) {
    return RegisterBytes(opcode, false, reg, rm);
}

std::vector<std::uint8_t> _twoByte(const std::uint8_t opcode, const std::uint8_t reg, const std::uint8_t rm) {
    return {Rex(false, reg, rm), 0x0F, opcode, static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))};
}

std::vector<std::uint8_t> _indexed(const std::vector<std::uint8_t>& opcode, const std::uint8_t reg, const std::uint8_t index, const std::uint8_t scale, const std::int32_t displacement) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(0x40 | ((reg & 8) >> 1) | ((index & 8) >> 2))};
    _append(bytes, opcode);
    bytes.push_back(static_cast<std::uint8_t>(0x84 | ((reg & 7) << 3)));
    bytes.push_back(static_cast<std::uint8_t>((scale << 6) | ((index & 7) << 3) | 4));
    Append32(bytes, displacement);
    return bytes;
}

std::vector<std::uint8_t> _loop(const std::uint8_t magnitude, const std::uint8_t sign, const std::uint8_t scratch, const std::uint8_t index, const int elements) {
    std::vector<std::uint8_t> start;
    _append(start, _indexed({0x0F, 0xB7}, magnitude, index, 1, kSourceSlot));
    _append(start, _registerPair(0x8B, sign, magnitude));
    _append(start, _immediate(4, sign, kSignBit));
    _append(start, _shiftImmediate(4, sign, kSignShift));
    _append(start, _registerPair(0x8B, scratch, magnitude));
    _append(start, _immediate(4, scratch, kExponentMask));
    _append(start, _immediate(4, magnitude, kMagnitudeMask));
    _append(start, _immediate(7, scratch, kExponentMask));

    std::vector<std::uint8_t> normal;
    _append(normal, _shiftImmediate(4, magnitude, kMantissaShift));
    _append(normal, _immediate(0, magnitude, kNormalBias));

    std::vector<std::uint8_t> quiet;
    _append(quiet, _immediate(1, magnitude, kQuietBit));

    std::vector<std::uint8_t> infinite;
    _append(infinite, _registerPair(0x8B, scratch, magnitude));
    _append(infinite, _immediate(4, scratch, kHalfPayloadMask));
    _append(infinite, _shiftImmediate(4, magnitude, kMantissaShift));
    _append(infinite, _immediate(0, magnitude, kInfiniteBias));
    _append(infinite, _registerPair(0x85, scratch, scratch));
    infinite.push_back(kJumpZero);
    infinite.push_back(static_cast<std::uint8_t>(quiet.size()));
    _append(infinite, quiet);

    std::vector<std::uint8_t> scale;
    _append(scale, _twoByte(0xBD, scratch, magnitude));
    _append(scale, _loadImmediate(kShiftRegister, kFloatMantissaBits));
    _append(scale, _registerPair(0x29, scratch, kShiftRegister));
    _append(scale, std::vector<std::uint8_t>{Rex(false, 0, magnitude), 0xD3, static_cast<std::uint8_t>(0xE0 | (magnitude & 7))});
    _append(scale, _immediate(4, magnitude, kMantissaMask));
    _append(scale, _immediate(0, scratch, kDenormalExponent));
    _append(scale, _shiftImmediate(4, scratch, kFloatMantissaBits));
    _append(scale, _registerPair(0x09, scratch, magnitude));

    std::vector<std::uint8_t> denormal;
    _append(denormal, _registerPair(0x85, magnitude, magnitude));
    denormal.push_back(kJumpZero);
    denormal.push_back(static_cast<std::uint8_t>(scale.size()));
    _append(denormal, scale);

    std::vector<std::uint8_t> finish;
    _append(finish, _registerPair(0x09, sign, magnitude));
    _append(finish, _indexed({0x89}, magnitude, index, 2, kResultSlot));
    _append(finish, std::vector<std::uint8_t>{Rex(false, 0, index), 0x83, static_cast<std::uint8_t>(0xC0 | (index & 7)), 1});
    _append(finish, std::vector<std::uint8_t>{Rex(false, 0, index), 0x83, static_cast<std::uint8_t>(0xF8 | (index & 7)), static_cast<std::uint8_t>(elements)});

    const auto test = _registerPair(0x85, scratch, scratch);
    std::vector<std::uint8_t> body;
    _append(body, start);
    body.push_back(kJumpEqual);
    body.push_back(static_cast<std::uint8_t>(test.size() + 2 + normal.size() + 2));
    _append(body, test);
    body.push_back(kJumpZero);
    body.push_back(static_cast<std::uint8_t>(normal.size() + 2 + infinite.size() + 2));
    _append(body, normal);
    body.push_back(kJump);
    body.push_back(static_cast<std::uint8_t>(infinite.size() + 2 + denormal.size()));
    _append(body, infinite);
    body.push_back(kJump);
    body.push_back(static_cast<std::uint8_t>(denormal.size()));
    _append(body, denormal);
    _append(body, finish);
    body.push_back(0x0F);
    body.push_back(kJumpBelowLong);
    Append32(body, -static_cast<std::int32_t>(body.size() + 4));
    return body;
}

}

std::optional<Float16Operands> DecodeFloat16(const std::uint8_t* data, const std::size_t length) {
    const auto instruction = Decode(data, length);
    if (!instruction || instruction->Map != Map0F38 || instruction->Prefix != Prefix66 || instruction->Opcode != 0x13)
        return std::nullopt;
    if (instruction->Wide) throw CodegenException(std::string(kName) + " requires VEX.W of zero");
    if (instruction->Vvvv != 0) throw CodegenException(std::string(kName) + " requires VEX.vvvv of zero");
    if (instruction->End != length) throw CodegenException(std::string(kName) + " has trailing bytes");
    return Float16Operands{instruction->Reg, instruction->Rm, instruction->Vector256, instruction->Memory};
}

void EmitFloat16(StubBodyBuilder& body, const Float16Operands& operands) {
    const auto elements = operands.Vector256 ? 8 : 4;
    auto used = UsedRegisters({kShiftRegister}, operands.Memory);
    const auto magnitude = Scratch(used, kName);
    const auto sign = Scratch(used, kName);
    const auto scratch = Scratch(used, kName);
    const auto index = Scratch(used, kName);
    std::uint8_t vector = 0;
    if (operands.Memory) {
        std::array<bool, 16> taken{};
        taken[operands.Destination] = true;
        for (std::uint8_t reg = 15;; --reg)
            if (!taken[reg]) { vector = reg; break; }
    }
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - kFrame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, kShiftRegister, kShiftSlot);
    StackSlot(body, 0x89, true, magnitude, kMagnitudeSlot);
    StackSlot(body, 0x89, true, sign, kSignSlot);
    StackSlot(body, 0x89, true, scratch, kScratchSlot);
    StackSlot(body, 0x89, true, index, kIndexSlot);
    if (operands.Memory) {
        VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kVectorSlot);
        VexEncoding::Memory(body, operands.Vector256 ? MoveUnaligned128Load : kMoveQuadLoad, vector, 0, *operands.Memory, kFrame, kName);
        VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kSourceSlot);
    } else {
        VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.Source, 0, kSourceSlot);
    }
    Register(body, 0x31, false, index, index);
    body.Raw(_loop(magnitude, sign, scratch, index, elements));
    VexEncoding::StackOperand(body, operands.Vector256 ? MoveUnaligned256Load : MoveUnaligned128Load, operands.Destination, 0, kResultSlot);
    if (operands.Memory)
        VexEncoding::StackOperand(body, MoveUnaligned256Load, vector, 0, kVectorSlot);
    StackSlot(body, 0x8B, true, index, kIndexSlot);
    StackSlot(body, 0x8B, true, scratch, kScratchSlot);
    StackSlot(body, 0x8B, true, sign, kSignSlot);
    StackSlot(body, 0x8B, true, magnitude, kMagnitudeSlot);
    StackSlot(body, 0x8B, true, kShiftRegister, kShiftSlot);
    body.Raw(std::array{kPopFlags});
    StackSlot(body, 0x8D, true, Stack, kFrame - kFlagsWord);
}

}
