#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>

namespace Render {
class ITexture;
class IFramebuffer;
}

#include "layout.hpp"

class CFunctionHook;
class CEventLoopTimer;

namespace gloview {

// Plugin config values registered with `addConfigValueV2` (main.cpp), kept so the
// cfg* helpers can read them through their V2 `value()` accessor. The deprecated
// `HyprlandAPI::getConfigValue()` path does NOT observe values set from a Lua
// `hl.config{}` config — it returned the registered default, so every setting
// looked like it "did nothing" under a Lua config. Reading the IValue directly
// works for both the legacy/ini and Lua config frontends.
struct ConfigRegistry {
    std::unordered_map<std::string, SP<Config::Values::CIntValue>>    ints;
    std::unordered_map<std::string, SP<Config::Values::CStringValue>> strings;
    std::unordered_map<std::string, SP<Config::Values::CColorValue>>  colors;
    std::unordered_map<std::string, SP<Config::Values::CFloatValue>>  floats;
};
inline ConfigRegistry g_config;

// macOS Mission Control-style overview for Hyprland.
//
//   ┌───────────────────────────────────────────────┐
//   │  [ws1] [ws2] [ws3] ...                    [ + ] │  translucent strip
//   ├───────────────────────────────────────────────┤
//   │     ┌────┐   ┌────────┐                         │
//   │     │win │   │  win   │   live window previews  │  main area
//   │     └────┘   └────────┘                         │
//   └───────────────────────────────────────────────┘
//
// The whole thing is drawn compositor-side from window snapshots over a blurred
// backdrop; real windows are hidden while it is up. Layout math lives in
// layout.hpp so it can be tweaked independently.
class Overview {
  public:
    explicit Overview(HANDLE handle);
    ~Overview();

    bool initialize();

    void toggle();
    void open();
    void close();
    void hardClose(); // immediate, animation-free teardown for the UNLOAD path (hyprctl gloviewunload)
    void toggleDesktop(); // open (or, if already open, switch into) free-arrange desktop mode
    void toggleAllWorkspaces(); // open (or, if already open, toggle) the all-workspaces "expo" main view

    // wired to Hyprland's event bus / render pass
    void renderStage(eRenderStage stage);
    void renderBackdrop() const;
    void renderStrip() const;
    void renderStripWindows() const; // live window surfaces inside the strip cards
    void renderStripRings() const;  // card active/hover rings, over the card previews
    void renderPreviews() const;  // static tiles' chrome (shadow/backing/title), drawn under the strip
    void renderMainWindows() const; // live window surfaces for the main-area tiles
    void renderPreviewRings() const; // tile hover/selection rings, over the main surfaces
    void renderDragTile() const;  // the picked-up tile's chrome, drawn over the strip
    void renderDragWindow() const; // the picked-up tile's live surface
    void renderDragRing() const;  // the picked-up tile's ring, over its surface
    void renderCursorOnTop() const; // redraw the SW cursor over our overlay
    bool isAboveLayer(const std::string& ns) const;
    void renderAboveLayers() const; // re-render opted-in TOP/OVERLAY layer surfaces on top of the overview
    bool onMouseButton(const IPointer::SButtonEvent& e);
    bool onMouseAxis(const IPointer::SAxisEvent& e); // scroll the workspace strip when it overflows
    void onMouseMove();
    void updateHover(); // recompute hovered tile/card from current cursor pos
    void onKey(const IKeyboard::SKeyEvent& e, bool& cancel);
    bool shouldHideWindow(const PHLWINDOW& w, const PHLMONITOR& m) const;
    // true while capturing the given window's snapshot: forces Hyprland to render
    // it even though it sits on an inactive workspace (otherwise the snapshot is
    // blank → grey preview). Checked before shouldHideWindow in the render hook.
    bool forceRenderWindow(const PHLWINDOW& w) const;

    [[nodiscard]] bool       active() const { return m_active; }
    [[nodiscard]] PHLMONITOR monitor() const { return m_monitor.lock(); }
    [[nodiscard]] bool       blurEnabled() const; // plugin:gloview:blur != 0 (queried by the pass)

  private:
    struct Tile {
        PHLWINDOWREF         win;
        LRect                natural;   // monitor-local logical: real place (goal); animation start
        LRect                target;    // monitor-local logical: grid slot
        LRect                snapSource; // window's frozen position when its snapshot was taken; crop source
        SP<Render::ITexture> label;   // cached window title, shown on hover
        bool                 captured = false; // snapshot was (re)taken THIS session; guards stale persistent FBs
    };

    struct StripWin {
        PHLWINDOWREF win;
        LRect        rel; // 0..1 within the monitor: the window's tiled slot in the card.
                          // The card preview renders the window's LIVE surface into this
                          // slot (renderStripWindows), so no snapshot/crop state is needed.
    };

    struct StripItem {
        PHLWORKSPACEREF       ws;
        int                   id = 0;
        bool                  active = false;
        bool                  isPlus = false;
        bool                  isAll  = false; // the leading "All workspaces" card (toggles the expo view)
        LRect                 card; // monitor-local logical
        std::vector<StripWin> wins;
        SP<Render::ITexture>  label; // cached rendered workspace name
    };

    HANDLE                                m_handle = nullptr;
    bool                                  m_active = false;
    bool                                  m_opening = false;
    // Close animation just hit progress 0 THIS frame. shouldRenderWindow (which
    // hides the real windows) is evaluated early in the frame, before our
    // RENDER_LAST_MOMENT pass; if we flipped m_active off mid-frame we'd skip
    // drawing the overlay on a frame whose real windows were already suppressed →
    // one fully-transparent frame (the close-flicker). Instead we draw this final
    // frame's overlay (opaque previews at natural pos cover the windows), then
    // deactivate AFTER the pass is built, so the NEXT frame's early window
    // decision sees m_active=false and renders the real windows cleanly.
    bool                                  m_pendingDeactivate = false;
    bool                                  m_capturing = false; // true during captureSnapshots so windows aren't hidden from makeSnapshot
    PHLWINDOWREF                          m_captureWin;        // the window whose snapshot is being taken (force-rendered even if off-workspace)
    double                                m_progress = 0.0;
    std::chrono::steady_clock::time_point m_animStart;
    // A post-move reflow glides the tiles into their new slots WITHOUT re-running
    // the chrome (backdrop + strip) reveal. m_progress stays pinned at 1 (chrome
    // settled) while this separate timer drives the tile natural->target lerp, so
    // the strip no longer re-slides and the backdrop no longer flashes on a drop.
    bool                                  m_reflowing = false;
    std::chrono::steady_clock::time_point m_reflowStart;
    PHLMONITORREF                         m_monitor;
    PHLWORKSPACEREF                       m_workspace;    // workspace shown in the main area
    PHLWORKSPACEREF                       m_liveWsAtOpen; // monitor's live active workspace when opened (exit_on_switch)
    std::vector<Tile>                     m_tiles;
    std::vector<StripItem>                m_strip;
    // window* -> monitor-local logical rect its persistent snapshot FB content sits in,
    // recorded only when the window was SETTLED (value≈goal, client buffer matches its
    // box) at capture. Lets a window retiled on a hidden workspace (frozen value!=goal,
    // stale-size buffer) reuse its last good snapshot instead of re-snapshotting into a
    // stretched/black thumbnail. Persists across the per-open tile/strip rebuilds.
    std::unordered_map<void*, LRect>      m_snapGeom;
    // window* -> the framebuffer makeSnapshotFB() handed us for it. 0.56 dropped
    // CWindow::m_snapshotFB, so the plugin holds the FB itself; we never sample it (tiles
    // render the live surface), it only answers "is there already a good snapshot?".
    // Stale keys are pruned each capture pass and the whole map is dropped on teardown.
    std::unordered_map<void*, SP<Render::IFramebuffer>> m_snapFB;
    // layer surfaces (bars/popups) we faded out while up, with their pre-hide alpha
    // goal, so deactivate() restores them exactly — even if config changed meanwhile.
    std::vector<std::pair<PHLLSREF, float>> m_hiddenLayers;
    int                                   m_hovered = -1;     // index into m_tiles
    int                                   m_hoveredStrip = -1; // index into m_strip
    int                                   m_selected = -1;    // keyboard-nav cursor into m_tiles

    // free-arrange "desktop" mode: tiles sit at the windows' real positions and a
    // drag floats + repositions the real window instead of snapping to a grid.
    bool                                  m_desktopMode = false;
    // expo view: -1 = follow plugin:gloview:show_all_workspaces, 0 = runtime-forced off,
    // 1 = runtime-forced on (the gloview:allworkspaces toggle). Reset to -1 on full close.
    int                                   m_allOverride = -1;
    double                                m_desktopS  = 1.0;  // monitor→preview scale (and its inverse for drops)
    double                                m_desktopOx = 0.0;  // monitor-local preview origin x
    double                                m_desktopOy = 0.0;  // monitor-local preview origin y
    // Canvas mode is purely VISUAL: dragging a preview parks it here (window* → canvas
    // box, monitor-local) so the arrangement survives per-frame rebuilds. Dragging a
    // preview never floats/moves the real window — that stays put.
    std::unordered_map<void*, LRect>      m_canvasPos;
    mutable SP<Render::ITexture>          m_closeGlyph;       // cached "✕" for the desktop-mode close buttons

    // "+" add-workspace pop-in: the freshly created card scales up from its center.
    int                                   m_newCardId    = 0; // workspace id of the animating card (0 = none)
    bool                                  m_newCardAnim  = false;
    std::chrono::steady_clock::time_point m_newCardStart;
    PHLWORKSPACEREF                       m_newWs;            // freshly "+"-created ws, held persistent until close so it isn't reaped empty
    double                                m_stripScroll = 0.0;    // strip group scroll offset along its main axis
    double                                m_stripScrollMax = 0.0; // max scroll (0 when the cards fit the band)
    SP<CEventLoopTimer>                   m_recaptureTimer;    // off-render-loop re-snapshot after a drop (makeSnapshot mid-render crashes)
    int                                   m_recaptureLeft = 0; // remaining recapture ticks while windows repaint at their new size

    // drag-and-drop of a window preview onto a workspace card
    int    m_pressTile = -1;          // tile under the press (drag candidate)
    bool   m_dragging  = false;       // moved past the threshold
    double m_pressX = 0, m_pressY = 0;// monitor-local press point
    double m_grabDX = 0, m_grabDY = 0;// cursor offset inside the tile at grab
    double m_dragX  = 0, m_dragY  = 0;// current monitor-local cursor

    CHyprSignalListener m_renderStageL;
    CHyprSignalListener m_mouseButtonL;
    CHyprSignalListener m_mouseAxisL;
    CHyprSignalListener m_mouseMoveL;
    CHyprSignalListener m_keyL;
    CFunctionHook*      m_shouldRenderHook = nullptr;
    CFunctionHook*      m_shouldRenderWindowHook = nullptr; // one-arg shouldRenderWindow, used by makeSnapshot()

    // config helpers
    int           cfgInt(const char* name, int fallback) const;
    float         cfgFloat(const char* name, float fallback) const;
    Hyprlang::INT cfgColor(const char* name, Hyprlang::INT fallback) const;
    std::string   cfgStr(const char* name, const char* fallback) const;

    // Which monitor edge the workspace strip is anchored to. Top/Bottom give a
    // horizontal strip (cards in a row); Left/Right give a vertical strip (cards
    // in a column).
    enum class Anchor { Top, Bottom, Left, Right };
    Anchor   stripAnchor() const;        // plugin:gloview:anchor (falls back to bar_position)
    bool     stripHorizontal() const;    // Top or Bottom
    double   stripThickness() const;     // band size perpendicular to its edge (strip_height)
    double   stripOffset() const;        // inset from the anchored edge (strip_offset, 0 default)
    LRect    stripBand() const;          // the band rect, monitor-local logical
    Vector2D stripSlide(double e) const; // reveal slide-in offset at progress e
    Vector2D stripScroll() const;        // current scroll offset of the card group
    LRect    stripCardAt(size_t i) const;// m_strip[i].card shifted by the current scroll (for hit-testing)
    bool   showAllWorkspaces() const; // effective expo state: runtime override (m_allOverride) else plugin:gloview:show_all_workspaces
    bool   tileBelongs(const PHLWINDOW& w, const PHLMONITOR& m, const PHLWORKSPACE& ws) const; // shared main-area membership test (buildTiles + syncTiles MUST agree)
    void   buildTiles();
    void   buildStrip();
    void   layoutTiles();
    void   captureSnapshots();
    void   scheduleRecapture(); // arm the off-render-loop re-snapshot timer
    void   updateAnimation();
    void   deactivate();
    double eased() const;                       // opacity / backdrop progress
    double tileBaseProgress() const;            // 0..1 driver for tile glide (reflow timer or m_progress)
    double tileProgress(int i) const;           // staggered raw progress for tile i
    LRect  currentBox(const Tile& t, int i) const; // lerped natural->target, staggered + overshoot
    LRect  tileContentBox(size_t i, const LRect& slot) const; // slot fitted to the window's aspect
    LRect  dragBox() const;                        // the picked-up tile's box at the cursor
    void   drawPreviewTile(size_t i, const LRect& slot, bool lift) const; // tile chrome (shadow/backing/title)
    void   drawPreviewRing(size_t i, const LRect& slot, bool lift) const; // hover/selection ring, over the live surface
    LRect  stripCardBox(size_t i, const Vector2D& slide, const Vector2D& scroll) const; // card box incl. scroll + pop-in
    void   switchToWorkspace(const StripItem& it);
    void   dropOnWorkspace(const PHLWINDOW& w, const StripItem& it);
    void   swapTiles(int a, int b);           // drag a preview onto another → swap the two windows' places (real layout + overview)
    void   addWorkspace();                    // "+" card: create a workspace (animate it in, optionally follow)
    void   closeWorkspaceWindows(const StripItem& it); // middle-click a card: send-close every window on it
    void   setDesktopMode(bool on);           // flip grid<->canvas while open, gliding the previews (purely visual; never mutates a real window)
    LRect  closeButtonRect(const LRect& tile) const;   // desktop-mode "✕" hit/draw rect for a tile content box
    double newCardScale() const;              // 0..1(+overshoot) pop-in scale for the just-added card
    float  blurStrength() const;              // plugin:gloview:blur as 0..1 (float); 0 = off
    // keyboard navigation
    bool   keyMatches(int keycode, uint32_t mods, const char* cfgName, const char* fallback) const; // keycode+held mods ∈ the configured list (names or "shift+tab" combos; empty = disabled)
    int    keyIndex(int keycode, uint32_t mods, const char* cfgName, const char* fallback) const;   // 0-based position of keycode in the list, else -1 (number-row → strip card N)
    void   moveSelection(int dx, int dy);     // step the selection cursor to the nearest tile in a direction
    void   activateSelection();               // focus the selected window and dismiss
    void   syncFocus() const;                 // point Hyprland's real focus at the selected tile (passthrough keybinds)
    void   closeTileWindow(int i);            // send-close a tile's window, then reflow the rest
    void   replayReflow(std::vector<std::pair<PHLWINDOW, LRect>>& oldBoxes); // glide tiles into new slots after a removal
    void   syncTiles();                       // add/drop tiles when the displayed workspace's window set changes, then reflow
    void   stepWorkspace(int dir);            // scroll-wheel over the main area: show prev/next workspace card
    void   hideLayers();                      // fade out Top/Overlay layer surfaces (bars) per config
    void   restoreLayers();                   // restore the alphas hideLayers() saved
    void   restoreFill();                     // reset m_fillIgnoreSmall on every window (see renderWindowLive)
    void   dbg(const std::string& msg) const; // plugin:gloview:debug_logs gated logging
    void   damage() const;
};

} // namespace gloview

inline gloview::Overview* g_overview = nullptr;
