set -e
echo "=== make release-lto + full test suite"
make -j4 release-lto >/dev/null
make -j4 LTO=1 test 2>&1 | tail -n 40
if [ "$(uname -m)" = arm64 ]; then
  echo "=== make release-pgo (LTO + PGO) + full test suite"
  make -j4 release-pgo >/dev/null 2>&1
  make -j4 LTO=1 PGO=use test 2>&1 | tail -n 40
fi
echo "=== symbols check catches the old name"
sed -i.bak 's/static void send_message(/static void send(/; s/send_message(s, &w)/send(s, \&w)/' src/lsp/server.c
make -j4 >/dev/null
if sh tests/symbols.sh build/release build/release/plum; then echo "NOT CAUGHT"; exit 1; else echo "caught"; fi
mv src/lsp/server.c.bak src/lsp/server.c
