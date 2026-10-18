#ifndef HMAWL_NATIVE_RENDERER_H
#define HMAWL_NATIVE_RENDERER_H

#include <cstddef>
#include <cstdint>

struct PpcState;

size_t NativeQuadVertexSize(const PpcState& state, uint8_t opcode);
void NativeRenderQuads(PpcState& state, size_t count);

#endif
