{
  description = "Samplebrain as an ARA plugin";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    juce = {
      url = "github:juce-framework/JUCE/8.0.17-security";
      flake = false;
    };
    ara-sdk = {
      url = "git+https://github.com/Celemony/ARA_SDK?ref=refs/tags/releases/2.2.0&submodules=1";
      flake = false;
    };
    samplebrain = {
      url = "git+https://gitlab.com/then-try-this/samplebrain?rev=30e290dcbd7d7c4ae84890a1f512b9568e4ee1e5";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, juce, ara-sdk, samplebrain }:
    let
      system = "aarch64-darwin";
      pkgs = nixpkgs.legacyPackages.${system};
      # clang 21's sanitizer runtimes hang at startup on macOS 26
      stdenv = pkgs.llvmPackages_22.stdenv;

      samplebrainSrc = pkgs.applyPatches {
        name = "samplebrain-patched";
        src = samplebrain;
        patches = builtins.sort builtins.lessThan
          (map (f: ./patches + "/${f}")
            (builtins.attrNames (builtins.readDir ./patches)));
      };

      engineDeps = with pkgs; [
        fftw liblo libsndfile libsamplerate xxhash
        # libsndfile's pkg-config file names its codecs
        flac libogg libvorbis libopus mpg123
      ];
      buildTools = with pkgs; [ cmake ninja pkg-config ];
      buildDeps = engineDeps ++ [ pkgs.catch2_3 pkgs.apple-sdk_15 ];

      env = {
        JUCE_DIR = "${juce}";
        ARA_SDK_DIR = "${ara-sdk}";
        SAMPLEBRAIN_SRC = "${samplebrainSrc}";
      };

      src = pkgs.lib.fileset.toSource {
        root = ./.;
        fileset = pkgs.lib.fileset.unions [
          ./CMakeLists.txt
          ./CMakePresets.json
          ./engine
          ./tests
          ./bench
          (pkgs.lib.fileset.maybeMissing ./plugin)
        ];
      };
    in
    {
      packages.${system} = {
        samplebrain-src = samplebrainSrc;

        tests = stdenv.mkDerivation (env // {
          pname = "samplebrain-plugin-tests";
          version = "0";
          inherit src;
          nativeBuildInputs = buildTools;
          buildInputs = buildDeps;
          cmakeFlags = [ "-DSB_BUILD_PLUGIN=OFF" "-DSB_BUILD_BENCH=OFF" ];
          doCheck = true;
          checkPhase = "ctest --output-on-failure";
          installPhase = "touch $out";
        });

        plugin = stdenv.mkDerivation (env // {
          pname = "samplebrain-plugin";
          version = "0";
          inherit src;
          nativeBuildInputs = buildTools;
          buildInputs = buildDeps;
          cmakeFlags = [ "-DSB_BUILD_TESTS=OFF" "-DSB_BUILD_BENCH=OFF" ];
          installPhase = ''
            mkdir -p $out
            cp -R plugin/Samplebrain_artefacts/Release/VST3/*.vst3 $out/
            cp -R plugin/Samplebrain_artefacts/Release/AU/*.component $out/
          '';
        });

        default = self.packages.${system}.plugin;
      };

      checks.${system}.tests = self.packages.${system}.tests;

      devShells.${system}.default = (pkgs.mkShell.override { inherit stdenv; }) (env // {
        nativeBuildInputs = buildTools;
        buildInputs = buildDeps;
      });
    };
}
