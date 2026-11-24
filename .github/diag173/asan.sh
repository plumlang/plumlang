. .github/diag173/common.sh
echo "=== ASan + LTO"
CFLAGS="-g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address" make -j4 release-lto >/dev/null || exit 1
for t in $NET_TESTS; do echo "--- $t"; ASAN_OPTIONS=detect_leaks=0 ./plum run "$t" 2>&1 | head -n 80; done
echo "=== UBSan + LTO"
make clean >/dev/null
CFLAGS="-g -fsanitize=undefined -fno-sanitize-recover=all" LDFLAGS="-fsanitize=undefined" make -j4 release-lto >/dev/null || exit 1
for t in $NET_TESTS; do echo "--- $t"; ./plum run "$t" 2>&1 | head -n 40; done
true
