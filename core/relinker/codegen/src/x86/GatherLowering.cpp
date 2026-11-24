#include <codegen/x86/GatherLowering.hpp>
#include <codegen/x86/VexEncoding.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace Codegen {

using namespace ScalarLowering;
using namespace VexEncoding;

namespace {

constexpr std::int32_t kIndexSlot = 8;
constexpr std::int32_t kMaskSlot = 16;
constexpr std::int32_t kElementSlot = 24;
constexpr std::int32_t kCounterSlot = 32;
constexpr std::int32_t kVectorSlot = 64;
constexpr std::int32_t kMaskWords = 96;
constexpr std::int32_t kIndexWords = 128;
constexpr std::int32_t kResultWords = 160;
constexpr std::int32_t kFrame = 128 + 192;
constexpr std::int32_t kFlagsWord = 8;
constexpr std::uint8_t kPushFlags = 0x9C;
constexpr std::uint8_t kPopFlags = 0x9D;
constexpr std::uint8_t kJumpNotSigned = 0x79;
constexpr std::uint8_t kJumpBelowLong = 0x82;
constexpr Form kZeroVector{Map0F, 0, false, true, 0x57};
constexpr Form kMoveQuadStore{Map0F, Prefix66, false, false, 0xD6};

const char* _name(const std::uint8_t opcode, const bool wide) {
    switch (opcode) {
    case 0x90: return wide ? "VPGATHERDQ" : "VPGATHERDD";
    case 0x91: return wide ? "VPGATHERQQ" : "VPGATHERQD";
    case 0x92: return wide ? "VGATHERDPD" : "VGATHERDPS";
    case 0x93: return wide ? "VGATHERQPD" : "VGATHERQPS";
    default: return nullptr;
    }
}

void _append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
}

std::vector<std::uint8_t> _stackIndexed(const std::vector<std::uint8_t>& opcode, const bool wide, const std::uint8_t reg, const std::uint8_t index, const std::uint8_t scale, const std::int32_t displacement) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(0x40 | (wide ? 8 : 0) | ((reg & 8) >> 1) | ((index & 8) >> 2))};
    _append(bytes, opcode);
    bytes.push_back(static_cast<std::uint8_t>(0x84 | ((reg & 7) << 3)));
    bytes.push_back(static_cast<std::uint8_t>((scale << 6) | ((index & 7) << 3) | 4));
    Append32(bytes, displacement);
    return bytes;
}

std::vector<std::uint8_t> _gathered(const GatherOperands& operands, const std::uint8_t element, const std::uint8_t index, const std::int32_t displacement) {
    const auto wide = operands.ElementSize == 8;
    std::vector<std::uint8_t> bytes = operands.Prefixes;
    const auto base = operands.HasBase ? operands.Base : std::uint8_t{5};
    bytes.push_back(static_cast<std::uint8_t>(0x40 | (wide ? 8 : 0) | ((element & 8) >> 1) | ((index & 8) >> 2) | ((base & 8) >> 3)));
    bytes.push_back(0x8B);
    const std::uint8_t mod = operands.HasBase ? 2 : 0;
    bytes.push_back(static_cast<std::uint8_t>((mod << 6) | ((element & 7) << 3) | 4));
    bytes.push_back(static_cast<std::uint8_t>((operands.Scale << 6) | ((index & 7) << 3) | (base & 7)));
    Append32(bytes, displacement);
    return bytes;
}

std::vector<std::uint8_t> _loop(const GatherOperands& operands, const std::uint8_t index, const std::uint8_t mask, const std::uint8_t element, const std::uint8_t counter, const std::int32_t displacement) {
    const auto maskWide = operands.ElementSize == 8;
    const auto indexWide = operands.IndexSize == 8;
    std::vector<std::uint8_t> work;
    if (indexWide)
        _append(work, _stackIndexed({0x8B}, true, index, counter, 3, kIndexWords));
    else
        _append(work, _stackIndexed({0x63}, true, index, counter, 2, kIndexWords));
    _append(work, _gathered(operands, element, index, displacement));
    _append(work, _stackIndexed({0x89}, maskWide, element, counter, operands.ElementSize == 8 ? 3 : 2, kResultWords));

    std::vector<std::uint8_t> body;
    _append(body, _stackIndexed({0x8B}, maskWide, mask, counter, operands.ElementSize == 8 ? 3 : 2, kMaskWords));
    _append(body, RegisterBytes(0x85, maskWide, mask, mask));
    body.push_back(kJumpNotSigned);
    body.push_back(static_cast<std::uint8_t>(work.size()));
    _append(body, work);
    _append(body, std::vector<std::uint8_t>{Rex(false, 0, counter), 0x83, static_cast<std::uint8_t>(0xC0 | (counter & 7)), 1});
    _append(body, std::vector<std::uint8_t>{Rex(false, 0, counter), 0x83, static_cast<std::uint8_t>(0xF8 | (counter & 7)), static_cast<std::uint8_t>(operands.Elements)});
    body.push_back(0x0F);
    body.push_back(kJumpBelowLong);
    Append32(body, -static_cast<std::int32_t>(body.size() + 4));
    return body;
}

}

std::optional<GatherOperands> DecodeGather(const std::uint8_t* data, const std::size_t length) {
    std::size_t vex = 0;
    std::vector<std::uint8_t> prefixes;
    while (vex < length && LegacyPrefix(data[vex])) prefixes.push_back(data[vex++]);
    if (vex + 5 > length || data[vex] != 0xC4 || (data[vex + 1] & 0x1F) != Map0F38 || (data[vex + 2] & 3) != Prefix66)
        return std::nullopt;
    const auto opcode = data[vex + 3];
    const bool wide = (data[vex + 2] & 0x80) != 0;
    const auto name = _name(opcode, wide);
    if (!name) return std::nullopt;
    const auto modrm = data[vex + 4];
    if ((modrm >> 6) == 3) throw CodegenException(std::string(name) + " requires a vector index operand");
    if ((modrm & 7) != 4) throw CodegenException(std::string(name) + " requires a SIB byte");
    if (std::find(prefixes.begin(), prefixes.end(), 0x67) != prefixes.end())
        throw CodegenException(std::string(name) + " with a 32-bit address size is not lowered");
    if (vex + 6 > length) throw CodegenException(std::string(name) + " is missing its SIB byte");
    const auto extensions = static_cast<std::uint8_t>(~data[vex + 1]);
    const auto sib = data[vex + 5];
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    const bool hasBase = mod != 0 || (sib & 7) != 5;
    std::size_t end = vex + 6;
    std::int32_t displacement = 0;
    if (mod == 1) {
        if (end + 1 > length) throw CodegenException(std::string(name) + " is missing its displacement");
        displacement = static_cast<std::int8_t>(data[end]);
        end += 1;
    } else if (mod == 2 || !hasBase) {
        if (end + 4 > length) throw CodegenException(std::string(name) + " is missing its displacement");
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(data[end + shift / 8]) << shift;
        displacement = static_cast<std::int32_t>(value);
        end += 4;
    }
    if (end != length) throw CodegenException(std::string(name) + " has trailing bytes");
    const auto destination = static_cast<std::uint8_t>(((modrm >> 3) & 7) | ((extensions & 0x80) >> 4));
    const auto maskRegister = static_cast<std::uint8_t>((~data[vex + 2] >> 3) & 15);
    const auto indexRegister = static_cast<std::uint8_t>(((sib >> 3) & 7) | ((extensions & 0x40) >> 3));
    if (destination == maskRegister || destination == indexRegister || maskRegister == indexRegister)
        throw CodegenException(std::string(name) + " needs three distinct vector registers");
    const std::uint8_t indexSize = opcode == 0x90 || opcode == 0x92 ? 4 : 8;
    const std::uint8_t elementSize = wide ? 8 : 4;
    const auto width = (data[vex + 2] & 4) != 0 ? 32 : 16;
    const auto elements = width / std::max(indexSize, elementSize);
    return GatherOperands{name, destination, maskRegister, indexRegister,
        static_cast<std::uint8_t>((sib & 7) | ((extensions & 0x20) >> 2)), hasBase,
        static_cast<std::uint8_t>(sib >> 6), displacement, indexSize, elementSize, elements, prefixes};
}

void EmitGather(StubBodyBuilder& body, const GatherOperands& operands) {
    auto displacement = static_cast<std::int64_t>(operands.Displacement);
    if (operands.HasBase && operands.Base == Stack) {
        displacement += kFrame;
        if (displacement > std::numeric_limits<std::int32_t>::max())
            throw CodegenException(std::string(operands.Name) + " stack displacement does not fit after spilling");
    }
    std::array<bool, 16> used{};
    used[Stack] = true;
    if (operands.HasBase) used[operands.Base] = true;
    const auto index = Scratch(used, operands.Name);
    const auto mask = Scratch(used, operands.Name);
    const auto element = Scratch(used, operands.Name);
    const auto counter = Scratch(used, operands.Name);
    std::array<bool, 16> vectors{};
    vectors[operands.Destination] = true;
    vectors[operands.Mask] = true;
    vectors[operands.Index] = true;
    std::uint8_t vector = 0;
    for (std::uint8_t reg = 15;; --reg)
        if (!vectors[reg]) { vector = reg; break; }
    const auto destinationBytes = operands.Elements * operands.ElementSize;
    StackSlot(body, 0x8D, true, Stack, kFlagsWord - kFrame);
    body.Raw(std::array{kPushFlags});
    StackSlot(body, 0x89, true, index, kIndexSlot);
    StackSlot(body, 0x89, true, mask, kMaskSlot);
    StackSlot(body, 0x89, true, element, kElementSlot);
    StackSlot(body, 0x89, true, counter, kCounterSlot);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kVectorSlot);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.Mask, 0, kMaskWords);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, operands.Index, 0, kIndexWords);
    VexEncoding::Register(body, kZeroVector, vector, vector, vector);
    VexEncoding::StackOperand(body, MoveUnaligned256Store, vector, 0, kResultWords);
    if (destinationBytes == 8)
        VexEncoding::StackOperand(body, kMoveQuadStore, operands.Destination, 0, kResultWords);
    else
        VexEncoding::StackOperand(body, destinationBytes == 32 ? MoveUnaligned256Store : Form{Map0F, PrefixF3, false, false, 0x7F}, operands.Destination, 0, kResultWords);
    Register(body, 0x31, false, counter, counter);
    body.Raw(_loop(operands, index, mask, element, counter, static_cast<std::int32_t>(displacement)));
    VexEncoding::StackOperand(body, destinationBytes == 32 ? MoveUnaligned256Load : MoveUnaligned128Load, operands.Destination, 0, kResultWords);
    VexEncoding::Register(body, kZeroVector, operands.Mask, operands.Mask, operands.Mask);
    VexEncoding::StackOperand(body, MoveUnaligned256Load, vector, 0, kVectorSlot);
    StackSlot(body, 0x8B, true, counter, kCounterSlot);
    StackSlot(body, 0x8B, true, element, kElementSlot);
    StackSlot(body, 0x8B, true, mask, kMaskSlot);
    StackSlot(body, 0x8B, true, index, kIndexSlot);
    body.Raw(std::array{kPopFlags});
    StackSlot(body, 0x8D, true, Stack, kFrame - kFlagsWord);
}

}
