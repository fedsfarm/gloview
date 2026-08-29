# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GloView is a **Hyprland compositor plugin** (C++23, built as `gloview.so`) that draws a
macOS Mission Control-style window overview: a workspace strip on one edge + live previews
of the displayed workspace's windows below, over a blurred backdrop. Everything is rendered
compositor-side during Hyprland's render stage; the real windows are hidden while it is up.

## Commands

```sh
# Build (produces build/gloview.so against the installed hyprland headers)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Rebuild + hot-reload into the running Hyprland (use this when iterating)
cmake --build build --target reload   # build + gloviewunload + same-path plugin unload/load

# Nix (what CI runs — the only CI job — and how NixOS users build it)
nix build .#gloview --print-build-logs
nix develop                            # shell with the pinned Hyprland + clang-tools

# Invoke at runtime
hyprctl gloview        # toggle (exact hyprctl command, not lua-evaluated)
hyprctl gloviewclose   # close-only (animated), no-op if closed
hyprctl gloviewdesktop # flip the free-arrange canvas mode
hyprctl gloviewall     # toggle the all-workspaces (expo) view
hyprctl gloviewnext / gloviewprev
hyprctl gloviewunload  # immediate animation-free teardown; the `reload` target runs this before unloading
# dispatchers: gloview:toggle / open / close / desktop / allworkspaces / next / prev / setworkspace <id>
# lua (Lua config only): hl.plugin.gloview.{toggle,open,close,desktop,allworkspaces,next,prev,setworkspace}
```

There are **no tests** and no linter beyond compiler warnings (`-Wall -Wextra`, C++23).
"Tested" means it ran in Hyprland: build, `--target reload`, then exercise the real path
(binds, dispatchers, config options, layout modes, multi-monitor). `plugin:gloview:debug_logs = 1`
plus `hyprctl rollinglog` gives the `[gloview]` trace.

The built ABI must match the running Hyprland exactly — a header/Hyprland version skew
produces a `.so` that crashes on load. Local builds use the installed Hyprland via
`pkg-config hyprland`; `flake.nix` pins a **tagged** Hyprland release (never `main`, whose
dep tree is intermittently unbuildable) and downstream sets `inputs.hyprland.follows`.
When that pin and the version the source targets diverge, `nix build` (and therefore CI) is
compiling against a different compositor than local dev.

## Reloading (there is no `reload.sh` — and on purpose)

Users install/reload via **`hyprpm`** (`hyprpm.toml` + `flake.nix` ship in-tree). The dev
fast-loop is the CMake **`reload`** target (`cmake --build build --target reload`), which
builds then does a *plain same-path* `hyprctl plugin unload <so>; hyprctl plugin load <so>`.
It also unloads a hyprpm-installed copy at `/var/cache/hyprpm/$USER/gloview/gloview.so` —
that copy autoloads at session start and already holds the `shouldRenderWindow` hooks, and
two instances cannot trampoline the same function prologue.

No unique-`/tmp`-path copy is needed — **but only because of `-fno-gnu-unique`** (in
`CMakeLists.txt` `add_compile_options`). This flag is load-bearing for reload, not cosmetic:
a C++ `.so` emits its static-init guard vars (`_ZGV...`) as `STB_GNU_UNIQUE` symbols, which
make glibc mark the library **`NODELETE`**; `dlclose` then *never* unmaps it, so a same-path
`hyprctl plugin load` after unload returns the **cached old image** (the dlopen handle stays
byte-for-byte identical) and the reload silently runs **stale code** — `plugins list` shows
"ok" and the old version. `-fno-gnu-unique` emits those guards as plain symbols, so `dlclose`
truly unmaps (verified: `.so` segment count in `/proc/<hypr>/maps` goes 5→0 on unload) and
the same path re-`mmap`s the fresh build. **Do not drop this flag** — without it the entire
`reload` target is a no-op that looks like it worked. (A separate, real failure mode also
exists: unloading while the overview is *up* can crash mid-unload, leaving the `.so` mapped
so `getPluginByPath` refuses the reload — that's what `gloviewunload` below prevents.)

Gotcha — **a session can stay poisoned for one Hyprland lifetime.** If a *pre-flag* (or any
`NODELETE`) build was ever loaded, that image is stuck mapped at its path until the Hyprland
**process** ends; every later same-path `plugin load` of that path returns the stuck old
image, so the build path silently serves stale code no matter how you rebuild. glibc also
dedups by realpath **and** dev+inode, so a symlink/hardlink won't dodge it — only a real
`cp` to a new path, or restarting Hyprland **once**, clears it. After a clean restart the
first load maps the fixed build and same-path reload works forever after.

What makes the unload succeed is **`hyprctl gloviewunload`** (`Overview::hardClose`), run
first by the `reload` target: an immediate, animation-free teardown that drops all overlay
state and **cancels the recapture timer** synchronously, then `damage()`s so the next frame
rebuilds Hyprland's render pass with zero plugin-owned elements (the "flush frame"). Without
it, unloading while up frees an overlay pass element or the post-drop recapture-timer
callback — both code that lives in the `.so` — while Hyprland still references them → SEGV,
or the 150%-CPU IPC-dead spin. (`gloviewclose` only *starts* the close animation, so the
retired `reload.sh`'s `gloviewclose; sleep 0.4` was a timing *bet* that 360ms+ finished in
400ms.) The `reload` target keeps a 0.15s settle for that one flush frame. A plugin crash
takes down the whole session — prefer testing in a **nested** Hyprland instance.

## Architecture

Four translation units, layered by Hyprland coupling:

- **`src/main.cpp`** (~375 lines) — plugin entry points (`PLUGIN_INIT` / `PLUGIN_EXIT` /
  `PLUGIN_API_VERSION`) and nothing else. Registers all `plugin:gloview:*` config values,
  the dispatchers, the `hyprctl` commands, and (Lua config only) the `gloview.*` Lua
  functions. Owns the `g_overview` singleton. Every user-facing entry point exists here in
  **three parallel registrations** — `addDispatcherV2`, `registerHyprCtlCommand`
  (`.exact = true`), `addLuaFunction` — and a new one should add all three.
- **`src/overview.{cpp,hpp}`** (~3450 lines) — the `Overview` class. All Hyprland-coupled
  logic: rendering, input, snapshot capture, animation, drag-and-drop, workspace ops.
  `overview.hpp` is the map of it, and its comments carry most of the hard-won reasoning.
- **`src/preview_filter.{cpp,hpp}`** — scale-aware GPU downsampling for reduced live
  previews, with Hyprland's normal surface pass as the compatibility fallback.
- **`src/layout.{cpp,hpp}`** — **pure, Hyprland-independent** geometry. `LRect`, the
  `Engine` enum (`rows`/`grid`/`natural`), and `computeLayout()`. Add a layout engine here
  by extending `Engine` + `computeLayout` — the renderer and input code don't change.

### Config plumbing (the Lua trap)

Config values are registered with **`addConfigValueV2`** and the returned `SP<IValue>` is
kept in `gloview::g_config`; the `cfgInt/cfgFloat/cfgColor/cfgStr` helpers read through
that. **Do not use `HyprlandAPI::getConfigValue()`** — it does not observe values set from
a Lua `hl.config{}` config, so it returns the registered default and every setting looks
like it does nothing under a Lua config. Config is read live (per frame); colors are
`0xAARRGGBB`.

### Rendering model

The overview is an overlay pass, not a window. `Overview` listens to Hyprland's event bus
(`render.stage`, mouse button/move/**axis**, keyboard). At the **`RENDER_LAST_MOMENT`** stage
(after bars/overlay layers, so the overview paints over them) `renderStage()` adds, in order:

```
COverlayPass(Back)        backdrop, preview shadows/backings/titles (incl. the outgoing set mid-slide)
main window surfaces      renderMainWindows / renderPrevWindows: CSurfacePassElement per window
COverlayPass(MainFront)   preview hover/select RINGS  (on top of the surfaces)
COverlayPass(Mid)         strip band + card bodies + labels
strip window surfaces     renderStripWindows
COverlayPass(StripFront)  card active/hover RINGS     (on top of the card previews)
fly / drag phases         moved-to-workspace flight and the picked-up tile, chrome → surface → ring
COverlayPass(Front)       opted-in layer surfaces (above_namespaces), software cursor
```

`COverlayPass` is split into these phases (immediate-mode GL) because the live window previews can
only be **queued** pass elements, not immediate GL, and must layer *between* the chrome. Previews
draw *before* the strip so tiles flying in from real positions (possibly under the bar) pass
beneath it; the dragged tile draws *last* so it floats over the bar onto a card. The overview
damages the monitor every frame while up (continuous repaint clears transient blur/snapshot/hover
residue).

**Rings (hover / keyboard-select / active card) are drawn ON TOP of the live surfaces, as HOLLOW
borders** (`renderRing` → Hyprland's `renderBorder`), *inside* the tile/card box, so the band
overlaps (crops) the window's own edge — a real border, not an outline hugging the tile. A grown
`renderRect` can't do this: it is *filled*, so on top it hides the window, and as an underlay
(what this used to be) the surface's rounded corners overdraw the ring's arc and the border reads
as **notched** at the corners. Hyprland's border shader paints `borderSize` px inward from the
passed box and discards the interior; **`round` is the box's OUTER corner radius** — the shader
cuts the inside at `round - borderSize`, so pass the tile's own radius (`preview_round` /
`strip_card_round`), not a grown one, or the band's inner arc is a different circle than the
surface's corner and the two leave **gaps**. Ring drawing lives in `drawPreviewRing` /
`renderStripRings`, never in `drawPreviewTile` / `renderStrip` (those are Back/Mid phases, i.e.
*under* the surfaces).

**Both the strip cards and the main-area tiles render LIVE window surfaces**
(`renderWindowLive`, ported from Hyprspace's `renderWindowStub`: render-modif TRANSLATE+SCALE
+ `CSurfacePassElement` per surface), not snapshot textures — robust for windows on hidden
workspaces (their last-committed texture persists) and immune to the snapshot crop/staleness
bugs below. **`renderWindowLive` scales the surface by the window's `m_realSize->goal()`, NOT
`value()`** — the destination box and the opaque per-window backing are sized from `goal()`,
so scaling the surface by a mid-resize `value()` left it filling only part of that box and the
dark backing showed as **black strips** on the sides. goal() keeps surface and slot the same
size, so it always fills (position cancels in the translate remap, so only size matters).

**Thin dark edge lines (the "black lines", worst mid open-glide) — the SUB-PIXEL SEAM, and
the actual fix.** The opaque per-window backing is a *logical* rect drawn via `renderRect`
(Hyprland rounds it OUTWARD) while the live surface is positioned/clipped in *manual pixel*
space (`lb*scale`, floored). On any tile whose edge lands on a fractional pixel (an internal
tile boundary; the monitor-edge case stays integer, hence "only some edges"), the backing ends
up ~1px wider than the surface and peeks through as a thin dark line on the right/bottom — and
the staggered overshoot animation makes every frame's box fractional, so the line is worst
during the glide. **Two coupled fixes, verified in the VM with a magenta backing (0 peek px
across settled + 10 animation frames):** (1) `renderWindowLive` **over-covers** — `scaleMod`
fills BOTH axes (`max(sW,sH)`) plus a ~1.5px pad so the surface spills *past* the slot; the TL
stays anchored (the translate cancels `scaleMod`) and `clipPx` trims the overflow, so the
surface meets the slot edge with no gap. (2) the backing rects (`drawPreviewTile` preview_bg,
`renderStrip` per-window) are **inset 1px** so they sit strictly under the over-covered surface
and can never show. Do not "simplify" either half away — the seam returns. Diagnose regressions
by recoloring the backing magenta (`0xff14181f`→`0xffff00ff`) and grepping screenshots for it.

**A separate `small()` guard — `m_fillIgnoreSmall` (defensive, distinct cause):** Hyprland's
`CSurfacePassElement::getTexBox` *centers* a surface whose committed client buffer is smaller
than the window's reported size (`CWLSurface::small()`: an X11 client with size hints Hyprland
doesn't accommodate, or a window mid-resize) at its real size — a *big* uncovered margin, not a
1px seam. `renderWindowLive` sets **`m_fillIgnoreSmall = true`** so the buffer stretches to fill
instead. Hyprland never sets this flag itself, so **`restoreFill()`** blanket-resets it to
`false` on teardown (`deactivate`/`hardClose`/dtor, same triple-guard as `restoreLayers`). NOTE:
this path was NOT reproducible with Wayland clients (their `getReportedSize` returns the *acked*
size, so reported==buffer); it is kept as a guard for X11/edge cases, not the observed bug.

The strip is anchored to any edge via `plugin:gloview:anchor`
(`top|bottom|left|right`; horizontal row or vertical column), inset by `strip_offset` (0 =
flush, no auto bar gap), and the card group (workspace cards + trailing "+"/empty card, plus
an optional leading "All" card) scrolls as one unit when it overflows the band (mouse wheel →
`onMouseAxis`; active card auto-centered on open; off-band cards clip at the monitor edges).

Real windows are hidden while the overview is up via two **function hooks** on Hyprland's
`shouldRenderWindow` (the `(window, monitor)` and `(window)` overloads, located by demangled
signature in `initialize()`). `shouldHideWindow` is per-monitor: a window on another monitor
must keep rendering, or the overview blanks the other screens.

### Snapshots — the fragile part

Tiles *render live surfaces*, but snapshots are still taken: capturing forces a window on a
hidden workspace to actually paint, which is what leaves a usable live texture behind.
**Hyprland 0.56 dropped `CWindow::m_snapshotFB`**, so the plugin now owns the framebuffers
`makeSnapshotFB()` hands back (`m_snapFB`, `window* → SP<IFramebuffer>`); it is never
sampled, it only answers "is there already a good snapshot for this window?". Stale keys are
pruned each capture pass and the map is dropped on teardown. Several hard-won invariants live
in `captureSnapshots()`; read its comments before touching it:

- **Reentrancy guard `m_capturing`**: `makeSnapshot` drives its own nested render pass and
  re-emits render-stage events. `renderStage()` bails while `m_capturing` is set, else the
  overlay pass is re-added mid-snapshot → reentrant render → SEGV.
- **Off-render-loop recapture**: calling `makeSnapshot` mid-render crashes; recaptures after
  a drop are deferred onto a `CEventLoopTimer` (`scheduleRecapture()`).
- **Off-workspace windows**: forced to render via the hook (`forceRenderWindow` /
  `m_captureWin`) plus the workspace's `m_forceRendering`/`m_visible`, else the snapshot is
  blank/black.
- **Save AND restore both value AND goal**, and only *warp* the workspace's
  offset/alpha — never assign its animation goal. Poking the goal pins mid-animation
  workspaces to a stale offset; that corruption lives on the *workspace*, survives a gloview
  close, and breaks every later open until a full config reload.
- **Only snapshot presentable windows** — a window mid-move/resize can be transiently
  unmapped or workspace-less, and `makeSnapshot` then null-derefs its surface → crash.
- **`makeSnapshot` re-renders ONLY the window into a freshly-CLEARED, monitor-sized FB**
  (verified against Hyprland `src/render/Renderer.cpp`: clear→`renderWindow`→endRender). It
  can never contain the overlay, other windows, or the desktop — so a thumbnail showing "the
  overview / a dark block" is **not** a captured overview. It is the opaque backing
  (`0xff14181f`) showing through because the crop `source` rect sampled the *empty* part of
  the FB. `m_snapGeom` (`window* → monitor-local rect`) records the rect only for a
  **settled** window (value≈goal, client buffer matches its box), so a window retiled on a
  hidden workspace reuses its last good geometry instead of re-snapshotting into a
  stretched/black thumbnail. `renderWindow` scales a surface DOWN to fit its box but never
  UP, so a box wider than the committed buffer has transparent margin — crop to
  `min(box, buffer)`, never to the box.
- Do **not** `scheduleRecapture()` from `open()` to heal unsettled-at-open tiles: the timer
  can fire inside `makeSnapshot`'s own nested render pass (it pumps the event loop), re-enter
  `captureSnapshots`, toggle `m_capturing` out from under the in-flight snapshot, and spin
  Hyprland forever inside one `renderWindow`. Recapture is only safe from the post-drop timer.

### Workspaces: displayed vs live

The overview shows `m_workspace` (the *displayed* workspace); the monitor's real active
workspace only changes on `commitWorkspace()` at close (which also warps Hyprland's own
slide away). Consequences:

- **`m_committedFrom`** holds the workspace we switched *away* from: it is no longer active,
  so `shouldHideWindow`'s active-workspace rule stops covering it and it bleeds through the
  backdrop for the rest of the close animation unless hidden explicitly.
- **`exit_on_switch` vs the deactivate commit (gotcha).** `deactivate()` commits the
  *displayed* workspace if it differs from the live one. So when an external switch trips
  `exit_on_switch`, you MUST set `m_workspace = m->m_activeWorkspace` before `close()`, else
  deactivate reverts the very switch that triggered the exit. (`switchToWorkspace` only
  changes the displayed workspace, so the poll only fires on genuine external switches.)
- **`gloview:next` / `prev` / `setworkspace` work both in and out of the overview.** Open:
  they move the *displayed* workspace (like tab / the wheel) and the live desktop follows on
  close. Closed: `stepLiveWorkspace` walks `liveWorkspaceList()` — the same ordered list the
  strip would show, empty tail included — and switches the live desktop immediately.
  `setworkspace` never creates; it only switches to a workspace already on the monitor.
- **`dynamic_workspaces`** (default on, GNOME/hyprnome-style): only populated workspaces are
  listed and the strip ends in ONE empty card drawn as a real workspace (`isNew`, still
  create-on-use). The next empty card appears only once a window actually lands there, so two
  blank desktops never show at once. Emptied workspaces drop off the strip. Forces
  `show_empty` off.
- **`autodelete_empty`** lets Hyprland reap empties this monitor pins — it only affects
  workspaces held by a `persistent:true` rule (and gloview's own abandoned trailing
  workspace), and **releasing a persistent workspace lasts until the next config reload**. It
  skips the displayed workspace, anything visible on any monitor, workspaces holding any
  window mapped or not, scratchpads, named workspaces, and other monitors' workspaces.
- **Expo (`show_all_workspaces`)**: the main area shows every window on the monitor.
  `m_allOverride` is the runtime tri-state (-1 follow config / 0 forced off / 1 forced on),
  reset on full close. Picking a preview in expo **follows** it — the overview closes onto
  that window's workspace (`activateWindow`), which is separate from plain selection.
- **`addWorkspace()`** (the `+`/empty card): a brand-new empty workspace is reaped within a
  frame or two unless focused, so it is held `setPersistent(true)` (tracked in `m_newWs`) for
  the overview's lifetime and released on `deactivate()`/`close()`/dtor.
  `switch_on_new_workspace` decides whether to follow the display to it.
- **Workspace-switch slide**: `beginWsSlide` freezes the outgoing tiles into `m_prevTiles` and
  slides them off one edge while the incoming set slides in. Purely visual — the frozen set
  never takes hover/selection/drag and is dropped when the slide ends.
- **Move-to-workspace flight** (`m_flying`): after a drop, the preview keeps flying into its
  destination card and fades there; the destination card must not draw a window still in
  flight (`isFlying`). The real window is already moved.

### Input & navigation

All input lands in `onMouseButton` / `onMouseAxis` / `onKey`, each behind a `plugin:gloview:*`
toggle (read live). The load-bearing rules:

- **`onKey` is selective, not modal.** It consumes close (`key_close`, default `escape`),
  next/prev displayed workspace (`key_next_workspace` / `key_prev_workspace`, default `tab` /
  `shift+tab` — **held modifiers must match exactly**, so a `SUPER+Tab` toggle bind still
  passes through and closes), activate (Enter — focus the `m_selected` tile), close-window
  (`key_close_window`, default `d` — `closeTileWindow(m_selected)`; the overview stays up and
  `syncTiles` reflows; this is the ONLY per-window close, middle-click closes a whole
  workspace card), the arrow keys (`moveSelection` — nearest tile in a screen direction),
  desktop flip (`key_desktop`, Shift), expo toggle (`key_all_workspaces`, `a`), and the number
  row 1..0 (`key_workspace` — switch to the Nth strip card's workspace for real, updating
  `m_liveWsAtOpen` so the self-switch doesn't trip `exit_on_switch`). Everything else sets
  `cancel=false` so it reaches Hyprland's keybind manager (`passthrough_keys`) — that's how
  the user's launch/workspace binds still fire with the overview up. With
  `passthrough_keys=0` every key is swallowed (fully modal).
- **Each action is a config list of key NAMES.** `keyNameToCodes` (anon-ns, in overview.cpp)
  maps a name → evdev keycode(s): `shift`/`ctrl`/`alt`/`super` and `enter` resolve to BOTH
  codes, a bare digit is that number-row key, letters `a`..`z` allow hjkl/wasd; `mod+key`
  combos are supported. `keyMatches` tests membership; `keyIndex` returns the matched token's
  *slot* (used only by `key_workspace`: slot picks card N). Empty string disables an action
  (its key passes through). Stored as evdev, not xkb — layout-independent.
- **`m_selected`** is the keyboard cursor; `focus_follows_mouse` syncs it to the hovered tile
  on hover *change* (so arrow-nav isn't clobbered while the pointer is still). Drawn as a
  `select_border` ring, distinct from the hover ring.
- **`syncFocus()` — passthrough keybinds act on the SELECTED tile.** `passthrough_keys` lets a
  user's own bind (e.g. `killactive`) fire while the overview is up, and such binds act on
  Hyprland's *focused* window. Without syncing, that focus stayed on whatever was focused
  *before* the overview opened, so a close/move hotkey hit the **wrong** window. `syncFocus()`
  (`fullWindowFocus`, called from `moveSelection` + the `focus_follows_mouse` branch of
  `updateHover` on every `m_selected` change) keeps Hyprland's focus on the selected tile.
  Guarded to the monitor's **active** workspace — when the overview is displaying a non-live
  workspace its tiles are on a hidden workspace and focusing one would desync focus from the
  live desktop (and `fullWindowFocus` does NOT switch the active workspace, so the
  same-workspace path only moves input focus, no `exit_on_switch` trip). Verified in the VM
  (A/B: hovering tile B with focus on A → killactive closes B with the fix, A without it).
  NOTE this leaves focus on the last-selected tile after an Esc close — intentional.
- **Closing a window** is async: `sendClose()` doesn't unmap immediately. `syncTiles()` runs
  each frame and, when a tile's weak window ptr goes null (or the window set changes),
  rebuilds + `replayReflow`s the survivors. `replayReflow` is the shared reflow tail (also
  used by drop-to-workspace and swap): capture survivor boxes → rebuild → glide
  natural→target with `m_progress` pinned at 1.
- **`buildTiles()` and `syncTiles()` must agree on membership** — both go through
  `tileBelongs()`. Divergence shows up as tiles that appear or vanish on the next frame.
- **`scroll_switches_workspace`**: wheel is routed by cursor position — over the strip band it
  scrolls the cards, over the main area it `stepWorkspace`s prev/next.
- **`drag_to_swap`** (grid mode only): dropping a dragged preview onto another swaps the two
  windows — in the real Hyprland layout AND the overview — via `swapTiles`, a **near-exact
  clone of `dropOnWorkspace`'s tail**: mutate the real layout, then `replayReflow`. Two
  load-bearing rules, learned the hard way:
    - **`switchTargets` (preserveFocus default) only does `a->swap(b)`, which swaps the two
      targets' ORDER in the space + algorithm and does NOT recalculate** — so a bare
      `switchTargets` changes nothing visible and vanishes on close. You MUST follow it with
      `ta->space()->recalculate()`.
    - **After the real swap you MUST rebuild the overview from the new geometry
      (`replayReflow`), not hand-swap the tile slots.** A manual `std::swap(tile.target)`
      leaves the slot pointing one place while the window's real `goal()` moved elsewhere;
      `renderWindowLive` maps the surface by `goal()`, so it lands OUTSIDE the tile and only
      the dark `preview_bg` backing shows — a tile that reads as **black, or an empty card
      with no preview**.
  The recalc runs inline at drop time (an input-event handler, not a render stage → no
  reentrancy). Guarded to same-workspace, non-fullscreen pairs. Engine note:
  `replayReflow`→`computeLayout` re-derives slots from geometry, so `rows`/`natural` reflect
  the swap visually; `grid` slots are index-based, so there the real windows swap but the
  cells don't visibly move. The workspace-card drop check runs first; desktop/canvas mode
  keeps its park-in-place drop instead.

### Desktop (free-arrange) mode

- **`gloview:desktop` / `toggleDesktop()`** is a **purely visual** free-arrange canvas.
  It NEVER floats/moves/resizes a real window — earlier versions did
  (`changeFloatingMode` on enter or on drop) and the user's tiled desktop came back
  floating; **do not reintroduce any real-window mutation here.** `layoutTiles()` (desktop
  branch) fits the WHOLE monitor into the usable area and places each preview at its real
  scaled position. Dragging a preview just parks its tile box in `m_canvasPos`
  (`window* → LRect`), which `layoutTiles` honours so the arrangement is sticky across the
  per-frame `syncTiles` rebuilds; cleared on open / mode-flip / close.
- **Opening/closing a window in the canvas must NOT move the OTHER previews.** A non-dragged
  preview is positioned by the window's REAL geometry, and Hyprland re-tiles the live windows
  whenever one is added/removed — so without intervention every survivor preview jumps. Two
  coupled freezes in the `syncTiles` add/remove path keep them put: (1) before the
  `replayReflow`, `syncTiles` parks every surviving tile's settled box into `m_canvasPos`,
  so only the newcomer flows to its real scaled spot; (2) `tileContentBox()` returns the
  **slot as-is** in desktop mode instead of refitting to the window's live `goal()` aspect —
  else a survivor Hyprland re-tiled to a new shape reshapes/shrinks inside its frozen slot.
  Residual (inherent to LIVE previews): a re-tiled survivor's surface is over-covered to fill
  its frozen slot, so its *content* looks zoomed/cropped — position and size stay put.
- **`toggleDesktop()` flips the MODE while open** (grid ⇄ canvas via `setDesktopMode`, which
  clears `m_canvasPos` then `replayReflow`s the previews) — it does not close.
- **Desktop `✕` buttons**: drawn per tile in `drawPreviewTile` (top-right, `closeButtonRect`
  — ONE formula shared by draw + hit-test so they can't disagree). The press handler must
  set `m_pressTile = PRESS_CONSUMED` so the matching *release* doesn't fall through to the
  empty-space `exit_on_click` and close the overview. The `✕` glyph is pre-rendered once in
  `buildTiles` (`m_closeGlyph`) — `renderText` mid-pass (inside the COverlayPass callback)
  is unsafe.

### Bar / layer-shell hiding

`hide_top_layers` / `hide_overlay_layers` fade the monitor's `m_layerSurfaceLayers[2]` (Top)
and `[3]` (Overlay) — bars, popups, incl. `quickshell-*` surfaces. `hideLayers()`
stashes each surface's `m_alpha->goal()` then drives `*ls->m_alpha = 0`; `restoreLayers()`
puts the goals back. **Restore is triple-guarded**: `close()` (so bars fade back in *over* the
close animation, not in a pop), `deactivate()` (safety net), and the destructor (never leave a
bar stuck at alpha 0 if torn down mid-hide). Same technique as Hyprspace's `hideRealLayers`.
The opposite case is `above_namespaces` (comma/space list, trailing `*` glob, and any
namespace containing `aboveoverview`): `isAboveLayer` / `renderAboveLayers` re-render those
layer surfaces *on top of* the overview.

## Conventions

- All geometry is **monitor-local logical pixels** in `LRect`; the strip additionally stores
  window slots as `0..1` fractions of the monitor.
- **Immediate-mode chrome must be pre-scaled logical→PIXEL before drawing (`pxb`/`pxr`).**
  Hyprland's `renderRect`/`renderTexture`/`renderRoundedShadow` feed the box STRAIGHT to
  `projectBoxToTarget`, which expects transformed monitor-**pixel** coords and applies NO
  monitor scale itself (verified in Renderer.cpp: `clipBox`/`scaledWindowBox` are
  `.scale(m_scale)`'d *before* the projection). So every gloview chrome box — authored in
  logical `LRect` — is wrapped in `pxb(box, m->m_scale)` and its round radius / blur range in
  `pxr(r, s)` / `r*s`. At `m_scale==1` this is a no-op, which is why it went unnoticed until a
  bug report on a **fractional-scale (1.2) / HiDPI** monitor: without it the whole chrome
  renders at `1/scale` size and top-left-biased while the live window **surfaces** land
  correctly → detached backings + a backdrop covering only the top-left `1/scale` of the
  screen. Verified in the VM at scale 1.25. Do NOT instead push a global scale render-modif:
  `renderRect`'s blur path intersects the *un-modif'd* logical box for its damage region, so
  the backdrop/strip blur would clip to `1/scale`. Input/layout stay in logical space (cursor
  coords are logical) — pixel conversion is render-only.
- Animation: `m_progress` drives the chrome (backdrop+strip) reveal. A post-drop reflow uses
  a *separate* `m_reflowing` timer to glide tiles into new slots while `m_progress` stays
  pinned at 1, so the strip doesn't re-slide and the backdrop doesn't flash on a drop.
- **Deactivation is deferred by one frame (`m_pendingDeactivate`).** `shouldRenderWindow` is
  evaluated early in the frame, before the `RENDER_LAST_MOMENT` pass, so flipping `m_active`
  mid-frame skips the overlay on a frame whose real windows are already suppressed → one
  fully transparent frame (the close flicker).
- **Close handoff invariant: `close()` re-seeds every tile's `natural` to the window's real
  settled geometry (`m_realPosition/Size->goal()`).** `renderMainWindows` assumes "progress 0
  == real geometry" so the opaque preview lands exactly where the real window reappears. But
  `replayReflow` repurposes `natural` as the glide *start* box, so after any reflow — and
  ALWAYS in desktop mode, entered via `setDesktopMode`→`replayReflow` — `natural` is no longer
  the real position. Without the re-seed the windows visibly **jump on close**.
- **A new `plugin:gloview:*` option touches four places**: register it in `PLUGIN_INIT`
  (`addInt`/`addColor`/`addFloat`/`addStr`), read it with the matching `cfg*` using the *same*
  fallback, add the README config-table row, and add it to **both** README example blocks
  (Lua and `hyprland.conf`).
- No clang-format config; match the surrounding code (4-space indent, `camelCase`, `g_`
  globals, `PascalCase` types). Comments explain *why*, and specifically which
  Hyprland/glibc/driver behaviour a line is dodging — do not delete one without understanding
  what it defends against.
- The version string lives in four places that must stay in sync: `CMakeLists.txt`,
  `flake.nix`, the `PLUGIN_INIT` return value, and `packaging/aur/PKGBUILD`.
