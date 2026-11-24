# Design note: concurrency on many cores

Status: accepted for issue
[#150](https://github.com/plumlang/plumlang/issues/150), with the
maintainer's decisions of 2026-11-24 (listed at the end); two questions
keep their proposed defaults until the maintainer confirms them. The rules
themselves are in [spec.md](../spec.md), sections 12.5 to 12.11 (planned
for 3.0); [#133](https://github.com/plumlang/plumlang/issues/133) builds
them. This note says why the design is what it is, what it costs, what the
2.3 VM work must keep possible, and what was decided.

## Where Plumlang is today

- **One thread.** All tasks run on one operating-system thread, taking
  turns (spec 12.4). The VM (`src/vm/vm.c`) keeps the running task's stack
  and frames, a ready queue, a timer heap and a poller for sockets. A task
  runs until it waits; nothing ever runs at the same moment as anything
  else.
- **Plain reference counts.** Every heap object starts with
  `PlumObj { uint32_t refcount; uint8_t type; }` (`src/vm/value.h`).
  `plum_retain` is a plain `refcount++`, and copy-on-write asks
  `refcount == 1` before changing a list or map in place
  (`plum_list_unique`, `plum_table_unique`).
- **No global mutable state.** PREAMBLE 3.3 already puts every piece of
  run-time state in a `PlumVM`, so two VMs in one process share nothing
  but the C library. An isolate is, nearly, a VM that a scheduler thread
  runs.
- **Tasks already don't share objects.** The checker refuses a tree
  object given to `spawn` (E0426, `check_spawn_args` in
  `src/check/typecheck.c`), and tasks talk through channels. Programs
  written this way are already shaped for isolates.
- **The program is reference counted too.** Functions, tree infos and
  constant strings are `PlumObj`s with counts. Several isolates running
  one program would race on those counts; see "What #133 changes" below.

## The alternatives

### 1. One shared heap, atomic reference counts

Every thread sees every object; every retain and release is an atomic
read-modify-write. This is Swift's model, and Rust's `Arc` used for
everything.

- **Cost on every program.** An uncontended atomic increment costs several
  times a plain one (a locked instruction on x86, a load-linked pair or
  LSE atomic on ARM, plus the ordering it imposes on the compiler), and a
  contended one bounces a cache line between cores. Plumlang touches
  counts on nearly every value move, so a single-threaded program would
  pay for multicore it never uses. #122 removes some count updates, not
  all.
- **Copy-on-write gets harder.** `refcount == 1` is only true for a moment
  when another thread can retain the value, so every in-place change
  needs an acquire load and care.
- **It is not memory-safe by itself.** Two threads pushing onto one list
  race on its length and buffer: a use-after-free in the runtime, which
  PREAMBLE 2.4 rules out. Every mutable object would need a lock, or the
  language would need a type system that proves no sharing.
- **It breaks spec principle 1.2,** no data races, unless every changeable
  object is locked.

### 2. A global interpreter lock

CPython until 3.12 and Ruby's MRI: threads exist, but only the one holding
the lock runs Plumlang code; the lock is let go around waits.

- Simple and safe, but it gives **no parallelism for Plumlang code**, which
  is the whole point of #133. Waiting concurrently is something Plumlang
  already has, through tasks, without threads.
- Rejected: all the cost of threads, none of the gain.

### 3. Free threading

CPython 3.13's `--disable-gil` build: biased reference counting (plain
counts for the owning thread, atomic for others), per-object locks on
lists and dicts, a thread-safe allocator (mimalloc), deferred counting and
quiescent-state reclamation for some objects.

- It works, but it is a large body of subtle code: CPython needed a new
  allocator and years of work, and its single-threaded speed dropped at
  first. Plumlang is a small C11 runtime with no dependencies (PREAMBLE
  2.1, 2.7).
- The language still allows races: two threads changing one object get
  some interleaving. Memory-safe, but results depend on timing, which is
  what principle 1.2 promises won't happen.

### 4. Isolates (chosen)

Each worker owns a heap and a VM; a pool of one thread per core runs the
isolates, as Erlang's schedulers run its processes. Isolates share nothing
changeable and talk through channels. Erlang processes, Dart isolates,
JavaScript workers and Pony actors are all versions of this.

## What we take from whom

| System | What it does | What Plumlang takes |
|---|---|---|
| Erlang/BEAM | a heap per process; messages copied into the receiver's heap; binaries over 64 bytes shared by atomic count; errors travel through links and monitors; many processes scheduled M:N over one scheduler thread per core, preempted after a budget of reductions; blocking work on dirty schedulers | per-isolate heaps; copying as the default; big immutable data shared by count, which here is any frozen value; errors delivered to whoever waits; the M:N scheduler, with steps for reductions and helper threads for dirty work |
| Dart | `Isolate.run(closure)` copies what the closure captures; deeply immutable objects are shared; `Isolate.exit` hands its result over without copying | `workers.start(() => ...)` with captures copied at start; frozen values shared; the worker's result and error handed over whole (spec 12.8) |
| JavaScript workers | structured clone copies; transferables (an `ArrayBuffer`, a `MessagePort`) move, leaving the sender's copy empty | moving instead of copying, but only on the last use of a variable, so the sender never sees an emptied value; handles move like transferables (spec 12.6.3); in the browser build, each worker *is* a Web Worker (spec 12.5.4) |
| Pony | reference capabilities in the types: `iso` (unique, can be sent), `val` (immutable, can be shared), and more | `val` becomes `Frozen[T]`, in the types; uniqueness stays out of the types and is found at run time (count of 1 plus last use), to keep the type system small |

## Why isolates

1. **No new cost for one-core programs.** Counts stay plain inside an
   isolate. The common path gains one predictable test (below), not an
   atomic.
2. **Safe by construction.** No changeable object is reachable from two
   threads, so there is nothing to race on. The runtime stays memory-safe
   without a lock per object, and programs get no data races (principle
   1.2).
3. **It fits what exists.** A VM with no global state is an isolate;
   E0426 already taught programs not to share objects with tasks; values
   already copy on assignment, so copying on send is the rule people know.
4. **It fits the main workload.** An HTTP server is many independent
   requests: one isolate per core scales without anything shared on the
   hot path.

The price: a program can't share a changeable object between cores. It
sends values, freezes what it shares, or keeps the object in one worker
and asks it through channels. For the programs Plumlang is for, that is
the right trade.

## Choices inside the model

- **A module function, not a keyword.** `seed workers` and
  `workers.start(f)` rather than a `worker f()` statement. `worker` is a
  name programs already use for variables, and a keyword would break
  them. Starting a worker costs more than `spawn` (a VM, a heap, its own
  copy of the modules it uses) and is done less often, so a visible module
  call suits it; `spawn` is frequent and cheap and earns its syntax. The checker treats `workers.start` as
  specially as it treats `spawn`, so nothing is lost in checking.
- **A lambda, not arguments.** `workers.start(() => crunch(rows))` needs
  no variadic, generic signature: the captures are the arguments, checked
  like any value that crosses (E0701), and E0704 stops the one confusing
  case, assigning a captured variable.
- **Objects don't cross channels.** A channel's sender keeps running, and
  its objects stay shared with it, so a copy would quietly break "objects
  are shared" (spec 8.3) and a move would need ownership in the types.
  A worker's result and error are different: the worker has ended, so
  nothing is left to share with, and they are handed over whole.
- **`Frozen[T]` is a built-in generic type, like `Task[T]` and
  `Channel[T]`,** not a keyword. Values need no marker: a value never
  changes under its holder anyway, so freezing one is visible only in
  speed. Objects need one, because a frozen object must refuse changes
  and the checker must know which methods change it.
- **`freeze` copies unless the value is unique.** Freezing in place an
  object that other references still treat as changeable would turn
  their later field assignments into run-time errors. Copying keeps the
  rule simple: a `T` is always changeable, a `Frozen[T]` never.
- **The main file's variables stay in the main isolate** (E0703), and
  imported modules load once per isolate. Re-running the main file in a
  worker would re-run the program; sharing its variables would share
  changeable state.
- **The shared `tree` of the old 12.3 is dropped.** It was never
  implemented, and an object with atomic methods is a shared mutable heap
  by another name. Frozen values cover the read-only sharing it was for.
- **Isolates are scheduled M:N, as on BEAM** (spec 12.5.3), not one OS
  thread per worker. A thread per worker would make every worker cost a
  stack of megabytes and a kernel thread, cap programs at a few hundred,
  and leave fairness to the OS scheduler, which knows nothing of channel
  waits. With a pool of one scheduler thread per core, a waiting worker
  costs only its memory, thousands are fine, and a worker per connection
  or per actor-like piece of state becomes a reasonable design. The price
  is a scheduler in the runtime: run queues, preemption and a place for
  calls that block, all below.
- **Preemption counts steps.** The VM already counts calls and backward
  jumps for `max_steps` (`src/vm/vm.c`); the scheduler reuses that
  counter as BEAM uses reductions. An isolate is only paused there,
  between instructions, where its whole state is in its `PlumVM` and none
  on the C stack. Preemption is between isolates only: inside an isolate,
  tasks still switch only when they wait (spec 12.4), so no code that is
  correct today gains an interleaving it didn't have.
- **Handles move** (spec 12.6.3). `http.serve` needs to hand sockets
  between isolates wherever the OS can't spread connections over several
  listeners, and once the runtime can do it, programs can too: an
  acceptor and a crew of workers is a common shape. A handle is one thing
  of the OS, so it moves rather than copies; the checker catches the
  common misuse (E0707) and the runtime catches the rest by marking the
  sender's handle moved.
- **In the browser, a worker is a Web Worker** (spec 12.5.4). Each has
  its own `plum.wasm` instance and linear memory, which is exactly an
  isolate, so sending maps onto `postMessage` with a transferred buffer
  and nothing needs `SharedArrayBuffer` or cross-origin isolation
  headers, which many hosts of the playground can't set. Where Web
  Workers are missing, the worker runs as an isolate on the page's one
  thread, the M:N scheduler with one thread, so programs keep their
  meaning and only lose parallelism.

## Performance

What to expect, to be confirmed by #133's benchmarks:

- **One isolate.** `retain` and `release` test one header bit (frozen or
  immortal) before the plain count change. The branch is almost never
  taken and is perfectly predicted; expect no measurable change on
  `bench/`, and a gain where program constants become immortal and stop
  being counted at all. The bar: within noise on every benchmark, checked
  with `python3 bench/run.py --runs 7`.
- **Sending.** A move costs one walk over the value checking counts (a
  list of numbers: constant time; a list of n unique strings: n checks,
  no copying). Sharing a frozen value: one atomic increment. A copy: the
  value's size, like `structuredClone` or Erlang's message copy. Large
  data that many workers read should be frozen once.
- **Channels between isolates.** A lock and, when the receiving isolate
  has nothing else to run, putting it back on a run queue (and waking a
  parked scheduler thread if one sleeps): around a microsecond, not
  nanoseconds. Per-item hand-offs in a hot loop should still be batched
  into lists.
- **Starting a worker.** No thread is created: a `PlumVM`, a heap and
  loading the modules it uses (their top-level code runs once per
  isolate), so its cost is mostly those modules. Expect tens of
  microseconds and a few tens of kilobytes for a worker that seeds
  little. Thousands of workers are fine; only `workers.cores()` of them
  run at any moment.
- **Switching isolates.** A scheduler thread switching isolates swaps one
  `PlumVM` pointer; the cost is the new isolate's data being cold in the
  cache. The step budget (BEAM uses 4,000 reductions; #133 tunes ours
  with benchmarks) keeps switches rare for busy isolates, and an isolate
  that waits gives its thread up at once.
- **Work stealing.** Each scheduler thread has its own run queue; an idle
  one steals runnable isolates from the others, so no single lock sits on
  the common path. An isolate that moves to another core moves its
  working set with it; stealing only from a queue longer than one, and
  preferring to resume an isolate on the thread it last ran on, keeps
  that rare.
- **Blocking calls.** DNS lookups, `os.run`, file reads and `ask` run on a
  small pool of helper threads (BEAM's dirty I/O schedulers), so a slow
  disk or resolver never takes a scheduler thread away from the other
  isolates. The call's isolate waits as it does today. The hand-off costs
  a few microseconds, far less than the calls themselves.
- **Moving a handle.** A pointer and a descriptor change owner, with the
  bytes it has buffered; on the poller side, re-registering the socket
  for its new isolate. Microseconds, independent of what it has read.
- **The browser.** A Web Worker is a thread of the JavaScript engine with
  its own `plum.wasm` instance: starting one takes milliseconds and a few
  megabytes, so there a program should start about
  `workers.cores()` workers and keep them. Every crossing is a copy into
  a transferred buffer, frozen values included (once per receiving
  worker), since instances share no memory.
- **HTTP.** With one isolate per core and nothing shared on the request
  path, throughput should scale nearly linearly with cores until the
  network or memory bandwidth limits it; #133's "done when" measures this
  on `bench/net`.
- **Frozen values.** Each retain and release on them is atomic, so a hot
  loop over a frozen list is slower than over a plain one. Code that
  reads frozen data heavily can copy the part it needs (`plant mine =
  shared_list + []`) into its own isolate.

## What the 2.3 VM work must keep possible

The 2.3 issues change the value representation and reference counting.
None of them needs to implement isolates; each must not close the door.

- **#121, NaN-boxed values.** Keep values self-contained: no pointer
  scheme that relies on a process-wide table (of small strings, handles
  or boxed ints) that isolates would have to share or lock. Keep the
  object header's count a naturally aligned 32-bit word of its own, which
  C11 `_Atomic` or the platform's interlocked calls can change when the
  object is frozen; don't pack other changing bits into the same word.
  Leave room in the header for a flags byte (frozen, immortal, a channel
  shared between isolates).
- **#122, fewer count updates.** Elision is per isolate and stays so:
  a borrowed value is kept alive by a holder in the same isolate, which
  is still true for frozen values. "Move on last use" is exactly what a
  zero-copy `send` and `freeze` need, so keep last-use information
  available to calls the compiler treats specially. Copy-on-write must
  treat a frozen value as never unique, whatever its count reads, and no
  elision may assume a frozen value's count is stable: other threads
  change it.
- **#148, appending to a string in place.** The one-owner test must also
  check the frozen bit: a frozen string is never changed in place.
- **#119, inline caches.** A per-call-site cache written while the program
  runs must not live in bytecode shared by isolates, or it becomes a data
  race. Keep such caches in the VM (per isolate), or fill them only at
  compile time.
- **#123, superinstructions and a register VM.** Fine either way, as long
  as compiled code is read-only once compiling ends: no quickening that
  rewrites shared bytecode in place.
- **#120 and #118.** No constraint beyond the above: frames and typed
  opcodes are per VM.
- **Allocation.** Memory moved to another isolate is freed by another
  thread, so `plum_alloc` must stay safe to call from any thread (the C
  library's `malloc` is) or gain a way to free remotely. `--mem-stats`
  counts per isolate and adds them up.
- **No new global state.** Interning tables, caches and random states
  added at run time live in the VM (PREAMBLE 3.3).
- **No state on the C stack between steps, and none in thread-locals.**
  An isolate is paused at a step and may resume on another scheduler
  thread (or, in the browser, after returning to the JavaScript event
  loop), so the interpreter loop must be able to return to the scheduler
  with everything in the `PlumVM`. Calls into the runtime that call back
  into Plumlang (`map`, `filter`, `to_str`) hold C frames; the step
  budget may run out inside them, and the pause then waits until they
  return, as waiting already does (spec 12.4). `_Thread_local` caches
  would follow the thread, not the isolate: keep per-isolate state in the
  VM.

## What #133 changes, in outline

- An isolate is today's `VM`, with its own tasks and timers. A pool of
  scheduler threads, one per core (pthreads, or the Windows thread API,
  since C11 `<threads.h>` isn't everywhere), runs them: a run queue per
  thread, work stealing between them, parking on a condition variable
  when there is nothing to run. Handing an isolate from one thread to
  another goes through a queue whose lock orders its memory, so the
  isolate's plain counts stay correct.
- The interpreter loop returns to the scheduler when the isolate's step
  budget runs out (the `max_steps` counter, at calls and backward jumps)
  or when all its tasks wait.
- I/O readiness moves from a poller per VM to the runtime: one poll
  thread (epoll, kqueue, or IOCP on Windows, where `WSAPoll` doesn't
  share well between threads) turns readiness and timer expiry into run
  queue entries for the isolate that owns the socket or timer. A
  scheduler thread with nothing to run may poll without blocking first.
- Calls that block in the OS (DNS lookups, `os.run`, file reads, `ask`)
  go to a small pool of helper threads; their isolate waits and the
  helper puts it back on a run queue when the call returns.
- The compiled program (functions, tree infos, constants) is built once
  and made immortal: its objects are never counted, so every isolate can
  read them.
- The header gets a frozen bit. `plum_retain` and `plum_release` take the
  atomic path only when it is set.
- Sending walks the value once: a frozen node is shared, a node with a
  count of 1 that the sender gives up is moved, anything else is copied.
  A table of nodes already seen keeps the shape of what is sent (the same
  string twice stays one string; a handed-over object graph keeps its
  cycles and its `weak` fields point into the copy).
- A channel gets a "shared" flag when it first crosses into another
  isolate; from then on its operations take a lock and wake receivers
  through their isolate's poller. Channels that never cross keep today's
  code.
- `stop()` sets a flag the worker checks where it already counts steps
  (`max_steps`: calls and backward jumps) and when it waits.
- Handles (`Connection`, `Listener`, `UdpSocket`) get a "moved" state.
  Sending one creates the receiver's handle with the descriptor, buffered
  bytes and TLS context, marks the sender's moved (its uses throw the
  closed-handle `NetError`, its waiting tasks wake with it), and
  re-registers the socket with the poll thread for the new isolate. The
  checker adds E0707: a use of a variable after a send, capture or return
  that moved its handle.
- `http.serve` opens a listener per isolate with `SO_REUSEPORT` on Linux
  (`SO_REUSEPORT_LB` on FreeBSD); elsewhere it runs one accepting isolate
  that moves each connection to the serving isolate with the fewest open
  connections. On Linux, connections already queued on one isolate's
  listener when it closes are lost, so `http.serve` keeps its listeners
  open until the whole server stops.
- The browser build (`src/wasm/`) starts a Web Worker per worker, sends
  it the compiled `WebAssembly.Module` (which `postMessage` can carry)
  and the program, and carries crossings as serialized bytes in a
  transferred `ArrayBuffer`. A frozen value carries an id, so each
  receiving instance keeps one copy of it and `==` between isolates still
  compares identity. The VM's wait for events returns to the JavaScript
  event loop instead of blocking, so the main isolate (on the page's
  thread) can wait for a worker's message. Without Web Workers, all
  isolates run on the page's thread with the one-thread scheduler, and the
  first `workers.start` prints the note of spec 12.5.4.
- Deadlock detection counts, under one lock, the isolates that have a
  task able to run or a wait that can end.
- The checker gains a `sendable` test on types, built from the existing
  `holds_object` (`src/check/typecheck.c`), and a "changes self" flag on
  methods, for E0706.
- Tests run under ThreadSanitizer as well as ASan and UBSan.

## Decisions (maintainer, 2026-11-24)

The questions this proposal left open, with what was decided. Questions 7
and 8 weren't answered; they keep the proposed default, which the
maintainer can still change.

1. **The API name.** Decided: `seed workers` and `workers.start(f)`, as
   proposed.
2. **Module variables.** Decided as proposed: the main file's `plant`
   variables stay out of workers (E0703); imported modules load once per
   isolate.
3. **Objects on channels.** Decided as proposed: not sendable unless
   frozen.
4. **Handing over results and errors with objects.** Decided as
   proposed: a worker's result and error are handed over whole, objects
   and trace included (spec 12.8).
5. **Copy or freeze on send.** Decided: a value that isn't uniquely owned
   is copied.
6. **`Frozen[T]` or a `frozen` modifier.** Decided: `Frozen[T]`, a type,
   and `freeze(x)` stays the way to make one, plain values included.
7. **The `shared` tree** of the old 12.3. *Default, pending confirmation:*
   dropped in favour of `Frozen[T]`, as proposed.
8. **The cycle collector.** *Default, pending confirmation:* an optional
   per-isolate collector, `--collect-cycles`, off by default, as proposed.
9. **Scheduling.** Decided, changed from the proposal: M:N, as Erlang
   does. Isolates run on a pool of one scheduler thread per core, with
   preemption by step budget and blocking calls on helper threads (spec
   12.5.3; "Choices inside the model" and "Performance" above).
10. **`exit()` in a worker.** Decided as proposed: it ends the whole
    program.
11. **`stop()`.** Decided as proposed: it takes effect at the next call,
    loop turn or wait, runs `finally` blocks, can't be caught, and
    `wait()` throws `StoppedError`.
12. **Handles between isolates.** Decided, changed from the proposal:
    moving a handle is part of the language. `Connection`, `Listener` and
    `UdpSocket` move, the sender can't use them after (E0707, and a
    run-time check), and `http.serve` uses `SO_REUSEPORT` listeners per
    isolate on Linux and FreeBSD, and one accepting isolate that moves
    connections elsewhere (spec 12.6.3, 12.11).
13. **The browser build.** Decided, changed from the proposal: each worker
    runs in a Web Worker of its own, with no shared memory and no
    cross-origin isolation headers; where Web Workers are missing, the
    worker runs on the page's thread with a note (spec 12.5.4).
14. **Function values.** Decided as proposed: functions stay unsendable.
15. **Error codes.** Decided as proposed: a new range, E07xx.

## Still open

Only the two defaults above wait for the maintainer's confirmation:

- **7.** Dropping the never-implemented `shared` tree of the old 12.3.
- **8.** Shipping `--collect-cycles` as an opt-in, off by default, rather
  than leaving it out and relying on `weak` and `--check-leaks`.
