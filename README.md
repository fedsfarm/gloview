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

Lua:

```lua
hl.bind("SUPER + TAB", hl.plugin.gloview.toggle)
hl.bind("SUPER + SHIFT + TAB", hl.plugin.gloview.desktop)
hl.bind("SUPER + CTRL + TAB", hl.plugin.gloview.allworkspaces)
```

```ini
bind = SUPER, TAB, gloview:toggle
bind = SUPER SHIFT, TAB, gloview:desktop
bind = SUPER CTRL, TAB, gloview:allworkspaces
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
| `duration` | int (ms) | `360` | Open/close animation length |
| `preview_round` | int (px) | `12` | Window preview corner radius |
| `blur` | float `0`..`1` | `1.0` | Backdrop + strip blur strength (`0` = off; fractions allowed) |
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
| `show_all_workspaces` | bool (0/1) | `0` | Main area shows every window on the monitor (expo), not just the displayed workspace. Toggle live with `gloview:allworkspaces`, the `key_all_workspaces` key, or the strip's "All" card |
| `show_empty` | bool (0/1) | `1` | Keep empty workspaces as strip cards |
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
                duration       = 200,
                preview_round  = 12,
                blur           = 1,

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
        duration = 200
        preview_round = 12
        blur = 1

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
        shadow_color        = 0x70000000
        hover_border        = 0xf0ffffff
        hover_border_size   = 3
    }
}
```

## Donate

#### BTC:
`bc1p2xkwf9elq8wgajtq2cc6zthuh4k998tgnk6365cnjqgal7mpd09q4jtfq8`  
#### ETH:
`0xBD636eBD3a6b9F046930101657459E90DA370e81`  
#### XMR:
`42uxSBp4aMyTAsPCMGEwHvJyGpemr1c7kdjtFsD5tnEsU7XsnYMjseyXBzLWHkruSWFGbQWagsh31bBRdU7vDNUBAzm1Mo4`  

Email [root@feds.farm](mailto:root@feds.farm) or DM [@root:feds.farm](https://escape.feds.farm/#@root:feds.farm) on Matrix if you want your donation to be visible
