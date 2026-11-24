mkdir -p red && cd red
cat > a.c <<'X'
#include <stdio.h>
void call_send(int fd);
static void send(const char *who) { printf("static send in a.c called (%s)\n", who); }
void lsp(void) { send("lsp"); }
int main(void) { lsp(); call_send(-1); return 0; }
X
cat > b.c <<'X'
#include <stdio.h>
#include <sys/socket.h>
void call_send(int fd) {
    long r = (long)send(fd, "GET /hello", 10, 0);
    printf("libc send returned %ld\n", r);
}
X
echo "=== how sys/socket.h declares send"; clang -E b.c | grep -E '\bsend\(' 
for f in "" "-flto" "-flto=thin"; do
  echo "=== clang -O2 $f"; clang -O2 $f -c a.c -o a.o && clang -O2 $f -c b.c -o b.o && clang -O2 $f a.o b.o -o t && ./t
done
echo "=== IR names"; clang -O2 -flto -S -emit-llvm b.c -o - | grep -E 'send'; clang -O2 -S -emit-llvm a.c -o - | grep -E 'define.*send'
echo "=== x86_64"; for f in "" "-flto"; do clang -arch x86_64 -O2 $f a.c b.c -o t86 && (arch -x86_64 ./t86 || ./t86); done
clang --version | head -1
