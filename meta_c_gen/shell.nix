{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell.override { stdenv = pkgs.gcc14Stdenv; } {
  buildInputs = with pkgs; [ raylib ];
  propogateBuildInputs = with pkgs; [ libGL ];
  hardeningDisable = [ "all" ];
}
