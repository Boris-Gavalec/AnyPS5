# relinker

## add Functional

- [VCVTPH2PS lowering](../../../core/relinker/codegen/src/x86/Float16Lowering.cpp) under `--to-avx` converts one half at a time through general-purpose registers, about twenty instructions per element, so a 256-bit site costs far more than the hardware instruction; the other half of F16C, VCVTPS2PH, is still not lowered, and a `VEX.W` of one, a non-zero `VEX.vvvv` or trailing bytes throw. Zero, denormal, normal and infinite inputs follow IEEE 754 binary16, a denormal is normalised through `BSR`, and a signalling NaN gains the quiet bit, which is what an Intel Xeon with F16C was measured to do for all 65536 half values; the SDM does not state it, and no other CPU was measured. The source is staged in a 288-byte frame below the red zone with five general-purpose registers and RFLAGS, which the instruction leaves unchanged, and a stack-relative memory source inside that frame is not detected, as for the other 256-bit lowerings.
