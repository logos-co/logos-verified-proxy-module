{
  description = "libverifproxy — the C static-library form of nimbus_verified_proxy";

  # libverifproxy is a large archive and the Nim toolchain behind it is slow to
  # build; pull both from the Logos Attic rather than rebuilding per machine.
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-nix.url = "github:logos-co/logos-nix";

    # git+https, NOT github: — the github: scheme does not carry submodules
    # (NixOS/nix#14982) and nimbus' nix/default.nix asserts on `self.submodules`.
    # Needs Nix >= 2.27 for the flake-level `self = { submodules = true; }`.
    nimbus-eth1.url = "git+https://github.com/status-im/nimbus-eth1?submodules=1&ref=refs/tags/v0.4.0";
  };

  outputs = inputs@{ self, logos-module-builder, logos-nix, nimbus-eth1 }:
    let
      nixpkgs = logos-nix.inputs.nixpkgs;
      lib = nixpkgs.lib;

      nativeSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      systems = nativeSystems ++ [ "x86_64-windows" ];

      pkgsFor = system:
        if system == "x86_64-windows"
        then logos-nix.lib.mkWindowsPkgs { buildSystem = "x86_64-linux"; }
        else import nixpkgs { inherit system; };

      libverifproxyFor = system:
        let
          pkgs = pkgsFor system;
          isWin = system == "x86_64-windows";

          # Windows cannot go through nimbus' own flake: it does
          # `import nixpkgs { system = "x86_64-windows"; }`, which yields a
          # NATIVE Windows package set — it evaluates, and it is unusable.
          # Call their nix/default.nix ourselves with a real cross pkgs set.
          base =
            if isWin then
              pkgs.callPackage "${nimbus-eth1}/nix/default.nix" {
                self = nimbus-eth1;
                # MUST contain the target: nix/default.nix feeds this straight
                # to meta.platforms, and nixpkgs refuses to evaluate a
                # derivation whose meta.platforms omits the hostPlatform.
                stableSystems = [ "x86_64-windows" ];
                # USE_SYSTEM_NIM=1 wants a BUILD-side Nim; pkgs.nim here is a PE.
                # Under that cross wrapper nimscript's `defined(windows)` is
                # already true, so no --os:windows has to be passed by hand.
                nim = pkgs.buildPackages.nim-2_2;
                targets = [ "libverifproxy" ];
              }
            else
              nimbus-eth1.packages.${system}.nimbus_verified_proxy.override {
                targets = [ "libverifproxy" ];
              };
        in
        base.overrideAttrs (old: {
          pname = "libverifproxy";

          # Upstream has `perl sqlite python3` in buildInputs. perl and python3
          # are Makefile TOOLS, not target libraries — harmless natively, fatal
          # under cross, because nixpkgs marks the mingw python3 BROKEN and the
          # derivation then refuses to evaluate.
          buildInputs =
            if isWin
            # nixpkgs builds mingw-w64 against mcfgthread, so pthread.h and
            # libpthread.a exist nowhere in the default closure — but the
            # vendored C assumes POSIX threads regardless.
            then [ pkgs.sqlite pkgs.windows.pthreads ]
            else old.buildInputs;

          nativeBuildInputs = old.nativeBuildInputs
            ++ lib.optionals isWin (with pkgs.buildPackages; [
                 perl python3 gnumake
                 nasm   # nim-boringssl's Windows branch shells out to `nasm -f win64`
               ]);

          makeFlags = old.makeFlags ++ lib.optionals isWin [
            # nim-libbacktrace vendors libbacktrace and configures it POSIX-shaped.
            "USE_LIBBACKTRACE=0"
          ];

          # Upstream builds the VENDORED RocksDB in preBuild unconditionally
          # ("takes almost double the time"), although `make libverifproxy`
          # never reaches the rocksdb target: deps is
          # `deps-common nat-libs nimbus.nims build/generate_makefile`.
          # Dropping it beats dynamicRocksDB = true, which would instead put a
          # (cross, on Windows) rocksdb in buildInputs.
          # Verify: nm -u $out/lib/libverifproxy.a | grep -c rocksdb_
          preBuild = lib.optionalString isWin ''
            # --app:staticlib makes Nim shell out to a BARE `ar`, and a cross
            # stdenv has only x86_64-w64-mingw32-ar on PATH. The nixpkgs nim
            # wrapper rewrites gcc.exe/gcc.linkerexe from $CC/$CXX but never the
            # archiver, and nim exposes no config key for it.
            mkdir -p $TMPDIR/arshim
            ln -sf "$(command -v $AR)" $TMPDIR/arshim/ar
            export PATH=$TMPDIR/arshim:$PATH

            # nimbus-build-system's nat-libs targets branch on $(OS) — the
            # cmd.exe variable, empty on a Linux builder — so a cross build
            # silently takes their POSIX branch and the archives then call
            # their own symbols through __imp_ stubs.
            make -C vendor/nim-nat-traversal/vendor/miniupnp/miniupnpc \
              CC="$CC" AR="$AR" RANLIB="$RANLIB" \
              CFLAGS="-Os -DMINIUPNP_STATICLIB" build/libminiupnpc.a
            make -C vendor/nim-nat-traversal/vendor/libnatpmp-upstream \
              CC="$CC" AR="$AR" RANLIB="$RANLIB" \
              CFLAGS="-Wall -Os -DENABLE_STRNATPMPERR -DNATPMP_MAX_RETRIES=4 -DNATPMP_STATICLIB" \
              libnatpmp.a
          '';

          env = old.env // {
            NIMFLAGS = old.env.NIMFLAGS
              # library/nim.cfg omits noSignalHandler, so NimMain() would
              # install Nim's SIGINT/SIGSEGV/SIGABRT handlers over the HOST's.
              + " -d:noSignalHandler"
              + " -d:release --debugger:off -d:disableLTO"
              # Nim only adds -fPIC when optGenDynLib is set, and --app:staticlib
              # does not set it. The archive is linked into a SHARED plugin.
              # Meaningless on PE.
              + lib.optionalString (!isWin) " --passC:-fPIC";
          };

          # Upstream installs only `-type f -executable` into $out/bin, so a .a
          # and a .h yield an EMPTY $out; and installCheckPhase then runs the
          # literal string "$out/bin/* --version".
          installPhase = ''
            runHook preInstall
            mkdir -p $out/lib $out/include
            install -m444 build/libverifproxy/libverifproxy.a $out/lib/
            install -m444 build/libverifproxy/verifproxy.h    $out/include/
            runHook postInstall
          '';
          doInstallCheck = false;
        });
      # A flake-SHAPED attrset, not a flake: resolveExtInput only needs
      # `x.packages.${system}.<name>`.
      #
      # Use the structured { input; packages.default; } form and NOT the barer
      # { packages.<sys>.default = drv; } escape hatch: buildCppPlugin accepts
      # both, but mkLogosModuleTests only checks `value ? input` and otherwise
      # hands the raw attrset to mkExternalLib as a `src`. The plugin would
      # build and the unit tests would fail to EVALUATE.
      libverifproxyFlake = {
        packages = lib.genAttrs systems (s: { libverifproxy = libverifproxyFor s; });
      };

      nimbusRev = nimbus-eth1.rev or nimbus-eth1.shortRev or "unknown";

      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;

        # `verifproxy`, not `libverifproxy`: find_library searches lib${name}.a,
        # which maps onto the real libverifproxy.a.
        externalLibInputs.verifproxy = {
          input = libverifproxyFlake;
          packages.default = "libverifproxy";
        };

        # The library exposes no version symbol, so stamp the upstream revision
        # in at build time for status()/libraryVersion().
        preConfigure = ''
          printf '#define VERIFIED_PROXY_NIMBUS_REV "%s"\n' "${nimbusRev}" \
            > src/verified_proxy_nimbus_rev.h
        '';

        tests = {
          dir = ./tests;
          # Keeps the ~25-minute upstream build out of the test derivation
          # entirely; unit tests link mocks/mock_libverifproxy.cpp instead.
          mockCLibs = [ "verifproxy" ];
        };
      };
    in
      module // {
        packages = lib.genAttrs systems (system:
          (module.packages.${system} or {}) // {
            libverifproxy = libverifproxyFor system;
          });
      };
}
