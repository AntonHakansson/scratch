{ pkgs ? import <nixpkgs> {} }:
pkgs.pkgsCross.mingwW64.mkShell {
  buildInputs = with pkgs; [
    gcc
  ];
}
