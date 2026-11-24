cd red 2>/dev/null || { mkdir red; cd red; }
cat > a.c <<'X'
#include <stdio.h>
void call_send(int fd);
__attribute__((noinline)) static void send(const char *who) { printf("static send in a.c called (%s)\n", who); }
void lsp(void) { send("lsp"); send("lsp again"); }
int main(void) { lsp(); call_send(-1); return 0; }
X
if [ "$(uname)" = Darwin ]; then
  cat > b.c <<'X'
#include <stdio.h>
#include <sys/socket.h>
void call_send(int fd) { printf("libc send returned %ld\n", (long)send(fd, "GET /hello", 10, 0)); }
X
  brew install -q llvm >/dev/null 2>&1 || true
  CL=$(brew --prefix llvm)/bin/clang
else
  cat > b.c <<'X'
#include <stdio.h>
#include <stddef.h>
long sock_send(int, const void *, size_t, int) __asm__("send");
void call_send(int fd) { printf("libc send returned %ld\n", sock_send(fd, "GET /hello", 10, 0)); }
X
  sudo apt-get install -y -q lld >/dev/null 2>&1 || true
  CL=clang
fi
$CL --version | head -1
for f in "" "-flto" "-flto=thin"; do echo "== $f"; $CL -O2 $f $( [ "$(uname)" = Linux ] && echo -fuse-ld=lld ) a.c b.c -o t && ./t; echo "rc=$?"; done
