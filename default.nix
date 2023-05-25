{
  system ? builtins.currentSystem,
  pkgs ? import <nixpkgs> { inherit system; },
}:

with pkgs;

stdenv.mkDerivation {
  name = "pping";
  src = ./.;
  buildInputs = [ libev ];
  disallowedRequisites = [ stdenv.cc.cc ];
  dontStrip = true;
  dontUnpack = true;
  buildPhase = ''
    $CC -o pping $src/*.c -lev
  '';
  installPhase = ''
    install -Dm755 pping $out/bin/pping
  '';
}

