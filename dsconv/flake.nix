{
  description = "dsconv: convert a music library for the Nintendo DS player";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      pyDeps = ps: with ps; [ av soxr numpy mutagen pillow pykakasi pypinyin ];
    in
    {
      packages = forAll (pkgs: rec {
        dsconv = pkgs.python3Packages.buildPythonApplication {
          pname = "dsconv";
          version = "1.0.0";
          pyproject = true;
          src = ./.;
          build-system = [ pkgs.python3Packages.setuptools ];
          dependencies = pyDeps pkgs.python3Packages;
          # The test suite runs in the dev shell (`nix develop -c pytest`); skipping it
          # here keeps `nix run` fast and independent of which encoders FFmpeg has.
          doCheck = false;
          pythonImportsCheck = [ "dsconv" ];
          meta = {
            description = "Convert a music library for alya-bangers, the Nintendo DS player";
            license = with pkgs.lib.licenses; [ gpl3Plus gpl2Plus ];
            mainProgram = "dsconv";
          };
        };
        default = dsconv;
      });

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.dsconv}/bin/dsconv";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [ (pkgs.python3.withPackages (ps: pyDeps ps ++ [ ps.pytest ])) ];
          shellHook = ''export PYTHONPATH="$PWD/src''${PYTHONPATH:+:$PYTHONPATH}"'';
        };
      });
    };
}
