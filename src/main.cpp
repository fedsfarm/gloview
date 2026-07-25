#include <memory>
#include <stdexcept>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "overview.hpp"

inline HANDLE                         g_handle = nullptr;
inline std::unique_ptr<gloview::Overview> g_overviewOwned;

namespace {
// Keep the SP so cfg* can read the resolved value() — see ConfigRegistry in overview.hpp.
void addInt(const char* name, Config::INTEGER fallback) {
    auto v = makeShared<Config::Values::CIntValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.ints[name] = v;
}
void addColor(const char* name, Config::INTEGER fallback) {
    auto v = makeShared<Config::Values::CColorValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.colors[name] = v;
}
void addFloat(const char* name, Config::FLOAT fallback) {
    auto v = makeShared<Config::Values::CFloatValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.floats[name] = v;
}
void addStr(const char* name, Config::STRING fallback) {
    auto v = makeShared<Config::Values::CStringValue>(name, "", fallback);
    HyprlandAPI::addConfigValueV2(g_handle, v);
    gloview::g_config.strings[name] = v;
}

SDispatchResult dispToggle(std::string) {
    if (g_overview)
        g_overview->toggle();
    return {.success = true};
}
SDispatchResult dispOpen(std::string) {
    if (g_overview)
        g_overview->open();
    return {.success = true};
}
SDispatchResult dispClose(std::string) {
    if (g_overview)
        g_overview->close();
    return {.success = true};
}
SDispatchResult dispDesktop(std::string) {
    if (g_overview)
        g_overview->toggleDesktop();
    return {.success = true};
}
SDispatchResult dispAllWorkspaces(std::string) {
    if (g_overview)
        g_overview->toggleAllWorkspaces();
    return {.success = true};
}
SDispatchResult dispNext(std::string) {
    if (g_overview)
        g_overview->nextWorkspace();
    return {.success = true};
}
SDispatchResult dispPrev(std::string) {
    if (g_overview)
        g_overview->prevWorkspace();
    return {.success = true};
}
// gloview:setworkspace <id> — show that workspace in the overview.
SDispatchResult dispSetWorkspace(std::string arg) {
    if (!g_overview)
        return {.success = true};
    try {
        const int id = std::stoi(arg);
        if (!g_overview->setWorkspace(id))
            return {.success = false, .error = "gloview: no workspace " + arg + " on this monitor"};
    } catch (const std::exception&) { // stoi throws on empty / non-numeric
        return {.success = false, .error = "gloview:setworkspace expects a workspace id"};
    }
    return {.success = true};
}

int luaToggle(lua_State*) {
    if (g_overview)
        g_overview->toggle();
    return 0;
}
int luaOpen(lua_State*) {
    if (g_overview)
        g_overview->open();
    return 0;
}
int luaClose(lua_State*) {
    if (g_overview)
        g_overview->close();
    return 0;
}
int luaDesktop(lua_State*) {
    if (g_overview)
        g_overview->toggleDesktop();
    return 0;
}
int luaAllWorkspaces(lua_State*) {
    if (g_overview)
        g_overview->toggleAllWorkspaces();
    return 0;
}
int luaNext(lua_State*) {
    if (g_overview)
        g_overview->nextWorkspace();
    return 0;
}
int luaPrev(lua_State*) {
    if (g_overview)
        g_overview->prevWorkspace();
    return 0;
}
// hl.plugin.gloview.setworkspace(2) — returns true when the workspace was found.
int luaSetWorkspace(lua_State* L) {
    const bool ok = g_overview && g_overview->setWorkspace(static_cast<int>(luaL_checkinteger(L, 1)));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    // --- layout / main area ---
    addStr("plugin:gloview:layout", "rows");              // rows | grid | natural
    addInt("plugin:gloview:gap", Config::INTEGER{34});    // min spacing between tiles (px)
    addInt("plugin:gloview:padding", Config::INTEGER{80}); // left/right outer margin (px)
    addInt("plugin:gloview:padding_top", Config::INTEGER{40});    // gap below the strip
    addInt("plugin:gloview:padding_bottom", Config::INTEGER{70}); // bottom margin
    addFloat("plugin:gloview:max_scale", Config::FLOAT{1.0F});    // never upscale past real*this
    addInt("plugin:gloview:duration", Config::INTEGER{360});      // open/close animation (ms)
    addInt("plugin:gloview:preview_round", Config::INTEGER{12});
    addFloat("plugin:gloview:blur", Config::FLOAT{1.0F});         // backdrop/strip blur strength 0..1 (0 = off; floats allowed)

    // --- in-overview transitions ---
    addInt("plugin:gloview:switch_animation", Config::INTEGER{1});  // slide the tiles when the displayed workspace changes
    addInt("plugin:gloview:switch_duration", Config::INTEGER{260}); // that slide's length (ms)
    addInt("plugin:gloview:move_animation", Config::INTEGER{1});    // fly a dropped window's preview into its new workspace card
    addInt("plugin:gloview:move_duration", Config::INTEGER{240});   // that flight's length (ms)

    // --- workspace strip ---
    addStr("plugin:gloview:anchor", "");                          // top | bottom | left | right — which edge the strip attaches to (default top)
    addStr("plugin:gloview:bar_position", "top");                 // deprecated alias for anchor (top | bottom); used only if anchor is unset
    addInt("plugin:gloview:strip_height", Config::INTEGER{150});  // band thickness (perpendicular to its edge)
    addInt("plugin:gloview:strip_offset", Config::INTEGER{0});    // inset from the anchored edge (0 = flush, no gap)
    addInt("plugin:gloview:strip_margin", Config::INTEGER{22});
    addInt("plugin:gloview:strip_gap", Config::INTEGER{18});
    addInt("plugin:gloview:strip_card_round", Config::INTEGER{10});

    // --- colors (0xAARRGGBB) ---
    addColor("plugin:gloview:backdrop_color", Config::INTEGER{0x73070a10LL});       // dim + blur
    addColor("plugin:gloview:strip_band_color", Config::INTEGER{0x24ffffffLL});     // faint top band
    addColor("plugin:gloview:strip_card_color", Config::INTEGER{0x3a0e131cLL});
    addColor("plugin:gloview:strip_active_color", Config::INTEGER{0x4d1c2c44LL});
    addColor("plugin:gloview:strip_active_border", Config::INTEGER{0xf0ffffffLL});
    addColor("plugin:gloview:strip_hover_border", Config::INTEGER{0x80ffffffLL});
    addColor("plugin:gloview:strip_plus_color", Config::INTEGER{0xd0eef4ffLL});
    addColor("plugin:gloview:preview_bg", Config::INTEGER{0xff14181fLL});           // opaque backing under a preview's live surface
    addColor("plugin:gloview:shadow_color", Config::INTEGER{0x70000000LL});
    addColor("plugin:gloview:hover_border", Config::INTEGER{0xf0ffffffLL});
    addInt("plugin:gloview:hover_border_size", Config::INTEGER{3});         // hovered preview ring thickness (px)
    addInt("plugin:gloview:strip_active_border_size", Config::INTEGER{2});  // active/expo card ring thickness (px)
    addInt("plugin:gloview:strip_hover_border_size", Config::INTEGER{2});   // hovered card ring thickness (px)

    // --- input / keyboard navigation ---
    addInt("plugin:gloview:focus_follows_mouse", Config::INTEGER{1});      // keyboard selection tracks the hovered tile
    addInt("plugin:gloview:scroll_switches_workspace", Config::INTEGER{1});// wheel over the main area steps prev/next workspace
    addInt("plugin:gloview:passthrough_keys", Config::INTEGER{1});         // unhandled keys reach Hyprland (keybinds keep working)
    addInt("plugin:gloview:exit_on_click", Config::INTEGER{1});           // click on empty space dismisses the overview
    addInt("plugin:gloview:debug_logs", Config::INTEGER{0});              // verbose [gloview] logging
    addInt("plugin:gloview:select_border_size", Config::INTEGER{3});      // keyboard-selected tile ring thickness (px)
    addColor("plugin:gloview:select_border", Config::INTEGER{0xf066ccffLL}); // keyboard-selected tile ring (distinct from hover)

    // --- keybinds (key names: esc/tab/enter/left/right/up/down/shift/hjkl/f1…/super/ctrl/alt;
    //     a bare digit = that number-row key; comma/space separated; modifier combos as
    //     "shift+tab" / "ctrl+shift+k"; "" disables → key falls through) ---
    addStr("plugin:gloview:key_close", "escape");              // dismiss (tab now cycles workspaces; add it back here to restore the old behavior)
    addStr("plugin:gloview:key_next_workspace", "tab");        // cycle the displayed workspace forward (wraps); "" disables
    addStr("plugin:gloview:key_prev_workspace", "shift+tab");  // …backward
    addStr("plugin:gloview:key_activate", "enter");       // focus the selected tile
    addStr("plugin:gloview:key_close_window", "d");       // send-close the selected tile's window (stays open, reflows); "" disables
    addStr("plugin:gloview:key_left", "left");            // move selection
    addStr("plugin:gloview:key_right", "right");
    addStr("plugin:gloview:key_up", "up");
    addStr("plugin:gloview:key_down", "down");
    addStr("plugin:gloview:key_desktop", "shift");        // flip canvas<->grid
    addStr("plugin:gloview:key_all_workspaces", "a");     // toggle the all-workspaces (expo) view; "" disables
    addStr("plugin:gloview:key_workspace", "1,2,3,4,5,6,7,8,9,0"); // each key switches to the Nth strip card's workspace (slot position = card index)

    // --- workspace scope / strip contents ---
    addInt("plugin:gloview:show_all_workspaces", Config::INTEGER{0}); // main area shows every window on the monitor (expo), not just the displayed workspace
    addInt("plugin:gloview:show_empty", Config::INTEGER{1});          // keep empty workspaces as strip cards
    // gnome / hyprnome-style workspaces: only populated workspaces are listed and the strip
    // always ends in ONE empty one (drawn as a card, not a "+"). Stepping onto it or dropping
    // a window into it materializes it and a fresh empty tail appears; emptied workspaces drop
    // off the strip and Hyprland reaps them. Forces show_empty off.
    addInt("plugin:gloview:dynamic_workspaces", Config::INTEGER{1});
    // Release the persistence pin on empty workspaces so Hyprland's normal reaping destroys
    // them. Hyprland already reaps unpinned empties; this is for `persistent:true` ones (and
    // gloview's own abandoned tail). Note it outlives the session's config: a persistent
    // workspace released here does not come back until the next config reload. Set to 0 if
    // you keep `persistent:true` workspaces you want left alone.
    addInt("plugin:gloview:autodelete_empty", Config::INTEGER{1});
    addInt("plugin:gloview:show_workspace_labels", Config::INTEGER{1}); // workspace names above the strip cards (off = cards grow into the freed band)
    addInt("plugin:gloview:show_window_labels", Config::INTEGER{1});    // window title pill under a hovered/selected preview
    addInt("plugin:gloview:show_special", Config::INTEGER{0});        // include the special (scratchpad) workspace as a strip card
    addInt("plugin:gloview:strip_all_card", Config::INTEGER{0});      // show a leading "All workspaces" card on the strip that toggles the expo view
    addInt("plugin:gloview:switch_on_drop", Config::INTEGER{0});      // dropping a window on a card also follows it to that workspace
    addInt("plugin:gloview:drag_to_swap", Config::INTEGER{1});        // dragging a preview onto another swaps the two windows' places
    addInt("plugin:gloview:exit_on_switch", Config::INTEGER{0});      // dismiss the overview when the live workspace changes underneath
    addInt("plugin:gloview:switch_on_new_workspace", Config::INTEGER{1}); // clicking "+" follows the display to the new workspace
    addColor("plugin:gloview:close_button_color", Config::INTEGER{0xe6e23b3bLL}); // desktop-mode "✕" close button fill

    // --- bar / layer-shell hiding (waybar, quickshell-based bars, …) ---
    addInt("plugin:gloview:hide_top_layers", Config::INTEGER{0});     // fade out Top layer surfaces (bars) while the overview is up
    addInt("plugin:gloview:hide_overlay_layers", Config::INTEGER{0}); // fade out Overlay layer surfaces (popups/notifications)
    addStr("plugin:gloview:above_namespaces", "");                    // comma/space list of layer namespaces to draw ABOVE the overview (supports trailing '*' glob); namespaces containing "aboveoverview" are always treated this way

    g_overviewOwned = std::make_unique<gloview::Overview>(handle);
    g_overview      = g_overviewOwned.get();
    if (!g_overview->initialize()) {
        HyprlandAPI::addNotification(handle, "[gloview] initialization failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 6000);
        g_overview = nullptr;
        g_overviewOwned.reset();
        // Throw so Hyprland ejects the plugin (it catches this and runs unloadPlugin).
        // Returning normally instead kept a half-alive instance loaded — dispatchers and
        // config registered but no render hooks — which then blocked every later load
        // attempt (two instances can't hook shouldRenderWindow twice).
        throw std::runtime_error("[gloview] initialization failed");
    }

    HyprlandAPI::addDispatcherV2(handle, "gloview:toggle", dispToggle);
    HyprlandAPI::addDispatcherV2(handle, "gloview:open", dispOpen);
    HyprlandAPI::addDispatcherV2(handle, "gloview:close", dispClose);
    HyprlandAPI::addDispatcherV2(handle, "gloview:desktop", dispDesktop); // toggle the free-arrange desktop mode
    HyprlandAPI::addDispatcherV2(handle, "gloview:allworkspaces", dispAllWorkspaces); // open/toggle the all-workspaces expo view
    // Workspace navigation inside the overview (no-ops while it is closed). Like tab and the
    // scroll wheel these move the DISPLAYED workspace; the live desktop follows on close.
    HyprlandAPI::addDispatcherV2(handle, "gloview:next", dispNext);
    HyprlandAPI::addDispatcherV2(handle, "gloview:prev", dispPrev);
    HyprlandAPI::addDispatcherV2(handle, "gloview:setworkspace", dispSetWorkspace);

    // hyprctl command (exact, not lua-evaluated) — reliable invoke path:  hyprctl gloview
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloview",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggle();
                                                        return "ok\n";
                                                    },
                                                });

    // close-only (no-op if not open): dismiss the overlay before unloading.
    // Unloading mid-render with the overview up tears down the render hooks while
    // an in-flight frame still references them → Hyprland crash.
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewclose",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->close();
                                                        return "ok\n";
                                                    },
                                                });

    // UNLOAD-safe teardown:  hyprctl gloviewunload  — run by the `reload` target
    // before `plugin unload`. Unlike gloviewclose (which only *starts* the close
    // animation), this drops all overlay state + the recapture timer synchronously,
    // so the next frame renders with no plugin-owned pass elements and dlclose can't
    // free a callback that is still referenced mid-frame. Makes reload deterministic.
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewunload",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->hardClose();
                                                        return "ok\n";
                                                    },
                                                });

    // free-arrange desktop mode toggle:  hyprctl gloviewdesktop
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewdesktop",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggleDesktop();
                                                        return "ok\n";
                                                    },
                                                });

    // all-workspaces (expo) view toggle:  hyprctl gloviewall — opens into expo if closed
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewall",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->toggleAllWorkspaces();
                                                        return "ok\n";
                                                    },
                                                });

    // step the displayed workspace:  hyprctl gloviewnext / gloviewprev
    // (setworkspace takes an argument, so it is dispatcher-only: hyprctl dispatch gloview:setworkspace 2)
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewnext",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->nextWorkspace();
                                                        return "ok\n";
                                                    },
                                                });
    HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{
                                                    .name  = "gloviewprev",
                                                    .exact = true,
                                                    .fn    = [](eHyprCtlOutputFormat, std::string) -> std::string {
                                                        if (g_overview)
                                                            g_overview->prevWorkspace();
                                                        return "ok\n";
                                                    },
                                                });

    const bool isLua = Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA;
    bool       luaOk = false;
    if (isLua) {
        luaOk = HyprlandAPI::addLuaFunction(handle, "gloview", "toggle", luaToggle);
        HyprlandAPI::addLuaFunction(handle, "gloview", "open", luaOpen);
        HyprlandAPI::addLuaFunction(handle, "gloview", "close", luaClose);
        HyprlandAPI::addLuaFunction(handle, "gloview", "desktop", luaDesktop);
        HyprlandAPI::addLuaFunction(handle, "gloview", "allworkspaces", luaAllWorkspaces);
        HyprlandAPI::addLuaFunction(handle, "gloview", "next", luaNext);
        HyprlandAPI::addLuaFunction(handle, "gloview", "prev", luaPrev);
        HyprlandAPI::addLuaFunction(handle, "gloview", "setworkspace", luaSetWorkspace);
    }
    (void)luaOk;

    HyprlandAPI::reloadConfig();

    return {
        .name        = "GloView",
        .description = "macOS Mission Control-style overview",
        .author      = "Vergil",
        .version     = "0.3.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overview = nullptr;
    g_overviewOwned.reset();
}
