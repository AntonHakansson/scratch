{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [ raylib ];
  hardeningDisable = [ "all" ];
  propogateBuildInputs = [ pkgs.wayland ];
  LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [ pkgs.wayland ];
}
