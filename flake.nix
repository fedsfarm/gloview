{
  description = "GloView — a macOS Mission Control-style overview plugin for Hyprland";

  inputs = {
    # Pin a tagged Hyprland release, not `main`: ABI must match the compositor, and main's
    # dep tree is intermittently unbuildable (e.g. a missing pango buildInput in
    # hyprland-guiutils broke it on 2026-06-27). v0.56.2 is the version this targets: 0.56
    # dropped CWindow::m_snapshotFB, so the plugin owns the snapshot framebuffers itself and
    # will not compile against 0.55.x. Downstream should set `inputs.hyprland.follows` to
    # build the plugin against THEIR exact Hyprland.
    hyprland.url = "github:hyprwm/Hyprland?ref=v0.56.2";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs = {
    self,
    hyprland,
    nixpkgs,
    systems,
    ...
  }: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    pkgsFor = eachSystem (system: import nixpkgs {inherit system;});
  in {
    packages = eachSystem (system: let
      pkgs = pkgsFor.${system};
      # Upstream's tagged flake package does not build in a nix sandbox as-is: Hyprland
      # 0.56.2's CMake asks for `find_package(glaze 7...<8)` while its own nixpkgs pin ships
      # glaze 8, so the check fails and the FetchContent fallback tries to `git clone` glaze
      # (no git, no network in the sandbox) -> "could not find git for clone of glaze".
      # nixpkgs' own hyprland 0.56.2 relaxes that exact constraint and builds fine against
      # glaze 8, so mirror its patch here instead of pinning an older Hyprland.
      hyprlandPkg = hyprland.packages.${system}.hyprland.overrideAttrs (old: {
        postPatch =
          (old.postPatch or "")
          + ''
            substituteInPlace CMakeLists.txt --replace-fail "glaze 7...<8" "glaze"
          '';
      });
    in {
      # mkHyprlandPlugin now lives in nixpkgs (pkgs.hyprlandPlugins.mkHyprlandPlugin), not in
      # the Hyprland flake's `lib`. It is built on hyprland.stdenv.mkDerivation and auto-adds
      # pkg-config + hyprland + hyprland.buildInputs. We `.override` the whole scope so BOTH
      # the build stdenv and the hyprland buildInput are the EXACT pinned Hyprland (ABI must
      # match the running compositor), not nixpkgs' possibly-skewed hyprland.
      gloview = (pkgs.hyprlandPlugins.override {hyprland = hyprlandPkg;}).mkHyprlandPlugin {
        pluginName = "gloview";
        version = "0.3.0";
        src = ./.;

        nativeBuildInputs = [pkgs.cmake pkgs.pkg-config];
        # Hyprland's own build inputs are propagated by mkHyprlandPlugin; we only add Lua
        # (for the gloview.* Lua config functions). luajit is what Hyprland links.
        buildInputs = [pkgs.luajit];

        # The build emits `gloview.so` (CMake PREFIX ""), but Home Manager's
        # `wayland.windowManager.hyprland.plugins = [ pkg ]` looks for `lib<pname>.so`.
        # Symlink it so the idiomatic one-liner install works with no extra config.
        postInstall = ''
          ln -sf gloview.so "$out/lib/libgloview.so"
        '';

        meta = {
          description = "macOS Mission Control-style overview for Hyprland";
          homepage = "https://github.com/fedsfarm/gloview";
          license = lib.licenses.gpl3Plus;
          platforms = lib.platforms.linux;
        };
      };

      default = self.packages.${system}.gloview;
    });

    devShells = eachSystem (system: let
      pkgs = pkgsFor.${system};
    in {
      default = pkgs.mkShell {
        # `nix develop` gives a shell that can configure+build the plugin against the
        # pinned Hyprland (cmake -S . -B build && cmake --build build).
        inputsFrom = [self.packages.${system}.gloview];
        packages = [pkgs.clang-tools];
      };
    });

    formatter = eachSystem (system: pkgsFor.${system}.alejandra);
  };
}
