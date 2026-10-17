#ifndef HMAWL_PPC_FLOAT_H
#define HMAWL_PPC_FLOAT_H

#include <cstdint>

struct PpcState;

enum class PpcFloatOperation { Add, Subtract, Multiply, Divide };

void PpcFloatArithmetic(PpcState& state, unsigned destination, unsigned left,
                        unsigned right, PpcFloatOperation operation, bool single);
void PpcFloatCompare(PpcState& state, unsigned field, unsigned left, unsigned right, bool ordered);
void PpcFloatToInteger(PpcState& state, unsigned destination, unsigned source);

#endif
