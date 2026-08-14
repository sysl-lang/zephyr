# zephyr

The Zephyr RTOS kernel for sysl — threads, semaphores, mutexes, condition variables, events, message
queues, timers and work queues, on the kernel *your* build configured.

```sysl
import sh.sysl.zephyr.*

var control: [thread_words]long = [0; thread_words]
var ready_storage: [sem_words]long = [0; sem_words]

c const
    stack_bytes: usize = "K_KERNEL_STACK_LEN(1024)"

@align(stack_align)
var stack: [stack_bytes]u8 = [0; stack_bytes]

worker(a: *u8, b: *u8, c: *u8)
    for _ in 0..<5
        print("working")
        sleep(msec(100))

    semaphore_at(ready_storage[..]) match
        Some(s) -> s.give()
        None -> ()

@export("main")
run() -> int
    val ready = semaphore(ready_storage[..], 0, 1).unwrap

    spawn(control[..], stack[..], &worker, 5)

    ready.take(forever())
    print("the worker finished")
    0
```

That is a whole program. There is no C in it, and none in the project that builds it: sysl exports
`main` and Zephyr's kernel thread calls it.

```hocon
dependencies {
  zephyr { git = "github.com/sysl-lang/zephyr", version = "0.2.0" }
}
```

`sysl-lang/zephyr-demo` is a worked example and is also this package's test suite — 70 checks against
the real kernel under QEMU.

## Zephyr owns the build, and everything here follows from that

Zephyr is not a library you link against. It is CMake plus Kconfig plus devicetree plus `west`: the
configuration decides what the kernel *is*, one generated header supplies every `CONFIG_*` the kernel
headers test, another lists the syscalls, and the final link is driven by a linker script the build
wrote.

So the arrangement is the other way up, exactly as `pico2` does it for the pico-sdk. **A program using
this package is built with `sysl build-c` into a static archive, and Zephyr's CMake links it.** A
plain `sysl build` will compile it happily and then fail to link.

What keeps the binding honest is `c const`: every struct size, every alignment, the tick rate and
`K_TICKS_FOREVER` are measured out of your build's headers by the C compiler on every build. Not one
number here is transcribed. The suite prints them, and they move with the configuration — turning
`CONFIG_EVENTS` on takes `struct k_thread` from 14 machine words to 15, and this package notices.

## Kernel objects are storage you own

That is Zephyr's model rather than a choice this package made, and it is what lets the whole kernel
run with no allocator at all. `struct k_sem` is a real struct that lives wherever you put it: there is
nothing to allocate, nothing to fail and nothing to free.

So every constructor takes a slice of storage, sized by a `c const` this package exports:

```sysl
var storage: [sem_words]long = [0; sem_words]

val sem = semaphore(storage[..], 0, 1).unwrap
```

**`long` rather than `usize`, and it is load bearing.** A kernel object holds an `int64_t` timeout
under the default `CONFIG_TIMEOUT_64BIT`, so it wants eight-byte alignment even on a 32-bit machine,
which an array of 32-bit words would not give it. An `@assert` in each module checks that against the
alignment the C compiler reports rather than trusting the argument.

**The storage must outlive the object**, which in practice means module storage. A kernel object in a
frame that returns is a scheduler walking freed memory, and nothing will say so.

**A second thread reaches the same object with `semaphore_at`, not `semaphore`.** The constructors
initialize; calling one twice resets the object out from under whoever was waiting on it. `_at` is the
view: `semaphore_at`, `mutex_at`, `condvar_at`, `event_at`, `message_queue_at`, `timer_at`.

**A thread's stack is the one thing this package cannot size for you.** How many bytes a stack of a
given size occupies, and what it must be aligned to, are macros over the architecture and the MPU
configuration — and a sysl array length is a compile-time constant, so the measurement has to happen
in the module that declares the array:

```sysl
c const
    stack_bytes: usize = "K_KERNEL_STACK_LEN(1024)"

@align(stack_align)
var stack: [stack_bytes]u8 = [0; stack_bytes]
```

That is `K_KERNEL_STACK_DEFINE` written out, and it is what that macro does.

## What crosses into a thread

`06`'s rules are the language's, and a Zephyr thread is an ordinary domain. **A plain `&T` may not
cross into one** — its count is not atomic, which is the race the whole model exists to prevent.
`&sync T` is the spelling that may, and `sysl.sync`'s `Atomic` and `SpinLock` need no capability and
are already reachable on a bare machine.

**As of 0.2.0 the compiler says that, rather than this paragraph.** `thread` is generic in its three
arguments and marked `@crossing(p1, p2, p3)`, so each is walked at every call:

```
error: what 'p1' of 'sh.sysl.zephyr.thread' points at reaches another concurrency domain, so every
count inside it has to be atomic — but its 'c' reaches a '&app.Cell', whose count is not. Hold it as
a '&sync app.Cell' ('06')
```

**Those three were `*u8` until 0.2.0, and that is why this release breaks a caller.** A `*u8` is what
`k_thread_create` takes and it has thrown the pointee away, so the walk found a byte and passed
whatever was handed over — the annotation was unwritable here for exactly that reason. The types stay
on this signature and are cast away on the way out, so the kernel gets what it always got.

`A`, `B` and `C` are read off the body, so a caller writes no more than it did:

```sysl
worker(cfg: *Config, q: *u8, p3: *u8) = ...

thread(control[..], stack[..], &worker, &cfg, no_arg, no_arg, 5, no_wait(), no_options)
```

Three type parameters rather than one, because the kernel's three arguments are unrelated and one
parameter would insist they were the same type.

Two things change for a caller that was already there. A body written `(a: *u8, b: *u8, c: *u8)` with
casts beside it still compiles and still says nothing — **type the body and the casts go away.** And
**`null` can no longer be written at a `thread` call**: it takes its type from its context, and the
context is the `*T` being inferred, so there is none. `no_arg` is the constant to write instead.

`spawn` is unchanged and carries no annotation, because it hands the thread nothing to check.

## The surface

| module file | what it binds |
|---|---|
| `zephyr.sysl` | `Timeout` and its constructors, `sleep`, `busy_wait`, `uptime`, `cycles`, `yield_now`, `in_isr`, `sched_lock` |
| `thread.sysl` | `Thread`, `spawn`, `thread`, `current`, priorities, `join`, `suspend`/`resume`, `abort` |
| `sync.sysl` | `Semaphore`, `Mutex`, `Condvar` |
| `event.sysl` | `Event` — thirty-two flags, `wait_any` and `wait_all` |
| `msgq.sysl` | `MessageQueue` — fixed-size messages, copied in and out |
| `timer.sysl` | `Timer`, one-shot and periodic, with or without a callback |
| `work.sysl` | `Work` and `DelayableWork` on the system work queue |

**A timeout is a type rather than a number**, so a tick count cannot be handed to a call that wanted
milliseconds: `no_wait()`, `forever()`, `msec`, `sec`, `usec`, `ticks_of`. It crosses to C as a tick
count rather than as `k_timeout_t`, because that struct is eight bytes under `CONFIG_TIMEOUT_64BIT`
and four without it — mirroring it would have fitted one configuration.

**Zephyr's mutex is recursive**, which is a real difference from most kernels: a thread that locks one
it already holds succeeds and raises a count. **Its priorities run the other way from most kernels**
too — lower numbers run first, and negative ones are cooperative.

## Using it from CMake

The demo's `CMakeLists.txt` is the copy-paste, and each of these lines is there because leaving it out
fails silently or fails misleadingly.

```cmake
add_custom_command(
  OUTPUT ${SYSL_ARCHIVE}
  COMMAND sysl build-c ${CMAKE_CURRENT_SOURCE_DIR}
          --target ${SYSL_TARGET} -o ${SYSL_ARCHIVE}
          --include-path zephyr=${ZEPHYR_BASE}/include
          --include-path zephyr-generated=${PROJECT_BINARY_DIR}/zephyr/include/generated
          --include-path libc=${ARM_SYSROOT}/include
          ${SYSL_INCLUDE_ARGS} ${SYSL_DEFINE_ARGS} -D __ZEPHYR__=1 -D KERNEL
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/app.sysl)

add_custom_target(sysl_archive DEPENDS ${SYSL_ARCHIVE})
add_dependencies(sysl_archive syscall_list_h_target kobj_types_h_target driver_validation_h_target)
add_dependencies(zephyr_pre0 sysl_archive)
add_dependencies(zephyr_final sysl_archive)
set_property(TARGET zephyr_pre0 APPEND PROPERTY LINK_DEPENDS ${SYSL_ARCHIVE})
set_property(TARGET zephyr_final APPEND PROPERTY LINK_DEPENDS ${SYSL_ARCHIVE})

target_sources(app PRIVATE ${ZEPHYR_BASE}/misc/empty_file.c)

zephyr_link_libraries(-Wl,--whole-archive,${SYSL_ARCHIVE},--no-whole-archive)
zephyr_link_libraries(-Wl,-u,z_impl_k_sem_init -Wl,-u,z_impl_k_mutex_init
                      -Wl,-u,z_impl_k_condvar_init -Wl,-u,z_impl_k_event_init
                      -Wl,-u,k_msgq_init -Wl,-u,k_timer_init -Wl,-u,k_work_init)
```

### The seven things that go wrong, and what each looks like

1. **`--whole-archive`, or the image boots and the program is not in it.** Zephyr keeps a weak `main`
   in `kernel/main_weak.c`, and an archive is searched only for symbols still undefined — so that weak
   definition resolves `main` before this archive is opened. Write it as a **single** argument with
   commas: CMake reorders a `.a` path and two flags independently and leaves the pair wrapped around
   nothing.
2. **`-u` for the kernel subsystems only sysl uses.** `libkernel.a` is listed *before* the
   application's link flags and is searched once, so a member no C referenced is already gone:
   `undefined reference to z_impl_k_event_init`, which reads like a missing `CONFIG_` and is not. One
   symbol names one object file, and it costs nothing for a subsystem the program never calls —
   `--gc-sections` drops the wrappers, and then the kernel functions they called. **Do not fix this by
   naming `libkernel.a` a second time**: that loads every member the shim mentions, each with its
   static objects and init entries, and on a small board the image hard-faults in `k_mutex_unlock`
   before `main` prints a character.
3. **`CONFIG_NEWLIB_LIBC=y`.** sysl emits `__aeabi_memcpy`, an EABI alias newlib carries and libgcc
   does not; Zephyr's minimal libc has plain `memcpy` and none of the aliases.
4. **The sysl step is a *build*-time command, not `execute_process`.** `zephyr/kernel.h` includes
   `zephyr/syscall_list.h`, which Zephyr generates — hence the dependency on `syscall_list_h_target`.
5. **`LINK_DEPENDS`, or an edit does not relink.** `add_dependencies` gives ordering and does not make
   the archive a link *input*, so a rebuild runs the compiler, writes a new archive, does not relink,
   and reports success with the old code in the image.
6. **Pass Zephyr's own `-D`s and the toolchain sysroot.** Without the defines a vendor HAL `#error`s
   ("Unknown device"); without the sysroot `<newlib.h>` is not found. Read both off `zephyr_interface`
   and skip entries matching `$<` — the include list ends in a generator expression that expands to
   nothing, and a bare `--include-path` then eats the next flag as its value.
7. **The sysroot goes last**, which is where Zephyr puts it too. First, the toolchain's `limits.h` and
   `stdint.h` win over Zephyr's own shims and a dozen macros are redefined.

`CONFIG_EVENTS=y` is needed for `event`; `kernel/events.c` is not compiled without it.

## Boards this can be built for

**Armv6-M works and Armv7-M cannot be built at all**, which is a gap in sysl's target registry rather
than in this package: there is no target describing an FPU-less core above Armv6-M, and Zephyr's
defaults are the FPU-less case. `qemu_cortex_m0` with `--target thumbv6m-freestanding` is the tested
combination; a Cortex-M33 board with the FPU on takes `thumb-freestanding`.

## What is not here

**No devicetree.** `DEVICE_DT_GET(DT_NODELABEL(led0))` resolves to an ordinal the build generates, so
no committed sysl file can name it. `device_get_binding("...")` at run time works today; the good
long-run answer is a generator that reads `devicetree_generated.h`, which is its own piece of work.

**No `SYS_INIT`, `LOG_MODULE_REGISTER`, `K_THREAD_DEFINE` or driver registration.** Those are Zephyr's
iterable sections, and sysl gained `@section` in 0.0.47 — so they are now buildable and simply are not
built.

**No pipes, stacks, slabs, `k_heap`, mailboxes, `k_poll` or futexes**, and no interrupt-handler
registration. Nothing about them is hard; they are the next piece of work.

**No `sysl test`.** A kernel image has no process to start and no exit status to read, so this
package's tests live in `sysl-lang/zephyr-demo` and run under QEMU.

## Needs

sysl **0.0.52** or newer, which is the release that added `@crossing`; a Zephyr workspace; and a
bare-metal Arm toolchain. The Zephyr SDK is not required — `arm-none-eabi-gcc` with
`ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb` is what this was built and tested with.

0.1.0 is the last version that builds on an older sysl, and its `thread` takes three `*u8`.

## Licence

ISC.
