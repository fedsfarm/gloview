#pragma once

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>

namespace gloview::PreviewFilter {

// Draws a live surface through a sampleGrid x sampleGrid box filter. Supported
// grids are 2 (box4) and 4 (box16).
[[nodiscard]] UP<IPassElement> makePass(CSurfacePassElement::SRenderData fallback, const CBox& boxPx, const CBox& clipPx,
                                        const Vector2D& uvMin, const Vector2D& uvMax, double radiusPx, int sampleGrid);

// Releases GL objects before the plugin is unloaded.
void reset();

} // namespace gloview::PreviewFilter
