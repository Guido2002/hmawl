#ifndef HMAWL_PPC_FLOAT_H
#define HMAWL_PPC_FLOAT_H

#include <cstdint>

struct PpcState;

enum class PpcFloatOperation { Add, Subtract, Multiply, Divide };

void PpcFloatArithmetic(PpcState& state, unsigned destination, unsigned left,
                        unsigned right, PpcFloatOperation operation, bool single);
void PpcPairedArithmetic(PpcState& state, unsigned destination, unsigned left,
                         unsigned right, PpcFloatOperation operation, int broadcast = -1);
void PpcFloatFused(PpcState& state, unsigned destination, unsigned left, unsigned multiplier,
                   unsigned addend, bool subtract, bool single, bool paired, bool negate = false);
void PpcFloatReciprocalSqrtEstimate(PpcState& state, unsigned destination, unsigned source);
void PpcFloatReciprocalEstimate(PpcState& state, unsigned destination, unsigned source);
void PpcFloatRoundSingle(PpcState& state, unsigned destination, unsigned source);
void PpcPairedSum(PpcState& state, unsigned destination, unsigned left, unsigned right,
                  unsigned copied, bool sumInSecond);
void PpcFloatCompare(PpcState& state, unsigned field, unsigned left, unsigned right,
                     bool ordered, bool secondLane = false);
void PpcFloatToInteger(PpcState& state, unsigned destination, unsigned source);

#endif
