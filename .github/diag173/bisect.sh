# Which translation units must be LTO'd for the crash: mix LTO (bitcode)
# and plain objects and link with -flto.
. .github/diag173/common.sh
make -j4 release-lto >/dev/null && make -j4 >/dev/null
L=build/release-lto P=build/release
objs=$(cd $L && find src vendor -name '*.o' | sort)
link() { # $@ = objects to take from the LTO build
    rm -rf mix; mkdir mix
    files=""
    for o in $objs; do
        case " $* " in *" $o "*) files="$files $L/$o";; *) files="$files $P/$o";; esac
    done
    clang $files -Wl,-dead_strip -O2 -DNDEBUG -flto -lm -o mix/plum 2>&1 | tail -3
}
test_set() { link "$@"; crashes mix/plum 2 >/dev/null; }
all=$objs
test_set $all && echo "all LTO: crash" || { echo "all LTO: no crash?!"; exit 0; }
srcs=$(echo "$objs" | grep '^src/')
test_set $srcs && echo "src only LTO: crash" || echo "src only LTO: no crash (vendor needed)"
# ddmin-style greedy reduction over $set
set_=$( test_set $srcs && echo "$srcs" || echo "$all")
chunk=$(( $(echo "$set_" | wc -w) / 2 ))
while [ $chunk -ge 1 ]; do
    changed=1
    while [ $changed = 1 ]; do
        changed=0
        arr=($set_); i=0
        while [ $i -lt ${#arr[@]} ]; do
            rest=("${arr[@]:0:$i}" "${arr[@]:$((i+chunk))}")
            if [ ${#rest[@]} -gt 0 ] && test_set "${rest[@]}"; then
                echo "drop: ${arr[*]:$i:$chunk}"; arr=("${rest[@]}"); changed=1
            else
                i=$((i+chunk))
            fi
        done
        set_="${arr[*]}"
    done
    chunk=$((chunk/2))
done
echo "=== minimal LTO set: $set_"
test_set $set_ && echo "confirmed crash" || echo "not reproducible?"
for o in $set_; do
  echo "--- only $o"; test_set $o && echo crash || echo ok
done
true
