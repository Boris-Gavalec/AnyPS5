# relinker

## add Functional

- [VCVTPS2PH lowering](../../../core/relinker/codegen/src/x86/Float16StoreLowering.cpp) under `--to-avx` does not update MXCSR: the hardware raises the precision, underflow, overflow and invalid flags, and the stub leaves them as they were, so code that reads MXCSR after a conversion sees stale flags. It converts one element at a time through general-purpose registers, about forty instructions each, and takes the rounding mode from the immediate or, when its bit 2 is set, from MXCSR.RC read at run time; its reserved bits 7 to 3 are ignored, as an Intel Xeon with F16C was measured to ignore them. Rounding, overflow clamping, denormals and the quiet bit a NaN gains follow what that CPU produced for 453,236 inputs in all four rounding modes; no other CPU was measured. Six registers and RFLAGS, which the instruction leaves unchanged, are saved in a 288-byte frame below the red zone, and a stack-relative destination inside it is not detected.
