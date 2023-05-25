with import (fetchTarball "channel:nixos-unstable") {};

let
pping = callPackage ./default.nix {};
in
dockerTools.buildImage {
  name = "pping";
  copyToRoot = pkgs.buildEnv {
    name = "image-root";
    paths = [ pkgs.busybox pping ];
    pathsToLink = [ "/bin" ];
  };
  config = {
    Cmd = [
      "${pping}/bin/pping"
    ];
  };
}
