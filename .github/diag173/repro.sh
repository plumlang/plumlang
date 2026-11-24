set -x
. .github/diag173/common.sh
# LTO with debug info
CFLAGS="-g" LDFLAGS="-g" make -j4 release-lto >/dev/null
cp plum plum-lto
set +x
echo "=== crashes (LTO -g)"; crashes ./plum-lto 2
echo "=== golden net dirs"
for d in tests/lang/http tests/lang/net tests/lang/tls; do sh tests/golden.sh ./plum-lto $d 2>&1 | tail -n 15; done
for t in tests/lang/http/hello.plum tests/lang/net/*.plum; do
  echo "=== lldb $t"
  lldb --batch -o 'settings set target.process.stop-on-exec false' -o run -k 'thread backtrace all' -k 'frame select 0' -k 'register read' -k 'frame variable' -k 'up' -k 'frame variable' -k 'up' -k 'frame variable' -k 'up' -k 'frame variable' -k 'up' -k 'frame variable' -k quit -- ./plum-lto run "$t" 2>&1 | tail -n 200
done
echo "=== MallocScribble/GuardEdges/StackLogging"
for t in tests/lang/http/hello.plum; do
  MallocStackLogging=1 MallocScribble=1 MallocGuardEdges=1 MallocCheckHeapStart=1 MallocCheckHeapEach=1 MallocErrorAbort=1 ./plum-lto run "$t" 2>&1 | tail -n 40
done
echo "=== without nano zone"
MallocNanoZone=0 ./plum-lto run tests/lang/http/hello.plum 2>&1 | tail -5
echo "=== LTO + -fno-strict-aliasing"
make clean >/dev/null; CFLAGS="-fno-strict-aliasing" make -j4 release-lto >/dev/null; crashes ./plum 2 && echo "still crashes" || echo "no crash"
echo "=== LTO + -fno-inline"
make clean >/dev/null; CFLAGS="-fno-inline" make -j4 release-lto >/dev/null; crashes ./plum 2 && echo "still crashes" || echo "no crash"
echo "=== plain release"
make clean >/dev/null; make -j4 >/dev/null; crashes ./plum 2 && echo "plain crashes" || echo "plain ok"
true
