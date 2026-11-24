# Plumlang for nixpkgs, built from source: pkgs/by-name/pl/plum/package.nix.
#
# Not submitted yet: v2.1.4 is under the Plumlang Commercial License only
# (unfree, and it may not be redistributed, so Hydra could not build it).
# Submit from the first Apache-2.0 release (2.2.0): set `version`, replace
# `hash` with the one `nix build` reports, and build it on Linux and macOS.
# After that, nixpkgs' update bot (r-ryantm) bumps it for each release.
# See packaging/README.md.
{
  lib,
  stdenv,
  fetchFromGitHub,
  versionCheckHook,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "plum";
  version = "2.2.0";

  src = fetchFromGitHub {
    owner = "plumlang";
    repo = "plumlang";
    tag = "v${finalAttrs.version}";
    hash = lib.fakeHash;
  };

  # The Makefile asks git for the commit `plum version` prints; there is no
  # .git in the source, so give it the tag.
  makeFlags = [ "COMMIT=v${finalAttrs.version}" ];

  enableParallelBuilding = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 plum "$out/bin/plum"
    runHook postInstall
  '';

  nativeInstallCheckInputs = [ versionCheckHook ];
  versionCheckProgramArg = "version";
  doInstallCheck = true;

  meta = {
    description = "Small, fast, memory-safe programming language";
    homepage = "https://plum.elixpo.com";
    changelog = "https://github.com/plumlang/plumlang/blob/v${finalAttrs.version}/CHANGELOG.md";
    # Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0: the commercial
    # licence is an alternative, so Apache-2.0 alone applies to nixpkgs.
    license = lib.licenses.asl20;
    mainProgram = "plum";
    maintainers = [ ];
    platforms = lib.platforms.unix;
  };
})
