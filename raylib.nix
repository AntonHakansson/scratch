{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [ raylib ];
  hardeningDisable = [ "all" ];
}
