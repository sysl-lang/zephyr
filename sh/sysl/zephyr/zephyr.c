/* The whole of the C in this package, and it is all trampolines.
 *
 * **Zephyr's public API is mostly macros and `static inline` functions, so most of it has no symbol
 * to bind.** There are two separate reasons, and between them they cover nearly every name a program
 * would want:
 *
 *   - **A syscall.** `k_sem_take` and its sixty-odd siblings are declared `__syscall`, which makes
 *     the *documented* name a `static inline` dispatcher: with `CONFIG_USERSPACE=n` it forwards
 *     straight to `z_impl_k_sem_take`, and with `=y` it decides between that and a trap into the
 *     kernel. Only the `z_impl_` half is a real symbol.
 *   - **An accessor.** `k_sem_count_get`, `k_msgq_num_used_get` and the rest read a field, so they
 *     are inline all the way down and have no implementation symbol at all.
 *
 * A binding could declare the `z_impl_` names directly -- the probes that led to this package did
 * exactly that, and it works. **It is not what a shipped package should do**, for two reasons: it is
 * internal API, and it is wrong the moment somebody sets `CONFIG_USERSPACE=y`, because a user-mode
 * thread calling `z_impl_k_sem_take` faults instead of trapping. It also cannot reach the accessors
 * at all.
 *
 * So every name this package binds goes through a wrapper here, uniformly -- including the handful
 * that *do* have a symbol of their own, because which ones those are is a property of Zephyr's
 * release rather than of its documentation, and a per-function judgement about which spelling links
 * is the kind of thing that goes quietly wrong. `-ffunction-sections` and Zephyr's `--gc-sections`
 * mean a wrapper nobody calls costs nothing in the image.
 *
 * **`k_timeout_t` crosses as a plain `int64_t` of ticks rather than as the struct.** sysl passes a
 * struct by value correctly -- that was measured before this package was written -- but
 * `k_timeout_t` is a *different struct* depending on `CONFIG_TIMEOUT_64BIT`: eight bytes of
 * `int64_t` with it, four of `uint32_t` without. Building it here, from a tick count, is what makes
 * one binding fit both. `K_TICKS_FOREVER` is `(k_ticks_t)-1` under either, so `-1` still means
 * forever after the cast.
 *
 * `autoconf.h` is included explicitly. Zephyr force-includes it into its own translation units with
 * `-imacros`, and without it every `CONFIG_*` the kernel headers test reads as absent. This used to
 * say sysl had no equivalent of that flag; since 0.0.90 `SYSL_EXTRA_CFLAGS` reaches every clang the
 * build drives, so it could carry one. Naming the header here is still right -- that variable
 * belongs to the whole build, and a consumer who forgot it would measure a kernel nobody built
 * rather than fail.
 */

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A tick count as the kernel's own timeout. `-1` is forever and `0` is no wait, which is what
 * `K_TICKS_FOREVER` and `K_NO_WAIT` are under both widths. */
static inline k_timeout_t to_timeout(int64_t ticks)
{
	k_timeout_t t = { (k_ticks_t)ticks };

	return t;
}

/* ---- time ------------------------------------------------------------------------------------ */

/* Ceiling rather than truncation, because these convert a *timeout*: a caller asking to wait a
 * millisecond on a 100 Hz tick means at least that long, and truncation would answer zero. It is
 * the rounding `K_MSEC` itself uses. */
int64_t syslZephyrMsToTicks(int64_t ms) { return (int64_t)k_ms_to_ticks_ceil64(ms < 0 ? 0 : ms); }
int64_t syslZephyrUsToTicks(int64_t us) { return (int64_t)k_us_to_ticks_ceil64(us < 0 ? 0 : us); }

int64_t syslZephyrTicksToMs(int64_t ticks) { return (int64_t)k_ticks_to_ms_floor64(ticks); }

int32_t syslZephyrSleep(int64_t ticks) { return k_sleep(to_timeout(ticks)); }
void syslZephyrBusyWait(uint32_t us) { k_busy_wait(us); }

int64_t syslZephyrUptimeTicks(void) { return k_uptime_ticks(); }
int64_t syslZephyrUptimeMs(void) { return k_uptime_get(); }
uint32_t syslZephyrCycleGet32(void) { return k_cycle_get_32(); }

/* ---- the scheduler --------------------------------------------------------------------------- */

void syslZephyrYield(void) { k_yield(); }
bool syslZephyrCanYield(void) { return k_can_yield(); }

bool syslZephyrIsInIsr(void) { return k_is_in_isr(); }
bool syslZephyrIsPreemptThread(void) { return k_is_preempt_thread() != 0; }
bool syslZephyrIsPreKernel(void) { return k_is_pre_kernel(); }

void syslZephyrSchedLock(void) { k_sched_lock(); }
void syslZephyrSchedUnlock(void) { k_sched_unlock(); }

void syslZephyrCpuIdle(void) { k_cpu_idle(); }

/* ---- threads ---------------------------------------------------------------------------------- */

void *syslZephyrThreadCreate(void *thread, void *stack, size_t stack_size,
			     void (*entry)(void *, void *, void *),
			     void *p1, void *p2, void *p3,
			     int prio, uint32_t options, int64_t delay_ticks)
{
	return (void *)k_thread_create((struct k_thread *)thread,
				       (k_thread_stack_t *)stack, stack_size,
				       (k_thread_entry_t)entry, p1, p2, p3,
				       prio, options, to_timeout(delay_ticks));
}

void syslZephyrThreadStart(void *thread) { k_thread_start((k_tid_t)thread); }
void syslZephyrThreadAbort(void *thread) { k_thread_abort((k_tid_t)thread); }
void syslZephyrThreadSuspend(void *thread) { k_thread_suspend((k_tid_t)thread); }
void syslZephyrThreadResume(void *thread) { k_thread_resume((k_tid_t)thread); }
void syslZephyrThreadWakeup(void *thread) { k_wakeup((k_tid_t)thread); }

int syslZephyrThreadJoin(void *thread, int64_t ticks)
{
	return k_thread_join((struct k_thread *)thread, to_timeout(ticks));
}

int syslZephyrThreadPriorityGet(void *thread) { return k_thread_priority_get((k_tid_t)thread); }
void syslZephyrThreadPrioritySet(void *thread, int prio) { k_thread_priority_set((k_tid_t)thread, prio); }

void *syslZephyrCurrentGet(void) { return (void *)k_current_get(); }

/* ---- semaphores -------------------------------------------------------------------------------- */

int syslZephyrSemInit(void *sem, unsigned int initial, unsigned int limit)
{
	return k_sem_init((struct k_sem *)sem, initial, limit);
}

int syslZephyrSemTake(void *sem, int64_t ticks)
{
	return k_sem_take((struct k_sem *)sem, to_timeout(ticks));
}

void syslZephyrSemGive(void *sem) { k_sem_give((struct k_sem *)sem); }
void syslZephyrSemReset(void *sem) { k_sem_reset((struct k_sem *)sem); }
unsigned int syslZephyrSemCount(void *sem) { return k_sem_count_get((struct k_sem *)sem); }

/* ---- mutexes and condition variables ------------------------------------------------------------ */

int syslZephyrMutexInit(void *mutex) { return k_mutex_init((struct k_mutex *)mutex); }
int syslZephyrMutexUnlock(void *mutex) { return k_mutex_unlock((struct k_mutex *)mutex); }

int syslZephyrMutexLock(void *mutex, int64_t ticks)
{
	return k_mutex_lock((struct k_mutex *)mutex, to_timeout(ticks));
}

int syslZephyrCondvarInit(void *cv) { return k_condvar_init((struct k_condvar *)cv); }
int syslZephyrCondvarSignal(void *cv) { return k_condvar_signal((struct k_condvar *)cv); }
int syslZephyrCondvarBroadcast(void *cv) { return k_condvar_broadcast((struct k_condvar *)cv); }

int syslZephyrCondvarWait(void *cv, void *mutex, int64_t ticks)
{
	return k_condvar_wait((struct k_condvar *)cv, (struct k_mutex *)mutex, to_timeout(ticks));
}

/* ---- events ------------------------------------------------------------------------------------- */

void syslZephyrEventInit(void *event) { k_event_init((struct k_event *)event); }
uint32_t syslZephyrEventPost(void *event, uint32_t bits) { return k_event_post((struct k_event *)event, bits); }
uint32_t syslZephyrEventSet(void *event, uint32_t bits) { return k_event_set((struct k_event *)event, bits); }
uint32_t syslZephyrEventClear(void *event, uint32_t bits) { return k_event_clear((struct k_event *)event, bits); }
uint32_t syslZephyrEventTest(void *event, uint32_t mask) { return k_event_test((struct k_event *)event, mask); }

uint32_t syslZephyrEventWait(void *event, uint32_t bits, bool reset, int64_t ticks)
{
	return k_event_wait((struct k_event *)event, bits, reset, to_timeout(ticks));
}

uint32_t syslZephyrEventWaitAll(void *event, uint32_t bits, bool reset, int64_t ticks)
{
	return k_event_wait_all((struct k_event *)event, bits, reset, to_timeout(ticks));
}

/* ---- message queues ------------------------------------------------------------------------------ */

void syslZephyrMsgqInit(void *q, char *buffer, size_t msg_size, uint32_t max_msgs)
{
	k_msgq_init((struct k_msgq *)q, buffer, msg_size, max_msgs);
}

int syslZephyrMsgqPut(void *q, const void *data, int64_t ticks)
{
	return k_msgq_put((struct k_msgq *)q, data, to_timeout(ticks));
}

int syslZephyrMsgqGet(void *q, void *data, int64_t ticks)
{
	return k_msgq_get((struct k_msgq *)q, data, to_timeout(ticks));
}

int syslZephyrMsgqPeek(void *q, void *data) { return k_msgq_peek((struct k_msgq *)q, data); }
void syslZephyrMsgqPurge(void *q) { k_msgq_purge((struct k_msgq *)q); }
uint32_t syslZephyrMsgqNumUsed(void *q) { return k_msgq_num_used_get((struct k_msgq *)q); }
uint32_t syslZephyrMsgqNumFree(void *q) { return k_msgq_num_free_get((struct k_msgq *)q); }

/* ---- timers ----------------------------------------------------------------------------------------- */

void syslZephyrTimerInit(void *timer, void (*expiry)(void *), void (*stop)(void *))
{
	k_timer_init((struct k_timer *)timer,
		     (k_timer_expiry_t)expiry, (k_timer_stop_t)stop);
}

void syslZephyrTimerStart(void *timer, int64_t duration, int64_t period)
{
	k_timer_start((struct k_timer *)timer, to_timeout(duration), to_timeout(period));
}

void syslZephyrTimerStop(void *timer) { k_timer_stop((struct k_timer *)timer); }
uint32_t syslZephyrTimerStatusGet(void *timer) { return k_timer_status_get((struct k_timer *)timer); }
uint32_t syslZephyrTimerStatusSync(void *timer) { return k_timer_status_sync((struct k_timer *)timer); }
int64_t syslZephyrTimerRemainingTicks(void *timer) { return k_timer_remaining_ticks((struct k_timer *)timer); }

/* ---- work queues -------------------------------------------------------------------------------------- */

void syslZephyrWorkInit(void *work, void (*handler)(void *))
{
	k_work_init((struct k_work *)work, (k_work_handler_t)handler);
}

int syslZephyrWorkSubmit(void *work) { return k_work_submit((struct k_work *)work); }
int syslZephyrWorkCancel(void *work) { return k_work_cancel((struct k_work *)work); }
bool syslZephyrWorkIsPending(void *work) { return k_work_is_pending((struct k_work *)work); }

void syslZephyrWorkInitDelayable(void *dwork, void (*handler)(void *))
{
	k_work_init_delayable((struct k_work_delayable *)dwork, (k_work_handler_t)handler);
}

int syslZephyrWorkSchedule(void *dwork, int64_t ticks)
{
	return k_work_schedule((struct k_work_delayable *)dwork, to_timeout(ticks));
}

int syslZephyrWorkReschedule(void *dwork, int64_t ticks)
{
	return k_work_reschedule((struct k_work_delayable *)dwork, to_timeout(ticks));
}

int syslZephyrWorkCancelDelayable(void *dwork)
{
	return k_work_cancel_delayable((struct k_work_delayable *)dwork);
}

bool syslZephyrWorkDelayableIsPending(void *dwork)
{
	return k_work_delayable_is_pending((struct k_work_delayable *)dwork);
}

/* The delayable work item a handler was given: the handler is called with the inner `k_work`, and
 * `k_work_delayable_from_work` is the documented way back out. It is a `CONTAINER_OF`, so there is
 * nothing to bind and this is the only way to reach it. */
void *syslZephyrDelayableFromWork(void *work)
{
	return (void *)k_work_delayable_from_work((struct k_work *)work);
}
