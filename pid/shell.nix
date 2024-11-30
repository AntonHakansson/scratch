{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [ raylib pkg-config ];
  propogateBuildInputs = with pkgs; [ libGL ];
}
