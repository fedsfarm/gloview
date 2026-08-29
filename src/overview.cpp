#include "overview.hpp"
#include "preview_filter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using Render::GL::g_pHyprOpenGL;

namespace gloview {

namespace {

double nowMs(const std::chrono::steady_clock::time_point& from) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

double easeOutCubic(double t) {
    t                = std::clamp(t, 0.0, 1.0);
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

// Accelerate out, decelerate in. easeOutCubic covers 87% of the distance by the halfway
// point, which reads as a teleport when the travel itself is the message.
double easeInOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (t < 0.5)
        return 4.0 * t * t * t;
    const double inv = -2.0 * t + 2.0;
    return 1.0 - (inv * inv * inv) / 2.0;
}

// Decelerate with a gentle overshoot — tiles "pop" as they settle into their slot.
double easeOutBack(double t) {
    t                = std::clamp(t, 0.0, 1.0);
    const double c1  = 1.70158 * 0.6; // softened overshoot
    const double c3  = c1 + 1.0;
    const double inv = t - 1.0;
    return 1.0 + c3 * inv * inv * inv + c1 * inv * inv;
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

CBox box(const LRect& r) {
    return CBox{r.x, r.y, r.w, r.h};
}

// Hyprland's immediate-mode renderRect/renderTexture/renderRoundedShadow feed the box
// STRAIGHT to projectBoxToTarget, which expects transformed monitor-PIXEL coordinates and
// applies NO monitor scale itself (verified against Renderer.cpp: clipBox/scaledWindowBox are
// pre-.scale(m_scale)'d before applyToBox). All gloview chrome is authored in monitor-LOGICAL
// pixels, so it MUST be pre-scaled by mon->m_scale before drawing — otherwise on any monitor
// with scale != 1 (HiDPI / fractional like 1.2) the whole chrome renders at 1/scale size and
// top-left-biased, while the live window surfaces (renderWindowLive, which converts to pixels
// itself) land correctly → the overview looks "distorted". Chrome-only; surfaces are already
// pixel-space. Round radii / blur ranges scale too so corners/shadows keep their proportion.
CBox pxb(const CBox& b, double s) {
    return CBox{b.x * s, b.y * s, b.w * s, b.h * s};
}
CBox pxb(const LRect& r, double s) {
    return CBox{r.x * s, r.y * s, r.w * s, r.h * s};
}
int pxr(double round, double s) {
    return static_cast<int>(round * s);
}

// Hollow ring drawn INSIDE `boxPx` and ON TOP of the live surface, so the band overlaps (crops)
// the window's own edge — like a real Hyprland border, not an outline hugging the tile. A grown
// renderRect can't do this: it's filled, so on top it hides the window and underneath its arc is
// eaten by the surface's corners. Hyprland's border shader paints `thick` px inward from the box
// and discards the interior; `round` is the box's corner radius (it cuts the inside at
// round - borderSize). Box/round in pixel coords (pxb); `thickLg` in LOGICAL px, because
// renderBorder scales borderSize by the monitor scale itself.
// The box MUST be snapped to the pixel grid: the border shader quantizes each edge's thickness
// independently from interpolated fragment distances (`smallest > thick` on pixel centers), so a
// fractional edge renders one px thicker/thinner than the others.
void renderRing(const CBox& boxPx, const CHyprColor& col, double roundPx, double thickLg) {
    if (thickLg < 1.0 || boxPx.w <= 0.0 || boxPx.h <= 0.0 || col.a <= 0.0)
        return;
    CBox snapped = boxPx;
    snapped.round();
    const Config::CGradientValueData grad(col);
    g_pHyprOpenGL->renderBorder(snapped, grad, {.round = static_cast<int>(roundPx), .roundingPower = 2.F, .borderSize = static_cast<int>(std::round(thickLg)), .a = 1.F});
}

LRect fitInside(const LRect& outer, double aspect) {
    if (outer.w <= 0.0 || outer.h <= 0.0 || aspect <= 0.0)
        return outer;

    const double outerAspect = outer.w / outer.h;
    if (std::abs(outerAspect - aspect) <= 0.01)
        return outer;

    if (outerAspect > aspect) {
        const double w = outer.h * aspect;
        return LRect{outer.x + (outer.w - w) / 2.0, outer.y, w, outer.h};
    }

    const double h = outer.w / aspect;
    return LRect{outer.x, outer.y + (outer.h - h) / 2.0, outer.w, h};
}

bool roughly(double a, double b, double tol = 3.0) {
    return std::abs(a - b) <= tol;
}

// Overlap of two boxes; w/h are 0 when they don't touch. Used to keep a card's contents
// inside the card: a window whose tiled slot pokes outside the monitor (floating, offscreen,
// oversized) maps to a rel rect outside 0..1, and without an explicit crop its preview
// spills over the strip band and the neighbouring cards.
CBox intersectBox(const CBox& a, const CBox& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    return CBox{x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

LRect intersectRect(const LRect& a, const LRect& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    return LRect{x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

// Hyprland applies extra UV corrections while a surface and its window geometry disagree.
// Use the custom filter only when its basic viewport UVs match the normal surface path.
bool hasStablePreviewUV(const PHLWINDOW& window, const SP<CWLSurfaceResource>& surface, bool mainSurface,
                        const CBox& texBoxLogical, const CBox& texBoxPx, const PHLMONITORREF& monitor) {
    if (!window || !surface || !monitor || window->sizeAnimation()->isBeingAnimated())
        return false;

    const auto& drag = g_layoutManager->dragController();
    if (drag && drag->target() == window->m_target && drag->mode() >= MBIND_RESIZE)
        return false;

    const auto& state = surface->m_current;
    if (window->m_isX11 && state.viewport.hasSource)
        return false;
    if (texBoxLogical.size() != state.size)
        return false;

    Vector2D expectedPx;
    if (state.viewport.hasDestination)
        expectedPx = (state.viewport.destination * monitor->m_scale).round();
    else if (state.viewport.hasSource)
        expectedPx = (state.viewport.source.size() * monitor->m_scale).round();
    else if (mainSurface && window->getReportedSize() != state.size)
        expectedPx = (state.size * monitor->m_scale).round();
    else if (mainSurface)
        expectedPx = (window->getReportedSize() * monitor->m_scale).round();
    else
        expectedPx = texBoxPx.size();

    if (texBoxPx.size() != expectedPx)
        return false;

    return true;
}

// Mirrors Hyprland's MISALIGNEDFSV1 correction in ElementRenderer: a fractional-scale
// client can submit a scale-1 buffer that differs from the rounded projected size by one
// or two pixels. Trim/extend the bottom-right UV so the source span matches the projected
// surface exactly. Hyprland uses this with nearest-neighbour sampling; our corrected span
// is instead safe to feed through box4/box16.
void fixFractionalScaleUV(const SP<CWLSurfaceResource>& surface, const PHLMONITORREF& monitor,
                          const Vector2D& projectedSizePx, Vector2D& uvMin, Vector2D& uvMax) {
    if (!surface || !monitor)
        return;

    const auto& state      = surface->m_current;
    const auto& bufferSize = state.bufferSize;
    if (bufferSize.x <= 0.0 || bufferSize.y <= 0.0 || std::floor(monitor->m_scale) == monitor->m_scale ||
        state.scale != 1 || projectedSizePx == bufferSize)
        return;

    const auto delta = projectedSizePx - bufferSize;
    if (std::abs(delta.x) >= 3.0 || std::abs(delta.y) >= 3.0)
        return;

    const Vector2D misalignment = (uvMax - uvMin) * bufferSize - projectedSizePx;
    if (misalignment != Vector2D{})
        uvMax -= misalignment / bufferSize;
}

// Render a window's LIVE surface tree scaled into `destPx`, clipped to `clipPx`
// (both monitor PIXEL coords) via real CSurfacePassElements. No crop rect to drift,
// so immune to snapshots' stale/mis-cropped tiles; works on hidden workspaces.
void renderWindowLive(const PHLWINDOW& w, const PHLMONITOR& mon, const CBox& destPx, const CBox& clipPx, float alpha, const Time::steady_tp& when,
                      int previewFilterGrid, double roundSlotPx = 0.0) {
    if (!w || !mon || !w->m_isMapped || !w->wlSurface() || !w->wlSurface()->resource())
        return;
    if (!(destPx.w > 0 && destPx.h > 0))
        return;

    // When reported size > committed buffer (CWLSurface::small(): X11 size hints/mid-resize),
    // getTexBox CENTERS it at real size, leaving an uncovered margin. m_fillIgnoreSmall
    // stretches to fill; Hyprland never sets it, so restoreFill() resets on teardown.
    w->wlSurface()->m_fillIgnoreSmall = true;

    // SETTLED goal() geometry, not mid-animation value(): destPx is sized from goal(), so
    // scaling by value() mid-resize fills only part of the box → black side strips. Position
    // cancels in the translate remap below, so only size matters.
    const auto pos      = w->positionAnimation()->goal();
    const auto size     = w->sizeAnimation()->goal();
    const float logicalW = std::max((float)size.x, 5.F);
    const float logicalH = std::max((float)size.y, 5.F);
    // Over-cover the slot (fill BOTH axes via max + ~1.5px pad), don't just fit it: a
    // fit-exact scale rounds the surface edge 1-3px inside the box and the opaque backing
    // peeks through as thin dark seams (worst mid open-glide, destPx fractional every frame).
    // TL stays anchored (translate cancels scaleMod); clipPx trims the overflow.
    // When we round the surface (roundSlotPx > 0) the over-cover would push the rounded
    // right/bottom corners past the slot so only the TL corner clips cleanly; drop the pad so
    // the surface fills the slot exactly and all four rounded corners land on the slot edges.
    const float pad      = roundSlotPx > 0.0 ? 0.F : 1.5F;
    const float sW       = (static_cast<float>(destPx.w) + pad) / std::max(logicalW * mon->m_scale, 5.F);
    const float sH       = (static_cast<float>(destPx.h) + pad) / std::max(logicalH * mon->m_scale, 5.F);
    const float scaleMod = std::max(sW, sH);
    if (!(scaleMod > 0.F))
        return;

    const Vector2D logicalTL = pos + w->m_floatingOffset;
    const Vector2D scaledTL  = (logicalTL - mon->m_position) * mon->m_scale;
    const Vector2D translate = destPx.pos() / scaleMod - scaledTL;

    Render::SRenderModifData modif;
    modif.modifs.push_back({Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE, std::any(translate)});
    modif.modifs.push_back({Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, std::any(scaleMod)});
    modif.enabled = true;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    Hyprutils::Utils::CScopeGuard reset([] {
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    });

    g_pHyprRenderer->damageWindow(w);

    CSurfacePassElement::SRenderData data{};
    data.pMonitor       = mon;
    data.when           = when;
    data.pos            = logicalTL;
    data.w              = std::max(size.x, 5.0);
    data.h              = std::max(size.y, 5.0);
    data.surface        = w->wlSurface()->resource();
    data.dontRound      = Fullscreen::controller()->getFullscreenModes(w).internal == Fullscreen::FSMODE_FULLSCREEN;
    data.fadeAlpha      = 1.F;
    data.alpha          = std::clamp(alpha, 0.F, 1.F);
    data.decorate       = false;
    data.rounding       = roundSlotPx > 0.0 ? roundSlotPx * mon->m_scale : 0.0;
    data.roundingPower  = w->roundingPower();
    data.blur           = false;
    data.pWindow        = w;
    data.clipBox        = clipPx;
    data.squishOversized = true;
    data.surfaceCounter = 0;

    w->wlSurface()->resource()->breadthfirst(
        [&data, &w, &clipPx, &modif, previewFilterGrid](SP<CWLSurfaceResource> s, const Vector2D& offset, void*) {
            if (!s || !s->m_current.texture || s->m_current.size.x < 1 || s->m_current.size.y < 1)
                return;
            data.localPos    = offset;
            data.texture     = s->m_current.texture;
            data.surface     = s;
            data.mainSurface = s == w->wlSurface()->resource();

            if (previewFilterGrid == 0 || data.texture->m_type == Render::TEXTURE_EXTERNAL) {
                g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(data));
                data.surfaceCounter++;
                return;
            }

            // Match CSurfacePassElement's size, rounding and animated-subsurface rules.
            CSurfacePassElement geometry(data);
            const CBox          texBoxLogical = geometry.getTexBox();
            CBox                targetPx = texBoxLogical;
            targetPx.scale(data.pMonitor->m_scale);
            targetPx.round();
            const bool stableUV = hasStablePreviewUV(w, s, data.mainSurface, texBoxLogical, targetPx, data.pMonitor);
            const Vector2D projectedSizePx = targetPx.size();
            modif.applyToBox(targetPx);

            Vector2D uvMin{0.0, 0.0};
            Vector2D uvMax{1.0, 1.0};
            const auto& viewport = s->m_current.viewport;
            const auto& bufferSize = s->m_current.bufferSize;
            if (viewport.hasSource && bufferSize.x > 0.0 && bufferSize.y > 0.0) {
                uvMin = Vector2D{viewport.source.x / bufferSize.x, viewport.source.y / bufferSize.y};
                uvMax = Vector2D{(viewport.source.x + viewport.source.width) / bufferSize.x,
                                 (viewport.source.y + viewport.source.height) / bufferSize.y};
            }
            if (stableUV)
                fixFractionalScaleUV(s, data.pMonitor, projectedSizePx, uvMin, uvMax);

            const Vector2D sampledPixels = (uvMax - uvMin) * data.texture->m_size;
            const bool minifying = sampledPixels.x > targetPx.w * 1.05 || sampledPixels.y > targetPx.h * 1.05;
            // Native-sized textures stay on Hyprland's normal path.
            if (stableUV && minifying && targetPx.w > 1.0 && targetPx.h > 1.0) {
                g_pHyprRenderer->m_renderPass.add(PreviewFilter::makePass(
                    data, targetPx, clipPx, uvMin, uvMax, data.mainSurface && !data.dontRound ? data.rounding : 0.0,
                    previewFilterGrid));
            } else {
                g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(data));
            }
            data.surfaceCounter++;
        },
        nullptr);
}

using PSHOULDRENDER              = bool (*)(void*, PHLWINDOW, PHLMONITOR);
using PSHOULDRENDERWINDOW        = bool (*)(void*, PHLWINDOW);
PSHOULDRENDER       g_shouldRenderOrig       = nullptr;
PSHOULDRENDERWINDOW g_shouldRenderWindowOrig = nullptr;

bool hkShouldRenderWindow(void* thisptr, PHLWINDOW window, PHLMONITOR monitor) {
    if (g_overview) {
        // force-render the snapshotting window (may be on an inactive workspace, which
        // Hyprland's original rejects → blank/grey preview).
        if (g_overview->forceRenderWindow(window))
            return true;
        if (g_overview->shouldHideWindow(window, monitor))
            return false;
    }
    return g_shouldRenderOrig ? g_shouldRenderOrig(thisptr, window, monitor) : true;
}

bool hkShouldRenderWindowAny(void* thisptr, PHLWINDOW window) {
    if (g_overview && g_overview->forceRenderWindow(window))
        return true;
    return g_shouldRenderWindowOrig ? g_shouldRenderWindowOrig(thisptr, window) : true;
}

CHyprColor argb(Hyprlang::INT raw, double alphaMul = 1.0) {
    const auto a = static_cast<double>((raw >> 24) & 0xFF) / 255.0;
    const auto r = static_cast<double>((raw >> 16) & 0xFF) / 255.0;
    const auto g = static_cast<double>((raw >> 8) & 0xFF) / 255.0;
    const auto b = static_cast<double>(raw & 0xFF) / 255.0;
    return CHyprColor(r, g, b, a * std::clamp(alphaMul, 0.0, 1.0));
}

// Immediate-mode overlay chrome split into phases so the LIVE window surfaces (queued
// CSurfacePassElements, not immediate GL) layer between the chrome:
// backdrop+backings (Back) → main surfaces → preview rings (MainFront) → strip chrome (Mid) →
// strip surfaces → card rings (StripFront) → flying tile chrome (FlyBack) → flying surface →
// dragged tile chrome (DragBack) → dragged surface → drag ring + cursor (Front). Ring phases
// run AFTER their surfaces (see renderRing).
class COverlayPass final : public IPassElement {
  public:
    enum class Phase { Back, MainFront, Mid, StripFront, FlyBack, DragBack, Front };
    COverlayPass(Overview* o, Phase phase) : m_owner(o), m_phase(phase) {}

    std::vector<UP<IPassElement>> draw() override {
        if (!m_owner)
            return {};
        switch (m_phase) {
            case Phase::Back:
                m_owner->renderBackdrop();
                m_owner->renderPrevPreviews(); // outgoing workspace, under the incoming one
                m_owner->renderPreviews(); // main tile chrome; surfaces queued right after
                break;
            case Phase::MainFront: m_owner->renderPreviewRings(); break; // over the main surfaces
            case Phase::Mid: m_owner->renderStrip(); break;              // strip chrome; surfaces queued after
            case Phase::StripFront: m_owner->renderStripRings(); break;  // over the card surfaces
            case Phase::FlyBack: m_owner->renderFlyTile(); break;        // moved-window flight chrome; surface queued after
            case Phase::DragBack: m_owner->renderDragTile(); break;      // dragged tile chrome; surface queued after
            case Phase::Front:
                m_owner->renderDragRing(); // over the dragged surface
                m_owner->renderCursorOnTop();
                break;
        }
        return {};
    }

    // Back (backdrop) and Mid (strip band) draw blurred rects. Hyprland refreshes the
    // live-blur framebuffer only from elements reporting true; false → stale blur residue.
    bool                needsLiveBlur() override { return (m_phase == Phase::Back || m_phase == Phase::Mid) && m_owner && m_owner->blurEnabled(); }
    bool                needsPrecomputeBlur() override { return false; }
    // Occlusion culling must be off while the overview is up. The queued preview surfaces
    // (CSurfacePassElement) report boundingBox/opaqueRegion at the window's REAL footprint —
    // they can't see our translate+scale render-modif — so once the open animation settles
    // (alpha hits 1, opaqueRegion turns non-empty) each preview "occludes" its real,
    // often monitor-filling rect and CRenderPass::simplify() empties the damage of every
    // element below it: backdrop, strip chrome, other previews, the background layer. With
    // blur ≠ 0 needsLiveBlur() masked this (the live-blur region neutralizes all opaque
    // subtraction in simplify()); with blur = 0 the overview drew for the open animation,
    // then collapsed to bare wallpaper/stale frames. No perf cost: the overview damages the
    // whole monitor every frame anyway, so there is nothing useful for simplify() to cull.
    bool                disableSimplification() override { return true; }
    bool                undiscardable() override { return true; }
    const char*         passName() override { return "GloviewOverlayPass"; }
    ePassElementType    type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        const auto m = m_owner ? m_owner->monitor() : nullptr;
        if (!m)
            return std::nullopt;
        return CBox{{}, m->m_size};
    }

  private:
    Overview*   m_owner = nullptr;
    Phase       m_phase = Phase::Back;
};

} // namespace

Overview::Overview(HANDLE handle) : m_handle(handle) {}

Overview::~Overview() {
    // stop rendering before state/hooks tear down, so any in-flight frame sees an
    // inactive overview and no dangling refs.
    m_active  = false;
    m_opening = false;
    PreviewFilter::reset();
    restoreLayers(); // never leave a bar stuck at alpha 0 if we're torn down mid-hide
    restoreFill();   // never leave a window's surface stuck stretching its small buffer
    if (const auto ws = m_newWs.lock()) // don't leak a persistent workspace either
        ws->setPersistent(false);
    m_newWs.reset();
    m_tiles.clear();
    m_strip.clear();
    m_prevTiles.clear();
    m_wsSliding = false;
    m_flying.clear();
    m_snapFB.clear(); // release the snapshot framebuffers we own since 0.56
    m_snapGeom.clear();
    if (m_recaptureTimer && g_pEventLoopManager) {
        m_recaptureTimer->cancel();
        g_pEventLoopManager->removeTimer(m_recaptureTimer);
        m_recaptureTimer.reset();
    }
    if (m_shouldRenderHook) {
        HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderHook);
        m_shouldRenderHook = nullptr;
    }
    if (m_shouldRenderWindowHook) {
        HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
        m_shouldRenderWindowHook = nullptr;
    }
    g_shouldRenderOrig       = nullptr;
    g_shouldRenderWindowOrig = nullptr;
}

bool Overview::initialize() {
    auto& events = Event::bus()->m_events;

    m_renderStageL = events.render.stage.listen([this](eRenderStage stage) { renderStage(stage); });
    m_mouseButtonL = events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        const auto copied = event;
        if (onMouseButton(copied))
            info.cancelled = true;
    });
    m_mouseAxisL = events.input.mouse.axis.listen([this](const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) {
        if (onMouseAxis(event))
            info.cancelled = true;
    });
    m_mouseMoveL = events.input.mouse.move.listen([this](const Vector2D&, Event::SCallbackInfo&) { onMouseMove(); });
    m_keyL       = events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        bool cancel = false;
        onKey(event, cancel);
        if (cancel)
            info.cancelled = true;
    });

    const auto matches = HyprlandAPI::findFunctionsByName(m_handle, "shouldRenderWindow");
    void*      addr    = nullptr;
    void*      addrAny = nullptr;
    // Match on arity, not on the full parameter spelling: Hyprland keeps re-namespacing the
    // argument types (0.55 CWindow -> Desktop::View::CWindow, 0.56 CMonitor -> Monitor::CMonitor)
    // and every rename silently nulled the address, failing init on an ABI bump. The one-arg
    // overload takes the window, the two-arg one takes (window, monitor) — that's stable.
    const std::string kPrefix = "shouldRenderWindow(";
    for (const auto& mt : matches) {
        const auto argsAt = mt.demangled.find(kPrefix);
        if (argsAt == std::string::npos)
            continue;
        const auto args = mt.demangled.substr(argsAt + kPrefix.length());
        if (args.find(',') != std::string::npos)
            addr = mt.address; // (window, monitor)
        else
            addrAny = mt.address; // (window)
    }
    if (!addr) {
        HyprlandAPI::addNotification(m_handle, "[gloview] could not find shouldRenderWindow to hook", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        return false;
    }
    if (!addrAny) {
        HyprlandAPI::addNotification(m_handle, "[gloview] could not find shouldRenderWindow(window) to hook", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        return false;
    }
    m_shouldRenderHook = HyprlandAPI::createFunctionHook(m_handle, addr, reinterpret_cast<void*>(&hkShouldRenderWindow));
    if (!m_shouldRenderHook || !m_shouldRenderHook->hook()) {
        // Most common cause: ANOTHER gloview instance already holds this hook (e.g. the
        // hyprpm-installed copy autoloaded at session start while the dev `reload` target
        // loads the build path) — Hyprland can't trampoline an already-hooked prologue.
        HyprlandAPI::addNotification(m_handle, "[gloview] failed to hook shouldRenderWindow — is another gloview instance loaded (hyprpm)?", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        return false;
    }
    g_shouldRenderOrig = reinterpret_cast<PSHOULDRENDER>(m_shouldRenderHook->m_original);

    m_shouldRenderWindowHook = HyprlandAPI::createFunctionHook(m_handle, addrAny, reinterpret_cast<void*>(&hkShouldRenderWindowAny));
    if (!m_shouldRenderWindowHook || !m_shouldRenderWindowHook->hook()) {
        HyprlandAPI::addNotification(m_handle, "[gloview] failed to hook shouldRenderWindow(window)", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        if (m_shouldRenderWindowHook) {
            HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
            m_shouldRenderWindowHook = nullptr;
        }
        HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderHook);
        m_shouldRenderHook = nullptr;
        g_shouldRenderOrig = nullptr;
        return false;
    }
    g_shouldRenderWindowOrig = reinterpret_cast<PSHOULDRENDERWINDOW>(m_shouldRenderWindowHook->m_original);
    return true;
}

// ---- config -----------------------------------------------------------------

// Read through the V2 value() accessor (ConfigRegistry in overview.hpp): the deprecated
// getConfigValue() ignored Lua-config values, so they had no effect.
int Overview::cfgInt(const char* name, int fallback) const {
    const auto it = g_config.ints.find(name);
    return (it != g_config.ints.end() && it->second) ? static_cast<int>(it->second->value()) : fallback;
}

float Overview::cfgFloat(const char* name, float fallback) const {
    const auto it = g_config.floats.find(name);
    return (it != g_config.floats.end() && it->second) ? static_cast<float>(it->second->value()) : fallback;
}

Hyprlang::INT Overview::cfgColor(const char* name, Hyprlang::INT fallback) const {
    const auto it = g_config.colors.find(name);
    return (it != g_config.colors.end() && it->second) ? static_cast<Hyprlang::INT>(it->second->value()) : fallback;
}

std::string Overview::cfgStr(const char* name, const char* fallback) const {
    const auto it = g_config.strings.find(name);
    return (it != g_config.strings.end() && it->second) ? it->second->value() : std::string{fallback};
}

Overview::Anchor Overview::stripAnchor() const {
    std::string a = cfgStr("plugin:gloview:anchor", "");
    if (a.empty()) // back-compat: the old top|bottom knob
        a = cfgStr("plugin:gloview:bar_position", "top");
    if (a == "bottom")
        return Anchor::Bottom;
    if (a == "left")
        return Anchor::Left;
    if (a == "right")
        return Anchor::Right;
    return Anchor::Top;
}

bool Overview::stripHorizontal() const {
    const Anchor a = stripAnchor();
    return a == Anchor::Top || a == Anchor::Bottom;
}

bool Overview::showWorkspaceLabels() const {
    return cfgInt("plugin:gloview:show_workspace_labels", 1) != 0;
}

bool Overview::showWindowLabels() const {
    return cfgInt("plugin:gloview:show_window_labels", 1) != 0;
}

// Band reserved above every card for its name. With workspace labels off it collapses to 0
// so the cards grow into the freed space instead of leaving a gap where the text was.
double Overview::stripLabelH() const {
    return showWorkspaceLabels() ? 26.0 : 0.0;
}

// gnome / hyprnome-style workspaces: only populated workspaces are listed, and the strip
// always ends in ONE empty workspace. Moving a window there materialises it and a fresh
// empty tail appears; workspaces emptied this way stop being listed and Hyprland reaps them.
bool Overview::dynamicWorkspaces() const {
    return cfgInt("plugin:gloview:dynamic_workspaces", 1) != 0;
}

// plugin:gloview:autodelete_empty — the "empty ones automatically get deleted" half of
// GNOME-style workspaces.
//
// Hyprland already destroys a plain empty workspace by itself the moment nothing holds a
// strong ref to it (that is exactly why addWorkspace has to pin its fresh card persistent to
// keep it alive at all). So there is nothing here to "delete": what actually lingers is
// workspaces something still pins — a `persistent:true` rule from the user's config, or
// gloview's own held tail. Dropping that pin lets the normal reaping take them.
//
// On by default, but note the one lasting effect: releasing a config-declared persistent
// workspace is not undone until the next config reload. Set autodelete_empty = 0 to keep
// `persistent:true` workspaces pinned.
void Overview::autodeleteEmpty() {
    if (cfgInt("plugin:gloview:autodelete_empty", 1) == 0)
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;

    const auto shown = m_workspace.lock();
    const auto held  = m_newWs.lock();

    // Copy, not the live view: setPersistent(false) can drop the last ref and destroy the
    // workspace mid-iteration.
    for (const auto& ws : State::workspaceState()->workspacesCopy()) {
        if (!ws || ws->m_isSpecialWorkspace || ws->m_id <= 0) // leave scratchpads and named (-1337…) workspaces alone
            continue;
        if (ws->m_monitor.lock() != m) // only this monitor's — other outputs aren't ours to prune
            continue;
        if (ws == shown)               // currently displayed in the overview
            continue;
        // Our own freshly created card. addWorkspace() pins it for exactly the reason this
        // function undoes pins, and with switch_on_new_workspace = 0 it is created WITHOUT
        // being displayed — so without this the sweep would delete the card the very frame it
        // was added. Releasing it when it is genuinely abandoned is switchToWorkspace's job.
        if (ws == held)
            continue;
        if (ws->isVisible())           // active on some monitor
            continue;
        if (workspaceOccupied(ws))
            continue;
        if (!ws->isPersistent())       // nothing pinning it; Hyprland reaps it without our help
            continue;

        ws->setPersistent(false);
        dbg("autodelete: released empty workspace " + std::to_string(ws->m_id));
    }
}

// "Is this workspace worth listing?" — a window that is actually on screen. Unmapped/hidden
// ones don't count, so a workspace holding only those still reads as the empty tail. Shared by
// buildStrip and the live (overview-closed) stepping so both list the same workspaces.
bool Overview::wsHasMappedWindows(const PHLWORKSPACE& ws) const {
    if (!ws)
        return false;
    for (const auto& win : Desktop::windowState()->windows())
        if (win && win->m_isMapped && !win->isHidden() && win->m_workspace == ws)
            return true;
    return false;
}

// Any window at all lives here? Deliberately broader than wsHasMappedWindows(): a
// workspace holding a window that is merely unmapped or hidden is NOT empty, and reaping it
// out from under that window would be far worse than leaving a stale card up.
bool Overview::workspaceOccupied(const PHLWORKSPACE& ws) const {
    if (!ws)
        return false;
    for (const auto& win : Desktop::windowState()->windows())
        if (win && win->m_workspace == ws)
            return true;
    return false;
}

int Overview::nextWorkspaceId() const {
    int id = 1;
    while (State::workspaceState()->query().id(id).run())
        ++id;
    return id;
}

// A create-on-use card advertises the id it will take (dynamic_workspaces labels the trailing
// card with it), so honour that id unless something claimed it between the build and the click.
int Overview::resolveNewId(int wanted) const {
    if (wanted > 0 && !State::workspaceState()->query().id(wanted).run())
        return wanted;
    return nextWorkspaceId();
}

double Overview::stripThickness() const {
    const auto m = m_monitor.lock();
    if (!m)
        return 150.0;
    // clamp against the axis perpendicular to the band: height for a horizontal
    // strip, width for a vertical one.
    const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
    return std::clamp(static_cast<double>(cfgInt("plugin:gloview:strip_height", 150)), 100.0, cross * 0.42);
}

double Overview::stripOffset() const {
    const auto m = m_monitor.lock();
    if (!m)
        return 0.0;
    // distance from the anchored edge. 0 = flush (no auto bar gap); purely cosmetic.
    const double cross = stripHorizontal() ? m->m_size.y : m->m_size.x;
    return std::clamp(static_cast<double>(cfgInt("plugin:gloview:strip_offset", 0)), 0.0, cross * 0.4);
}

LRect Overview::stripBand() const {
    const auto m = m_monitor.lock();
    if (!m)
        return LRect{0, 0, 0, 0};
    const double T   = stripThickness();
    const double off = stripOffset();
    const double W   = m->m_size.x;
    const double H   = m->m_size.y;
    switch (stripAnchor()) {
        case Anchor::Bottom: return LRect{0, H - T - off, W, T};
        case Anchor::Left:   return LRect{off, 0, T, H};
        case Anchor::Right:  return LRect{W - T - off, 0, T, H};
        case Anchor::Top:
        default:             return LRect{0, off, W, T};
    }
}

Vector2D Overview::stripSlide(double e) const {
    // slide the strip in from its own edge as it fades (distance ∝ band thickness).
    const double d = (1.0 - e) * stripThickness() * 0.55;
    switch (stripAnchor()) {
        case Anchor::Bottom: return Vector2D{0.0, d};
        case Anchor::Left:   return Vector2D{-d, 0.0};
        case Anchor::Right:  return Vector2D{d, 0.0};
        case Anchor::Top:
        default:             return Vector2D{0.0, -d};
    }
}

bool Overview::blurEnabled() const {
    return blurStrength() > 0.0F;
}

// plugin:gloview:blur is a float: 0 = off, 1 = full, between scales blur strength via the
// pass alpha. Clamped 0..1.
float Overview::blurStrength() const {
    return std::clamp(cfgFloat("plugin:gloview:blur", 1.0F), 0.0F, 1.0F);
}

// ---- open / close -----------------------------------------------------------

void Overview::toggle() {
    if (m_active && m_opening)
        close();
    else
        open(); // opens into the tidy grid; Shift / gloview:desktop flips to the canvas
}

// gloview:allworkspaces — toggle the "expo" view (every window on the monitor). OPENS the
// overview if closed (so it works as a single-key bind); while open flips expo on/off and
// glides tiles. m_allOverride overrides show_all_workspaces until close (deactivate → -1).
void Overview::toggleAllWorkspaces() {
    if (!(m_active && m_opening)) {
        m_allOverride = 1; // open directly into expo (set BEFORE open() so buildTiles sees it)
        open();
        if (!m_active)     // open declined (no monitor / nothing to show) — don't leave it armed
            m_allOverride = -1;
        return;
    }
    m_allOverride = showAllWorkspaces() ? 0 : 1;
    // rebuild from the new membership and glide tiles into place (chrome settled at progress 1).
    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
    replayReflow(oldBoxes);
}

// gloview:desktop — flip grid <-> canvas, ONLY while open (no-op closed).
void Overview::toggleDesktop() {
    if (m_active && m_opening)
        setDesktopMode(!m_desktopMode);
}

// Flip grid <-> canvas while open, gliding previews into the new layout. Canvas is
// purely visual — NO real window is floated/moved.
void Overview::setDesktopMode(bool on) {
    m_desktopMode = on;
    m_canvasPos.clear(); // each entry into the canvas starts from the real positions

    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
    m_hovered = m_hoveredStrip = -1;
    m_selected = -1;
    // Canvas is PURELY VISUAL — never floats/moves/resizes a real window. Survivors don't
    // shuffle on add/remove: syncTiles parks them in m_canvasPos (frozen slot) + frozen
    // aspect, so only the newcomer flows (residual: a re-tiled survivor's live content is
    // over-covered to fill its slot).
    replayReflow(oldBoxes); // rebuild + glide previews grid<->canvas, chrome pinned at 1
}

void Overview::open() {
    if (m_active && m_opening)
        return;

    const auto m = State::monitorState()->query().vec(Pointer::mgr()->position()).run();
    if (!m || !m->m_activeWorkspace)
        return;

    m_monitor      = m;
    m_workspace    = m->m_activeWorkspace;
    m_liveWsAtOpen = m->m_activeWorkspace; // exit_on_switch watches this for external changes
    m_hovered = m_hoveredStrip = -1;

    // Force-rebuild the pre-blurred bg cache (m_blurFB): built once at boot from a stale
    // pre-window scene and only re-dirtied on bg damage, so the first overview's band else
    // samples the cold cache and renders see-through ("transparent strip").
    m->m_blurFBDirty = true;

    // Clear drag/press state: a mid-drag dismiss (ESC/TAB/click) skips the release handler,
    // so the next open would inherit m_dragging + stale m_pressTile (a tile floats at cursor).
    m_pressTile = -1;
    m_dragging  = false;
    m_dragX = m_dragY = m_pressX = m_pressY = m_grabDX = m_grabDY = 0.0;
    m_canvasPos.clear();
    m_desktopMode = false; // open into the tidy grid; Shift / gloview:desktop flips to the canvas
    m_newCardAnim = false;
    m_newCardId   = 0;
    m_newWs.reset();
    m_committedFrom.reset();
    endWsSlide();    // nothing to slide out of on a fresh open
    m_flying.clear();

    buildTiles();
    buildStrip();
    // Nothing to show at all. (Not "<= 1 card": with dynamic_workspaces a bare desktop lists
    // exactly one card — the empty workspace you are on, with no create-on-use tail behind
    // it — and that must still open.)
    if (m_tiles.empty() && m_strip.empty())
        return;

    layoutTiles();

    // seed keyboard selection on the focused window (else first tile) for arrow-nav/Enter.
    m_selected = m_tiles.empty() ? -1 : 0;
    if (const auto fw = Desktop::focusState()->window()) {
        for (size_t i = 0; i < m_tiles.size(); ++i)
            if (m_tiles[i].win.lock() == fw) {
                m_selected = static_cast<int>(i);
                break;
            }
    }

    m_active    = true;
    m_opening   = true;
    m_reflowing = false;
    m_pendingDeactivate = false;
    m_progress  = 0.0;
    m_animStart = std::chrono::steady_clock::now();
    hideLayers(); // fade bars out (no-op unless hide_top/overlay_layers set)
    damage();
}

void Overview::close() {
    if (!m_active)
        return;
    m_opening   = false;
    m_reflowing = false;
    endWsSlide();
    m_flying.clear();

    // Commit the displayed workspace HERE rather than in deactivate(), so Hyprland's switch
    // is already done by the time the overlay fades — no second, delayed transition.
    commitWorkspace();

    // Re-seed every tile's `natural` to the window's REAL settled geometry so the close anim
    // glides target -> real pixel-perfect (renderMainWindows assumes "progress 0 == real").
    // `natural` gets repurposed as a reflow START box (always in desktop mode, after any
    // swap/drop reflow), so without this windows jump on close.
    if (const auto m = m_monitor.lock()) {
        for (auto& t : m_tiles) {
            if (const auto w = t.win.lock()) {
                const auto p = w->positionAnimation()->goal();
                const auto s = w->sizeAnimation()->goal();
                t.natural    = LRect{p.x - m->m_position.x, p.y - m->m_position.y, std::max(1.0, s.x), std::max(1.0, s.y)};
            }
        }
    }

    restoreLayers(); // bars fade back in over the close animation, not in a pop at the end
    m_animStart = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(static_cast<long>((1.0 - m_progress) * std::max(1, cfgInt("plugin:gloview:duration", 360))));
    damage();
}

// Immediate, animation-free teardown for the UNLOAD path (`hyprctl gloviewunload`, run before
// unloading). close() only *starts* the close anim; unloading the .so before it finishes can
// yank the library while an overlay COverlayPass element or a pending recapture timer — both
// holding callbacks in this .so — is still referenced → SEGV / IPC-dead spin. hardClose drops
// all that state synchronously + damages, so the next frame is plugin-free ("flush frame") and
// dlclose is safe. Hooks stay installed (harmless at m_active=false; dtor removes them). Unlike
// deactivate it does NOT commit the displayed workspace — unload snaps back to the live one.
void Overview::hardClose() {
    // Kill the recapture timer FIRST: its fire lambda captures `this` in this .so, so a
    // tick still pending at unload is the IPC-dead-spin hazard.
    m_recaptureLeft = 0;
    if (m_recaptureTimer) {
        m_recaptureTimer->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_recaptureTimer);
        m_recaptureTimer.reset();
    }

    restoreLayers(); // never leave a bar stuck at alpha 0 if we tear down mid-hide
    restoreFill();   // never leave a window's surface stuck stretching its small buffer
    if (const auto ws = m_newWs.lock()) // don't leak a held-persistent "+"-created workspace
        ws->setPersistent(false);
    m_newWs.reset();

    m_active            = false;
    m_opening           = false;
    m_reflowing         = false;
    m_pendingDeactivate = false;
    m_desktopMode       = false;
    m_newCardAnim       = false;
    m_newCardId         = 0;
    m_progress          = 0.0;
    m_tiles.clear();
    m_strip.clear();
    m_prevTiles.clear();
    m_wsSliding = false;
    m_flying.clear();
    m_committedFrom.reset();
    m_canvasPos.clear();
    m_snapFB.clear(); // release the snapshot framebuffers we own since 0.56
    m_snapGeom.clear();
    m_hovered = m_hoveredStrip = -1;
    m_selected                 = -1;

    damage(); // schedule the plugin-free flush frame
}

// ---- collection -------------------------------------------------------------

// Effective expo state. Runtime m_allOverride wins over the show_all_workspaces config;
// -1 means "follow config".
bool Overview::showAllWorkspaces() const {
    return m_allOverride < 0 ? (cfgInt("plugin:gloview:show_all_workspaces", 0) != 0) : (m_allOverride != 0);
}

// Shared membership predicate for main-area tiles. buildTiles (full rebuild) and syncTiles
// (per-frame add/remove detector) MUST agree, else syncTiles sees a phantom diff every frame
// and reflow-churns.
bool Overview::tileBelongs(const PHLWINDOW& w, const PHLMONITOR& m, const PHLWORKSPACE& ws) const {
    if (!w || !w->m_isMapped || w->isHidden())
        return false;
    const auto wws = w->m_workspace;
    if (!wws)
        return false;
    if (showAllWorkspaces()) { // expo: every window living on this monitor, any workspace
        if (wws->m_isSpecialWorkspace && cfgInt("plugin:gloview:show_special", 0) == 0)
            return false;
        return wws->m_monitor.lock() == m;
    }
    return wws == ws; // single displayed workspace (may be inactive; fine)
}

void Overview::buildTiles() {
    m_tiles.clear();
    const auto m  = m_monitor.lock();
    const auto ws = m_workspace.lock();
    if (!m || !ws)
        return;

    // Off-workspace windows (expo) render live from their last-committed texture, same as
    // the strip cards. Membership shared with syncTiles via tileBelongs().
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!tileBelongs(w, m, ws))
            continue;

        Tile t;
        t.win = w;
        // settled goal(), not value(): a mid-desktop-jump value() carries the workspace-slide
        // offset and would warp every preview.
        const auto p = w->positionAnimation()->goal();
        const auto s = w->sizeAnimation()->goal();
        t.natural    = LRect{p.x - m->m_position.x, p.y - m->m_position.y, std::max(1.0, s.x), std::max(1.0, s.y)};
        t.snapSource = t.natural; // refined to the live frozen rect in captureSnapshots()
        m_tiles.push_back(t);
    }

    // cache each window's title texture, drawn under the tile on hover.
    if (g_pHyprOpenGL && g_pHyprRenderer) {
        g_pHyprOpenGL->makeEGLCurrent();
        const auto lblCol = CHyprColor(1.0, 1.0, 1.0, 0.96);
        // show_window_labels = 0: skip the texture entirely rather than skipping the draw, so
        // a hidden label costs no renderText per window per rebuild.
        for (auto& t : m_tiles) {
            const auto w = t.win.lock();
            if (!w || !showWindowLabels())
                continue;
            std::string text = w->m_title;
            if (text.empty())
                text = w->m_class;
            if (text.size() > 80)
                text = text.substr(0, 79) + "…";
            t.label = g_pHyprRenderer->renderText(text, lblCol, 15, false, "", 0, 700);
        }
        // pre-render the desktop-mode "✕" glyph once (rendering text mid-pass is unsafe)
        if (!m_closeGlyph)
            m_closeGlyph = g_pHyprRenderer->renderText("✕", CHyprColor(1.0, 1.0, 1.0, 1.0), 16, false, "", 0, 800);
    }
}

void Overview::buildStrip() {
    m_strip.clear();
    const auto m = m_monitor.lock();
    if (!m)
        return;

    // Prune first, then list: this runs on every rebuild (open, workspace switch, reflow after
    // a drop), so a workspace you just emptied or stepped off is gone before its card is drawn.
    autodeleteEmpty();

    const auto cur = m_workspace.lock();

    // dynamic_workspaces implies "no stray empties": the only empty workspace on the strip is
    // the trailing one, so show_empty is forced off regardless of how it is configured.
    const bool dynamic     = dynamicWorkspaces();
    const bool showEmpty   = !dynamic && cfgInt("plugin:gloview:show_empty", 1) != 0;
    const bool showSpecial = cfgInt("plugin:gloview:show_special", 0) != 0;
    const auto wsHasWindows = [this](const PHLWORKSPACE& w) { return wsHasMappedWindows(w); };

    std::vector<PHLWORKSPACE> wss;
    for (const auto& wref : State::workspaceState()->workspaces()) {
        const auto ws = wref.lock();
        if (!ws || ws->m_monitor.lock() != m)
            continue;
        if (ws->m_isSpecialWorkspace) {
            if (showSpecial && wsHasWindows(ws)) // a scratchpad is only meaningful when populated
                wss.push_back(ws);
            continue;
        }
        if (ws->m_id <= 0)
            continue;
        if (!showEmpty && ws != cur && !wsHasWindows(ws)) // hide empties (keep the displayed one)
            continue;
        wss.push_back(ws);
    }

    if (cur && !cur->m_isSpecialWorkspace && cur->m_id > 0 && cur->m_monitor.lock() == m &&
        std::none_of(wss.begin(), wss.end(), [&](const PHLWORKSPACE& ws) { return ws == cur; })) {
        wss.push_back(cur);
    }

    std::sort(wss.begin(), wss.end(), [](const PHLWORKSPACE& a, const PHLWORKSPACE& b) { return a->m_id < b->m_id; });

    // optional leading "All workspaces" card (toggles expo). Pushed FIRST so it sits at the
    // strip's leading edge; skipped wherever cards are treated as workspaces (see isAll guards).
    if (cfgInt("plugin:gloview:strip_all_card", 0) != 0) {
        StripItem all;
        all.isAll = true;
        all.id    = 0;
        m_strip.push_back(std::move(all));
    }

    for (const auto& ws : wss) {
        StripItem it;
        it.ws     = ws;
        it.id     = ws->m_id;
        it.active = (ws == cur);
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
                continue;
            const auto p = w->positionAnimation()->goal();
            const auto s = w->sizeAnimation()->goal();
            StripWin sw;
            sw.win = w;
            sw.rel = LRect{(p.x - m->m_position.x) / m->m_size.x, (p.y - m->m_position.y) / m->m_size.y, s.x / m->m_size.x, s.y / m->m_size.y};
            it.wins.push_back(sw);
        }
        m_strip.push_back(std::move(it));
    }

    // Trailing card. Normally the "+" that creates a workspace on demand; under
    // dynamic_workspaces it is drawn as an ordinary EMPTY workspace card carrying the id it
    // will take, so the strip always ends in one blank desktop you can step onto or drop a
    // window into (gnome / hyprnome behaviour). Either way it materialises on use.
    //
    // …but only ONE empty desktop at a time. Stepping onto the tail used to materialise it AND
    // immediately grow a fresh tail behind it, so the strip ended in two blank cards and the
    // one you were standing on looked duplicated. When the highest-id listed workspace is
    // already empty it IS the tail, so no create-on-use card is appended; putting a window on
    // it makes the next one appear (and emptying it again takes it back away).
    const bool haveEmptyTail = dynamic && !wss.empty() && wss.back()->m_id > 0 && !wsHasWindows(wss.back());
    StripItem  plus;
    plus.isPlus = true;
    plus.isNew  = dynamic;
    if (dynamic) {
        // The id right after the last listed card, so the strip reads 1, 2, 3, … rather than
        // filling whatever hole the lowest free id happens to be.
        int tail = 1;
        for (const auto& s : m_strip)
            if (!s.isAll && s.id >= tail)
                tail = s.id + 1;
        while (State::workspaceState()->query().id(tail).run())
            ++tail;
        plus.id = tail;
    }
    if (!haveEmptyTail)
        m_strip.push_back(std::move(plus));

    // render workspace name labels (cached textures) up front
    if (g_pHyprOpenGL && g_pHyprRenderer && showWorkspaceLabels()) {
        g_pHyprOpenGL->makeEGLCurrent();
        const auto lblCol = CHyprColor(1.0, 1.0, 1.0, 0.92);
        for (auto& it : m_strip) {
            if (it.isPlus && !it.isNew)
                continue;
            if (it.isAll) {
                it.label = g_pHyprRenderer->renderText("All workspaces", lblCol, 13, false, "", 0, 600);
                continue;
            }
            const auto ws = it.ws.lock();
            std::string nm = ws ? ws->m_name : std::to_string(it.id);
            const bool numeric = !nm.empty() && std::all_of(nm.begin(), nm.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            const std::string text = numeric ? ("Workspace " + nm) : nm;
            it.label               = g_pHyprRenderer->renderText(text, lblCol, 13, false, "", 0, 600);
        }
    }

    // --- lay out the cards: monitor-aspect cards along the band, trailing "+" as the last.
    //     Top/Bottom → horizontal row, Left/Right → vertical column. Each reserves labelH
    //     above it for the name. The whole sequence (cards + "+") is one scrollable group:
    //     centered when it fits, else scrolled (m_stripScroll) so off-screen ends clip at
    //     the monitor edges and nothing spills into the preview area.
    const LRect  band   = stripBand();
    const bool   horiz  = stripHorizontal();
    const double margin = cfgInt("plugin:gloview:strip_margin", 22);
    const double gap    = cfgInt("plugin:gloview:strip_gap", 18);
    const double labelH = stripLabelH(); // 0 when workspace labels are off — cards take the space
    const double aspect = m->m_size.x / std::max(1.0, m->m_size.y);

    double cardW, cardH;
    if (horiz) {
        cardH = std::max(10.0, band.h - 2 * margin - labelH);
        cardW = cardH * aspect;
    } else {
        cardW = std::max(10.0, band.w - 2 * margin);
        cardH = cardW / aspect;
    }

    // size one card occupies along the band's main axis (a vertical column adds the
    // label height per card; a horizontal row's labels live in the band's top margin)
    const double cellMain  = horiz ? cardW : (cardH + labelH);
    const int    n         = static_cast<int>(m_strip.size()); // includes the "+"
    const double bandMain  = horiz ? band.w : band.h;
    const double availMain = std::max(1.0, bandMain - 2 * margin);
    const double groupMain = n * cellMain + std::max(0, n - 1) * gap;
    const double mainOrigin = (horiz ? band.x : band.y) + margin;

    // base position of the first card (scroll == 0). Centered when the group fits,
    // else flush to the start and scrollable.
    m_stripScrollMax = std::max(0.0, groupMain - availMain);
    const double start = (m_stripScrollMax <= 0.0) ? mainOrigin + (availMain - groupMain) / 2.0 : mainOrigin;

    const double cardX = band.x + margin;          // vertical: card column x
    const double cardY = band.y + margin + labelH; // horizontal: card row y

    double main = start;
    for (auto& it : m_strip) {
        if (horiz)
            it.card = LRect{main, cardY, cardW, cardH};
        else
            it.card = LRect{cardX, main + labelH, cardW, cardH};
        main += cellMain + gap;
    }

    // On open / rebuild, scroll the active workspace into view (centered) when the
    // group overflows. Manual wheel scrolling (onMouseAxis) overrides this afterwards.
    m_stripScroll = 0.0;
    if (m_stripScrollMax > 0.0) {
        for (const auto& it : m_strip) {
            if (!it.active)
                continue;
            const double cardCenter = (horiz ? it.card.x + it.card.w / 2.0 : it.card.y + it.card.h / 2.0);
            const double viewCenter = mainOrigin + availMain / 2.0;
            m_stripScroll = std::clamp(cardCenter - viewCenter, 0.0, m_stripScrollMax);
            break;
        }
    }
}

// Scroll offset of the strip group along its main axis (horizontal → x, vertical → y),
// applied on top of each card's base position wherever cards are drawn or hit-tested.
Vector2D Overview::stripScroll() const {
    return stripHorizontal() ? Vector2D{-m_stripScroll, 0.0} : Vector2D{0.0, -m_stripScroll};
}

LRect Overview::stripCardAt(size_t i) const {
    if (i >= m_strip.size())
        return LRect{0, 0, 0, 0};
    const LRect&   c = m_strip[i].card;
    const Vector2D s = stripScroll();
    return LRect{c.x + s.x, c.y + s.y, c.w, c.h};
}

void Overview::layoutTiles() {
    const auto m = m_monitor.lock();
    if (!m || m_tiles.empty())
        return;

    LayoutCfg cfg;
    cfg.engine    = parseEngine(cfgStr("plugin:gloview:layout", "rows").c_str());
    cfg.gap       = cfgInt("plugin:gloview:gap", 34);
    const int padX = cfgInt("plugin:gloview:padding", 80);
    const int padT = cfgInt("plugin:gloview:padding_top", 40);
    const int padB = cfgInt("plugin:gloview:padding_bottom", 70);
    cfg.padLeft   = padX;
    cfg.padRight  = padX;
    cfg.padTop    = padT;
    cfg.padBottom = padB;
    // Keep the main previews clear of the strip: reserve the band's full footprint
    // (offset + thickness) on whichever edge it is anchored to, on top of the
    // configured breathing room.
    const double bandSpan = stripThickness() + stripOffset();
    switch (stripAnchor()) {
        case Anchor::Bottom: cfg.padBottom += bandSpan; break;
        case Anchor::Left:   cfg.padLeft += bandSpan; break;
        case Anchor::Right:  cfg.padRight += bandSpan; break;
        case Anchor::Top:
        default:             cfg.padTop += bandSpan; break;
    }
    cfg.maxScale  = cfgFloat("plugin:gloview:max_scale", 1.0F);

    // Desktop (canvas) mode: fit the WHOLE monitor into the usable area and place each
    // preview at its real scaled position — a shrunk live desktop. A dragged preview keeps
    // its parked spot (m_canvasPos), sticky across rebuilds. Never touches real windows.
    if (m_desktopMode) {
        const double usableX = cfg.padLeft;
        const double usableY = cfg.padTop;
        const double usableW = std::max(1.0, m->m_size.x - cfg.padLeft - cfg.padRight);
        const double usableH = std::max(1.0, m->m_size.y - cfg.padTop - cfg.padBottom);
        const double s       = std::min({usableW / m->m_size.x, usableH / m->m_size.y, static_cast<double>(cfg.maxScale)});
        m_desktopS  = s;
        m_desktopOx = usableX + (usableW - m->m_size.x * s) / 2.0;
        m_desktopOy = usableY + (usableH - m->m_size.y * s) / 2.0;
        for (auto& t : m_tiles) {
            LRect box{m_desktopOx + t.natural.x * s, m_desktopOy + t.natural.y * s, std::max(1.0, t.natural.w * s), std::max(1.0, t.natural.h * s)};
            if (const auto w = t.win.lock()) {
                const auto it = m_canvasPos.find(w.get());
                if (it != m_canvasPos.end())
                    box = it->second; // user-parked position wins
            }
            t.target = box;
        }
        return;
    }

    std::vector<LRect> nat;
    nat.reserve(m_tiles.size());
    for (const auto& t : m_tiles)
        nat.push_back(t.natural);

    const auto out = computeLayout(nat, LRect{0, 0, m->m_size.x, m->m_size.y}, cfg);
    for (size_t i = 0; i < m_tiles.size(); ++i)
        m_tiles[i].target = out[i];
}

void Overview::captureSnapshots() {
    if (!g_pHyprOpenGL || !g_pHyprRenderer)
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;

    // Only snapshot presentable windows: a window mid-move/resize can be transiently
    // unmapped/workspace-less, and makeSnapshot then null-derefs the surface → crash.
    const auto snap = [this](const PHLWINDOW& w) -> bool {
        if (w && w->m_isMapped && w->m_workspace && !w->isHidden()) {
            const auto     ws          = w->m_workspace;
            const bool     wsVis       = ws->m_visible;
            const bool     wsForce     = ws->m_forceRendering;
            // Save BOTH value AND goal: setValueAndWarp(x) also sets goal:=x, so restoring
            // only value() pins a mid-animation workspace at a stale goal. That corruption
            // lives on the workspace, survives a close, and breaks every later open.
            const Vector2D wsOffVal    = ws->m_renderOffset->value();
            const Vector2D wsOffGoal   = ws->m_renderOffset->goal();
            const float    wsAlphaVal  = ws->m_alpha->value();
            const float    wsAlphaGoal = ws->m_alpha->goal();

            // m_forceRendering is THE flag makeSnapshot honours to paint a window on a
            // non-active workspace; without it the window renders empty → black/blank thumb.
            // Only WARP (never assign the goal), else the goal thrash corrupts the workspace.
            ws->m_visible        = true;
            ws->m_forceRendering = true;
            ws->m_renderOffset->setValueAndWarp(Vector2D{});
            ws->m_alpha->setValueAndWarp(1.0F);

            // No window-geometry warp: snap() runs only for SETTLED windows (value≈goal),
            // whose buffer already matches the box → 1:1 paint; warping would stretch a
            // stale-size buffer into the new box.

            // force-render this exact window through the hook for makeSnapshot's duration,
            // so an inactive-workspace window still paints into its FB.
            m_captureWin = w;
            // 0.56: the FB is no longer parked on the window (CWindow::m_snapshotFB is gone),
            // makeSnapshotFB hands it back to the caller — keep it so the "do we already have
            // a good snapshot?" test below still works. Dropped in clearSnapshots().
            if (const auto fb = g_pHyprRenderer->makeSnapshotFB(w))
                m_snapFB[w.get()] = fb;
            m_captureWin.reset();

            ws->m_visible        = wsVis;
            ws->m_forceRendering = wsForce;
            // Warp back to the captured value, then re-aim at the original goal so an
            // in-flight slide resumes to its true destination (operator= no-ops if settled).
            ws->m_renderOffset->setValueAndWarp(wsOffVal);
            *ws->m_renderOffset = wsOffGoal;
            ws->m_alpha->setValueAndWarp(wsAlphaVal);
            *ws->m_alpha = wsAlphaGoal;
            return true;
        }
        return false;
    };

    // Snapshot only when SETTLED (value≈goal), else the buffer/box mismatch stretches or
    // blacks the tile and a hidden workspace never repaints to heal. When unsettled, KEEP
    // the last good snapshot (FB persists) and reuse its recorded geometry. Returns the
    // monitor-local FB-content rect, w<=0 if none.
    const auto captureOrKeep = [&](const PHLWINDOW& w) -> LRect {
        if (!w)
            return LRect{0, 0, 0, 0};
        void* const    key  = w.get();
        const Vector2D gp   = w->positionAnimation()->goal();
        const Vector2D gs   = w->sizeAnimation()->goal();
        const Vector2D vp   = w->positionAnimation()->value();
        const Vector2D vs   = w->sizeAnimation()->value();
        const bool settled  = roughly(vp.x, gp.x, 2) && roughly(vp.y, gp.y, 2) && roughly(vs.x, gs.x, 2) && roughly(vs.y, gs.y, 2);
        const auto fbIt     = m_snapFB.find(key);
        const bool haveFB   = fbIt != m_snapFB.end() && fbIt->second && fbIt->second->isAllocated();
        // renderWindow scales the surface DOWN to fit the box but never UP, so content spans
        // min(box, buffer): when the buffer lags the box (retiled/grown, or any window on a
        // HIDDEN workspace — no frame callbacks) the margin stays transparent. Crop to the
        // CONTENT rect, not the box, else the crop samples empty margin → dark card backing.
        Vector2D content = vs;
        if (const auto s = w->wlSurface()) {
            const auto bs = s->getViewporterCorrectedSize();
            if (bs.x > 0 && bs.y > 0)
                content = Vector2D{std::min(vs.x, bs.x), std::min(vs.y, bs.y)};
        }
        const auto record = [&]() -> LRect {
            const LRect r{vp.x - m->m_position.x, vp.y - m->m_position.y, std::max(1.0, content.x), std::max(1.0, content.y)};
            m_snapGeom[key] = r;
            return r;
        };

        // Always RE-snapshot a settled window, never trust a kept FB: the window's geometry
        // may have moved under the recorded crop rect since (a switch/fade re-render at a
        // different box), desyncing rect from content → blank/dark thumbs. A fresh snap
        // re-renders and re-records the rect together, so they can't disagree.
        if (settled && snap(w))
            return record();
        // Unsettled (mid open/close/reflow): a snap would freeze a half-resized buffer.
        // Reuse the last good FB+rect; the recapture timer re-snaps once settled.
        const auto prev = m_snapGeom.find(key);
        if (prev != m_snapGeom.end() && haveFB)
            return prev->second;
        // First sighting and not yet settled: best-effort snap so it isn't blank.
        if (snap(w))
            return record();
        return LRect{0, 0, 0, 0};
    };

    // The snapshot maps are keyed by raw window pointer and outlive a close on purpose, but
    // now that WE own the framebuffers a dead window's entry pins GPU memory forever. Drop
    // every key that no longer names a live window before adding this pass's.
    {
        std::unordered_set<void*> live;
        for (const auto& w : Desktop::windowState()->windows())
            live.insert(w.get());
        std::erase_if(m_snapFB, [&](const auto& e) { return !live.contains(e.first); });
        std::erase_if(m_snapGeom, [&](const auto& e) { return !live.contains(e.first); });
    }

    m_capturing = true; // let hidden tile windows render into their snapshots
    g_pHyprOpenGL->makeEGLCurrent();
    for (auto& t : m_tiles) {
        const LRect src = captureOrKeep(t.win.lock());
        if (src.w > 0) {
            t.captured   = true;
            t.snapSource = src; // monitor-local logical, matches the FB content
        }
    }
    // Strip cards render LIVE surfaces (no snapshots) — just the current tiled slot from the
    // window's goal position, refreshed each pass to track reflows.
    for (auto& it : m_strip)
        for (auto& sw : it.wins) {
            const auto w = sw.win.lock();
            if (!w)
                continue;
            const auto gp = w->positionAnimation()->goal();
            const auto gs = w->sizeAnimation()->goal();
            sw.rel = LRect{(gp.x - m->m_position.x) / m->m_size.x, (gp.y - m->m_position.y) / m->m_size.y,
                           std::max(0.001, gs.x / m->m_size.x), std::max(0.001, gs.y / m->m_size.y)};
        }
    m_capturing = false;
}

void Overview::scheduleRecapture() {
    if (!g_pEventLoopManager)
        return;

    // captureSnapshots drives a render pass; calling it inside a render stage reenters the
    // renderer and crashes. A timer fires BETWEEN frames (safe), and the delay lets reflowed
    // client buffers commit before we snapshot.
    m_recaptureLeft = 10; // ~600ms, enough to outlast the window move/resize animation
    const auto fire = [this](SP<CEventLoopTimer> self, void*) {
        if (!m_active) {
            m_recaptureLeft = 0;
            return;
        }
        captureSnapshots();
        damage();
        if (--m_recaptureLeft > 0)
            self->updateTimeout(std::chrono::milliseconds(60));
    };

    if (!m_recaptureTimer) {
        m_recaptureTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(60), fire, nullptr);
        g_pEventLoopManager->addTimer(m_recaptureTimer);
    } else
        m_recaptureTimer->updateTimeout(std::chrono::milliseconds(60));
}

// ---- animation --------------------------------------------------------------

double Overview::eased() const {
    return easeOutCubic(m_progress);
}

double Overview::tileBaseProgress() const {
    // During a drop reflow the tiles glide on their own timer while the chrome
    // (m_progress) stays pinned at 1; everywhere else they ride m_progress.
    if (m_reflowing) {
        const double dur = std::max(1, cfgInt("plugin:gloview:duration", 360));
        return std::clamp(nowMs(m_reflowStart) / dur, 0.0, 1.0);
    }
    return m_progress;
}

double Overview::tileProgress(int i) const {
    const double base = tileBaseProgress();
    const int    n    = static_cast<int>(m_tiles.size());
    if (n <= 1)
        return base;
    const double spread = std::min(0.35, 0.05 * n); // total cascade window
    const double start  = spread * (static_cast<double>(i) / (n - 1));
    const double span   = std::max(0.001, 1.0 - spread);
    return std::clamp((base - start) / span, 0.0, 1.0);
}

LRect Overview::currentBox(const Tile& t, int i) const {
    const double   e  = easeOutBack(tileProgress(i));
    const auto&    a  = t.natural;
    const auto&    b  = t.target;
    // The workspace-switch slide rides ON TOP of the normal glide, and lives here rather than
    // in the renderers so hit-testing, rings and keyboard nav all see the same boxes.
    const Vector2D sl = wsSlideOffset(false);
    return LRect{lerp(a.x, b.x, e) + sl.x, lerp(a.y, b.y, e) + sl.y, lerp(a.w, b.w, e), lerp(a.h, b.h, e)};
}

// ---- workspace-switch slide -------------------------------------------------

// Freeze the tiles of the workspace we're leaving so they can slide out while the incoming
// set slides in. Called BEFORE buildTiles() replaces m_tiles. `dir` > 0 means the new
// workspace sits later on the strip, so it enters from the far edge.
void Overview::beginWsSlide(int dir) {
    // Expo shows every window regardless of the displayed workspace, so a "switch" changes
    // nothing on screen — sliding would just duplicate every tile against itself.
    if (cfgInt("plugin:gloview:switch_animation", 1) == 0 || showAllWorkspaces()) {
        endWsSlide();
        return;
    }

    std::vector<Tile> outgoing;
    outgoing.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        Tile t = m_tiles[i];
        // Store the CONTENT box (already fitted to the window's aspect) and pin
        // natural == target: from here the tile is static and only the slide moves it.
        // currentBox() still carries any in-flight slide offset, which is exactly where the
        // tile is on screen — so switching again mid-slide picks up from where it looks.
        t.natural = t.target = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        outgoing.push_back(std::move(t));
    }

    m_prevTiles    = std::move(outgoing);
    m_wsSliding    = true;
    m_wsSlideDir   = dir >= 0 ? 1 : -1;
    m_wsSlideStart = std::chrono::steady_clock::now();
}

void Overview::endWsSlide() {
    m_wsSliding = false;
    m_prevTiles.clear();
}

double Overview::wsSlideRaw() const {
    if (!m_wsSliding)
        return 1.0;
    const double dur = std::max(1, cfgInt("plugin:gloview:switch_duration", 260));
    return std::clamp(nowMs(m_wsSlideStart) / dur, 0.0, 1.0);
}

// Incoming tiles start one screen out and ease to 0; the outgoing set starts at 0 and leaves
// through the opposite edge. The axis follows the strip: a top/bottom strip lays workspaces
// out left-to-right, a left/right strip stacks them, so the slide matches the card order.
Vector2D Overview::wsSlideOffset(bool outgoing) const {
    if (!m_wsSliding)
        return Vector2D{};
    const auto m = m_monitor.lock();
    if (!m)
        return Vector2D{};
    const double e    = easeOutCubic(wsSlideRaw());
    const double span = stripHorizontal() ? m->m_size.x : m->m_size.y;
    const double d    = span * m_wsSlideDir * (outgoing ? -e : (1.0 - e));
    return stripHorizontal() ? Vector2D{d, 0.0} : Vector2D{0.0, d};
}

// ---- move-to-workspace flight ------------------------------------------------

// After a drop the window is already on its new workspace, so its tile would simply vanish.
// Keep drawing it for one animation, flying from where it was released into the destination
// card, so the move reads as a move.
void Overview::beginFly(const PHLWINDOW& w, const LRect& from, const PHLWORKSPACE& ws, const LRect& card) {
    m_flying.clear();
    if (!w || !ws || cfgInt("plugin:gloview:move_animation", 1) == 0 || from.w <= 0.0 || from.h <= 0.0)
        return;
    // Expo keeps the window on screen after the move — it just reflows into a new slot — so a
    // flight would draw a second copy of a tile that never left.
    if (showAllWorkspaces())
        return;
    FlyWin f;
    f.win  = w;
    f.from = from;
    f.to   = card;
    f.ws   = ws;
    m_flying.push_back(std::move(f));
    m_flyStart = std::chrono::steady_clock::now();
}

double Overview::flyRaw() const {
    if (m_flying.empty())
        return 1.0;
    const double dur = std::max(1, cfgInt("plugin:gloview:move_duration", 240));
    return std::clamp(nowMs(m_flyStart) / dur, 0.0, 1.0);
}

// Destination: the window's OWN slot inside its new card — not the card as a whole. Aiming at
// the card meant the preview landed centred and oversized, then popped into its real slot the
// moment the flight ended. Re-resolved every frame, through the same box the card chrome uses,
// so scroll / a "+" drop shifting the strip / the new card's pop-in scale all stay matched.
LRect Overview::flyTarget(const FlyWin& f) const {
    const auto ws = f.ws.lock();
    if (!ws)
        return f.to;
    const Vector2D slide  = stripSlide(eased());
    const Vector2D scroll = stripScroll();
    const auto     w      = f.win.lock();

    for (size_t i = 0; i < m_strip.size(); ++i) {
        if (m_strip[i].ws.lock() != ws)
            continue;
        const LRect card = stripCardBox(i, slide, scroll);
        for (const auto& sw : m_strip[i].wins) {
            if (sw.win.lock() != w)
                continue;
            return LRect{card.x + sw.rel.x * card.w, card.y + sw.rel.y * card.h, std::max(2.0, sw.rel.w * card.w), std::max(2.0, sw.rel.h * card.h)};
        }
        // card is there but the window hasn't been tiled into it yet — aim at the card
        return fitInside(card, f.from.h > 0.0 ? f.from.w / f.from.h : 1.0);
    }
    return f.to;
}

LRect Overview::flyBox(const FlyWin& f) const {
    const double e  = easeInOutCubic(flyRaw());
    const LRect  to = flyTarget(f);
    return LRect{lerp(f.from.x, to.x, e), lerp(f.from.y, to.y, e), lerp(f.from.w, to.w, e), lerp(f.from.h, to.h, e)};
}

// Corner radius along the way: a preview's radius on a slot the size of a card's window is
// enormous, so ease it towards the strip's own radius and clamp it to the shrinking box —
// landing on exactly what renderStripWindows will draw a frame later.
double Overview::flyRound(const LRect& box) const {
    const double e = easeInOutCubic(flyRaw());
    const double r = lerp(cfgInt("plugin:gloview:preview_round", 12), cfgInt("plugin:gloview:strip_card_round", 10), e);
    return std::min(r, std::min(box.w, box.h) * 0.35);
}

// True while this window's flight is still in the air. Its destination card must not draw it
// yet: the card preview appearing at once turned the flight into a copy landing on top of an
// original that was already sitting there. Suppressed, the flight IS the window arriving, and
// the card takes over on the frame it ends.
bool Overview::isFlying(const PHLWINDOW& w) const {
    for (const auto& f : m_flying)
        if (f.win.lock() == w)
            return true;
    return false;
}

void Overview::updateAnimation() {
    const double dur = std::max(1, cfgInt("plugin:gloview:duration", 360));
    const double t   = std::clamp(nowMs(m_animStart) / dur, 0.0, 1.0);
    m_progress       = m_opening ? t : 1.0 - t;
    if (m_reflowing && nowMs(m_reflowStart) >= dur)
        m_reflowing = false;
    if (m_wsSliding && wsSlideRaw() >= 1.0)
        endWsSlide();
    if (!m_flying.empty() && flyRaw() >= 1.0)
        m_flying.clear();
    if (m_newCardAnim && nowMs(m_newCardStart) >= std::max(120, cfgInt("plugin:gloview:duration", 360))) {
        m_newCardAnim = false;
        m_newCardId   = 0;
    }
    // Close complete: DON'T deactivate here — flipping m_active off mid-frame would make
    // renderStage skip this frame's overlay → one transparent frame (real windows already
    // suppressed). Pin progress to 0, let renderStage draw the final opaque-preview frame,
    // then deactivate once the pass is built (m_pendingDeactivate).
    if (!m_opening && t >= 1.0) {
        m_progress          = 0.0;
        m_pendingDeactivate = true;
    }
}

// ---- render -----------------------------------------------------------------

void Overview::renderStage(eRenderStage stage) {
    if (!m_active)
        return;
    // captureSnapshots drives a nested render pass that re-emits render-stage events; without
    // this guard we'd re-add the overlay pass mid-snapshot → reentrant render → SEGV.
    if (m_capturing)
        return;
    const auto rm = g_pHyprRenderer->m_renderData.pMonitor.lock();
    const auto m  = m_monitor.lock();
    // RENDER_LAST_MOMENT is after the top/overlay layers (bars), so the overview paints
    // over them instead of the bar bleeding on top.
    if (!rm || !m || rm != m)
        return;

    if (stage != RENDER_LAST_MOMENT)
        return;

    const auto previewFilter = cfgStr("plugin:gloview:preview_filter", "box4");
    m_previewFilterGrid = previewFilter == "box16" ? 4 : previewFilter == "box4" ? 2 : 0;

    updateAnimation();
    if (!m_active)
        return;

    updateHover(); // keep hover fresh even when the pointer is warped, not moved
    syncTiles(); // window opened/closed/moved on this workspace → reflow the grid

    // exit_on_switch: live workspace changed underneath us. switchToWorkspace only moves the
    // DISPLAYED workspace, so this fires only on genuine external switches.
    if (m_opening && cfgInt("plugin:gloview:exit_on_switch", 0) != 0 && m->m_activeWorkspace != m_liveWsAtOpen.lock()) {
        m_workspace = m->m_activeWorkspace; // accept the external switch so deactivate() doesn't revert it
        close();
    }

    // Layer order: backdrop + main tile chrome → main surfaces → strip chrome → strip
    // surfaces → flying-tile chrome + surface → drag chrome → drag surface → cursor. The
    // immediate-mode chrome is split across COverlayPass phases so the queued surfaces slot
    // between them.
    auto& pass = g_pHyprRenderer->m_renderPass;
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Back));
    renderPrevWindows(); // outgoing workspace's surfaces, under the incoming set
    renderMainWindows();
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::MainFront));
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Mid));
    renderStripWindows();
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::StripFront));
    // a window flying into its new workspace card passes OVER the strip, so it lands on top
    if (!m_flying.empty()) {
        pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::FlyBack));
        renderFlyWindow();
    }
    const bool dragging = m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size());
    if (dragging) {
        pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::DragBack));
        renderDragWindow();
    }
    pass.add(makeUnique<COverlayPass>(this, COverlayPass::Phase::Front));

    renderAboveLayers(); // opted-in layer surfaces (e.g. the live-input HUD) sit on top of the overview

    // Final close frame: the overlay (opaque previews at natural positions) is now queued,
    // covering the windows shouldRenderWindow suppressed earlier this frame. Flip off NOW,
    // after the pass is built, so the NEXT frame renders the real windows — pixel-perfect
    // handoff, no transparent gap. The queued surfaces already captured their data; the
    // deferred chrome callbacks no-op at progress 0. deactivate() damages the next frame.
    if (m_pendingDeactivate) {
        m_pendingDeactivate = false;
        deactivate();
        return;
    }

    // Repaint every frame while up. Event-driven redraws let transient artifacts (partial
    // damage-region blur edges, snapshot mid-reflow, leftover hover rings) persist until the
    // next event; continuous repaint clears any such residue on the following frame.
    damage();
}

void Overview::renderBackdrop() const {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const double e   = eased();
    const auto   col = argb(cfgColor("plugin:gloview:backdrop_color", 0x73070a10), e);
    if (col.a <= 0.0)
        return;
    const double s = m->m_scale;
    g_pHyprOpenGL->renderRect(pxb(CBox(0, 0, m->m_size.x, m->m_size.y), s), col, {.blur = blurEnabled(), .blurA = static_cast<float>(e) * blurStrength()});
}

void Overview::renderStrip() const {
    const auto m = m_monitor.lock();
    if (!m || m_strip.empty())
        return;
    const double e = eased();
    if (e <= 0.01)
        return;
    const double s = m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords

    // translucent band behind the cards (kept faint per request)
    const auto     bandCol = argb(cfgColor("plugin:gloview:strip_band_color", 0x24ffffff), e);
    const bool     blur    = blurEnabled();
    const LRect    bandR   = stripBand();
    const Vector2D slide   = stripSlide(e);  // slide the whole strip in from its edge
    const Vector2D scroll  = stripScroll();  // scroll the card group along the band
    g_pHyprOpenGL->renderRect(pxb(CBox(bandR.x + slide.x, bandR.y + slide.y, bandR.w, bandR.h), s), bandCol, {.blur = blur, .blurA = static_cast<float>(e) * blurStrength()});

    const int  cardRound  = cfgInt("plugin:gloview:strip_card_round", 10);
    const auto cardBg     = argb(cfgColor("plugin:gloview:strip_card_color", 0x3a0e131c), e);
    const auto activeBg   = argb(cfgColor("plugin:gloview:strip_active_color", 0x4d1c2c44), e);
    const auto plusCol    = argb(cfgColor("plugin:gloview:strip_plus_color", 0xd0eef4ff), e);
    // Expo indicator: when all-workspaces is active, the "All" card (if present) lights up
    // active-style. The live workspace keeps its activeBg fill either way.
    const bool allWs       = showAllWorkspaces();
    const bool allCardShown = cfgInt("plugin:gloview:strip_all_card", 0) != 0;

    for (size_t i = 0; i < m_strip.size(); ++i) {
        const auto&  it     = m_strip[i];
        const LRect  card   = stripCardBox(i, slide, scroll);
        const CBox   c      = box(card);

        // card rings are drawn later, over the live card previews (renderStripRings)
        const bool actLike = it.active || (allWs && allCardShown && it.isAll); // filled + thick ring

        g_pHyprOpenGL->renderRect(pxb(c, s), actLike ? activeBg : cardBg, {.round = pxr(cardRound, s)});

        if (it.isPlus && !it.isNew) {
            // draw a centered plus
            const double t  = std::max(2.0, card.h * 0.04);
            const double L  = std::min(card.w, card.h) * 0.34;
            const double cx = card.cx(), cy = card.cy();
            g_pHyprOpenGL->renderRect(pxb(CBox(cx - L / 2, cy - t / 2, L, t), s), plusCol, {.round = pxr(t / 2, s)});
            g_pHyprOpenGL->renderRect(pxb(CBox(cx - t / 2, cy - L / 2, t, L), s), plusCol, {.round = pxr(t / 2, s)});
        } else if (it.isAll) {
            // 2x2 grid-of-squares glyph = "all windows / every workspace"
            const double pad = std::min(card.w, card.h) * 0.26;
            const double gw  = card.w - 2 * pad, gh = card.h - 2 * pad;
            const double cg  = std::max(2.0, std::min(card.w, card.h) * 0.07); // gap between cells
            const double cw  = std::max(2.0, (gw - cg) / 2.0), ch = std::max(2.0, (gh - cg) / 2.0);
            const double gx  = card.x + pad, gy = card.y + pad;
            for (int r = 0; r < 2; ++r)
                for (int col = 0; col < 2; ++col)
                    g_pHyprOpenGL->renderRect(pxb(CBox(gx + col * (cw + cg), gy + r * (ch + cg), cw, ch), s), plusCol, {.round = pxr(2, s)});
        } else {
            // Opaque backing per window slot: the live surface (queued on top by
            // renderStripWindows) may carry transparency, so without it the translucent
            // card band over the blurred backdrop bleeds through.
            for (const auto& sw : it.wins) {
                const auto w = sw.win.lock();
                if (!w || !w->m_isMapped || w->isHidden())
                    continue;
                if (isFlying(w))
                    continue; // still arriving; renderFlyTile is drawing it
                // inset 1px so the backing stays under the live surface and can't peek as a
                // thin dark edge line (see drawPreviewTile), then CROP to the card. A window
                // whose tiled slot falls partly outside the monitor (floating/offscreen/
                // oversized) has a rel rect outside 0..1, and an uncropped backing paints
                // over the band and its neighbours.
                const LRect wb{card.x + sw.rel.x * card.w + 1.0, card.y + sw.rel.y * card.h + 1.0,
                               std::max(2.0, sw.rel.w * card.w - 2.0), std::max(2.0, sw.rel.h * card.h - 2.0)};
                const LRect vis = intersectRect(wb, card);
                if (vis.w <= 0.0 || vis.h <= 0.0)
                    continue;
                // a cropped edge must stay square — rounding it would leave the corner arc
                // floating in the middle of the card, where nothing covers it.
                const bool cropped = !roughly(vis.w, wb.w, 0.01) || !roughly(vis.h, wb.h, 0.01);
                g_pHyprOpenGL->renderRect(pxb(vis, s), argb(0xff14181f, e), {.round = cropped ? 0 : pxr(4, s)});
            }
        }

        // workspace label, centered above the card
        if (showWorkspaceLabels() && it.label && it.label->m_size.x > 0) {
            double       lw    = it.label->m_size.x;
            double       lh    = it.label->m_size.y;
            const double maxLw = card.w + 24.0;
            if (lw > maxLw) {
                const double s = maxLw / lw;
                lw *= s;
                lh *= s;
            }
            // the same band buildStrip reserved above every card (both layouts).
            const double labelBand = stripLabelH();
            const double lx        = card.cx() - lw / 2.0;
            const double ly        = card.y - labelBand + (labelBand - lh) / 2.0;
            const float  la        = it.active ? static_cast<float>(e) : static_cast<float>(e) * 0.75F;
            g_pHyprOpenGL->renderTexture(it.label, pxb(CBox(lx, ly, lw, lh), s), {.a = la});
        }
    }
}

// Queues the LIVE surfaces for every strip card, layered above the BACK chrome (card
// backings) but under the FRONT chrome (drag tile, cursor).
void Overview::renderStripWindows() const {
    const auto m = m_monitor.lock();
    if (!m || m_strip.empty())
        return;
    const double e = eased();
    if (e <= 0.01)
        return;
    // mirror renderStrip()'s slide-in + scroll so the previews travel with their cards
    const Vector2D slide  = stripSlide(e);
    const Vector2D scroll = stripScroll();
    const double   scale  = m->m_scale;
    const auto     when   = Time::steadyNow();

    for (const auto& it : m_strip) {
        if (it.isPlus || it.isAll) // neither carries window previews
            continue;
        LRect card = it.card;
        card.x += slide.x + scroll.x;
        card.y += slide.y + scroll.y;
        for (const auto& sw : it.wins) {
            const auto w = sw.win.lock();
            if (!w || !w->m_isMapped || w->isHidden())
                continue;
            if (isFlying(w))
                continue; // still arriving; renderFlyWindow is drawing it
            // window slot inside the card, from its tiled goal position (logical)
            const LRect slot{card.x + sw.rel.x * card.w, card.y + sw.rel.y * card.h, std::max(2.0, sw.rel.w * card.w),
                             std::max(2.0, sw.rel.h * card.h)};
            // renderWindowLive works in monitor PIXEL coords; the card chrome (renderStrip) is
            // pre-scaled to pixels too (pxb), so surface and backing coincide at any monitor scale.
            const CBox slotPx(slot.x * scale, slot.y * scale, slot.w * scale, slot.h * scale);
            const CBox cardPx(card.x * scale, card.y * scale, card.w * scale, card.h * scale);
            // CROP to the card, not just to the window's own slot: a window whose tiled slot
            // pokes outside the monitor maps to a rel rect outside 0..1, and clipping to the
            // slot alone let the preview spill over the band and the neighbouring cards.
            const CBox clipPx = intersectBox(slotPx, cardPx);
            if (clipPx.w <= 0.0 || clipPx.h <= 0.0)
                continue; // entirely outside the card
            // Round the surface only while it is fully inside the card — on a cropped edge the
            // corner arc would sit mid-card with nothing behind it.
            const bool   cropped    = !roughly(clipPx.w, slotPx.w, 0.01) || !roughly(clipPx.h, slotPx.h, 0.01);
            const double stripRound = cropped ? 0.0 :
                                                std::min(static_cast<double>(cfgInt("plugin:gloview:strip_card_round", 10)),
                                                         std::min(slot.w, slot.h) * 0.35);
            renderWindowLive(w, m, slotPx, clipPx, static_cast<float>(e), when, m_previewFilterGrid, stripRound);
        }
    }
}

// One card's on-screen box: base slot + strip slide-in + card-group scroll + the add-workspace
// pop-in. Shared by the card chrome and its ring so they can't drift apart.
LRect Overview::stripCardBox(size_t i, const Vector2D& slide, const Vector2D& scroll) const {
    const auto& it   = m_strip[i];
    LRect       card = it.card;
    card.x += slide.x + scroll.x;
    card.y += slide.y + scroll.y;
    if (m_newCardAnim && it.id == m_newCardId && !it.isPlus && !it.isAll) {
        const double f  = newCardScale(); // pop-in: scale up from the card center
        const double cx = card.cx(), cy = card.cy();
        card = LRect{cx - card.w * f / 2.0, cy - card.h * f / 2.0, card.w * f, card.h * f};
    }
    return card;
}

// Active / hover / expo card rings, drawn OVER the cards' live window previews.
void Overview::renderStripRings() const {
    const auto m = m_monitor.lock();
    if (!m || m_strip.empty())
        return;
    const double e = eased();
    if (e <= 0.01)
        return;
    const double   s          = m->m_scale;
    const Vector2D slide      = stripSlide(e);
    const Vector2D scroll     = stripScroll();
    const int      cardRound  = cfgInt("plugin:gloview:strip_card_round", 10);
    const auto     activeLine = argb(cfgColor("plugin:gloview:strip_active_border", 0xf0ffffff), e);
    const auto     hoverLine  = argb(cfgColor("plugin:gloview:strip_hover_border", 0x80ffffff), e);
    const double   activeSize = std::max(1, cfgInt("plugin:gloview:strip_active_border_size", 2));
    const double   hoverSize  = std::max(1, cfgInt("plugin:gloview:strip_hover_border_size", 2));
    const bool     allWs        = showAllWorkspaces();
    const bool     allCardShown = cfgInt("plugin:gloview:strip_all_card", 0) != 0;

    for (size_t i = 0; i < m_strip.size(); ++i) {
        const auto& it       = m_strip[i];
        const bool  hover    = static_cast<int>(i) == m_hoveredStrip;
        const bool  actLike  = it.active || (allWs && allCardShown && it.isAll);
        const bool  expoRing = allWs && !allCardShown && !it.isPlus; // outline-all fallback
        const bool  ring     = actLike || expoRing;
        if (!ring && !hover)
            continue;
        const auto&  lc = ring ? activeLine : hoverLine;
        const double t  = ring ? activeSize : hoverSize;
        renderRing(pxb(stripCardBox(i, slide, scroll), s), lc, cardRound * s, t);
    }
}

// Top-right "✕" hit/draw rect for a tile's content box (desktop mode). One formula so the
// drawn button and the click target can't disagree.
LRect Overview::closeButtonRect(const LRect& lb) const {
    const double r = std::clamp(std::min(lb.w, lb.h) * 0.11, 9.0, 18.0);
    const double cx = lb.x + lb.w - r - 6.0;
    const double cy = lb.y + r + 6.0;
    return LRect{cx - r, cy - r, 2 * r, 2 * r};
}

void Overview::drawPreviewTile(size_t i, const LRect& slot, bool lift) const {
    const auto m = m_monitor.lock();
    if (!m || i >= m_tiles.size())
        return;
    const double s         = m->m_scale; // logical→pixel; Hyprland's renderRect wants pixel coords
    const double e         = eased();
    const int    round     = cfgInt("plugin:gloview:preview_round", 12);
    const auto   shadowCol = argb(cfgColor("plugin:gloview:shadow_color", 0x70000000), 1.0);

    const auto& t = m_tiles[i];
    const auto  w = t.win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
        return;

    // Tile box fitted to the window's real aspect; live surface fills it at uniform scale so
    // backing/border/content share one rect and can't stretch.
    const LRect lb = tileContentBox(i, slot);

    // soft drop shadow. renderRoundedShadow is a real gaussian; a blurred dark rect samples
    // the backdrop blur and reads as a murky halo.
    const double range = lift ? 30.0 : 16.0;
    const double dy    = lift ? 14.0 : 6.0;
    g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + dy, lb.w, lb.h}, s), pxr(round, s), 2.F, static_cast<int>(range * s), shadowCol, e * 0.9);

    const bool framed   = (static_cast<int>(i) == m_hovered || lift);
    const bool selected = (static_cast<int>(i) == m_selected) && !lift; // keyboard-nav cursor

    // the hover/select ring is drawn later, on top of the live surface (drawPreviewRing)

    // opaque backing under the live surface (transparent clients would leak the blurred
    // backdrop). INSET 1px: backing is a logical rect (rounded OUTWARD), surface is clipped in
    // pixel space, so on a fractional edge the backing is ~1px wider and peeks as a dark seam;
    // the inset keeps it under the over-covered surface.
    const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0), std::max(0.0, lb.h - 2.0)};
    g_pHyprOpenGL->renderRect(pxb(bb, s), argb(cfgColor("plugin:gloview:preview_bg", 0xff14181f), 1.0), {.round = pxr(round, s)});

    // desktop (canvas) mode: a "✕" close button in the top-right of every preview.
    if (m_desktopMode && !lift) {
        const LRect br = closeButtonRect(lb);
        g_pHyprOpenGL->renderRect(pxb(br, s), argb(cfgColor("plugin:gloview:close_button_color", 0xe6e23b3b), e), {.round = pxr(br.h / 2.0, s)});
        if (m_closeGlyph && m_closeGlyph->m_size.x > 0) {
            const double gw = m_closeGlyph->m_size.x, gh = m_closeGlyph->m_size.y;
            const double gs = std::min((br.w * 0.62) / std::max(1.0, gw), (br.h * 0.62) / std::max(1.0, gh));
            const double dw = gw * gs, dh = gh * gs;
            g_pHyprOpenGL->renderTexture(m_closeGlyph, pxb(CBox(br.x + (br.w - dw) / 2.0, br.y + (br.h - dh) / 2.0, dw, dh), s), {.a = static_cast<float>(e)});
        }
    }

    // window title in a dark pill below the tile (on hover or keyboard selection)
    if ((framed || selected) && !lift && t.label && t.label->m_size.x > 0) {
        const double lw   = t.label->m_size.x;
        const double lh   = t.label->m_size.y;
        const double padX = 14.0, padY = 6.0;
        const double pw   = lw + 2 * padX;
        const double ph   = lh + 2 * padY;
        const double px   = std::clamp(lb.cx() - pw / 2.0, 6.0, m->m_size.x - pw - 6.0);
        const double py   = std::min(lb.y + lb.h + 10.0, m->m_size.y - ph - 6.0);
        g_pHyprOpenGL->renderRect(pxb(CBox(px, py, pw, ph), s), argb(0xcc11151c, e), {.round = pxr(ph / 2.0, s)});
        g_pHyprOpenGL->renderTexture(t.label, pxb(CBox(px + padX, py + padY, lw, lh), s), {.a = static_cast<float>(e)});
    }
}

// Hover / keyboard-selection ring for one tile, drawn OVER its live surface. Hover wins over the
// coincident selection ring.
void Overview::drawPreviewRing(size_t i, const LRect& slot, bool lift) const {
    const auto m = m_monitor.lock();
    if (!m || i >= m_tiles.size())
        return;
    const auto w = m_tiles[i].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
        return;

    const bool framed   = (static_cast<int>(i) == m_hovered || lift);
    const bool selected = (static_cast<int>(i) == m_selected) && !lift;
    if (!framed && !selected)
        return;

    const double s     = m->m_scale;
    const double e     = eased();
    const double round = cfgInt("plugin:gloview:preview_round", 12);
    const double th    = framed ? std::max(1, cfgInt("plugin:gloview:hover_border_size", 3)) : std::max(1, cfgInt("plugin:gloview:select_border_size", 3));
    const auto   col   = framed ? argb(cfgColor("plugin:gloview:hover_border", 0xf0ffffff), e) : argb(cfgColor("plugin:gloview:select_border", 0xf066ccff), e);

    renderRing(pxb(tileContentBox(i, slot), s), col, round * s, th);
}

void Overview::renderPreviewRings() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue;
        drawPreviewRing(i, currentBox(m_tiles[i], static_cast<int>(i)), false);
    }
}

void Overview::renderDragRing() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    if (dragIdx < 0)
        return;
    drawPreviewRing(static_cast<size_t>(dragIdx), dragBox(), true);
}

// Fit `slot` to the window's real aspect so the live surface fills it exactly (uniform scale).
// Used by both tile chrome and the queued surface so they coincide.
LRect Overview::tileContentBox(size_t i, const LRect& slot) const {
    // Desktop (canvas) mode: slot already carries the window's aspect (m_canvasPos froze
    // survivors). Use it AS-IS, not the live aspect, so a survivor Hyprland re-tiled to a new
    // shape keeps its frozen preview shape instead of reshaping inside the frozen slot.
    if (m_desktopMode)
        return slot;
    double aspect = slot.w / std::max(1.0, slot.h);
    if (i < m_tiles.size()) {
        if (const auto w = m_tiles[i].win.lock()) {
            const auto s = w->sizeAnimation()->goal();
            if (s.x > 0 && s.y > 0)
                aspect = s.x / s.y;
        }
    }
    return fitInside(slot, aspect);
}

LRect Overview::dragBox() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    if (dragIdx < 0)
        return LRect{0, 0, 0, 0};
    // follow the cursor. Grid mode shrinks to half so the tile fits a workspace card;
    // canvas keeps full size (lands where it parks on drop).
    const LRect  base = m_tiles[dragIdx].target;
    const double s    = m_desktopMode ? 1.0 : 0.5;
    const double w    = base.w * s;
    const double h    = base.h * s;
    return LRect{m_dragX - (m_grabDX + (w - base.w) / 2.0), m_dragY - (m_grabDY + (h - base.h) / 2.0), w, h};
}

void Overview::renderPreviews() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue; // the dragged tile floats over the strip; drawn later in renderDragTile()
        drawPreviewTile(i, currentBox(m_tiles[i], static_cast<int>(i)), false);
    }
}

// Queues the LIVE surfaces for the main-area tiles (except the dragged one), above their
// chrome (the Back phase) and under the strip. Mirrors renderStripWindows.
void Overview::renderMainWindows() const {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    // Previews render fully OPAQUE the whole time (only backdrop/strip chrome fade with `e`).
    // At progress 0, currentBox == t.natural == the real window's settled geometry, so the
    // opaque preview overlays it pixel-perfect. Fading with `e` would flicker the close tail:
    // preview alpha hits 0 while the real window is still hidden → desktop shows through.
    const int    dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    const double scale   = m->m_scale;
    const auto   when    = Time::steadyNow();
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == dragIdx)
            continue;
        const auto w = m_tiles[i].win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
        const CBox  px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
        renderWindowLive(w, m, px, px, 1.0F, when, m_previewFilterGrid, static_cast<double>(cfgInt("plugin:gloview:preview_round", 12)));
    }
}

// Chrome for the outgoing workspace's tiles during a switch slide: shadow + opaque backing
// only. No labels, rings or close buttons — the set is leaving and takes no input.
void Overview::renderPrevPreviews() const {
    const auto m = m_monitor.lock();
    if (!m || m_prevTiles.empty())
        return;
    const double   s         = m->m_scale;
    const double   e         = eased();
    const int      round     = cfgInt("plugin:gloview:preview_round", 12);
    const auto     shadowCol = argb(cfgColor("plugin:gloview:shadow_color", 0x70000000), 1.0);
    const auto     bg        = argb(cfgColor("plugin:gloview:preview_bg", 0xff14181f), 1.0);
    const Vector2D off       = wsSlideOffset(true);

    for (const auto& t : m_prevTiles) {
        const auto w = t.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb{t.target.x + off.x, t.target.y + off.y, t.target.w, t.target.h};
        g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + 6.0, lb.w, lb.h}, s), pxr(round, s), 2.F, static_cast<int>(16.0 * s), shadowCol, e * 0.9);
        // same 1px inset as drawPreviewTile: keeps the backing under the over-covered surface
        const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0), std::max(0.0, lb.h - 2.0)};
        g_pHyprOpenGL->renderRect(pxb(bb, s), bg, {.round = pxr(round, s)});
    }
}

void Overview::renderPrevWindows() const {
    const auto m = m_monitor.lock();
    if (!m || m_prevTiles.empty())
        return;
    const double   scale = m->m_scale;
    const auto     when  = Time::steadyNow();
    const Vector2D off   = wsSlideOffset(true);
    const double   round = cfgInt("plugin:gloview:preview_round", 12);

    for (const auto& t : m_prevTiles) {
        const auto w = t.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const CBox px((t.target.x + off.x) * scale, (t.target.y + off.y) * scale, t.target.w * scale, t.target.h * scale);
        renderWindowLive(w, m, px, px, 1.0F, when, m_previewFilterGrid, round);
    }
}

// Chrome for a window flying into the workspace card it was just dropped on. Fades out as it
// lands, by which point the card's own preview has taken over.
void Overview::renderFlyTile() const {
    const auto m = m_monitor.lock();
    if (!m || m_flying.empty())
        return;
    const double s         = m->m_scale;
    // No fade: the tile stays solid the whole way and simply BECOMES the card's preview. It
    // used to fade out as it flew, which — on top of the card already showing the window —
    // read as the drop failing rather than completing.
    const double lift      = 1.0 - easeInOutCubic(flyRaw()); // 1 in the air, 0 on the card
    const auto   shadowCol = argb(cfgColor("plugin:gloview:shadow_color", 0x70000000), 1.0);
    const auto   bg        = argb(cfgColor("plugin:gloview:preview_bg", 0xff14181f), 1.0);

    for (const auto& f : m_flying) {
        const auto w = f.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect  lb    = flyBox(f);
        const double round = flyRound(lb);
        // Shadow tracks the shrinking box and settles out entirely — cards cast none, so a
        // full-size drop shadow on a card-sized tile is what made the landing look pasted on.
        const double range = std::clamp(std::min(lb.w, lb.h) * 0.14, 3.0, 26.0) * lift;
        if (range >= 1.0)
            g_pHyprOpenGL->renderRoundedShadow(pxb(LRect{lb.x, lb.y + range * 0.45, lb.w, lb.h}, s), pxr(round, s), 2.F, static_cast<int>(range * s), shadowCol, lift * 0.9);
        const LRect bb{lb.x + 1.0, lb.y + 1.0, std::max(0.0, lb.w - 2.0), std::max(0.0, lb.h - 2.0)};
        g_pHyprOpenGL->renderRect(pxb(bb, s), bg, {.round = pxr(round, s)});
    }
}

void Overview::renderFlyWindow() const {
    const auto m = m_monitor.lock();
    if (!m || m_flying.empty())
        return;
    const double scale = m->m_scale;
    const auto   when  = Time::steadyNow();

    for (const auto& f : m_flying) {
        const auto w = f.win.lock();
        if (!w || !w->m_isMapped || w->isHidden())
            continue;
        const LRect lb = flyBox(f);
        const CBox  px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
        renderWindowLive(w, m, px, px, 1.0F, when, m_previewFilterGrid, flyRound(lb)); // opaque all the way — see renderFlyTile
    }
}

void Overview::renderDragTile() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    if (dragIdx < 0)
        return;
    drawPreviewTile(static_cast<size_t>(dragIdx), dragBox(), true); // chrome; surface queued in renderDragWindow
}

void Overview::renderDragWindow() const {
    const int dragIdx = (m_dragging && m_pressTile >= 0 && m_pressTile < static_cast<int>(m_tiles.size())) ? m_pressTile : -1;
    if (dragIdx < 0)
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const auto w = m_tiles[dragIdx].win.lock();
    if (!w || !w->m_isMapped || w->isHidden())
        return;
    const double e     = eased();
    const double scale = m->m_scale;
    const LRect  lb    = tileContentBox(static_cast<size_t>(dragIdx), dragBox());
    const CBox   px(lb.x * scale, lb.y * scale, lb.w * scale, lb.h * scale);
    renderWindowLive(w, m, px, px, static_cast<float>(e), Time::steadyNow(), m_previewFilterGrid,
                     static_cast<double>(cfgInt("plugin:gloview:preview_round", 12)));
}

bool Overview::isAboveLayer(const std::string& ns) const {
    if (ns.find("aboveoverview") != std::string::npos)
        return true;
    const std::string list = cfgStr("plugin:gloview:above_namespaces", "");
    size_t            i     = 0;
    while (i < list.size()) {
        // split on commas AND whitespace
        while (i < list.size() && (list[i] == ',' || std::isspace(static_cast<unsigned char>(list[i]))))
            ++i;
        size_t start = i;
        while (i < list.size() && list[i] != ',' && !std::isspace(static_cast<unsigned char>(list[i])))
            ++i;
        if (i == start)
            continue;
        std::string entry = list.substr(start, i - start);
        if (entry.back() == '*') {
            const std::string prefix = entry.substr(0, entry.size() - 1);
            if (ns.compare(0, prefix.size(), prefix) == 0)
                return true;
        } else if (ns == entry) {
            return true;
        }
    }
    return false;
}

// Re-render opted-in layer surfaces ON TOP of the overview: queue real CSurfacePassElements
// after the overview chrome so they composite last, above the backdrop/strip.
// (IHyprRenderer::renderLayer is protected in 0.55.4, hence this immediate-mode queue.)
void Overview::renderAboveLayers() const {
    const auto m = m_monitor.lock();
    if (!m || !g_pHyprRenderer)
        return;
    const auto when = Time::steadyNow();
    for (int idx : {2, 3}) {
        for (const auto& ref : m->m_layerSurfaceLayers[idx]) {
            const auto ls = ref.lock();
            if (!ls || !ls->m_mapped || !ls->wlSurface() || !ls->wlSurface()->resource())
                continue;
            if (!isAboveLayer(ls->m_namespace))
                continue;

            const auto pos  = ls->positionAnimation()->value(); // absolute logical (layout) coords
            const auto size = ls->sizeAnimation()->value();
            if (!(size.x > 0 && size.y > 0))
                continue;

            g_pHyprRenderer->damageBox(CBox{pos.x, pos.y, size.x, size.y}); // force a composite even with no client damage

            CSurfacePassElement::SRenderData data{};
            data.pMonitor      = m;
            data.when          = when;
            data.pos           = pos;
            data.w             = std::max(size.x, 1.0);
            data.h             = std::max(size.y, 1.0);
            data.fadeAlpha     = 1.F;
            data.alpha         = 1.F;
            data.decorate      = false;
            data.rounding      = 0;
            data.blur          = false;
            data.surfaceCounter = 0;

            const auto root = ls->wlSurface()->resource();
            root->breadthfirst(
                [&data, &root](SP<CWLSurfaceResource> s, const Vector2D& offset, void*) {
                    if (!s || !s->m_current.texture || s->m_current.size.x < 1 || s->m_current.size.y < 1)
                        return;
                    data.localPos    = offset;
                    data.texture     = s->m_current.texture;
                    data.surface     = s;
                    data.mainSurface = s == root;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CSurfacePassElement>(data));
                    data.surfaceCounter++;
                },
                nullptr);
        }
    }
}

void Overview::renderCursorOnTop() const {
    // Hyprland composites the software cursor before our RENDER_LAST_MOMENT pass, so the
    // backdrop paints over it. Redraw on top so the pointer stays visible while up.
    const auto m = m_monitor.lock();
    if (!m || !Pointer::mgr() || !g_pHyprOpenGL)
        return;
    const auto tex = Pointer::mgr()->getCurrentCursorTexture();
    if (!tex)
        return;
    const CBox g  = Pointer::mgr()->getCursorBoxGlobal();
    const CBox lb(g.x - m->m_position.x, g.y - m->m_position.y, g.w, g.h);
    g_pHyprOpenGL->renderTexture(tex, pxb(lb, m->m_scale), {.a = 1.0F});
}

// ---- input ------------------------------------------------------------------

bool Overview::onMouseAxis(const IPointer::SAxisEvent& e) {
    if (!m_active)
        return false;
    // deltaDiscrete is in notches (±1); fall back to the continuous delta for high-res/touchpad.
    const double notches = e.deltaDiscrete != 0 ? static_cast<double>(e.deltaDiscrete) : e.delta / 15.0;

    // Wheel over the STRIP band scrolls the card group; over the MAIN area it steps the
    // displayed workspace (when enabled).
    bool       overStrip = false;
    if (const auto m = m_monitor.lock()) {
        const auto   mc   = g_pInputManager->getMouseCoordsInternal();
        const LRect  band = stripBand();
        const double lx   = mc.x - m->m_position.x;
        const double ly   = mc.y - m->m_position.y;
        overStrip         = (lx >= band.x && ly >= band.y && lx <= band.x + band.w && ly <= band.y + band.h);
    }

    if (!overStrip && cfgInt("plugin:gloview:scroll_switches_workspace", 1) != 0) {
        if (notches != 0.0)
            stepWorkspace(notches > 0 ? 1 : -1);
        return true;
    }

    // Modal: swallow the wheel even with nothing to scroll so it never reaches windows behind.
    if (m_stripScrollMax <= 0.0)
        return true;
    const double step = stripThickness() * 0.9; // ~one card per notch
    m_stripScroll     = std::clamp(m_stripScroll + notches * step, 0.0, m_stripScrollMax);
    updateHover(); // the card under the cursor changed
    damage();
    return true;
}

void Overview::onMouseMove() {
    updateHover();
    // We repaint the cursor ourselves (renderCursorOnTop). updateHover only damages on hover
    // *change*, so moving within one tile would leave the old cursor → trail. Damage every move.
    if (m_active)
        damage();
}

void Overview::updateHover() {
    if (!m_active)
        return;
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const auto   mc = g_pInputManager->getMouseCoordsInternal();
    const double lx = mc.x - m->m_position.x;
    const double ly = mc.y - m->m_position.y;

    // drag tracking: promote a pressed tile to a real drag once the pointer passes a
    // small threshold, then follow the cursor.
    if (m_pressTile >= 0) {
        const double dx = lx - m_pressX;
        const double dy = ly - m_pressY;
        if (!m_dragging && (dx * dx + dy * dy) > 64.0) // ~8px
            m_dragging = true;
        if (m_dragging) {
            m_dragX = lx;
            m_dragY = ly;
            int newStrip = -1; // highlight the workspace card under the cursor
            for (size_t i = 0; i < m_strip.size(); ++i) {
                const LRect c = stripCardAt(i);
                if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h)
                    newStrip = static_cast<int>(i);
            }
            m_hoveredStrip = newStrip;
            m_hovered      = m_pressTile;
            damage();
            return;
        }
    }

    int newTile = -1;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
        if (lx >= b.x && ly >= b.y && lx <= b.x + b.w && ly <= b.y + b.h)
            newTile = static_cast<int>(i);
    }
    int newStrip = -1;
    for (size_t i = 0; i < m_strip.size(); ++i) {
        const LRect c = stripCardAt(i);
        if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h)
            newStrip = static_cast<int>(i);
    }
    if (newTile != m_hovered || newStrip != m_hoveredStrip) {
        m_hovered      = newTile;
        m_hoveredStrip = newStrip;
        // keep the keyboard selection under the pointer so arrow-nav picks up where
        // the mouse left off (macOS-like). Only when actually over a tile.
        if (newTile >= 0 && cfgInt("plugin:gloview:focus_follows_mouse", 1) != 0) {
            m_selected = newTile;
            syncFocus(); // so a passthrough killactive/hotkey hits the hovered window
        }
        damage();
    }
}

namespace {
constexpr int PRESS_NONE  = -1;
constexpr int PRESS_STRIP = -2; // press landed on a strip card (switch happened)
constexpr int PRESS_EMPTY = -3; // press landed on empty space (close on release)
constexpr int PRESS_CONSUMED = -4; // press fully handled (e.g. desktop ✕) — release must do nothing
// BTN_MIDDLE (0x112) comes from linux/input-event-codes.h, pulled in transitively.
} // namespace

bool Overview::onMouseButton(const IPointer::SButtonEvent& e) {
    if (!m_active)
        return false;

    const auto m = m_monitor.lock();
    if (!m) {
        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            close();
        return true;
    }
    const auto   mc = g_pInputManager->getMouseCoordsInternal();
    const double lx = mc.x - m->m_position.x;
    const double ly = mc.y - m->m_position.y;

    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_pressTile = PRESS_NONE;
        m_dragging  = false;

        // middle-click a workspace card → close every window on it (handled fully on
        // press). Per-window close is keyboard-only now (key_close_window, see onKey).
        if (e.button == BTN_MIDDLE) {
            for (size_t i = 0; i < m_strip.size(); ++i) {
                const LRect c = stripCardAt(i);
                if (!m_strip[i].isPlus && !m_strip[i].isAll && lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h) {
                    closeWorkspaceWindows(m_strip[i]);
                    break;
                }
            }
            return true; // swallow; middle is never a switch/drag/dismiss
        }

        // desktop mode: the "✕" button on a preview closes that window
        if (m_desktopMode) {
            for (size_t i = 0; i < m_tiles.size(); ++i) {
                const LRect lb = tileContentBox(i, currentBox(m_tiles[i], static_cast<int>(i)));
                const LRect br = closeButtonRect(lb);
                if (lx >= br.x && ly >= br.y && lx <= br.x + br.w && ly <= br.y + br.h) {
                    m_pressTile = PRESS_CONSUMED; // so the release doesn't treat it as an empty-space click
                    closeTileWindow(static_cast<int>(i));
                    return true;
                }
            }
        }

        // strip card → switch the displayed workspace right away (no drag here); the
        // "+" card creates a new workspace (addWorkspace handles follow + pop-in anim).
        for (size_t i = 0; i < m_strip.size(); ++i) {
            const LRect c = stripCardAt(i);
            if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h) {
                m_pressTile = PRESS_STRIP;
                if (m_strip[i].isAll)
                    toggleAllWorkspaces();
                else if (m_strip[i].isPlus)
                    addWorkspace(m_strip[i].id); // 0 for the plain "+"; the advertised id for the dynamic tail
                else
                    switchToWorkspace(m_strip[i]);
                return true;
            }
        }
        // window tile → arm a drag candidate; click vs drag decided on release
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
            if (lx >= b.x && ly >= b.y && lx <= b.x + b.w && ly <= b.y + b.h) {
                m_pressTile = static_cast<int>(i);
                m_pressX = m_dragX = lx;
                m_pressY = m_dragY = ly;
                m_grabDX = lx - b.x;
                m_grabDY = ly - b.y;
                return true;
            }
        }
        // empty space
        m_pressTile = PRESS_EMPTY;
        return true;
    }

    // ---- release ----
    if (e.button == BTN_MIDDLE) {
        m_pressTile = PRESS_NONE;
        return true; // middle was fully handled on press
    }
    const int press = m_pressTile;
    // The dragged tile's on-screen box, captured while m_pressTile/m_dragging are still set —
    // dragBox() reads both, and a drop onto a card starts its flight from exactly here.
    const LRect draggedBox = (m_dragging && press >= 0 && press < static_cast<int>(m_tiles.size())) ? tileContentBox(static_cast<size_t>(press), dragBox()) : LRect{};
    m_pressTile            = PRESS_NONE;

    if (press == PRESS_STRIP || press == PRESS_CONSUMED)
        return true; // switch / ✕ already handled on press; ignore the release

    if (press >= 0 && press < static_cast<int>(m_tiles.size())) {
        const auto w = m_tiles[press].win.lock();
        if (m_dragging) {
            m_dragging = false;
            // dropped onto a workspace card → move the window there
            for (size_t i = 0; i < m_strip.size(); ++i) {
                const LRect c = stripCardAt(i);
                if (lx >= c.x && ly >= c.y && lx <= c.x + c.w && ly <= c.y + c.h) {
                    dropOnWorkspace(w, m_strip[i], draggedBox);
                    return true;
                }
            }
            // grid mode: dropped onto another preview → swap the two windows' places.
            // (Skipped in desktop/canvas mode, where a drop parks the preview instead.)
            if (!m_desktopMode && w && cfgInt("plugin:gloview:drag_to_swap", 1) != 0) {
                for (size_t i = 0; i < m_tiles.size(); ++i) {
                    if (static_cast<int>(i) == press)
                        continue;
                    const LRect b = currentBox(m_tiles[i], static_cast<int>(i));
                    if (lx >= b.x && ly >= b.y && lx <= b.x + b.w && ly <= b.y + b.h) {
                        swapTiles(press, static_cast<int>(i));
                        return true;
                    }
                }
            }
            // desktop (canvas) mode: a drop just PARKS the preview where released. Real
            // window never floated/moved; m_canvasPos survives per-frame rebuilds.
            if (m_desktopMode && w) {
                const LRect cur = m_tiles[press].target; // keep the canvas size, move the corner
                const LRect parked{lx - m_grabDX, ly - m_grabDY, cur.w, cur.h};
                m_canvasPos[w.get()]        = parked;
                m_tiles[press].target       = parked;
                m_tiles[press].natural      = parked; // settled — currentBox returns it directly
                m_hovered                   = -1;
                damage();
                return true;
            }
            damage(); // dropped in empty space → tile snaps back to its slot
            return true;
        }
        // a plain click → follow that window's workspace, focus it and dismiss
        activateWindow(w, false);
        return true;
    }

    if (cfgInt("plugin:gloview:exit_on_click", 1) != 0)
        close(); // released on empty space
    return true;
}

void Overview::dropOnWorkspace(const PHLWINDOW& w, const StripItem& it, const LRect& fromBox) {
    const auto m = m_monitor.lock();
    if (!w || !m) {
        damage();
        return;
    }

    // the card's on-screen box, for the flight's destination (re-resolved per frame after this)
    const Vector2D sc = stripScroll();
    const LRect    cardBox{it.card.x + sc.x, it.card.y + sc.y, it.card.w, it.card.h};

    PHLWORKSPACE target;
    if (it.isPlus) { // the "+" card, or dynamic_workspaces' trailing empty one
        const int id   = resolveNewId(it.id);
        target         = State::workspaceState()->create(id, m->m_id);
        m_newCardId    = id; // pop the new card in
        m_newCardStart = std::chrono::steady_clock::now();
        m_newCardAnim  = true;
    } else
        target = it.ws.lock();

    if (!target || target == w->m_workspace) {
        damage(); // same workspace: nothing to do, tile snaps back
        return;
    }

    // switch_on_drop follows the window to its new workspace, so it reappears as a full-size
    // main-area tile — flying a second copy of it into the card at the same time reads as two
    // windows. Only animate the move when the window is actually leaving the view.
    const bool follow = cfgInt("plugin:gloview:switch_on_drop", 0) != 0;
    if (!follow)
        beginFly(w, fromBox, target, cardBox);

    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        const auto win = m_tiles[i].win.lock();
        if (win && win != w)
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));
    }

    Desktop::globalWindowController()->moveWindowToWorkspace(w, target);

    // switch_on_drop: follow the window to its new workspace instead of staying put.
    if (follow) {
        StripItem dst;
        dst.ws = target;
        switchToWorkspace(dst);
        return;
    }

    // Window left the displayed workspace; rebuild and glide the remaining tiles into
    // their new slots. replayReflow keeps the chrome settled (m_progress pinned at 1 —
    // no backdrop flash / strip re-slide); tiles render live, nothing to recapture.
    replayReflow(oldBoxes);
    damage();
}

// Swap two previews' windows in the real Hyprland layout AND the overview. Mirrors
// dropOnWorkspace: mutate the real layout, then replayReflow() rebuilds tiles from the
// NEW geometry. Rebuild is load-bearing: a manual slot-swap desyncs the tile slot from
// the window's real goal(), so renderWindowLive maps the surface outside its tile and
// only the dark backing shows ("black"/empty preview).
void Overview::swapTiles(int a, int b) {
    if (a < 0 || b < 0 || a == b || a >= static_cast<int>(m_tiles.size()) || b >= static_cast<int>(m_tiles.size())) {
        damage();
        return;
    }
    const auto wa = m_tiles[a].win.lock();
    const auto wb = m_tiles[b].win.lock();
    // only swap two real windows on the SAME workspace; fullscreen or a cross-workspace
    // pair (expo) has no well-defined tiled slot to trade — else snap the drag back.
    if (!wa || !wb || wa == wb || wa->m_workspace != wb->m_workspace || Fullscreen::controller()->isFullscreen(wa) || Fullscreen::controller()->isFullscreen(wb)) {
        damage();
        return;
    }
    const auto ta = wa->layoutTarget();
    const auto tb = wb->layoutTarget();
    if (!g_layoutManager || !ta || !tb) {
        damage();
        return;
    }

    // capture where every tile sits NOW so the previews glide from their current spots
    // into the post-swap slots (same tail as dropOnWorkspace).
    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));

    // Real swap. switchTargets only swaps the targets' ORDER and does NOT recalculate;
    // recalculate() the space is what actually moves them into each other's slot (and
    // makes the swap persist after close).
    g_layoutManager->switchTargets(ta, tb);
    if (const auto sp = ta->space())
        sp->recalculate();

    // Rebuild the overview from the swapped real geometry and glide the tiles in. Tiles
    // render live, so there is nothing to recapture.
    replayReflow(oldBoxes);
    damage();
}

void Overview::switchToWorkspace(const StripItem& it) {
    const auto m = m_monitor.lock();
    if (!m)
        return;

    const auto cur = m_workspace.lock();

    PHLWORKSPACE ws;
    if (it.isPlus) {
        ws = State::workspaceState()->create(resolveNewId(it.id), m->m_id, "", false);
        if (!ws)
            return;
        // A brand-new empty workspace is reaped within a frame or two unless it is focused,
        // and we only display it (the live switch lands on close) — hold it so its card and
        // its tiles survive. deactivate() releases the hold; the previous held one is let go
        // here, which is what makes dynamic_workspaces' abandoned empties disappear again.
        if (const auto old = m_newWs.lock(); old && old != ws)
            old->setPersistent(false);
        ws->setPersistent(true);
        m_newWs = ws;
    } else if (const auto target = it.ws.lock()) {
        ws = target;
        if (ws == cur)
            return; // already showing it
    } else
        return;

    // Slide direction from strip order — the cards are sorted by id, so the ids decide it.
    // A freshly created workspace always takes the highest id, hence "forward".
    beginWsSlide((!cur || ws->m_id >= cur->m_id) ? 1 : -1);

    // Display the target inside the overview without changing the live desktop yet;
    // captureSnapshots() force-renders inactive workspaces without a real slide.
    m_workspace = ws;

    // Stepping OFF a tail workspace we created and never put anything on: drop the hold so it
    // reaps, rather than leaving a pinned empty around for the rest of the session. Only under
    // dynamic_workspaces, where the tail is ephemeral by design — with the plain "+" card the
    // user asked for that workspace, so it stays until close.
    if (dynamicWorkspaces()) {
        if (const auto held = m_newWs.lock(); held && held != ws && !workspaceOccupied(held)) {
            held->setPersistent(false);
            m_newWs.reset();
        }
    }

    // Rebuild around the displayed workspace and keep the overview visually
    // settled; clicking strip cards should not replay the opening animation.
    m_hovered = m_hoveredStrip = -1;
    buildTiles();
    buildStrip();
    layoutTiles();
    m_progress  = 1.0;
    m_opening   = true;
    m_reflowing = false;
    m_animStart = std::chrono::steady_clock::now() - std::chrono::milliseconds(std::max(1, cfgInt("plugin:gloview:duration", 360)));
    damage();
}

// Push the displayed workspace onto the live desktop. Run at the START of the close
// animation: committing it in deactivate() (i.e. at the END) meant Hyprland only began its
// own workspace-change transition once our overlay had already faded out, so a switch made
// in the overview visibly replayed a beat late. Hyprland's transition is warped to its end
// here as well, so it can neither play under the fade nor after it.
void Overview::commitWorkspace() {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const auto ws = m_workspace.lock();
    if (!ws || ws == m->m_activeWorkspace)
        return;

    const auto prev = m->m_activeWorkspace;
    m->changeWorkspace(ws, false, true, false);
    m_committedFrom = prev; // keep it hidden behind the fade (see the member's comment)

    // Warp value AND re-aim at the goal, never assign the goal — see captureSnapshots() for
    // why writing a workspace's animation goal directly corrupts it for every later open.
    const auto settle = [](const PHLWORKSPACE& w) {
        if (!w)
            return;
        if (w->m_renderOffset)
            w->m_renderOffset->setValueAndWarp(w->m_renderOffset->goal());
        if (w->m_alpha)
            w->m_alpha->setValueAndWarp(w->m_alpha->goal());
    };
    settle(prev);
    settle(ws);
}

// gloview:next / gloview:prev — while the overview is up, same semantics as tab and the
// scroll wheel: move the DISPLAYED workspace, commit to the live desktop on close. While it
// is closed the same binds switch the live desktop, so one key drives workspaces everywhere.
void Overview::nextWorkspace() {
    if (m_active && m_opening)
        stepWorkspace(1);
    else
        stepLiveWorkspace(1);
}

void Overview::prevWorkspace() {
    if (m_active && m_opening)
        stepWorkspace(-1);
    else
        stepLiveWorkspace(-1);
}

// The monitor the user is on: keyboard focus first (these arrive from keybinds), cursor
// position as the fallback — matching what open() picks.
PHLMONITOR Overview::activeMonitor() const {
    if (const auto m = Desktop::focusState()->monitor())
        return m;
    return State::monitorState()->query().vec(Pointer::mgr()->position()).run();
}

// The monitor's workspaces in the order the strip lists them: sorted by id, and under
// dynamic_workspaces narrowed to the populated ones (plus whichever is active, even if it is
// empty) — exactly buildStrip's rule, so stepping in and out of the overview walks the same
// sequence. The create-on-use tail is NOT in here; stepLiveWorkspace appends it.
std::vector<PHLWORKSPACE> Overview::liveWorkspaceList(const PHLMONITOR& m) const {
    std::vector<PHLWORKSPACE> wss;
    if (!m)
        return wss;
    const auto cur      = m->m_activeWorkspace;
    const bool dynamic   = dynamicWorkspaces();
    const bool showEmpty = !dynamic && cfgInt("plugin:gloview:show_empty", 1) != 0;

    for (const auto& wref : State::workspaceState()->workspaces()) {
        const auto ws = wref.lock();
        if (!ws || ws->m_isSpecialWorkspace || ws->m_id <= 0) // scratchpads and named (-1337…) aren't stepped through
            continue;
        if (ws->m_monitor.lock() != m)
            continue;
        if (!showEmpty && ws != cur && !wsHasMappedWindows(ws))
            continue;
        wss.push_back(ws);
    }
    if (cur && cur->m_id > 0 && !cur->m_isSpecialWorkspace && std::none_of(wss.begin(), wss.end(), [&](const PHLWORKSPACE& ws) { return ws == cur; }))
        wss.push_back(cur);

    std::sort(wss.begin(), wss.end(), [](const PHLWORKSPACE& a, const PHLWORKSPACE& b) { return a->m_id < b->m_id; });
    return wss;
}

// Step the LIVE desktop one workspace forward/back (wrapping), used when gloview:next /
// gloview:prev fire with the overview closed. Under dynamic_workspaces the list gains a
// trailing create-on-use slot whenever the last workspace is populated, so walking off the
// end lands you on a fresh empty desktop (gnome / hyprnome); the one you left behind is
// unpinned and empty, so Hyprland reaps it. Returns false when there was nowhere to go.
bool Overview::stepLiveWorkspace(int dir) {
    const auto m = activeMonitor();
    if (!m || !m->m_activeWorkspace)
        return false;
    const auto cur = m->m_activeWorkspace;

    // …plus a trailing create-on-use slot, unless the highest workspace is ALREADY the empty
    // tail (same rule as the strip: never two blank desktops in a row).
    auto       wss  = liveWorkspaceList(m);
    const bool tail = dynamicWorkspaces() && (wss.empty() || wsHasMappedWindows(wss.back()));
    const int  n    = static_cast<int>(wss.size()) + (tail ? 1 : 0);
    if (n <= 1)
        return false;

    int pos = -1;
    for (size_t i = 0; i < wss.size(); ++i)
        if (wss[i] == cur)
            pos = static_cast<int>(i);
    if (pos < 0)
        pos = 0;

    int next = pos + (dir > 0 ? 1 : -1);
    if (next < 0)
        next = n - 1;
    else if (next >= n)
        next = 0;
    if (next == pos)
        return false;

    // Fired during the close animation (m_active, no longer m_opening): deactivate() re-commits
    // m_workspace at the end, so retarget it too or it would snap right back.
    const auto land = [this](const PHLMONITOR& mon, const PHLWORKSPACE& ws) {
        mon->changeWorkspace(ws, false, true, false);
        if (m_active)
            m_workspace = ws;
    };

    if (next < static_cast<int>(wss.size())) {
        land(m, wss[next]);
        return true;
    }

    // …the create-on-use tail: take the id right after the last one, like buildStrip.
    int id = 1;
    for (const auto& ws : wss)
        if (ws->m_id >= id)
            id = ws->m_id + 1;
    while (State::workspaceState()->query().id(id).run())
        ++id;
    const auto fresh = State::workspaceState()->create(id, m->m_id, "", false);
    if (!fresh)
        return false;
    // No persistence pin needed here (unlike the overview's held tail): changeWorkspace makes
    // it the active workspace immediately, which is enough to keep it alive.
    land(m, fresh);
    dbg("stepped onto new workspace " + std::to_string(id));
    return true;
}

// gloview:setworkspace <id>. Prefers the strip card, but falls back to any workspace living
// on this monitor so the dispatcher isn't at the mercy of show_empty / dynamic_workspaces
// hiding the target's card.
bool Overview::setWorkspace(int id) {
    // Closed: switch the live desktop, like gloview:next/prev now do. Only to a workspace that
    // already exists on this monitor — creating one on demand is Hyprland's own `workspace`
    // dispatcher's job.
    if (!(m_active && m_opening)) {
        const auto m = activeMonitor();
        if (!m)
            return false;
        const auto ws = State::workspaceState()->query().id(id).run();
        if (!ws || ws->m_monitor.lock() != m)
            return false;
        if (ws != m->m_activeWorkspace)
            m->changeWorkspace(ws, false, true, false);
        if (m_active) // mid-close: keep deactivate()'s re-commit from undoing this
            m_workspace = ws;
        return true;
    }
    for (const auto& it : m_strip) {
        if (it.isAll || (it.isPlus && !it.isNew))
            continue;
        if (it.id == id) {
            switchToWorkspace(it); // NOTE: rebuilds m_strip — `it` must not be touched after
            return true;
        }
    }
    const auto m = m_monitor.lock();
    if (!m)
        return false;
    const auto ws = State::workspaceState()->query().id(id).run();
    if (!ws || ws->m_monitor.lock() != m)
        return false;
    StripItem it;
    it.ws = ws;
    it.id = id;
    switchToWorkspace(it);
    return true;
}

namespace {
// The modifier bit a pressed key ITSELF contributes (evdev codes), so bare modifier
// bindings can mask their own bit out of the held-mods state before matching.
uint32_t modBitForKeycode(int kc) {
    switch (kc) {
        case 42:
        case 54: return HL_MODIFIER_SHIFT;
        case 29:
        case 97: return HL_MODIFIER_CTRL;
        case 56:
        case 100: return HL_MODIFIER_ALT;
        case 125:
        case 126: return HL_MODIFIER_META;
        default: return 0;
    }
}
} // namespace

void Overview::onKey(const IKeyboard::SKeyEvent& e, bool& cancel) {
    if (!m_active)
        return;
    // Keyboard model: the overview consumes ESC/TAB (dismiss), Enter (focus selection),
    // arrows (move cursor); everything else passes THROUGH to Hyprland in passthrough
    // mode so the user's normal keybinds keep working. With passthrough off it's fully
    // modal — every key swallowed. Keycodes are evdev (layout-independent).
    const bool passthrough = cfgInt("plugin:gloview:passthrough_keys", 1) != 0;

    if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        cancel = !passthrough; // balance the release half of any key we let through on press
        return;
    }

    // Each action binds a config list of key NAMES (esc/tab/enter/left/shift/hjkl/…;
    // bare digit = number-row key), optionally with modifier prefixes ("shift+tab").
    // key_* = "" disables the action (key falls through).
    const int k = e.keycode;
    // Held modifiers across all keyboards (not just keys pressed since the overview
    // opened), minus the pressed key's OWN modifier bit — else a bare modifier name
    // ("shift", the key_desktop default) could never match its own press.
    uint32_t mods = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    mods &= ~modBitForKeycode(k);
    bool handled = true;
    if (keyMatches(k, mods, "plugin:gloview:key_close", "escape"))
        close();
    else if (keyMatches(k, mods, "plugin:gloview:key_next_workspace", "tab"))
        stepWorkspace(1); // cycle the displayed workspace card (wraps; committed on close)
    else if (keyMatches(k, mods, "plugin:gloview:key_prev_workspace", "shift+tab"))
        stepWorkspace(-1);
    else if (keyMatches(k, mods, "plugin:gloview:key_activate", "enter"))
        activateSelection();
    else if (keyMatches(k, mods, "plugin:gloview:key_close_window", "d"))
        closeTileWindow(m_selected); // sendClose the SELECTED tile (keyboard/hover cursor), stay open & reflow
    else if (keyMatches(k, mods, "plugin:gloview:key_left", "left"))
        moveSelection(-1, 0);
    else if (keyMatches(k, mods, "plugin:gloview:key_right", "right"))
        moveSelection(1, 0);
    else if (keyMatches(k, mods, "plugin:gloview:key_up", "up"))
        moveSelection(0, -1);
    else if (keyMatches(k, mods, "plugin:gloview:key_down", "down"))
        moveSelection(0, 1);
    else if (keyMatches(k, mods, "plugin:gloview:key_desktop", "shift"))
        setDesktopMode(!m_desktopMode);
    else if (keyMatches(k, mods, "plugin:gloview:key_all_workspaces", "a"))
        toggleAllWorkspaces(); // flip the expo (all-workspaces) main view
    else if (const int ws = keyIndex(k, mods, "plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"); ws >= 0) {
        // number-row 1..0 → switch to the Nth (non-"+") card's workspace, in the overview
        // AND for real now. Update m_liveWsAtOpen so the exit_on_switch poll doesn't read
        // this self-switch as external.
        int n = 0;
        for (const auto& it : m_strip) {
            if ((it.isPlus && !it.isNew) || it.isAll) // count only real workspace cards
                continue;
            if (n++ == ws) {
                switchToWorkspace(it);
                if (const auto m = m_monitor.lock()) {
                    if (const auto target = m_workspace.lock(); target && target != m->m_activeWorkspace) {
                        m->changeWorkspace(target, false, true, false);
                        m_liveWsAtOpen = m->m_activeWorkspace;
                    }
                }
                break;
            }
        }
    } else
        handled = false;
    cancel = handled || !passthrough;
}

namespace {
// Split a config string into key tokens on commas / whitespace.
std::vector<std::string> keyTokens(const std::string& s) {
    std::vector<std::string> out;
    std::string              cur;
    for (const char c : s) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Resolve a key NAME (case-insensitive) to its evdev keycode(s). Bare digit = number-row
// key; left/right-variant names (shift/ctrl/alt/super) and enter resolve to BOTH codes.
const std::vector<int>& keyNameToCodes(std::string t) {
    static const std::vector<int> NONE;
    for (auto& c : t)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::unordered_map<std::string, std::vector<int>> M = {
        {"0", {11}}, {"1", {2}}, {"2", {3}}, {"3", {4}}, {"4", {5}}, {"5", {6}},
        {"6", {7}}, {"7", {8}}, {"8", {9}}, {"9", {10}},
        {"esc", {1}}, {"escape", {1}}, {"tab", {15}}, {"space", {57}},
        {"enter", {28, 96}}, {"return", {28, 96}}, {"kpenter", {96}},
        {"backspace", {14}}, {"delete", {111}}, {"del", {111}}, {"insert", {110}},
        {"left", {105}}, {"right", {106}}, {"up", {103}}, {"down", {108}},
        {"home", {102}}, {"end", {107}}, {"pageup", {104}}, {"pagedown", {109}},
        {"shift", {42, 54}}, {"lshift", {42}}, {"rshift", {54}},
        {"ctrl", {29, 97}}, {"control", {29, 97}}, {"lctrl", {29}}, {"rctrl", {97}},
        {"alt", {56, 100}}, {"lalt", {56}}, {"ralt", {100}},
        {"super", {125, 126}}, {"meta", {125, 126}}, {"win", {125, 126}},
        {"f1", {59}}, {"f2", {60}}, {"f3", {61}}, {"f4", {62}}, {"f5", {63}}, {"f6", {64}},
        {"f7", {65}}, {"f8", {66}}, {"f9", {67}}, {"f10", {68}}, {"f11", {87}}, {"f12", {88}},
        // letters (evdev rows; lets users bind hjkl / wasd etc.)
        {"a", {30}}, {"b", {48}}, {"c", {46}}, {"d", {32}}, {"e", {18}}, {"f", {33}},
        {"g", {34}}, {"h", {35}}, {"i", {23}}, {"j", {36}}, {"k", {37}}, {"l", {38}},
        {"m", {50}}, {"n", {49}}, {"o", {24}}, {"p", {25}}, {"q", {16}}, {"r", {19}},
        {"s", {31}}, {"t", {20}}, {"u", {22}}, {"v", {47}}, {"w", {17}}, {"x", {45}},
        {"y", {21}}, {"z", {44}},
    };
    const auto it = M.find(t);
    return it != M.end() ? it->second : NONE;
}

// Modifier NAME (a "+"-prefix in a combo token) → HL_MODIFIER bit; 0 = not a modifier.
uint32_t modNameToBit(std::string t) {
    for (auto& c : t)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "shift")
        return HL_MODIFIER_SHIFT;
    if (t == "ctrl" || t == "control")
        return HL_MODIFIER_CTRL;
    if (t == "alt")
        return HL_MODIFIER_ALT;
    if (t == "super" || t == "meta" || t == "win")
        return HL_MODIFIER_META;
    return 0;
}

// Match one token ("tab", "shift+tab", "ctrl+shift+k") against the pressed keycode and
// the currently held modifiers — EXACT on shift/ctrl/alt/super. Unrequested modifiers
// must NOT be held: a bare "tab" must not swallow shift+tab (its own action) nor
// super+tab (commonly the user's gloview:toggle bind — with passthrough it falls
// through to Hyprland's keybind manager, so the same bind that opened the overview
// closes it). Only lock states (caps/num) are ignored.
bool comboMatches(int keycode, uint32_t heldMods, std::string token) {
    constexpr uint32_t STRICTMODS = HL_MODIFIER_SHIFT | HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META;

    uint32_t           need = 0;
    size_t             pos;
    while ((pos = token.find('+')) != std::string::npos) {
        const uint32_t bit = modNameToBit(token.substr(0, pos));
        if (bit == 0)
            return false; // unknown modifier name → the token can never match
        need |= bit;
        token.erase(0, pos + 1);
    }

    bool codeHit = false;
    for (const int c : keyNameToCodes(token))
        if (c == keycode) {
            codeHit = true;
            break;
        }
    if (!codeHit)
        return false;
    if ((heldMods & need) != need)
        return false;
    return (heldMods & STRICTMODS & ~need) == 0;
}
} // namespace

// keycode+mods bound by the configured key list? (comma/space separated names; bare digit
// = number-row key; "shift+tab"-style combos). Empty config → no match (action disabled,
// key falls through).
bool Overview::keyMatches(int keycode, uint32_t mods, const char* cfgName, const char* fallback) const {
    for (const auto& tok : keyTokens(cfgStr(cfgName, fallback)))
        if (comboMatches(keycode, mods, tok))
            return true;
    return false;
}

// 0-based token position of keycode within the list, else -1. The number row uses
// it: the matched token's slot in key_workspace selects strip card N.
int Overview::keyIndex(int keycode, uint32_t mods, const char* cfgName, const char* fallback) const {
    int idx = 0;
    for (const auto& tok : keyTokens(cfgStr(cfgName, fallback))) {
        if (comboMatches(keycode, mods, tok))
            return idx;
        ++idx;
    }
    return -1;
}

void Overview::dbg(const std::string& msg) const {
    if (cfgInt("plugin:gloview:debug_logs", 0) != 0 && Log::logger)
        Log::logger->log(Log::INFO, "[gloview] {}", msg);
}

// Step the selection cursor to the nearest tile in a screen direction (dx/dy unit step).
// Picks the candidate furthest along the requested axis but least off it, so the cursor
// reads as spatial rather than list-order.
void Overview::moveSelection(int dx, int dy) {
    if (m_tiles.empty())
        return;
    if (m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size())) {
        m_selected = (m_hovered >= 0) ? m_hovered : 0;
        syncFocus();
        damage();
        return;
    }
    const LRect  cur = currentBox(m_tiles[m_selected], m_selected);
    const double cx = cur.cx(), cy = cur.cy();
    int          best = -1;
    double       bestScore = 1e18;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (static_cast<int>(i) == m_selected)
            continue;
        const LRect  b     = currentBox(m_tiles[i], static_cast<int>(i));
        const double ddx   = b.cx() - cx;
        const double ddy   = b.cy() - cy;
        const double along = dx * ddx + dy * ddy;       // distance in the requested direction
        if (along <= 1.0)
            continue;                                   // not in that direction
        const double perp  = std::abs(dx * ddy - dy * ddx); // lateral offset
        const double score = along + perp * 2.0;        // prefer aligned, penalize sideways drift
        if (score < bestScore) {
            bestScore = score;
            best      = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        m_selected = best;
        syncFocus();
        damage();
    }
}

void Overview::activateSelection() {
    PHLWINDOW w;
    if (m_selected >= 0 && m_selected < static_cast<int>(m_tiles.size()))
        w = m_tiles[m_selected].win.lock();
    activateWindow(w, true);
}

// Picking a tile (click or key_activate) dismisses the overview onto that window's workspace.
// In the all-workspaces (expo) view the tile can live on a workspace OTHER than the displayed
// one, and close() commits m_workspace — so without retargeting it first the overview closed
// back onto the workspace you started on and merely raised a window you could not see.
void Overview::activateWindow(const PHLWINDOW& w, bool keybind) {
    if (w) {
        const auto m  = m_monitor.lock();
        const auto ws = w->m_workspace;
        // Special (scratchpad) workspaces are not something changeWorkspace should land on —
        // focusing the window is enough; Hyprland keeps the scratchpad up on its own.
        if (m && ws && !ws->m_isSpecialWorkspace && ws->m_monitor.lock() == m)
            m_workspace = ws;
    }
    close();
    if (w)
        Desktop::focusState()->fullWindowFocus(w, keybind ? Desktop::FOCUS_REASON_KEYBIND : Desktop::FOCUS_REASON_CLICK);
}

// Point Hyprland's REAL focus at the selected tile while the overview is up. passthrough
// keybinds like `killactive` act on the focused window; without this, focus stayed on
// whatever was focused before open, so a hotkey hit the WRONG window (ring and real focus
// diverged). Keep focus in lockstep with m_selected.
//
// Guarded to the monitor's ACTIVE workspace: a displayed (uncommitted) workspace's tiles
// are hidden, so focusing one would desync focus from the live desktop without a real
// switch. fullWindowFocus does NOT change the active workspace, so same-workspace only
// moves input focus.
void Overview::syncFocus() const {
    if (!m_active || m_selected < 0 || m_selected >= static_cast<int>(m_tiles.size()))
        return;
    const auto m = m_monitor.lock();
    const auto w = m_tiles[m_selected].win.lock();
    if (!m || !w || !w->m_isMapped || w->isHidden())
        return;
    if (w->m_workspace != m->m_activeWorkspace) // displaying a non-live workspace — don't desync
        return;
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_KEYBIND);
}

// Rebuild tiles/strip, then glide each survivor from its captured box into its new slot
// without re-running the chrome reveal (m_progress pinned at 1). Shared by
// drop-to-workspace and close-window.
void Overview::replayReflow(std::vector<std::pair<PHLWINDOW, LRect>>& oldBoxes) {
    // A workspace slide in flight has its offset baked into every box the caller captured
    // (currentBox adds it). The reflow cancels the slide, so take the offset back out —
    // otherwise every tile would start its glide a full screen off to one side.
    if (m_wsSliding) {
        const Vector2D off = wsSlideOffset(false);
        for (auto& [oldWin, oldBox] : oldBoxes) {
            oldBox.x -= off.x;
            oldBox.y -= off.y;
        }
        endWsSlide();
    }

    m_hovered = m_hoveredStrip = -1;
    buildTiles();
    buildStrip();
    layoutTiles();
    for (auto& t : m_tiles) {
        const auto win = t.win.lock();
        t.natural      = t.target;
        for (const auto& [oldWin, oldBox] : oldBoxes) {
            if (oldWin == win) {
                t.natural = oldBox;
                break;
            }
        }
    }
    if (m_selected >= static_cast<int>(m_tiles.size()))
        m_selected = m_tiles.empty() ? -1 : static_cast<int>(m_tiles.size()) - 1;
    const auto now = std::chrono::steady_clock::now();
    m_progress     = 1.0;
    m_opening      = true;
    m_animStart    = now - std::chrono::milliseconds(std::max(1, cfgInt("plugin:gloview:duration", 360)));
    m_reflowing    = true;
    m_reflowStart  = now;
    damage();
}

void Overview::closeTileWindow(int i) {
    if (i < 0 || i >= static_cast<int>(m_tiles.size()))
        return;
    const auto w = m_tiles[i].win.lock();
    if (!w)
        return;
    dbg("close tile window");
    // sendClose is async — the client decides when to unmap. Don't touch m_tiles
    // here: syncTiles() (run each frame) notices the window vanish and reflows,
    // which also covers windows that close themselves while the overview is up.
    w->sendClose();
}

// Drop any tile whose window has gone away (closed by us or by itself), then glide
// the survivors into their re-laid-out slots. No-op while capturing/closing.
void Overview::syncTiles() {
    // m_wsSliding too: the tile set is mid-swap during a workspace slide, and a reflow here
    // would cancel the slide and snap everything into place.
    if (!m_active || !m_opening || m_capturing || m_pendingDeactivate || m_reflowing || m_wsSliding)
        return;
    const auto ws = m_workspace.lock();
    if (!ws)
        return;

    // The displayed window set can change while up (window closes, or opens/moves onto
    // this workspace/monitor). Detect any add/remove with tileBelongs() — the SAME
    // predicate buildTiles() uses, so the count settles in one frame (else reflow every
    // frame, the expo-mode churn bug) — then glide via replayReflow (chrome stays at 1).
    const auto m       = m_monitor.lock();
    const auto belongs = [&](const PHLWINDOW& w) { return tileBelongs(w, m, ws); };
    size_t expected = 0;
    for (const auto& w : Desktop::windowState()->windows())
        if (belongs(w))
            ++expected;

    bool diff = expected != m_tiles.size();
    if (!diff)
        for (const auto& t : m_tiles) {
            const auto w = t.win.lock();
            if (!belongs(w)) { // a tracked window died or left this workspace
                diff = true;
                break;
            }
        }
    if (!diff)
        return;

    std::vector<std::pair<PHLWINDOW, LRect>> oldBoxes;
    oldBoxes.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (const auto win = m_tiles[i].win.lock())
            oldBoxes.emplace_back(win, currentBox(m_tiles[i], static_cast<int>(i)));

    // Desktop mode: adding/removing a window must NOT shuffle the others. Non-dragged
    // previews track their REAL window position, which Hyprland re-tiles on add/remove →
    // every survivor jumps. Park each survivor at its settled box in m_canvasPos so the
    // rebuild treats them as user-placed; only the newcomer flows to its real scaled spot.
    if (m_desktopMode)
        for (auto& t : m_tiles)
            if (const auto win = t.win.lock())
                m_canvasPos[win.get()] = t.target;

    replayReflow(oldBoxes);
}

// Scroll wheel over the main area: show the previous/next workspace card (wraps).
void Overview::stepWorkspace(int dir) {
    if (m_strip.empty())
        return;
    // collect only the real workspace cards (skip the "+" and the optional "All" card),
    // then step within that list so the wrap can't land on a non-workspace card.
    // dynamic_workspaces' trailing empty card DOES count: stepping onto it is how you reach
    // (and thereby create) the next desktop, gnome-style.
    std::vector<int> real;
    int              activePos = -1;
    for (size_t i = 0; i < m_strip.size(); ++i) {
        if ((m_strip[i].isPlus && !m_strip[i].isNew) || m_strip[i].isAll)
            continue;
        if (m_strip[i].active)
            activePos = static_cast<int>(real.size());
        real.push_back(static_cast<int>(i));
    }
    if (real.empty())
        return; // only the "+"/"All" cards
    if (activePos < 0)
        activePos = 0;
    int next = activePos + (dir > 0 ? 1 : -1);
    if (next < 0)
        next = static_cast<int>(real.size()) - 1; // wrap to last
    else if (next >= static_cast<int>(real.size()))
        next = 0;                                 // wrap to first
    if (next != activePos)
        switchToWorkspace(m_strip[real[next]]);
}

// Fade out bars/popups so they don't bleed through the translucent backdrop: stash each
// layer surface's alpha goal and drive it to 0; restoreLayers() animates them back. Fully
// reversible (nothing persists past close); works for any layer-shell client.
void Overview::hideLayers() {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const bool top = cfgInt("plugin:gloview:hide_top_layers", 0) != 0;
    const bool ovl = cfgInt("plugin:gloview:hide_overlay_layers", 0) != 0;
    if (!top && !ovl)
        return;
    const auto fade = [this](const std::vector<PHLLSREF>& layer) {
        for (const auto& ref : layer) {
            const auto ls = ref.lock();
            if (!ls || !ls->alpha()[Desktop::View::LS_ALPHA_FADE])
                continue;
            if (isAboveLayer(ls->m_namespace))
                continue; // keep above-overview surfaces fully visible
            m_hiddenLayers.emplace_back(ref, ls->alpha()[Desktop::View::LS_ALPHA_FADE]->goal());
            *ls->alpha()[Desktop::View::LS_ALPHA_FADE] = 0.F;
        }
    };
    if (top)
        fade(m->m_layerSurfaceLayers[2]); // ZWLR_LAYER_SHELL_V1_LAYER_TOP
    if (ovl)
        fade(m->m_layerSurfaceLayers[3]); // ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
    if (!m_hiddenLayers.empty()) {
        dbg("hid " + std::to_string(m_hiddenLayers.size()) + " layer surface(s)");
        damage();
    }
}

void Overview::restoreLayers() {
    for (auto& [ref, alpha] : m_hiddenLayers)
        if (const auto ls = ref.lock(); ls && ls->alpha()[Desktop::View::LS_ALPHA_FADE])
            *ls->alpha()[Desktop::View::LS_ALPHA_FADE] = alpha;
    m_hiddenLayers.clear();
}

// renderWindowLive() sets m_fillIgnoreSmall=true on previewed surfaces so the texture
// fills the slot instead of centering a smaller buffer (which showed the backing as black
// bars). Hyprland never sets this flag, so a blanket reset to false restores normal
// rendering — no per-surface tracking. Guarded in deactivate()/hardClose()/dtor.
void Overview::restoreFill() {
    for (const auto& w : Desktop::windowState()->windows())
        if (w && w->wlSurface())
            w->wlSurface()->m_fillIgnoreSmall = false;
}

// "+" card: create the lowest-free-id workspace, pop its card in, and (per
// switch_on_new_workspace) optionally follow the display to it.
void Overview::addWorkspace(int id_) {
    const auto m = m_monitor.lock();
    if (!m)
        return;
    const int  id = resolveNewId(id_);
    const auto ws = State::workspaceState()->create(id, m->m_id, "", false);
    if (!ws)
        return;
    // A new empty workspace is reaped within a frame or two unless focused. Hold it
    // persistent so its card survives while up; deactivate() releases it (normal reaping
    // applies after). Releasing the previously held one is what lets an abandoned empty
    // workspace disappear again under dynamic_workspaces.
    if (const auto old = m_newWs.lock(); old && old != ws)
        old->setPersistent(false);
    ws->setPersistent(true);
    m_newWs        = ws;
    m_newCardId    = id;
    m_newCardStart = std::chrono::steady_clock::now();
    m_newCardAnim  = true;
    dbg("added workspace " + std::to_string(id));
    if (cfgInt("plugin:gloview:switch_on_new_workspace", 1) != 0) {
        StripItem it;
        it.ws = ws;
        switchToWorkspace(it); // follow the display (rebuilds the strip with the new card)
    } else {
        m_hovered = m_hoveredStrip = -1;
        buildStrip(); // keep the current display; just surface the new card so it animates in
        damage();
    }
}

// Middle-click a workspace card: send-close every window on it (async, like the
// per-window middle-click). syncTiles() reflows once the windows actually go.
void Overview::closeWorkspaceWindows(const StripItem& it) {
    if (it.isPlus || it.isAll)
        return;
    const auto ws = it.ws.lock();
    if (!ws)
        return;
    int n = 0;
    for (const auto& w : Desktop::windowState()->windows())
        if (w && w->m_isMapped && !w->isHidden() && w->m_workspace == ws) {
            w->sendClose();
            ++n;
        }
    dbg("middle-click workspace: closed " + std::to_string(n) + " window(s)");
}

double Overview::newCardScale() const {
    if (!m_newCardAnim)
        return 1.0;
    const double dur = std::max(120, cfgInt("plugin:gloview:duration", 360));
    const double p   = std::clamp(nowMs(m_newCardStart) / dur, 0.0, 1.0);
    // easeOutBack — a little overshoot so the card "pops" in
    const double c1 = 1.70158, c3 = c1 + 1.0;
    const double x  = p - 1.0;
    return 1.0 + c3 * x * x * x + c1 * x * x;
}

bool Overview::forceRenderWindow(const PHLWINDOW& w) const {
    return m_capturing && w && m_captureWin && m_captureWin.lock() == w;
}

bool Overview::shouldHideWindow(const PHLWINDOW& w, const PHLMONITOR& mon) const {
    const auto m = m_monitor.lock();
    // While snapshotting, let the window render: makeSnapshot's own shouldRenderWindow
    // check routes through this hook, so hiding here would bail it → empty tiles.
    if (m_capturing)
        return false;
    if (!m_active || !w || !m)
        return false;
    // Deliberately NOT scoped to `mon == m`. Hyprland renders a window on EVERY monitor its
    // box overlaps, so a window straddling a monitor edge kept painting its overhang on the
    // neighbouring output while the overview hid it here — it read as a duplicate of the
    // preview. Every rule below keys off the window's WORKSPACE (which lives on OUR monitor),
    // so suppressing those windows on all outputs is correct; windows owned by another
    // monitor's workspace never match and keep rendering normally.
    (void)mon;
    // hide the windows we draw previews for
    for (const auto& t : m_tiles)
        if (t.win.lock() == w)
            return true;
    // also hide the monitor's active workspace, so displaying a different desktop
    // doesn't bleed the current one through the translucent backdrop.
    if (const auto aw = m->m_activeWorkspace; aw && w->m_workspace == aw)
        return true;
    // …and the workspace close() committed away from, which the rule above no longer covers.
    if (const auto from = m_committedFrom.lock(); from && w->m_workspace == from)
        return true;
    return false;
}

void Overview::deactivate() {
    restoreLayers(); // safety net: normally close() already restored; harmless if empty
    restoreFill();   // drop the fill-small override so real windows render normally again
    m_canvasPos.clear();

    if (const auto m = m_monitor.lock()) {
        if (const auto ws = m_workspace.lock(); ws && ws != m->m_activeWorkspace)
            m->changeWorkspace(ws, false, true, false);
    }

    // Drop the hold on a "+"-created workspace: it stays if active or a window landed
    // there, otherwise reaps like any empty one.
    if (const auto ws = m_newWs.lock())
        ws->setPersistent(false);
    m_newWs.reset();

    m_active  = false;
    m_opening = false;
    m_reflowing = false;
    m_pendingDeactivate = false;
    m_desktopMode = false;
    m_allOverride = -1; // next open follows plugin:gloview:show_all_workspaces again
    m_newCardAnim = false;
    m_newCardId   = 0;
    m_progress = 0.0;
    m_tiles.clear();
    m_strip.clear();
    m_committedFrom.reset();
    endWsSlide();
    m_flying.clear();
    m_hovered = m_hoveredStrip = -1;
    m_selected = -1;
    m_recaptureLeft = 0;
    if (m_recaptureTimer)
        m_recaptureTimer->cancel();
    damage();
}

void Overview::damage() const {
    if (const auto m = m_monitor.lock(); m && g_pHyprRenderer)
        g_pHyprRenderer->damageMonitor(m);
}

} // namespace gloview
