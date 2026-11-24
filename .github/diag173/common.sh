# shellcheck shell=bash
NET_TESTS="tests/lang/http/hello.plum tests/lang/http/server_client.plum tests/lang/http/handle.plum tests/lang/net/*.plum tests/lang/tls/echo.plum tests/lang/tls/https.plum"
# run each net test N times; print status; return 0 if any crashed
crashes() {
    bin=$1; n=${2:-3}; any=1
    for t in $NET_TESTS; do
        for i in $(seq 1 "$n"); do
            out=$("$bin" run "$t" 2>&1); rc=$?
            if [ $rc -ge 128 ] || echo "$out" | grep -q 'not allocated\|malloc:'; then
                echo "  CRASH rc=$rc $t: $(echo "$out" | grep -m1 -i 'malloc\|abort\|fault')"; any=0; break
            fi
        done
    done
    return $any
}
