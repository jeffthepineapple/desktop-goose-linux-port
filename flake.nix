{
  description = "Desktop Goose Linux port";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f {
          inherit system;
          pkgs = import nixpkgs { inherit system; };
        });
    in {
      packages = forAllSystems ({ pkgs, ... }: {
        default = pkgs.stdenv.mkDerivation {
          pname = "desktop-goose";
          version = "0.1";
          src = self;

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            gtk4
            gtk4-layer-shell
            SDL2
            SDL2_mixer
            gdk-pixbuf
            wayland
            libx11
            libxtst
            libsysprof-capture
            pcre2
            libxi
          ];

          cmakeFlags = [ "-DBUILD_TESTING=OFF" ];

          meta.mainProgram = "CppGoose";
        };
      });

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            gtk4
            gtk4-layer-shell
            SDL2
            SDL2_mixer
            gdk-pixbuf
            wayland
            libx11
            libxtst
            libsysprof-capture
            pcre2
            libxi
          ];
        };
      });
    };
}
