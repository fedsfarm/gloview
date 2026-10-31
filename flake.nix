{
  description = "GloView — a macOS Mission Control-style overview plugin for Hyprland";

  inputs = {
    # Pin a tagged Hyprland release, not `main`: ABI must match the compositor, and main's
    # dep tree is intermittently unbuildable (e.g. a missing pango buildInput in
    # hyprland-guiutils broke it on 2026-06-27). v0.57.0 is the version this targets: 0.57
    # moved per-frame render state into Render::CRenderContext and reworked workspace ids, so
    # the plugin will not compile against 0.56.x. Downstream should set
    # `inputs.hyprland.follows` to build the plugin against THEIR exact Hyprland.
    hyprland.url = "github:hyprwm/Hyprland?ref=v0.57.0";
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
      # 0.56.2 needed a postPatch relaxing `find_package(glaze 7...<8)`; 0.57.0 asks for an
      # unversioned `find_package(glaze QUIET)`, so the tagged package is used as-is (a
      # leftover `--replace-fail` on that string would now FAIL the build).
      hyprlandPkg = hyprland.packages.${system}.hyprland;
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
