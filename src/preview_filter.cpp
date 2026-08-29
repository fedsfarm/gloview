#include "preview_filter.hpp"

#include <algorithm>
#include <utility>

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview::PreviewFilter {

// Uses Hyprland's live surface textures directly; no capture or intermediate texture.
namespace {

// ---- shader -----------------------------------------------------------------

constexpr char VERTEX_SHADER[] = R"(
#version 300 es
precision highp float;
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;
void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)";

// A 2x2 (box4) or 4x4 (box16) grid of bilinear reads covers each destination
// pixel's source footprint.
constexpr char FRAGMENT_SHADER[] = R"(
#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2 footprint;
uniform vec2 uv_min;
uniform vec2 uv_max;
uniform vec2 target_size;
uniform int sample_grid;
uniform float radius;
uniform float rounding_power;
uniform float alpha;
uniform int force_opaque;
layout(location = 0) out vec4 frag_color;

const float PI = 3.1415926535897932384626433832795;
const float SMOOTHING = PI / 5.34665792551;

float rounded_alpha(vec2 pixel, float r) {
    vec2 corner = pixel - target_size * 0.5;
    corner *= vec2(lessThan(corner, vec2(0.0))) * -2.0 + 1.0;
    corner -= target_size * 0.5 - r;
    corner += vec2(1.0) / target_size;

    if (corner.x + corner.y <= r)
        return 1.0;

    float distance = pow(pow(corner.x, rounding_power) + pow(corner.y, rounding_power), 1.0 / rounding_power);
    if (distance > r + SMOOTHING)
        discard;

    return 1.0 - smoothstep(0.0, 1.0, (distance - r + SMOOTHING) / (SMOOTHING * 2.0));
}

void main() {
    vec2 sample_uv = mix(uv_min, uv_max, v_texcoord);
    vec2 pixel = v_texcoord * target_size;

    vec2 half_texel = 0.5 / vec2(textureSize(tex, 0));
    vec2 lo = min(uv_min + half_texel, uv_max);
    vec2 hi = max(uv_max - half_texel, uv_min);
    vec4 sum = vec4(0.0);
    float grid = float(sample_grid);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (x < sample_grid && y < sample_grid) {
                vec2 offset = (vec2(float(x), float(y)) + 0.5) / grid - 0.5;
                sum += texture(tex, clamp(sample_uv + offset * footprint, lo, hi));
            }
        }
    }

    frag_color = sum / (grid * grid);
    if (force_opaque != 0)
        frag_color.a = 1.0;
    if (radius > 0.0)
        frag_color *= rounded_alpha(pixel, radius);
    frag_color *= alpha;
}
)";

struct ShaderState {
    SP<CShader> shader;
    bool        unavailable = false;
    GLint       footprint = -1;
    GLint       uvMin = -1;
    GLint       uvMax = -1;
    GLint       targetSize = -1;
    GLint       sampleGrid = -1;
    GLint       radius = -1;
    GLint       roundingPower = -1;
    GLint       forceOpaque = -1;
};

ShaderState g_shader;

bool ensureShader() {
    if (g_shader.shader)
        return true;
    if (g_shader.unavailable || !g_pHyprOpenGL)
        return false;

    g_pHyprOpenGL->makeEGLCurrent();
    auto shader = makeShared<CShader>();
    if (!shader->createProgram(VERTEX_SHADER, FRAGMENT_SHADER, true, true)) {
        g_shader.unavailable = true;
        return false;
    }

    const auto program = shader->program();
    g_shader.footprint   = glGetUniformLocation(program, "footprint");
    g_shader.uvMin       = glGetUniformLocation(program, "uv_min");
    g_shader.uvMax       = glGetUniformLocation(program, "uv_max");
    g_shader.targetSize  = glGetUniformLocation(program, "target_size");
    g_shader.sampleGrid  = glGetUniformLocation(program, "sample_grid");
    g_shader.radius      = glGetUniformLocation(program, "radius");
    g_shader.roundingPower = glGetUniformLocation(program, "rounding_power");
    g_shader.forceOpaque = glGetUniformLocation(program, "force_opaque");
    if (g_shader.footprint < 0 || g_shader.uvMin < 0 || g_shader.uvMax < 0 || g_shader.targetSize < 0 ||
        g_shader.sampleGrid < 0 || g_shader.radius < 0 || g_shader.roundingPower < 0 || g_shader.forceOpaque < 0) {
        shader.reset();
        g_shader.unavailable = true;
        return false;
    }

    g_shader.shader = std::move(shader);
    return true;
}

struct SurfaceData {
    CSurfacePassElement::SRenderData fallback;
    CBox                            boxPx;
    CBox                            clipPx;
    Vector2D                        uvMin;
    Vector2D                        uvMax{1.0, 1.0};
    double                          radiusPx = 0.0;
    int                             sampleGrid = 2;
};

// ---- render pass ------------------------------------------------------------

CBox intersection(const CBox& a, const CBox& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    return CBox{x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

bool swapsAxes(eTransform transform) {
    return transform == HYPRUTILS_TRANSFORM_90 || transform == HYPRUTILS_TRANSFORM_270 ||
        transform == HYPRUTILS_TRANSFORM_FLIPPED_90 || transform == HYPRUTILS_TRANSFORM_FLIPPED_270;
}

bool needsColorConversion(const SurfaceData& data) {
    static auto enableCM = CConfigValue<Config::INTEGER>("render:cm_enabled");
    if (!*enableCM || data.fallback.pMonitor->doesNoShaderCM())
        return false;

    auto source = data.fallback.texture->m_imageDescription;
    if (!source && data.fallback.surface->m_colorManagement.valid())
        source = NColorManagement::CImageDescription::from(data.fallback.surface->m_colorManagement->imageDescription());
    if (!source)
        source = NColorManagement::getDefaultImageDescription();

    const auto& currentFB = g_pHyprRenderer->m_renderData.currentFB;
    auto        target = data.fallback.pMonitor->workBufferImageDescription();
    if (currentFB) {
        const auto description = currentFB->imageDescription();
        if (description)
            target = description;
    }

    return target && source->needsCM(target);
}

bool drawSurface(const SurfaceData& data) {
    const auto& fallback = data.fallback;
    const auto& texture  = fallback.texture;
    if (!texture || !texture->ok() || texture->m_type == Render::TEXTURE_EXTERNAL || !fallback.surface ||
        !fallback.pMonitor || data.boxPx.w <= 1.0 || data.boxPx.h <= 1.0 || needsColorConversion(data) || !ensureShader())
        return false;

    const auto surface = Desktop::View::CWLSurface::fromResource(fallback.surface);
    const float alpha = std::clamp(fallback.alpha * fallback.fadeAlpha *
                                       (surface ? surface->m_alphaModifier * surface->m_overallOpacity : 1.0F),
                                   0.0F, 1.0F);

    auto transform = texture->m_transform;
    if (g_pHyprRenderer->monitorTransformEnabled()) {
        const auto monitorTransform = Math::wlTransformToHyprutils(Math::invertTransform(fallback.pMonitor->m_transform));
        transform = Math::composeTransform(monitorTransform, transform);
    }
    // The filter footprint is expressed in destination X/Y. Rotated textures swap those
    // axes, so leave them to Hyprland's normal surface renderer.
    if (swapsAxes(transform))
        return false;

    const auto& projection = g_pHyprRenderer->projectBoxToTarget(data.boxPx, transform);

    g_pHyprOpenGL->useShader(g_shader.shader);
    g_shader.shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, projection.getMatrix());
    g_shader.shader->setUniformInt(SHADER_TEX, 0);
    g_shader.shader->setUniformFloat(SHADER_ALPHA, alpha);
    glUniform2f(g_shader.footprint, (data.uvMax.x - data.uvMin.x) / data.boxPx.w,
                (data.uvMax.y - data.uvMin.y) / data.boxPx.h);
    glUniform2f(g_shader.uvMin, data.uvMin.x, data.uvMin.y);
    glUniform2f(g_shader.uvMax, data.uvMax.x, data.uvMax.y);
    glUniform2f(g_shader.targetSize, data.boxPx.w, data.boxPx.h);
    glUniform1i(g_shader.sampleGrid, data.sampleGrid == 4 ? 4 : 2);
    glUniform1f(g_shader.radius, static_cast<float>(std::max(0.0, data.radiusPx - 1.0)));
    glUniform1f(g_shader.roundingPower, fallback.roundingPower);
    glUniform1i(g_shader.forceOpaque, texture->m_type == Render::TEXTURE_RGBX || texture->m_opaque ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    texture->bind();
    texture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, texture->magFilter);
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, texture->minFilter);
    glBindVertexArray(g_shader.shader->getUniformLocation(SHADER_SHADER_VAO));

    g_pHyprRenderer->blend(true);
    const auto clipped = intersection(data.boxPx, data.clipPx);
    if (clipped.w > 0.0 && clipped.h > 0.0) {
        g_pHyprOpenGL->scissor(clipped);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    g_pHyprOpenGL->scissor(static_cast<const pixman_box32*>(nullptr));
    glBindVertexArray(0);
    texture->unbind();

    if (!g_pHyprRenderer->m_bBlockSurfaceFeedback)
        fallback.surface->presentFeedback(fallback.when, fallback.pMonitor->m_self.lock());
    if (fallback.surface->m_current.buffer && !fallback.surface->m_current.buffer->isSynchronous())
        g_pHyprRenderer->m_usedAsyncBuffers.emplace_back(fallback.surface->m_current.buffer);
    return true;
}

// Fall back to Hyprland's normal surface pass if the shader cannot draw.
class SurfacePass final : public IPassElement {
  public:
    explicit SurfacePass(SurfaceData data) : m_data(std::move(data)) {}

    std::vector<UP<IPassElement>> draw() override {
        if (drawSurface(m_data))
            return {};
        std::vector<UP<IPassElement>> fallback;
        fallback.emplace_back(makeUnique<CSurfacePassElement>(m_data.fallback));
        return fallback;
    }

    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    void discard() override { CSurfacePassElement(m_data.fallback).discard(); }
    const char* passName() override { return "GloViewPreviewFilter"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        if (!m_data.fallback.pMonitor || m_data.fallback.pMonitor->m_scale <= 0.0)
            return std::nullopt;
        const double scale = m_data.fallback.pMonitor->m_scale;
        const auto   clipped = intersection(m_data.boxPx, m_data.clipPx);
        return CBox{clipped.x / scale, clipped.y / scale, clipped.w / scale, clipped.h / scale};
    }

  private:
    SurfaceData m_data;
};

} // namespace

UP<IPassElement> makePass(CSurfacePassElement::SRenderData fallback, const CBox& boxPx, const CBox& clipPx,
                          const Vector2D& uvMin, const Vector2D& uvMax, double radiusPx, int sampleGrid) {
    return makeUnique<SurfacePass>(SurfaceData{
        .fallback = std::move(fallback),
        .boxPx = boxPx,
        .clipPx = clipPx,
        .uvMin = uvMin,
        .uvMax = uvMax,
        .radiusPx = radiusPx,
        .sampleGrid = sampleGrid,
    });
}

void reset() {
    if (g_shader.shader && g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    g_shader.shader.reset();
    g_shader = {};
}

} // namespace gloview::PreviewFilter
