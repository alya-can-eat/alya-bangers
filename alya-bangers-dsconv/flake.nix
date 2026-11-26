{
  description = "alya-bangers-dsconv: puts your music on the card for alya-bangers, the Nintendo DS music player";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      pyDeps = ps: with ps; [ av soxr numpy mutagen pillow pykakasi pypinyin pyside6 ];
    in
    {
      packages = forAll (pkgs: rec {
        alya-bangers-dsconv = pkgs.python3Packages.buildPythonApplication {
          pname = "alya-bangers-dsconv";
          version = "1.0.0.dev0";
          pyproject = true;
          src = ./.;
          build-system = [ pkgs.python3Packages.setuptools ];
          dependencies = pyDeps pkgs.python3Packages;
          # pyproject.toml names the smaller PyPI package; nixpkgs' pyside6 is the whole of it
          pythonRemoveDeps = [ "PySide6-Essentials" ];

          # the window: Qt's plugins (X11, Wayland) found through the wrapper
          nativeBuildInputs = [ pkgs.qt6.wrapQtAppsHook pkgs.copyDesktopItems ];
          buildInputs = [ pkgs.qt6.qtbase ] ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.qt6.qtwayland;
          dontWrapQtApps = true;
          preFixup = ''
            makeWrapperArgs+=("''${qtWrapperArgs[@]}")
          '';

          # an entry in the app launcher, with the icon
          desktopItems = [
            (pkgs.makeDesktopItem {
              name = "alya-bangers-dsconv";
              desktopName = "alya-bangers-dsconv";
              genericName = "Music converter for the DS";
              comment = "Puts your music on the card for alya-bangers, the Nintendo DS music player";
              exec = "alya-bangers-dsconv";
              icon = "alya-bangers-dsconv";
              categories = [ "AudioVideo" "Audio" ];
              startupWMClass = "alya-bangers-dsconv";
            })
          ];
          postInstall = ''
            install -Dm644 src/alya_bangers_dsconv/data/icon.png \
              $out/share/icons/hicolor/256x256/apps/alya-bangers-dsconv.png
          '';

          # The test suite runs in the dev shell (`nix develop -c pytest`); skipping it
          # here keeps `nix run` fast and independent of which encoders FFmpeg has.
          doCheck = false;
          pythonImportsCheck = [ "alya_bangers_dsconv" ];
          meta = {
            description = "Convert a music library for alya-bangers, the Nintendo DS music player";
            license = with pkgs.lib.licenses; [ gpl3Plus gpl2Plus ];
            mainProgram = "alya-bangers-dsconv";
          };
        };
        default = alya-bangers-dsconv;
      });

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.alya-bangers-dsconv}/bin/alya-bangers-dsconv";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [ (pkgs.python3.withPackages (ps: pyDeps ps ++ [ ps.pytest ])) ];
          shellHook = ''
            export PYTHONPATH="$PWD/src''${PYTHONPATH:+:$PYTHONPATH}"
            # the window tests run offscreen
            export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
          '';
        };
      });
    };
}
