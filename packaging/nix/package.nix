# Plumlang for Nix: the same package as nixpkgs' pkgs/by-name/pl/plum/package.nix,
# submitted in https://github.com/NixOS/nixpkgs/pull/592489 (in review).
# Until it lands, build it from a clone of this repository:
#
#   nix-build -E 'with import <nixpkgs> {}; callPackage ./packaging/nix/package.nix {}'
#
# The only difference from the nixpkgs copy is `maintainers`, which names an
# entry that exists only in nixpkgs' maintainer list.
{
  lib,
  stdenv,
  fetchFromGitHub,
  versionCheckHook,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "plum";
  version = "2.2.0";

  strictDeps = true;
  __structuredAttrs = true;

  src = fetchFromGitHub {
    owner = "plumlang";
    repo = "plumlang";
    tag = "v${finalAttrs.version}";
    hash = "sha256-th1RCqG5UnJ/pgvNCkLeNbUImRjLU7vvvp3vsP1UNeM=";
  };

  # The Makefile asks git for the commit `plum version` prints; there is no
  # .git in the source, so give it the tag.
  makeFlags = [ "COMMIT=${finalAttrs.src.tag}" ];

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
    changelog = "https://github.com/plumlang/plumlang/blob/${finalAttrs.src.tag}/CHANGELOG.md";
    license = lib.licenses.asl20;
    mainProgram = "plum";
    maintainers = [ ];
    platforms = lib.platforms.unix;
  };
})
