# GloView
[![license](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://github.com/fedsfarm/gloview/blob/main/LICENSE) [![Matrix](https://img.shields.io/badge/Matrix-Join%20chat-green?logo=matrix&logoColor=white)](https://escape.feds.farm/#main:feds.farm) [![Donate](https://img.shields.io/badge/Donate-XMR%20%C2%B7%20BTC%20%C2%B7%20ETH-orange?logo=monero&logoColor=white&labelColor=555)](#donate)

https://github.com/user-attachments/assets/0a3d812a-eae0-4ca5-8698-7a006e540857

A better macOS Mission Control-style overview plugin for Hyprland

## Install

Via hyprpm:

```sh
hyprpm add https://github.com/fedsfarm/gloview
hyprpm enable gloview
```

### Arch (AUR)

```sh
yay -S gloview
```

### Nixos

```nix
inputs.gloview = { url = "github:fedsfarm/gloview"; inputs.hyprland.follows = "hyprland"; };
```
```nix
wayland.windowManager.hyprland = {
  enable = true;
  plugins = [ inputs.gloview.packages.${pkgs.system}.gloview ];
  settings.bind = [ "SUPER, TAB, gloview:toggle" ];
};
```

## Manual build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/gloview.so`. The ABI must match the running Hyprland exactly —
build against the same headers, or a version skew gives a `.so` that crashes on
load. While iterating, `cmake --build build --target reload` rebuilds and
hot-reloads into the running Hyprland.

## Usage

Dispatchers: `gloview:toggle`, `gloview:open`, `gloview:close`, `gloview:desktop`,
`gloview:allworkspaces` (the all-workspaces "expo" view — opens into it if closed)
Or `hyprctl gloview` / `gloviewclose` / `gloviewdesktop` / `gloviewall`

Workspace navigation: `gloview:next`, `gloview:prev`, `gloview:setworkspace <id>`
(or `hyprctl gloviewnext` / `gloviewprev`). **Inside** the overview they behave like
`tab` and the scroll wheel: they move the *displayed* workspace and commit it to the
live desktop on close. **Outside** it they switch the live desktop right away, walking
the same ordered list the strip shows — so one pair of binds drives workspaces
everywhere. With `dynamic_workspaces` on, stepping past the last populated workspace
lands you on a fresh empty one and the workspace you left behind is reaped if you
emptied it (`gloview:setworkspace` never creates; it only switches to a workspace that
already exists on the monitor).

Lua:

```lua
hl.bind("SUPER + TAB", hl.plugin.gloview.toggle)
hl.bind("SUPER + SHIFT + TAB", hl.plugin.gloview.desktop)
hl.bind("SUPER + CTRL + TAB", hl.plugin.gloview.allworkspaces)

hl.bind("SUPER + bracketright", hl.plugin.gloview.next)
hl.bind("SUPER + bracketleft", hl.plugin.gloview.prev)
hl.bind("SUPER + 2", function() hl.plugin.gloview.setworkspace(2) end)
```

```ini
bind = SUPER, TAB, gloview:toggle
bind = SUPER SHIFT, TAB, gloview:desktop
bind = SUPER CTRL, TAB, gloview:allworkspaces

bind = SUPER, bracketright, gloview:next
bind = SUPER, bracketleft, gloview:prev
bind = SUPER, 2, gloview:setworkspace, 2
```

## Config

All keys live under `plugin:gloview:*`. Colors are `0xAARRGGBB` integers.

- **`rows`** (default) — macOS-like: previews keep their aspect ratio and are packed into balanced rows, with the row count chosen to make the previews as large as possible. Reads spatially like the real desktop.
- **`grid`** — uniform cells, one preview centered in each.
- **`natural`** — keeps each window's real on-screen position, uniformly scaling the whole arrangement to fit.

| Option | Type | Default | Description |
|---|---|---|---|
| `layout` | `rows` \| `grid` \| `natural` | `rows` | Main-area preview layout engine |
| `gap` | int (px) | `34` | Min spacing between window previews |
| `padding` | int (px) | `80` | Left/right outer margin of the preview area |
| `padding_top` | int (px) | `40` | Extra gap between the strip and the previews |
| `padding_bottom` | int (px) | `70` | Bottom outer margin |
| `max_scale` | float | `1.0` | Never enlarge a preview past real size × this |
| `preview_filter` | `box4` \| `box16` \| `linear` | `box4` | Scale-aware GPU downsampling for reduced previews: `box4` uses a 2×2 kernel, `box16` uses a smoother but costlier 4×4 kernel, and `linear` restores Hyprland's normal single-sample path |
| `duration` | int (ms) | `360` | Open/close animation length |
| `preview_round` | int (px) | `12` | Window preview corner radius |
| `blur` | float `0`..`1` | `1.0` | Backdrop + strip blur strength (`0` = off; fractions allowed) |
| `switch_animation` | bool (0/1) | `1` | Slide the previews sideways when the displayed workspace changes (the outgoing set leaves as the incoming one arrives) |
| `switch_duration` | int (ms) | `260` | Length of that slide |
| `move_animation` | bool (0/1) | `1` | A window dropped on a workspace card keeps flying into it, shrinking into its slot; the card holds off drawing it until it lands |
| `move_duration` | int (ms) | `240` | Length of that flight |
| `anchor` | `top` \| `bottom` \| `left` \| `right` | `top` | Edge the workspace strip attaches to |
| `strip_offset` | int (px) | `0` | Inset from the anchored edge (0 = flush, no gap) |
| `strip_height` | int (px) | `150` | Strip band thickness, label included |
| `strip_margin` | int (px) | `22` | Padding around the strip |
| `strip_gap` | int (px) | `18` | Spacing between workspace cards |
| `strip_card_round` | int (px) | `10` | Workspace card corner radius |
| `backdrop_color` | color | `0x73070a10` | Dim + blur fill over the desktop |
| `strip_band_color` | color | `0x24ffffff` | Band behind the cards |
| `strip_card_color` | color | `0x3a0e131c` | Inactive workspace card fill |
| `strip_active_color` | color | `0x4d1c2c44` | Active workspace card fill |
| `strip_active_border` | color | `0xf0ffffff` | Active card border |
| `strip_active_border_size` | int (px) | `2` | Active card border thickness |
| `strip_hover_border` | color | `0x80ffffff` | Hovered card border |
| `strip_hover_border_size` | int (px) | `2` | Hovered card border thickness |
| `strip_plus_color` | color | `0xd0eef4ff` | The "+" glyph |
| `preview_bg` | color | `0xff14181f` | Opaque backing drawn under a preview's live surface |
| `shadow_color` | color | `0x70000000` | Window preview drop shadow |
| `hover_border` | color | `0xf0ffffff` | Hovered window preview border |
| `hover_border_size` | int (px) | `3` | Hovered preview border thickness |
| `select_border` | color | `0xf066ccff` | Keyboard-selected preview border |
| `select_border_size` | int (px) | `3` | Keyboard-selected preview border thickness |
| `focus_follows_mouse` | bool (0/1) | `1` | Keyboard selection tracks the hovered preview |
| `scroll_switches_workspace` | bool (0/1) | `1` | Wheel over the main area steps prev/next workspace |
| `passthrough_keys` | bool (0/1) | `1` | Let keys the overview doesn't use reach Hyprland (keybinds keep working) |
| `key_close` | key names | `escape` | Keys that dismiss |
| `key_next_workspace` | key names | `tab` | Cycle the displayed workspace forward (wraps); `""` to disable. Held modifiers match exactly, so e.g. a `SUPER+Tab` toggle bind still passes through and closes |
| `key_prev_workspace` | key names | `shift+tab` | Cycle the displayed workspace backward (`mod+key` combos supported: `shift`/`ctrl`/`alt`/`super`) |
| `key_activate` | key names | `enter` | Keys that focus the selected preview |
| `key_close_window` | key names | `d` | Keys that close the selected preview's window (overview stays open); `""` to disable |
| `key_left` / `key_right` / `key_up` / `key_down` | key names | `left` / `right` / `up` / `down` | Move the keyboard selection (e.g. set `h`/`l`/`k`/`j` for vim nav) |
| `key_desktop` | key names | `shift` | Flip canvas↔grid |
| `key_all_workspaces` | key names | `a` | Toggle the all-workspaces (expo) view; `""` to disable |
| `key_workspace` | key names | `1,2,3,4,5,6,7,8,9,0` | Each key switches to the Nth strip card's workspace, for real (slot position = card index) |
| `exit_on_click` | bool (0/1) | `1` | Click on empty space dismisses the overview |
| `exit_on_switch` | bool (0/1) | `0` | Dismiss when the live workspace changes underneath (e.g. a keybind) |
| `show_all_workspaces` | bool (0/1) | `0` | Main area shows every window on the monitor (expo), not just the displayed workspace. Toggle live with `gloview:allworkspaces`, the `key_all_workspaces` key, or the strip's "All" card. Picking a preview here follows it: the overview closes onto that window's workspace |
| `show_empty` | bool (0/1) | `1` | Keep empty workspaces as strip cards. Has no effect while `dynamic_workspaces` is on (which it is by default) — set that to `0` to get the old always-list-everything strip back |
| `dynamic_workspaces` | bool (0/1) | `1` | GNOME/hyprnome-style workspaces: only populated workspaces are listed and the strip ends in one empty card (drawn as a workspace, not a `+`). Stepping onto it or dropping a window into it creates it; the *next* empty card only appears once a window actually lands there, so you never see two blank desktops in a row, and emptying it again takes the extra card back away. Workspaces you empty drop off the strip. Forces `show_empty` off. Pair with `autodelete_empty` if you also want workspaces your config pins to be destroyed, not just hidden |
| `autodelete_empty` | bool (0/1) | `1` | Let Hyprland reap empty workspaces this monitor still pins. Hyprland already destroys unpinned empties on its own, so this only affects ones held by a `persistent:true` rule (and gloview's own abandoned trailing workspace) — the "empty ones get deleted automatically" half of GNOME-style workspaces. **Releasing a persistent workspace lasts until your next config reload — set this to `0` if you keep `persistent:true` workspaces you want left alone.** Skips the displayed workspace, anything visible on any monitor, workspaces holding any window (mapped or not), scratchpads, named workspaces, and other monitors' workspaces |
| `show_workspace_labels` | bool (0/1) | `1` | Workspace names above the strip cards. Off frees the label band, so the cards grow into it |
| `show_window_labels` | bool (0/1) | `1` | Window title pill under a hovered/selected preview |
| `show_special` | bool (0/1) | `0` | Include the special (scratchpad) workspace as a strip card |
| `strip_all_card` | bool (0/1) | `0` | Show a leading "All workspaces" card on the strip that toggles the expo view |
| `drag_to_swap` | bool (0/1) | `1` | Grid mode: dropping a preview onto another swaps the two windows' places |
| `switch_on_drop` | bool (0/1) | `0` | Dropping a window on a card also follows it to that workspace |
| `switch_on_new_workspace` | bool (0/1) | `1` | Clicking `+` follows the display to the new workspace |
| `close_button_color` | color | `0xe6e23b3b` | Desktop-mode `✕` close-button fill |
| `hide_top_layers` | bool (0/1) | `0` | Fade out Top layer surfaces (bars, e.g. Waybar) while open |
| `hide_overlay_layers` | bool (0/1) | `0` | Fade out Overlay layer surfaces (popups/notifications) while open |
| `above_namespaces` | string | `""` | Comma/space list of layer namespaces to draw *above* the overview (trailing `*` glob; a namespace containing `aboveoverview` always qualifies) |
| `debug_logs` | bool (0/1) | `0` | Verbose `[gloview]` logging |

`top`/`bottom` give a horizontal strip, `left`/`right` a vertical one. `anchor`
supersedes the older `bar_position` (top/bottom only); set `anchor` and it wins.

### Lua

```lua
    hl.config({
        plugin = {
            gloview = {
                layout         = "rows",
                gap            = 34,
                padding        = 80,
                padding_top    = 40,
                padding_bottom = 70,
                max_scale      = 1.0,
                preview_filter = "box4",
                duration       = 200,
                preview_round  = 12,
                blur           = 1,

                switch_animation = 1,
                switch_duration  = 260,
                move_animation   = 1,
                move_duration    = 240,

                anchor           = "top",
                strip_offset     = 0,
                strip_height     = 150,
                strip_margin     = 22,
                strip_gap        = 18,
                strip_card_round = 10,

                focus_follows_mouse       = 1,
                scroll_switches_workspace = 1,
                passthrough_keys          = 1,
                exit_on_click             = 1,
                exit_on_switch            = 0,

                key_close     = "escape",
                key_next_workspace = "tab",
                key_prev_workspace = "shift+tab",
                key_activate  = "enter",
                key_close_window = "d",
                key_left      = "left",
                key_right     = "right",
                key_up        = "up",
                key_down      = "down",
                key_desktop   = "shift",
                key_all_workspaces = "a",
                key_workspace = "1,2,3,4,5,6,7,8,9,0",

                show_all_workspaces     = 0,
                show_empty              = 1,
                dynamic_workspaces      = 1,
                autodelete_empty        = 1,
                show_workspace_labels   = 1,
                show_window_labels      = 1,
                show_special            = 0,
                strip_all_card          = 1,
                drag_to_swap            = 1,
                switch_on_drop          = 0,
                switch_on_new_workspace = 1,

                hide_top_layers     = 0,
                hide_overlay_layers = 0,
                above_namespaces    = "",
                debug_logs = 0,

                select_border_size  = 3,
                select_border       = 0xf066ccff,
                close_button_color  = 0xe6e23b3b,
                backdrop_color      = 0x73070a10,
                strip_band_color    = 0x24ffffff,
                strip_card_color    = 0x3a0e131c,
                strip_active_color  = 0x4d1c2c44,
                strip_active_border = 0xf0ffffff,
                strip_hover_border  = 0x80ffffff,
                strip_active_border_size = 2,
                strip_hover_border_size  = 2,
                strip_plus_color    = 0xd0eef4ff,
                preview_bg          = 0xff14181f,
                shadow_color        = 0x70000000,
                hover_border        = 0xf0ffffff,
                hover_border_size   = 3,
            },
        },
    })
```

### hyprland.conf

```ini
plugin {
    gloview {
        layout = rows
        gap = 34
        padding = 80
        padding_top = 40
        padding_bottom = 70
        max_scale = 1.0
        preview_filter = box4
        duration = 200
        preview_round = 12
        blur = 1

        switch_animation = 1
        switch_duration  = 260
        move_animation   = 1
        move_duration    = 240

        anchor = top
        strip_offset = 0
        strip_height = 150
        strip_margin = 22
        strip_gap = 18
        strip_card_round = 10

        focus_follows_mouse       = 1
        scroll_switches_workspace = 1
        passthrough_keys          = 1
        exit_on_click             = 1
        exit_on_switch            = 0

        key_close     = escape
        key_next_workspace = tab
        key_prev_workspace = shift+tab
        key_activate  = enter
        key_close_window = d
        key_left      = left
        key_right     = right
        key_up        = up
        key_down      = down
        key_desktop   = shift
        key_all_workspaces = a
        key_workspace = 1,2,3,4,5,6,7,8,9,0

        show_all_workspaces     = 0
        show_empty              = 1
        dynamic_workspaces      = 1
        autodelete_empty        = 1
        show_workspace_labels   = 1
        show_window_labels      = 1
        show_special            = 0
        strip_all_card          = 0
        drag_to_swap            = 1
        switch_on_drop          = 0
        switch_on_new_workspace = 1

        hide_top_layers     = 0
        hide_overlay_layers = 0
        above_namespaces    =
        debug_logs = 0

        select_border_size  = 3
        select_border       = 0xf066ccff
        close_button_color  = 0xe6e23b3b
        backdrop_color      = 0x73070a10
        strip_band_color    = 0x24ffffff
        strip_card_color    = 0x3a0e131c
        strip_active_color  = 0x4d1c2c44
        strip_active_border = 0xf0ffffff
        strip_hover_border  = 0x80ffffff
        strip_active_border_size = 2
        strip_hover_border_size  = 2
        strip_plus_color    = 0xd0eef4ff
        preview_bg          = 0xff14181f
        shadow_color        = 0x70000000
        hover_border        = 0xf0ffffff
        hover_border_size   = 3
    }
}
```

## Contribute

If you want to contribute, you can check out the feature requests by users in the Discussions tab, or implement your own feature

It will be merged if it builds and fits. If you aren't sure, you can contact me (see below)

AI code is allowed if it's submitted and tested by a human

## Donate

#### XMR:
`42uxSBp4aMyTAsPCMGEwHvJyGpemr1c7kdjtFsD5tnEsU7XsnYMjseyXBzLWHkruSWFGbQWagsh31bBRdU7vDNUBAzm1Mo4`  

#### BTC:
`bc1p2xkwf9elq8wgajtq2cc6zthuh4k998tgnk6365cnjqgal7mpd09q4jtfq8`

#### ETH (ERC-20):
`0xBD636eBD3a6b9F046930101657459E90DA370e81`  

---

Email [root@feds.farm](mailto:root@feds.farm) or DM [@root:feds.farm](https://escape.feds.farm/#@root:feds.farm) on Matrix if you want your donation to be visible
