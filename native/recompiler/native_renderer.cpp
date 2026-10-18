#include "native_renderer.h"
#include "ppc_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace {

using Color = std::array<int, 4>;

struct Vertex {
    double x, y, z, s, t, clipZ;
    std::array<double, 4> color;
};

uint32_t Word(const std::vector<uint8_t>& bytes, size_t offset) {
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
           (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

double Float(uint32_t word) {
    float value;
    std::memcpy(&value, &word, sizeof(value));
    if (!std::isfinite(value)) throw std::runtime_error("Native renderer rejects non-finite vertex/transform data.");
    return value;
}

Color Unpack(uint32_t word) {
    return {int(word >> 24), int((word >> 16) & 255), int((word >> 8) & 255), int(word & 255)};
}

uint32_t Pack(const Color& color) {
    return (uint32_t(color[0]) << 24) | (uint32_t(color[1]) << 16) |
           (uint32_t(color[2]) << 8) | 255u;
}

bool Compare(unsigned function, int a, int b) {
    switch (function) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    case 7: return true;
    }
    throw std::runtime_error("Invalid native renderer comparison.");
}

bool Alpha(uint32_t control, int alpha) {
    const bool a = Compare((control >> 16) & 7, alpha, control & 255);
    const bool b = Compare((control >> 19) & 7, alpha, (control >> 8) & 255);
    switch (control >> 22) {
    case 0: return a && b;
    case 1: return a || b;
    case 2: return a != b;
    case 3: return a == b;
    }
    throw std::runtime_error("Invalid native renderer alpha control.");
}

int Wrap(int value, int extent, unsigned mode) {
    if (mode == 0) return std::clamp(value, 0, extent - 1);
    const int period = mode == 2 ? extent * 2 : extent;
    const int wrapped = (value % period + period) % period;
    return wrapped >= extent ? period - wrapped - 1 : wrapped;
}

struct Texture {
    const PpcState& state;
    size_t base;
    int width, height;
    unsigned wrapS, wrapT;
    bool linear;

    Color Texel(int x, int y) const {
        x = Wrap(x, width, wrapS);
        y = Wrap(y, height, wrapT);
        const size_t index = ((size_t(y / 4) * ((width + 3) / 4) + x / 4) * 16 +
                              (y % 4) * 4 + x % 4) * 2;
        const auto word = (uint32_t(state.memory[base + index]) << 8) | state.memory[base + index + 1];
        if (word & 0x8000) {
            const auto expand = [](unsigned v) { return int((v << 3) | (v >> 2)); };
            return {expand((word >> 10) & 31), expand((word >> 5) & 31), expand(word & 31), 255};
        }
        const unsigned a = (word >> 12) & 7;
        return {int((word >> 8) & 15) * 17, int((word >> 4) & 15) * 17,
                int(word & 15) * 17, int((a << 5) | (a << 2) | (a >> 1))};
    }

    Color Sample(double s, double t) const {
        // Keep conversion to integer bounded even for large finite input coordinates.
        const auto coordinate = [](double v, int extent, unsigned mode) {
            if (mode == 0) return std::clamp(v, 0.0, 1.0) * extent;
            return std::fmod(v, mode == 2 ? 2.0 : 1.0) * extent;
        };
        const double u = coordinate(s, width, wrapS), v = coordinate(t, height, wrapT);
        if (!linear) return Texel(int(std::floor(u)), int(std::floor(v)));
        const int x = int(std::floor(u - 0.5)), y = int(std::floor(v - 0.5));
        const double fx = u - 0.5 - x, fy = v - 0.5 - y;
        const auto a = Texel(x, y), b = Texel(x + 1, y), c = Texel(x, y + 1), d = Texel(x + 1, y + 1);
        Color result{};
        for (unsigned channel = 0; channel < 4; ++channel) {
            result[channel] = int(std::lround((a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
                                              (c[channel] * (1 - fx) + d[channel] * fx) * fy));
        }
        return result;
    }
};

double Edge(const Vertex& a, const Vertex& b, double x, double y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

bool TopLeft(const Vertex& a, const Vertex& b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}

std::vector<Vertex> ClipDepth(const std::vector<Vertex>& input, double plane, bool minimum) {
    std::vector<Vertex> output;
    auto previous = input.back();
    bool previousInside = minimum ? previous.clipZ >= plane : previous.clipZ <= plane;
    for (const auto& current : input) {
        const bool inside = minimum ? current.clipZ >= plane : current.clipZ <= plane;
        if (inside != previousInside) {
            const double t = (plane - previous.clipZ) / (current.clipZ - previous.clipZ);
            const auto lerp = [t](double a, double b) { return a + (b - a) * t; };
            Vertex intersection{};
            intersection.x = lerp(previous.x, current.x);
            intersection.y = lerp(previous.y, current.y);
            intersection.z = lerp(previous.z, current.z);
            intersection.s = lerp(previous.s, current.s);
            intersection.t = lerp(previous.t, current.t);
            intersection.clipZ = plane;
            for (unsigned ch = 0; ch < 4; ++ch)
                intersection.color[ch] = lerp(previous.color[ch], current.color[ch]);
            output.push_back(intersection);
        }
        if (inside) output.push_back(current);
        previous = current;
        previousInside = inside;
    }
    return output;
}

void Triangle(NativeEfb& efb, const NativeGx& gx, const Texture* texture,
              Vertex a, Vertex b, Vertex c, uint64_t& written) {
    double area = Edge(a, b, c.x, c.y);
    const auto cull = (gx.bp[0] >> 14) & 3;
    if (area == 0 || cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) return;
    if (area < 0) { std::swap(b, c); area = -area; }
    const int ox = int(gx.bp[0x59] & 0x3ff) * 2, oy = int((gx.bp[0x59] >> 10) & 0x3ff) * 2;
    const int left = std::max({0, int((gx.bp[0x20] >> 12) & 0xfff) - ox,
                               int(std::ceil(std::min({a.x, b.x, c.x}) - 0.5))});
    const int top = std::max({0, int(gx.bp[0x20] & 0xfff) - oy,
                              int(std::ceil(std::min({a.y, b.y, c.y}) - 0.5))});
    const int right = std::min({int(NativeEfb::width) - 1, int((gx.bp[0x21] >> 12) & 0xfff) - ox,
                                int(std::floor(std::max({a.x, b.x, c.x}) - 0.5))});
    const int bottom = std::min({int(NativeEfb::height) - 1, int(gx.bp[0x21] & 0xfff) - oy,
                                 int(std::floor(std::max({a.y, b.y, c.y}) - 0.5))});
    const auto depth = gx.bp[0x40], blend = gx.bp[0x41];
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double e0 = Edge(b, c, x + 0.5, y + 0.5);
            const double e1 = Edge(c, a, x + 0.5, y + 0.5);
            const double e2 = Edge(a, b, x + 0.5, y + 0.5);
            if (e0 < 0 || (e0 == 0 && !TopLeft(b, c)) ||
                e1 < 0 || (e1 == 0 && !TopLeft(c, a)) ||
                e2 < 0 || (e2 == 0 && !TopLeft(a, b))) continue;
            const double w0 = e0 / area, w1 = e1 / area, w2 = e2 / area;
            const int z = int(std::clamp(a.z * w0 + b.z * w1 + c.z * w2, 0.0, 16777215.0));
            const auto index = size_t(y) * NativeEfb::width + x;
            const bool depthDefined = (efb.defined[index] & 2) != 0;
            if ((depth & 1) && !Compare((depth >> 1) & 7, z, int(efb.depth[index]))) continue;
            Color source{};
            for (unsigned ch = 0; ch < 4; ++ch)
                source[ch] = std::clamp(int(std::lround(a.color[ch] * w0 + b.color[ch] * w1 + c.color[ch] * w2)), 0, 255);
            if (texture) {
                const auto texel = texture->Sample(a.s * w0 + b.s * w1 + c.s * w2,
                                                   a.t * w0 + b.t * w1 + c.t * w2);
                for (unsigned ch = 0; ch < 4; ++ch) source[ch] = (source[ch] * texel[ch] + 127) / 255;
            }
            const bool pass = Alpha(gx.bp[0xf3], source[3]);
            // Z compare location selects whether alpha rejection can suppress the depth write.
            if ((depth & 17) == 17 && ((gx.bp[0x43] & 0x40) || pass)) {
                efb.depth[index] = uint32_t(z);
                const auto comparison = (depth >> 1) & 7;
                efb.defined[index] = uint8_t((efb.defined[index] & ~2u) |
                    ((depthDefined || comparison == 7) ? 2u : 0u));
            }
            if (!pass || !(blend & 8)) continue;
            bool defined = true;
            if (blend & 1) {
                const int alpha = source[3];
                const auto destination = Unpack(efb.color[index]);
                for (unsigned ch = 0; ch < 3; ++ch)
                    source[ch] = (source[ch] * alpha + destination[ch] * (255 - alpha) + 127) / 255;
                defined = alpha == 255 || (efb.defined[index] & 1) != 0;
            }
            if ((depth & 1) && ((depth >> 1) & 7) != 0 && ((depth >> 1) & 7) != 7)
                defined = defined && depthDefined;
            efb.color[index] = Pack(source);
            efb.defined[index] = uint8_t((efb.defined[index] & ~1u) | (defined ? 1u : 0u));
            ++written;
        }
    }
}

} // namespace

size_t NativeQuadVertexSize(const PpcState& state, uint8_t opcode) {
    const auto& gx = state.graphics.value();
    if (opcode != 0x80 || gx.cp[0x60] > 1 ||
        (gx.cp[0x50] != 0x2200 && gx.cp[0x50] != 0x200))
        throw std::runtime_error("Native renderer supports only format-0 direct XY/RGBA/ST or XYZ quads.");
    const bool textured = gx.cp[0x50] == 0x2200 && gx.cp[0x60] == 1;
    const auto vat = gx.cp[0x70];
    if ((textured && (vat & 0x3fe1e1ffu) != 0x01216006u) ||
        (!textured && (gx.cp[0x60] != 0 || (vat & 0x1ffu) != 7)))
        throw std::runtime_error("Unsupported native renderer vertex attribute format.");
    return textured ? 16 : 6;
}

void NativeRenderQuads(PpcState& state, size_t count) {
    auto& gx = state.graphics.value();
    const auto stride = NativeQuadVertexSize(state, gx.commands[0]);
    const bool textured = stride == 16;
    if (count == 0 || count % 4 != 0 || gx.commands.size() < 3 + count * stride)
        throw std::runtime_error("Native renderer requires complete groups of four quad vertices.");
    if (!gx.efb) throw std::runtime_error("Native renderer requires EFB storage.");
    const auto& bp = gx.bp;
    const auto& xf = gx.xf;
    // This bounded pipeline is the original UI's unlit, single-stage orthographic GX profile.
    if ((bp[0] & ~0xc000u) != (textured ? 0x11u : 0x10u) ||
        bp[0x43] & ~0x40u || bp[0x42] != 0 || (bp[0x41] & ~0x1fu) != 0x4a0u ||
        (bp[0x41] & 2) || bp[0x40] & ~31u ||
        xf[0x1026] != 1 || xf[0x1005] != 0 || xf[0x1009] != 1 || xf[0x1008] != (textured ? 0x11u : 0u) ||
        xf[0x1018] != gx.cp[0x30] || xf[0x1019] != gx.cp[0x40] ||
        xf[0x100e] != (textured ? 0x401u : 0x400u) || xf[0x1010] != (textured ? 0x401u : 0x400u) ||
        // Fog type zero disables its coefficients, projection selector and color.
        (bp[0xf1] & 0xe00000u) != 0 ||
        (bp[0xe8] & 0x400) || bp[0x25] != 0 || bp[0x27] != 0 || bp[0x10] != 0 ||
        bp[0xf4] != 0 || bp[0xf5] != 0) {
        std::ostringstream message;
        message << "Unsupported native renderer lighting/projection/PE/fog/indirect state."
                << std::hex << " textured=" << textured;
        for (const unsigned reg : {0u, 0x40u, 0x41u, 0x42u, 0x43u, 0xf0u, 0xf1u, 0xf2u,
                                   0xeeu, 0xefu, 0xe8u, 0x25u, 0x27u, 0x10u, 0xf4u, 0xf5u})
            message << " BP[" << reg << "]=" << bp[reg];
        for (const unsigned reg : {0x1026u, 0x1005u, 0x1009u, 0x1008u, 0x1018u, 0x1019u,
                                   0x100eu, 0x1010u})
            message << " XF[" << reg << "]=" << xf[reg];
        message << " CP[30]=" << gx.cp[0x30] << " CP[40]=" << gx.cp[0x40];
        throw std::runtime_error(message.str());
    }
    if (textured && (bp[0xc0] != 0x8f8af || bp[0xc1] != 0x8f2f0 ||
                     (bp[0x28] & 0x3ffu) != 0x40 || xf[0x103f] != 1 ||
                     xf[0x1040] != 0x280 || xf[0x1050] != 0x3d))
        throw std::runtime_error("Unsupported native renderer texture generator/TEV modulation state.");
    if (!textured && (bp[0xc0] != 0x8fffa || bp[0xc1] != 0x8ffd0 ||
                      (bp[0x28] & 0x40) != 0 || xf[0x103f] != 0))
        throw std::runtime_error("Unsupported native renderer untextured TEV state: " +
                                 std::to_string(bp[0xc0]) + "/" + std::to_string(bp[0xc1]));
    const size_t pixels = size_t(NativeEfb::width) * NativeEfb::height;
    if (gx.efb->color.size() != pixels || gx.efb->depth.size() != pixels || gx.efb->defined.size() != pixels)
        throw std::runtime_error("Native renderer EFB storage dimensions are invalid.");
    std::optional<Texture> texture;
    if (textured) {
        const auto image = bp[0x88], mode = bp[0x80];
        if ((image >> 20) != 5 || (bp[0x94] >> 21) != 0 ||
            (mode & ~0x1ff9fu) != 0 || (((mode >> 5) & 7) != 0 && ((mode >> 5) & 7) != 4) ||
            bool(mode & 16) != (((mode >> 5) & 7) == 4) ||
            // LOD limits/bias cannot change samples when both filters match and use only the base level.
            (mode & 3) == 3 || ((mode >> 2) & 3) == 3 || (bp[0x84] & ~0xffffu) != 0 ||
            (bp[0x30] & 0xffffu) != (image & 0x3ffu) ||
            (bp[0x31] & 0xffffu) != ((image >> 10) & 0x3ffu) ||
            (bp[0x30] >> 16) != 0 || (bp[0x31] >> 16) != 0) {
            std::ostringstream message;
            message << "Native renderer supports only unscaled, non-mipmapped RGB5A3 textures."
                    << std::hex << " image=0x" << image << " address=0x" << bp[0x94]
                    << " mode=0x" << mode << " lod=0x" << bp[0x84]
                    << " scaleS=0x" << bp[0x30] << " scaleT=0x" << bp[0x31];
            throw std::runtime_error(message.str());
        }
        const int width = (image & 0x3ff) + 1, height = ((image >> 10) & 0x3ff) + 1;
        const auto base = ppc_memory_offset(state, bp[0x94] << 5,
                                            size_t((width + 3) / 4) * ((height + 3) / 4) * 32);
        texture.emplace(Texture{state, base, width, height, mode & 3, (mode >> 2) & 3, (mode & 16) != 0});
    }
    std::vector<Vertex> vertices;
    vertices.reserve(count);
    const unsigned matrix = (gx.cp[0x30] & 63u) * 4;
    const unsigned texMatrix = ((gx.cp[0x30] >> 6) & 63u) * 4;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = 3 + i * stride;
        const auto shortAt = [&](size_t n) {
            const unsigned word = (unsigned(gx.commands[offset + n]) << 8) | gx.commands[offset + n + 1];
            return double(word < 0x8000 ? int(word) : int(word) - 65536);
        };
        const double x = shortAt(0), y = shortAt(2), z = textured ? 0 : shortAt(4);
        std::array<double, 3> position{};
        for (unsigned row = 0; row < 3; ++row) {
            const unsigned m = matrix + row * 4;
            position[row] = Float(xf[m]) * x + Float(xf[m + 1]) * y +
                            Float(xf[m + 2]) * z + Float(xf[m + 3]);
        }
        Vertex vertex{};
        vertex.x = (position[0] * Float(xf[0x1020]) + Float(xf[0x1021])) *
                    Float(xf[0x101a]) + Float(xf[0x101d]) - 342;
        vertex.y = (position[1] * Float(xf[0x1022]) + Float(xf[0x1023])) *
                    Float(xf[0x101b]) + Float(xf[0x101e]) - 342;
        vertex.clipZ = position[2] * Float(xf[0x1024]) + Float(xf[0x1025]);
        vertex.z = vertex.clipZ * Float(xf[0x101c]) + Float(xf[0x101f]);
        const auto color = Unpack(textured ? Word(gx.commands, offset + 4) : xf[0x100c]);
        std::copy(color.begin(), color.end(), vertex.color.begin());
        if (textured) {
            const double s = Float(Word(gx.commands, offset + 8)), t = Float(Word(gx.commands, offset + 12));
            if (texMatrix == 240) { vertex.s = s; vertex.t = t; }
            else {
                vertex.s = Float(xf[texMatrix]) * s + Float(xf[texMatrix + 1]) * t + Float(xf[texMatrix + 3]);
                vertex.t = Float(xf[texMatrix + 4]) * s + Float(xf[texMatrix + 5]) * t + Float(xf[texMatrix + 7]);
            }
        }
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
            !std::isfinite(vertex.s) || !std::isfinite(vertex.t) || !std::isfinite(vertex.clipZ) ||
            std::abs(vertex.x) > 1000000 || std::abs(vertex.y) > 1000000) {
            std::ostringstream message;
            message << "Native renderer vertex exceeds supported orthographic clip range."
                    << " index=" << i << " x=" << vertex.x << " y=" << vertex.y
                    << " z=" << vertex.z << " clipZ=" << vertex.clipZ
                    << " s=" << vertex.s << " t=" << vertex.t;
            throw std::runtime_error(message.str());
        }
        vertices.push_back(vertex);
    }
    uint64_t written = 0;
    for (size_t i = 0; i < count; i += 4) {
        for (unsigned corner = 1; corner < 3; ++corner) {
            std::vector<Vertex> polygon{vertices[i], vertices[i + corner], vertices[i + corner + 1]};
            polygon = ClipDepth(polygon, -1, true);
            if (polygon.empty()) continue;
            polygon = ClipDepth(polygon, 0, false);
            for (size_t j = 1; j + 1 < polygon.size(); ++j)
                Triangle(*gx.efb, gx, texture ? &*texture : nullptr,
                         polygon[0], polygon[j], polygon[j + 1], written);
        }
    }
    gx.draws += count / 4;
    gx.rasterizedPixels += written;
}
