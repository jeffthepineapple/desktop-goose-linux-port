{
  description = "Desktop Goose Linux port";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};

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

    nativeBuildInputs = with pkgs; [
      cmake
      pkg-config
      wayland-scanner
    ];
  in {
    packages.${system}.default = pkgs.stdenv.mkDerivation {
      pname = "desktop-goose";
      version = "0.1";
      src = self;

      inherit nativeBuildInputs buildInputs;
    };

    devShells.${system}.default = pkgs.mkShell {
      inherit nativeBuildInputs buildInputs;
    };
  };
}
