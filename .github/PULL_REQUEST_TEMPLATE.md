## What changed and why

<!-- One or two sentences. Link the issue or discussion if there is one. -->

## How you tested it

gloview is a compositor plugin with no unit tests, so "tested" means it ran in Hyprland.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake --build build --target reload   # unload + load gloview.so in the live session
```

Use a nested Hyprland for anything that could take the session down.

<!-- Say what you actually exercised: which keybinds, which config, how many monitors. -->

## Checklist

- [ ] Builds clean (`cmake --build build`)
- [ ] Exercised in a running Hyprland via `--target reload`
- [ ] README.md config table updated if a `plugin:gloview:*` option was added or changed
