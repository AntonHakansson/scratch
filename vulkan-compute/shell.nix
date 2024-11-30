{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [
    vulkan-headers
    vulkan-validation-layers
    vulkan-loader
    vulkan-tools
    shaderc.bin
  ];
}
