# Packaged port for Linux, x86_64 and aarch64: the prebuilt game binaries (bash build.sh first; on
# aarch64 also the guest CPU library, out/fex/libtlgcpu.so), the preparation scripts and the Tk
# launcher, with their whole Nix closure and Mesa's Vulkan drivers (on aarch64 Turnip, built here,
# with this port's patches for the Adreno 8xx). `bash packaging/appimage.sh` turns it into an
# AppImage; `nix-build packaging` alone gives result/bin/gr2. Based on the Bloodborne and inFAMOUS
# ARM ports' packaging/default.nix.
#
# result/bin/gr2         the launcher
# result/bin/gr2 --play  the game with the launcher's saved settings, no window
# Data (generated files out/, the default saves folder user/, system_lib/ for the console's
# libSceNgs2.sprx): ~/.local/share/gr2-pc (GR2_DATA_DIR).
{ pkgs ? import <nixpkgs> { }
  # Store paths the prebuilt binaries load libraries from (their RUNPATHs), written by
  # appimage.sh. Nix only finds references to its inputs, and the binaries were built outside.
, runtimePaths ? (if builtins.pathExists ./runtime-paths.nix then import ./runtime-paths.nix else [ ])
}:
let
  lib = pkgs.lib;
  # aarch64 hosts: the game's x86-64 code runs in FEXCore (out/fex/libtlgcpu.so, build.sh).
  arm = pkgs.stdenv.hostPlatform.isAarch64;
  root = ./..;
  getEnvOr = names: lib.findFirst (value: value != "") "" (map builtins.getEnv names);
  assetDirs = [ "scripts" "launcher" ];
  # Only what the package needs (the tree also holds builds, profiles, caches and the game's files).
  wanted = [
    "LICENSE" "packaging" "packaging/README-player.md"
    "out" "out/tlg-probe" "out/tlg-gpu-capabilities" "out/gpu" "out/gpu/libtlggpu.so"
  ] ++ lib.optionals arm [ "out/fex" "out/fex/libtlgcpu.so" ] ++ assetDirs;
  src = builtins.path {
    name = "gr2-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in baseNameOf path != "__pycache__"
        && (builtins.elem rel wanted || lib.any (dir: lib.hasPrefix (dir + "/") rel) assetDirs);
  };

  # The Tk launcher (gr2_launcher.py) and the preparation scripts. Pillow: the game's icon0.png as
  # the window icon. iced-x86 (x86_64 only): scripts/isa_rewrite.py, for CPUs without the PS4's
  # SSE4A/MOVBE/BMI1/LZCNT (every Intel CPU); on aarch64 FEXCore runs every one of them.
  python = pkgs.python3.withPackages (ps: [ ps.tkinter ps.pillow ]
    ++ lib.optional (!arm && ps ? iced-x86) ps.iced-x86);

  # Mesa comes with the package. tlg_vulkan.py adds the host NVIDIA ICD and only its vendor
  # libraries (matching the host's kernel module).
  # aarch64: Adreno (Turnip), Mali (Panfrost), Raspberry Pi (V3DV), Apple (Asahi), AMD.
  icdArch = pkgs.stdenv.hostPlatform.parsed.cpu.name;
  icdNames = if arm then [ "freedreno" "panfrost" "broadcom" "asahi" "radeon" ] else [ "radeon" "intel" ];
  # The Bloodborne ARM port's Turnip patches: Mesa 26's Turnip loses the device on the Adreno 740
  # (SM8550) without them, and the Adreno 830 (SM8750) needs its chip IDs. Only freedreno is
  # rebuilt; the other drivers stay the binary cache's Mesa.
  turnipPatches = [
    ./mesa-turnip/0001-add-a830-chip-id.patch
    ./mesa-turnip/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch
    ./mesa-turnip/001-fix-freedreno-vulkan.patch
  ];
  # GR2_TURNIP_SRC (or ISS_TURNIP_SRC / BB_TURNIP_SRC, the inFAMOUS and Bloodborne ports' names): a
  # Mesa source tree for Turnip instead of nixpkgs' release, e.g. a checkout of
  # whitebelyash/mesa-unified's turnip/gen8 branch (Adreno 8xx work not yet in Mesa) or Mesa main.
  # The Adreno 830 chip IDs are added where the tree lacks them; the other patches are applied where
  # they apply, skipped where the tree has them already, and a conflict stops the build.
  # GR2_TURNIP_PATCHES=0 builds the tree as it is. Without a tree the package uses nixpkgs' Mesa
  # with the patches above.
  # The derivation below is the Bloodborne port's, word for word (its messages too): with the same
  # nixpkgs, patches and Mesa tree it is the same store path, so ../build_docker.sh with
  # GR2_NIX_VOLUME=bbport-nix (or iss-nix) reuses a Turnip that another port's build compiled already.
  turnipSrc = getEnvOr [ "GR2_TURNIP_SRC" "ISS_TURNIP_SRC" "BB_TURNIP_SRC" ];
  turnipPortPatches = getEnvOr [ "GR2_TURNIP_PATCHES" "ISS_TURNIP_PATCHES" "BB_TURNIP_PATCHES" ] != "0";
  turnip = (pkgs.mesa.override {
    galliumDrivers = [ "freedreno" ];
    vulkanDrivers = [ "freedreno" ];
    vulkanLayers = [ ];
    enablePatentEncumberedCodecs = false;
  }).overrideAttrs (old: {
    patches = old.patches ++ turnipPatches;
    mesonFlags = map (flag: if lib.hasPrefix "-Dtools=" flag then "-Dtools=" else flag) old.mesonFlags
      ++ [ "-Dgallium-va=disabled" ];
    postInstall = old.postInstall + ''
      mkdir -p $opencl $spirv2dxil
    '';
  } // lib.optionalAttrs (turnipSrc != "") {
    version = lib.removeSuffix "\n" (builtins.readFile (/. + turnipSrc + "/VERSION")) + "-src";
    src = builtins.path {
      name = "mesa-src";
      path = /. + turnipSrc;
      filter = path: type: baseNameOf path != ".git";
    };
    # nixpkgs' own patches only (the port's are applied below, where they still apply).
    patches = old.patches;
    # Not nixpkgs' check that the tree's GL headers match its mesa-gl-headers (the release's): a
    # Vulkan-only build of another tree does not use them.
    postPatch = ''
      patchShebangs .
    '' + lib.optionalString turnipPortPatches ''
      python3 - src/freedreno/common/freedreno_devices.py <<'PY'
      import re, sys
      path = sys.argv[1]
      s = open(path).read()
      anchor = 'GPUId(chip_id=0xffff44050000, name="Adreno (TM) 830"),'
      if anchor not in s:
          sys.exit("bbport: no Adreno 830 entry in freedreno_devices.py (BB_TURNIP_PATCHES=0: none of the port's changes)")
      added = []
      for chip in ("0xffff44050001", "0x44050000"):
          if not re.search(r"chip_id=" + chip + r"\b", s, re.I):
              s = s.replace(anchor, anchor + "\n        GPUId(chip_id=" + chip + ", name=\"Adreno (TM) 830\"),", 1)
              added.append(chip)
      open(path, "w").write(s)
      print("bbport: Adreno 830 chip IDs", "added: " + ", ".join(added) if added else "already in the tree")
      PY
      bb_patch() {
        if patch -p1 -R --dry-run -s -f < "$1" > /dev/null 2>&1; then
          echo "bbport: $(basename "$1" | cut -d- -f2-) is already in this Mesa tree"
        elif patch -p1 --dry-run -s -f < "$1" > /dev/null 2>&1; then
          patch -p1 -s -f < "$1"
          echo "bbport: applied $(basename "$1" | cut -d- -f2-)"
        else
          echo "bbport: $(basename "$1" | cut -d- -f2-) does not apply to this Mesa tree (BB_TURNIP_PATCHES=0: build without the port's patches)" >&2
          exit 1
        fi
      }
      bb_patch ${./mesa-turnip/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch}
      bb_patch ${./mesa-turnip/001-fix-freedreno-vulkan.patch}
    '';
  });
  icdMesa = name: if arm && name == "freedreno" then turnip else pkgs.mesa;
  icds = lib.concatMapStringsSep ":"
    (name: "${icdMesa name}/share/vulkan/icd.d/${name}_icd.${icdArch}.json") icdNames;

  # Fonts: DejaVu for the launcher (Tk draws with Xft/fontconfig), plus the host's usual font
  # directories, but not the host's /etc/fonts (on NixOS it names fonts in the host's /nix/store,
  # which the AppImage hides behind its own store in some environments).
  fontsConf = pkgs.writeText "gr2-fonts.conf" ''
    <?xml version="1.0"?>
    <!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
    <fontconfig>
      <dir>${pkgs.dejavu_fonts}/share/fonts</dir>
      <dir>/usr/share/fonts</dir>
      <dir>/usr/local/share/fonts</dir>
      <dir prefix="xdg">fonts</dir>
      <cachedir prefix="xdg">gr2-pc/fontconfig</cachedir>
      <include ignore_missing="yes">${pkgs.fontconfig.out}/etc/fonts/conf.d</include>
    </fontconfig>
  '';
  # The launcher's environment on any host: fonts, UTF-8 for Python without overriding the
  # player's language (the launcher follows LANG), certificates, the data folder.
  # The AppImage cannot write next to itself (a read-only Nix store): gr2_run.py keeps the
  # generated files in GR2_OUT_DIR, the default saves folder in GR2_DEFAULT_USER_DIR, looks for
  # libSceNgs2.sprx in GR2_SYSTEM_LIB_DIR, and runs the loader named in GR2_PROBE.
  # GR2_LAUNCHER_WAITS_FOR_GAME=1: a closed launcher keeps its process (no window) until the game
  # exits. The AppImage mounts the package's files, the game's own code included, for that process
  # only; without it closing the launcher took them away from the running game (SIGBUS in whatever
  # library it touched next).
  share = "$out/share/gr2";
  launcherEnv = ''
      --set-default FONTCONFIG_FILE ${fontsConf} \
      --set-default LC_CTYPE C.UTF-8 \
      --set-default PYTHONUTF8 1 \
      --set PYTHONDONTWRITEBYTECODE 1 \
      --set TLG_VULKANINFO ${pkgs.vulkan-tools}/bin/vulkaninfo \
      --set TLG_BUNDLED_VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ python pkgs.bash pkgs.coreutils ]} \
      --run '[ -r "''${SSL_CERT_FILE:-}" ] || export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt; [ -d "''${SSL_CERT_DIR:-}" ] || unset SSL_CERT_DIR' \
      --run 'export GR2_DATA_DIR=''${GR2_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/gr2-pc}; mkdir -p "$GR2_DATA_DIR/out" "$GR2_DATA_DIR/system_lib"' \
      --run 'export GR2_OUT_DIR=''${GR2_OUT_DIR:-$GR2_DATA_DIR/out} GR2_DEFAULT_USER_DIR=''${GR2_DEFAULT_USER_DIR:-$GR2_DATA_DIR/user} GR2_SYSTEM_LIB_DIR=''${GR2_SYSTEM_LIB_DIR:-$GR2_DATA_DIR/system_lib} TLG_DATA_DIR=$GR2_DATA_DIR' \
      --set GR2_PROBE ${share}/bin/tlg-probe \
      --set GR2_LAUNCHER_WAITS_FOR_GAME 1 \
  '';
  # A smoke test of the package (GR2_PACKAGE_SELFTEST=0 skips it): the launcher's and the
  # preparation scripts' modules import with the package's Python.
  selftest = builtins.getEnv "GR2_PACKAGE_SELFTEST" != "0";
in
pkgs.stdenv.mkDerivation {
  pname = "gr2";
  version = "0.1";
  inherit src;
  nativeBuildInputs = [ pkgs.makeShellWrapper ];
  buildInputs = map builtins.storePath runtimePaths;
  dontBuild = true;
  dontConfigure = true;
  # The binaries live under share/ (next to the scripts): strip them too, which also drops the
  # compiler and header paths their debug info would keep in the closure.
  stripDebugList = [ "share/gr2/bin" ];
  # patchelf (RPATH shrinking) corrupts the non-PIE game binary's symbol versions; it finds its
  # libraries through $ORIGIN/gpu, $ORIGIN/cpu and the build's RUNPATH.
  dontPatchELF = true;
  installPhase = ''
    runHook preInstall
    d=${share}
    mkdir -p $d/bin $out/bin $out/libexec
    cp -r scripts launcher $d/
    install -m644 LICENSE packaging/README-player.md $d/
    install -m755 out/tlg-probe out/tlg-gpu-capabilities $d/bin/
    install -Dm755 out/gpu/libtlggpu.so $d/bin/gpu/libtlggpu.so
    ${lib.optionalString arm "install -Dm755 out/fex/libtlgcpu.so $d/bin/cpu/libtlgcpu.so"}
    # Their debug info names the build environment's headers (the full GCC, Vulkan headers): Nix
    # would take those for dependencies. Symbols stay (crash reports).
    ${pkgs.binutils-unwrapped}/bin/strip --strip-debug $d/bin/tlg-probe $d/bin/tlg-gpu-capabilities
    # The launcher takes its own options (--play: the game with the saved settings).
    cat > $d/start.sh <<EOF
    #!${pkgs.bash}/bin/bash
    exec ${python}/bin/python3 $d/launcher/gr2_launcher.py "\$@"
    EOF
    chmod +x $d/start.sh
    runHook postInstall
  '';
  postFixup = ''
    # bin/gr2 is a static program (tlg-entry.c) that clears the host's LD_PRELOAD (Steam's
    # overlay) and similar before the wrapper's own dynamic programs start.
    gcc -O2 -Wall -Werror -static -L${pkgs.glibc.static}/lib -DTARGET="\"$out/libexec/gr2\"" \
      ${./tlg-entry.c} -o $out/bin/gr2
    # tlg_vulkan.py picks the Vulkan drivers (bundled Mesa, or the host's NVIDIA one) for the
    # launcher and the game it starts.
    makeShellWrapper ${python}/bin/python3 $out/libexec/gr2 \
      ${launcherEnv}      --add-flags "${share}/launcher/tlg_vulkan.py ${pkgs.bash}/bin/bash ${share}/start.sh"
  '';
  doInstallCheck = selftest;
  installCheckPhase = ''
    runHook preInstallCheck
    export HOME=$TMPDIR XDG_CONFIG_HOME=$TMPDIR/config XDG_DATA_HOME=$TMPDIR/data
    GR2_DATA_DIR=$TMPDIR/data/gr2-pc GR2_OUT_DIR=$TMPDIR/data/gr2-pc/out \
      ${python}/bin/python3 -c "import sys; sys.path[:0] = ['${share}/launcher', '${share}/scripts']; import gr2_launcher, gr2_run, prepare, link_modules, content_profile, gr2_patches; print('selftest ok')" \
      || { echo "the launcher's modules do not import (GR2_PACKAGE_SELFTEST=0 skips this check)" >&2; exit 1; }
    runHook postInstallCheck
  '';
  passthru = { inherit turnip python; };
  meta.mainProgram = "gr2";
}
