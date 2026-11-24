# Design note: concurrency on many cores

Status: proposed, for issue
[#150](https://github.com/plumlang/plumlang/issues/150). The rules
themselves are in [spec.md](../spec.md), sections 12.5 to 12.11 (planned
for 3.0); [#133](https://github.com/plumlang/plumlang/issues/133) builds
them. This note says why the design is what it is, what it costs, what the
2.3 VM work must keep possible, and what is still open.

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
  but the C library. An isolate is, nearly, a VM with a thread.
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

Each worker owns a heap and a VM on its own thread; isolates share nothing
changeable and talk through channels. Erlang processes, Dart isolates,
JavaScript workers and Pony actors are all versions of this.

## What we take from whom

| System | What it does | What Plumlang takes |
|---|---|---|
| Erlang/BEAM | a heap per process; messages copied into the receiver's heap; binaries over 64 bytes shared by atomic count; errors travel through links and monitors | per-isolate heaps; copying as the default; big immutable data shared by count, which here is any frozen value; errors delivered to whoever waits |
| Dart | `Isolate.run(closure)` copies what the closure captures; deeply immutable objects are shared; `Isolate.exit` hands its result over without copying | `workers.start(() => ...)` with captures copied at start; frozen values shared; the worker's result and error handed over whole (spec 12.8) |
| JavaScript workers | structured clone copies; transferables (an `ArrayBuffer`) move, leaving the sender's copy empty | moving instead of copying, but only on the last use of a variable, so the sender never sees an emptied value |
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
  them. Starting a worker is rare and costly (a thread, a heap), unlike
  `spawn`, so a visible module call suits it; `spawn` is frequent and
  cheap and earns its syntax. The checker treats `workers.start` as
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
- **Channels between isolates.** A lock and, when a receiver sleeps, a
  wakeup through the receiving isolate's poller (an eventfd, a pipe, or a
  posted completion on Windows): microseconds, not nanoseconds. Workers
  are for coarse work; per-item hand-offs should be batched into lists.
- **Starting a worker.** A thread, a VM and loading the modules it uses:
  tens to hundreds of microseconds. Start about one per core and keep
  them.
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

## What #133 changes, in outline

- An isolate is today's `VM` plus a thread (pthreads, or the Windows
  thread API, since C11 `<threads.h>` isn't everywhere), with its own
  poller, timers and tasks.
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
- Deadlock detection counts, under one lock, the isolates that have a
  task able to run or a wait that can end.
- The checker gains a `sendable` test on types, built from the existing
  `holds_object` (`src/check/typecheck.c`), and a "changes self" flag on
  methods, for E0706.
- Tests run under ThreadSanitizer as well as ASan and UBSan.

## Open questions for the maintainer

1. **The API name.** `seed workers` with `workers.start(f)` (this
   proposal), a keyword `worker f(x)` mirroring `spawn`, or a built-in
   constructor `Worker(f)` like `Channel[T]()`?
2. **Module variables.** This proposal keeps the main file's `plant`
   variables out of workers (E0703) and loads imported modules once per
   isolate. Alternatives: keep every module's variables out of worker
   code, or re-run every module but the main file.
3. **Objects on channels.** Not sendable unless frozen (this proposal).
   Alternatives: deep-copy them (Dart), or move them when the checker
   sees the send is the variable's last use (a light version of Pony's
   `iso`).
4. **Handing over results and errors with objects** (spec 12.8): keep, or
   limit a worker's result to sendable types and send errors as
   `WorkerError` only?
5. **Copy or freeze on send.** The issue says a value that isn't unique is
   deep-copied. The alternative freezes it in place: no copy ever, but
   the sender's later counts on it become atomic. Copy is proposed.
6. **`Frozen[T]` or a `frozen` modifier** like `weak`? And should `freeze`
   of a value (not an object) exist at all, given it changes only speed?
7. **The `shared` tree** of the old 12.3, never implemented, is dropped in
   this pull request in favour of `Frozen[T]`. Agreed?
8. **The cycle collector.** PREAMBLE 2.4 promises no garbage-collector
   pause. Ship `--collect-cycles` as an opt-in (proposed), or leave it out
   and rely on `weak` and `--check-leaks`?
9. **Scheduling.** One thread per worker (proposed, simple), or many
   isolates on a pool of one thread per core, as Erlang does, so that
   thousands of workers are cheap?
10. **`exit()` in a worker** ends the whole program (proposed), or only the
    worker?
11. **`stop()`.** Runs `finally` blocks and can't be caught (proposed), or a
    catchable `StoppedError` thrown at the next safe point?
12. **Handles between isolates.** On macOS and Windows, a shared listening
    port doesn't spread connections well; `http.serve` may accept in one
    isolate and hand sockets to others internally. Should moving a socket
    to a worker be part of the language too?
13. **The browser build** (`make wasm`) has no threads by default. Should
    `workers.start` there run the worker as a task on the one thread, or
    be an error?
14. **Function values.** A top-level function captures nothing and could
    cross safely, but its type, `() -> int`, doesn't say so. Leave
    functions unsendable (proposed), or add a way to say "captures
    nothing"?
