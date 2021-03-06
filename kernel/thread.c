/*
 * Copyright (c) 2010-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Kernel thread support
 *
 * This module provides general purpose thread support.
 */

#include <kernel.h>
#include <spinlock.h>
#include <sys/math_extras.h>
#include <sys_clock.h>
#include <drivers/timer/system_timer.h>
#include <ksched.h>
#include <wait_q.h>
#include <sys/atomic.h>
#include <syscall_handler.h>
#include <kernel_internal.h>
#include <kswap.h>
#include <init.h>
#include <tracing/tracing.h>
#include <string.h>
#include <stdbool.h>
#include <irq_offload.h>
#include <sys/check.h>

#ifdef CONFIG_THREAD_MONITOR
/* This lock protects the linked list of active threads; i.e. the
 * initial _kernel.threads pointer and the linked list made up of
 * thread->next_thread (until NULL)
 */
static struct k_spinlock z_thread_monitor_lock;
#endif /* CONFIG_THREAD_MONITOR */

#define _FOREACH_STATIC_THREAD(thread_data)              \
	Z_STRUCT_SECTION_FOREACH(_static_thread_data, thread_data)

void k_thread_foreach(k_thread_user_cb_t user_cb, void *user_data)
{
#if defined(CONFIG_THREAD_MONITOR)
	struct k_thread *thread;
	k_spinlock_key_t key;

	__ASSERT(user_cb != NULL, "user_cb can not be NULL");

	/*
	 * Lock is needed to make sure that the _kernel.threads is not being
	 * modified by the user_cb either directly or indirectly.
	 * The indirect ways are through calling k_thread_create and
	 * k_thread_abort from user_cb.
	 */
	key = k_spin_lock(&z_thread_monitor_lock);
	for (thread = _kernel.threads; thread; thread = thread->next_thread) {
		user_cb(thread, user_data);
	}
	k_spin_unlock(&z_thread_monitor_lock, key);
#endif
}

void k_thread_foreach_unlocked(k_thread_user_cb_t user_cb, void *user_data)
{
#if defined(CONFIG_THREAD_MONITOR)
	struct k_thread *thread;
	k_spinlock_key_t key;

	__ASSERT(user_cb != NULL, "user_cb can not be NULL");

	key = k_spin_lock(&z_thread_monitor_lock);
	for (thread = _kernel.threads; thread; thread = thread->next_thread) {
		k_spin_unlock(&z_thread_monitor_lock, key);
		user_cb(thread, user_data);
		key = k_spin_lock(&z_thread_monitor_lock);
	}
	k_spin_unlock(&z_thread_monitor_lock, key);
#endif
}

bool k_is_in_isr(void)
{
	return arch_is_in_isr();
}

/*
 * This function tags the current thread as essential to system operation.
 * Exceptions raised by this thread will be treated as a fatal system error.
 */
void z_thread_essential_set(void)
{
	_current->base.user_options |= K_ESSENTIAL;
}

/*
 * This function tags the current thread as not essential to system operation.
 * Exceptions raised by this thread may be recoverable.
 * (This is the default tag for a thread.)
 */
void z_thread_essential_clear(void)
{
	_current->base.user_options &= ~K_ESSENTIAL;
}

/*
 * This routine indicates if the current thread is an essential system thread.
 *
 * Returns true if current thread is essential, false if it is not.
 */
bool z_is_thread_essential(void)
{
	return (_current->base.user_options & K_ESSENTIAL) == K_ESSENTIAL;
}

#ifdef CONFIG_SYS_CLOCK_EXISTS
void z_impl_k_busy_wait(u32_t usec_to_wait)
{
#if !defined(CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT)
	/* use 64-bit math to prevent overflow when multiplying */
	u32_t cycles_to_wait = (u32_t)(
		(u64_t)usec_to_wait *
		(u64_t)sys_clock_hw_cycles_per_sec() /
		(u64_t)USEC_PER_SEC
	);
	u32_t start_cycles = k_cycle_get_32();

	for (;;) {
		u32_t current_cycles = k_cycle_get_32();

		/* this handles the rollover on an unsigned 32-bit value */
		if ((current_cycles - start_cycles) >= cycles_to_wait) {
			break;
		}
	}
#else
	arch_busy_wait(usec_to_wait);
#endif /* CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT */
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_busy_wait(u32_t usec_to_wait)
{
	z_impl_k_busy_wait(usec_to_wait);
}
#include <syscalls/k_busy_wait_mrsh.c>
#endif /* CONFIG_USERSPACE */
#endif /* CONFIG_SYS_CLOCK_EXISTS */

#ifdef CONFIG_THREAD_CUSTOM_DATA
void z_impl_k_thread_custom_data_set(void *value)
{
	_current->custom_data = value;
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_custom_data_set(void *data)
{
	z_impl_k_thread_custom_data_set(data);
}
#include <syscalls/k_thread_custom_data_set_mrsh.c>
#endif

void *z_impl_k_thread_custom_data_get(void)
{
	return _current->custom_data;
}

#ifdef CONFIG_USERSPACE
static inline void *z_vrfy_k_thread_custom_data_get(void)
{
	return z_impl_k_thread_custom_data_get();
}
#include <syscalls/k_thread_custom_data_get_mrsh.c>

#endif /* CONFIG_USERSPACE */
#endif /* CONFIG_THREAD_CUSTOM_DATA */

#if defined(CONFIG_THREAD_MONITOR)
/*
 * Remove a thread from the kernel's list of active threads.
 */
void z_thread_monitor_exit(struct k_thread *thread)
{
	k_spinlock_key_t key = k_spin_lock(&z_thread_monitor_lock);

	if (thread == _kernel.threads) {
		_kernel.threads = _kernel.threads->next_thread;
	} else {
		struct k_thread *prev_thread;

		prev_thread = _kernel.threads;
		while ((prev_thread != NULL) &&
			(thread != prev_thread->next_thread)) {
			prev_thread = prev_thread->next_thread;
		}
		if (prev_thread != NULL) {
			prev_thread->next_thread = thread->next_thread;
		}
	}

	k_spin_unlock(&z_thread_monitor_lock, key);
}
#endif

int z_impl_k_thread_name_set(struct k_thread *thread, const char *value)
{
#ifdef CONFIG_THREAD_NAME
	if (thread == NULL) {
		thread = _current;
	}

	strncpy(thread->name, value, CONFIG_THREAD_MAX_NAME_LEN);
	thread->name[CONFIG_THREAD_MAX_NAME_LEN - 1] = '\0';
	sys_trace_thread_name_set(thread);
	return 0;
#else
	ARG_UNUSED(thread);
	ARG_UNUSED(value);
	return -ENOSYS;
#endif /* CONFIG_THREAD_NAME */
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_k_thread_name_set(struct k_thread *t, const char *str)
{
#ifdef CONFIG_THREAD_NAME
	size_t len;
	int err;

	if (t != NULL) {
		if (Z_SYSCALL_OBJ(t, K_OBJ_THREAD) != 0) {
			return -EINVAL;
		}
	}

	len = z_user_string_nlen(str, CONFIG_THREAD_MAX_NAME_LEN, &err);
	if (err != 0) {
		return -EFAULT;
	}
	if (Z_SYSCALL_MEMORY_READ(str, len) != 0) {
		return -EFAULT;
	}

	return z_impl_k_thread_name_set(t, str);
#else
	return -ENOSYS;
#endif /* CONFIG_THREAD_NAME */
}
#include <syscalls/k_thread_name_set_mrsh.c>
#endif /* CONFIG_USERSPACE */

const char *k_thread_name_get(struct k_thread *thread)
{
#ifdef CONFIG_THREAD_NAME
	return (const char *)thread->name;
#else
	ARG_UNUSED(thread);
	return NULL;
#endif /* CONFIG_THREAD_NAME */
}

int z_impl_k_thread_name_copy(k_tid_t thread_id, char *buf, size_t size)
{
#ifdef CONFIG_THREAD_NAME
	strncpy(buf, thread_id->name, size);
	return 0;
#else
	ARG_UNUSED(thread_id);
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	return -ENOSYS;
#endif /* CONFIG_THREAD_NAME */
}

const char *k_thread_state_str(k_tid_t thread_id)
{
	switch (thread_id->base.thread_state) {
	case 0:
		return "";
		break;
	case _THREAD_DUMMY:
		return "dummy";
		break;
	case _THREAD_PENDING:
		return "pending";
		break;
	case _THREAD_PRESTART:
		return "prestart";
		break;
	case _THREAD_DEAD:
		return "dead";
		break;
	case _THREAD_SUSPENDED:
		return "suspended";
		break;
	case _THREAD_ABORTING:
		return "aborting";
		break;
	case _THREAD_QUEUED:
		return "queued";
		break;
	}
	return "unknown";
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_k_thread_name_copy(k_tid_t thread,
					    char *buf, size_t size)
{
#ifdef CONFIG_THREAD_NAME
	size_t len;
	struct z_object *ko = z_object_find(thread);

	/* Special case: we allow reading the names of initialized threads
	 * even if we don't have permission on them
	 */
	if (thread == NULL || ko->type != K_OBJ_THREAD ||
	    (ko->flags & K_OBJ_FLAG_INITIALIZED) == 0) {
		return -EINVAL;
	}
	if (Z_SYSCALL_MEMORY_WRITE(buf, size) != 0) {
		return -EFAULT;
	}
	len = strlen(thread->name);
	if (len + 1 > size) {
		return -ENOSPC;
	}

	return z_user_to_copy((void *)buf, thread->name, len + 1);
#else
	ARG_UNUSED(thread);
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	return -ENOSYS;
#endif /* CONFIG_THREAD_NAME */
}
#include <syscalls/k_thread_name_copy_mrsh.c>
#endif /* CONFIG_USERSPACE */


#ifdef CONFIG_STACK_SENTINEL
/* Check that the stack sentinel is still present
 *
 * The stack sentinel feature writes a magic value to the lowest 4 bytes of
 * the thread's stack when the thread is initialized. This value gets checked
 * in a few places:
 *
 * 1) In k_yield() if the current thread is not swapped out
 * 2) After servicing a non-nested interrupt
 * 3) In z_swap(), check the sentinel in the outgoing thread
 *
 * Item 2 requires support in arch/ code.
 *
 * If the check fails, the thread will be terminated appropriately through
 * the system fatal error handler.
 */
void z_check_stack_sentinel(void)
{
	u32_t *stack;

	if ((_current->base.thread_state & _THREAD_DUMMY) != 0) {
		return;
	}

	stack = (u32_t *)_current->stack_info.start;
	if (*stack != STACK_SENTINEL) {
		/* Restore it so further checks don't trigger this same error */
		*stack = STACK_SENTINEL;
		z_except_reason(K_ERR_STACK_CHK_FAIL);
	}
}
#endif

#ifdef CONFIG_MULTITHREADING
void z_impl_k_thread_start(struct k_thread *thread)
{
	z_sched_start(thread);
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_start(struct k_thread *thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	return z_impl_k_thread_start(thread);
}
#include <syscalls/k_thread_start_mrsh.c>
#endif
#endif

#ifdef CONFIG_MULTITHREADING
static void schedule_new_thread(struct k_thread *thread, k_timeout_t delay)
{
#ifdef CONFIG_SYS_CLOCK_EXISTS
	if (K_TIMEOUT_EQ(delay, K_NO_WAIT)) {
		k_thread_start(thread);
	} else {
#ifdef CONFIG_LEGACY_TIMEOUT_API
		delay = _TICK_ALIGN + k_ms_to_ticks_ceil32(delay);
#endif

		z_add_thread_timeout(thread, delay);
	}
#else
	ARG_UNUSED(delay);
	k_thread_start(thread);
#endif
}
#endif

#if !CONFIG_STACK_POINTER_RANDOM
static inline size_t adjust_stack_size(size_t stack_size)
{
	return stack_size;
}
#else
int z_stack_adjust_initialized;

static inline size_t adjust_stack_size(size_t stack_size)
{
	size_t random_val;

	if (!z_stack_adjust_initialized) {
		z_early_boot_rand_get((u8_t *)&random_val, sizeof(random_val));
	} else {
		sys_rand_get((u8_t *)&random_val, sizeof(random_val));
	}

	/* Don't need to worry about alignment of the size here,
	 * arch_new_thread() is required to do it.
	 *
	 * FIXME: Not the best way to get a random number in a range.
	 * See #6493
	 */
	const size_t fuzz = random_val % CONFIG_STACK_POINTER_RANDOM;

	if (unlikely(fuzz * 2 > stack_size)) {
		return stack_size;
	}

	return stack_size - fuzz;
}
#if defined(CONFIG_STACK_GROWS_UP)
	/* This is so rare not bothering for now */
#error "Stack pointer randomization not implemented for upward growing stacks"
#endif /* CONFIG_STACK_GROWS_UP */

#endif /* CONFIG_STACK_POINTER_RANDOM */

/*
 * Note:
 * The caller must guarantee that the stack_size passed here corresponds
 * to the amount of stack memory available for the thread.
 */
void z_setup_new_thread(struct k_thread *new_thread,
		       k_thread_stack_t *stack, size_t stack_size,
		       k_thread_entry_t entry,
		       void *p1, void *p2, void *p3,
		       int prio, u32_t options, const char *name)
{
	Z_ASSERT_VALID_PRIO(prio, entry);

#ifdef CONFIG_USERSPACE
	z_object_init(new_thread);
	z_object_init(stack);
	new_thread->stack_obj = stack;
	new_thread->mem_domain_info.mem_domain = NULL;

	/* Any given thread has access to itself */
	k_object_access_grant(new_thread, new_thread);
#endif
	stack_size = adjust_stack_size(stack_size);

	z_waitq_init(&new_thread->base.join_waiters);

#ifdef CONFIG_THREAD_USERSPACE_LOCAL_DATA
#ifndef CONFIG_THREAD_USERSPACE_LOCAL_DATA_ARCH_DEFER_SETUP
	/* reserve space on top of stack for local data */
	stack_size = Z_STACK_PTR_ALIGN(stack_size
			- sizeof(*new_thread->userspace_local_data));
#endif
#endif
	/* Initialize various struct k_thread members */
	z_init_thread_base(&new_thread->base, prio, _THREAD_PRESTART, options);

	arch_new_thread(new_thread, stack, stack_size, entry, p1, p2, p3,
			  prio, options);
	/* static threads overwrite it afterwards with real value */
	new_thread->init_data = NULL;
	new_thread->fn_abort = NULL;

#ifdef CONFIG_USE_SWITCH
	/* switch_handle must be non-null except when inside z_swap()
	 * for synchronization reasons.  Historically some notional
	 * USE_SWITCH architectures have actually ignored the field
	 */
	__ASSERT(new_thread->switch_handle != NULL,
		 "arch layer failed to initialize switch_handle");
#endif
#ifdef CONFIG_STACK_SENTINEL
	/* Put the stack sentinel at the lowest 4 bytes of the stack area.
	 * We periodically check that it's still present and kill the thread
	 * if it isn't.
	 */
	*((u32_t *)new_thread->stack_info.start) = STACK_SENTINEL;
#endif /* CONFIG_STACK_SENTINEL */
#ifdef CONFIG_THREAD_USERSPACE_LOCAL_DATA
#ifndef CONFIG_THREAD_USERSPACE_LOCAL_DATA_ARCH_DEFER_SETUP
	/* don't set again if the arch's own code in arch_new_thread() has
	 * already set the pointer.
	 */
	new_thread->userspace_local_data =
		(struct _thread_userspace_local_data *)
		(Z_THREAD_STACK_BUFFER(stack) + stack_size);
#endif
#endif
#ifdef CONFIG_THREAD_CUSTOM_DATA
	/* Initialize custom data field (value is opaque to kernel) */
	new_thread->custom_data = NULL;
#endif
#ifdef CONFIG_THREAD_MONITOR
	new_thread->entry.pEntry = entry;
	new_thread->entry.parameter1 = p1;
	new_thread->entry.parameter2 = p2;
	new_thread->entry.parameter3 = p3;

	k_spinlock_key_t key = k_spin_lock(&z_thread_monitor_lock);

	new_thread->next_thread = _kernel.threads;
	_kernel.threads = new_thread;
	k_spin_unlock(&z_thread_monitor_lock, key);
#endif
#ifdef CONFIG_THREAD_NAME
	if (name != NULL) {
		strncpy(new_thread->name, name,
			CONFIG_THREAD_MAX_NAME_LEN - 1);
		/* Ensure NULL termination, truncate if longer */
		new_thread->name[CONFIG_THREAD_MAX_NAME_LEN - 1] = '\0';
	} else {
		new_thread->name[0] = '\0';
	}
#endif
#ifdef CONFIG_SCHED_CPU_MASK
	new_thread->base.cpu_mask = -1;
#endif
#ifdef CONFIG_ARCH_HAS_CUSTOM_SWAP_TO_MAIN
	/* _current may be null if the dummy thread is not used */
	if (!_current) {
		new_thread->resource_pool = NULL;
		return;
	}
#endif
#ifdef CONFIG_USERSPACE
	/* New threads inherit any memory domain membership by the parent */
	if (_current->mem_domain_info.mem_domain != NULL) {
		k_mem_domain_add_thread(_current->mem_domain_info.mem_domain,
					new_thread);
	}

	if ((options & K_INHERIT_PERMS) != 0U) {
		z_thread_perms_inherit(_current, new_thread);
	}
#endif
#ifdef CONFIG_SCHED_DEADLINE
	new_thread->base.prio_deadline = 0;
#endif
	new_thread->resource_pool = _current->resource_pool;
	sys_trace_thread_create(new_thread);
}

#ifdef CONFIG_MULTITHREADING
k_tid_t z_impl_k_thread_create(struct k_thread *new_thread,
			      k_thread_stack_t *stack,
			      size_t stack_size, k_thread_entry_t entry,
			      void *p1, void *p2, void *p3,
			      int prio, u32_t options, k_timeout_t delay)
{
	__ASSERT(!arch_is_in_isr(), "Threads may not be created in ISRs");

	/* Special case, only for unit tests */
#if defined(CONFIG_TEST) && defined(CONFIG_ARCH_HAS_USERSPACE) && !defined(CONFIG_USERSPACE)
	__ASSERT((options & K_USER) == 0,
		 "Platform is capable of user mode, and test thread created with K_USER option,"
		 " but neither CONFIG_TEST_USERSPACE nor CONFIG_USERSPACE is set\n");
#endif

	z_setup_new_thread(new_thread, stack, stack_size, entry, p1, p2, p3,
			  prio, options, NULL);

	if (!K_TIMEOUT_EQ(delay, K_FOREVER)) {
		schedule_new_thread(new_thread, delay);
	}

	return new_thread;
}


#ifdef CONFIG_USERSPACE
k_tid_t z_vrfy_k_thread_create(struct k_thread *new_thread,
			       k_thread_stack_t *stack,
			       size_t stack_size, k_thread_entry_t entry,
			       void *p1, void *p2, void *p3,
			       int prio, u32_t options, k_timeout_t delay)
{
	size_t total_size, stack_obj_size;
	struct z_object *stack_object;

	/* The thread and stack objects *must* be in an uninitialized state */
	Z_OOPS(Z_SYSCALL_OBJ_NEVER_INIT(new_thread, K_OBJ_THREAD));
	stack_object = z_object_find(stack);
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(z_obj_validation_check(stack_object, stack,
						K_OBJ_THREAD_STACK_ELEMENT,
						_OBJ_INIT_FALSE) == 0,
				    "bad stack object"));

	/* Verify that the stack size passed in is OK by computing the total
	 * size and comparing it with the size value in the object metadata
	 */
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(!size_add_overflow(K_THREAD_STACK_RESERVED,
						       stack_size, &total_size),
				    "stack size overflow (%zu+%zu)",
				    stack_size,
				    K_THREAD_STACK_RESERVED));

	/* Testing less-than-or-equal since additional room may have been
	 * allocated for alignment constraints
	 */
#ifdef CONFIG_GEN_PRIV_STACKS
	stack_obj_size = stack_object->data.stack_data->size;
#else
	stack_obj_size = stack_object->data.stack_size;
#endif
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(total_size <= stack_obj_size,
				    "stack size %zu is too big, max is %zu",
				    total_size, stack_obj_size));

	/* User threads may only create other user threads and they can't
	 * be marked as essential
	 */
	Z_OOPS(Z_SYSCALL_VERIFY(options & K_USER));
	Z_OOPS(Z_SYSCALL_VERIFY(!(options & K_ESSENTIAL)));

	/* Check validity of prio argument; must be the same or worse priority
	 * than the caller
	 */
	Z_OOPS(Z_SYSCALL_VERIFY(_is_valid_prio(prio, NULL)));
	Z_OOPS(Z_SYSCALL_VERIFY(z_is_prio_lower_or_equal(prio,
							_current->base.prio)));

	z_setup_new_thread(new_thread, stack, stack_size,
			   entry, p1, p2, p3, prio, options, NULL);

	if (!K_TIMEOUT_EQ(delay, K_FOREVER)) {
		schedule_new_thread(new_thread, delay);
	}

	return new_thread;
}
#include <syscalls/k_thread_create_mrsh.c>
#endif /* CONFIG_USERSPACE */
#endif /* CONFIG_MULTITHREADING */

#ifdef CONFIG_MULTITHREADING
#ifdef CONFIG_USERSPACE

static void grant_static_access(void)
{
	Z_STRUCT_SECTION_FOREACH(z_object_assignment, pos) {
		for (int i = 0; pos->objects[i] != NULL; i++) {
			k_object_access_grant(pos->objects[i],
					      pos->thread);
		}
	}
}
#endif /* CONFIG_USERSPACE */

void z_init_static_threads(void)
{
	_FOREACH_STATIC_THREAD(thread_data) {
		z_setup_new_thread(
			thread_data->init_thread,
			thread_data->init_stack,
			thread_data->init_stack_size,
			thread_data->init_entry,
			thread_data->init_p1,
			thread_data->init_p2,
			thread_data->init_p3,
			thread_data->init_prio,
			thread_data->init_options,
			thread_data->init_name);

		thread_data->init_thread->init_data = thread_data;
	}

#ifdef CONFIG_USERSPACE
	grant_static_access();
#endif

	/*
	 * Non-legacy static threads may be started immediately or
	 * after a previously specified delay. Even though the
	 * scheduler is locked, ticks can still be delivered and
	 * processed. Take a sched lock to prevent them from running
	 * until they are all started.
	 *
	 * Note that static threads defined using the legacy API have a
	 * delay of K_FOREVER.
	 */
	k_sched_lock();
	_FOREACH_STATIC_THREAD(thread_data) {
		if (thread_data->init_delay != K_TICKS_FOREVER) {
			schedule_new_thread(thread_data->init_thread,
					    K_MSEC(thread_data->init_delay));
		}
	}
	k_sched_unlock();
}
#endif

void z_init_thread_base(struct _thread_base *thread_base, int priority,
		       u32_t initial_state, unsigned int options)
{
	/* k_q_node is initialized upon first insertion in a list */

	thread_base->user_options = (u8_t)options;
	thread_base->thread_state = (u8_t)initial_state;

	thread_base->prio = priority;

	thread_base->sched_locked = 0U;

#ifdef CONFIG_SMP
	thread_base->is_idle = 0;
#endif

	/* swap_data does not need to be initialized */

	z_init_thread_timeout(thread_base);
}

FUNC_NORETURN void k_thread_user_mode_enter(k_thread_entry_t entry,
					    void *p1, void *p2, void *p3)
{
	_current->base.user_options |= K_USER;
	z_thread_essential_clear();
#ifdef CONFIG_THREAD_MONITOR
	_current->entry.pEntry = entry;
	_current->entry.parameter1 = p1;
	_current->entry.parameter2 = p2;
	_current->entry.parameter3 = p3;
#endif
#ifdef CONFIG_USERSPACE
	arch_user_mode_enter(entry, p1, p2, p3);
#else
	/* XXX In this case we do not reset the stack */
	z_thread_entry(entry, p1, p2, p3);
#endif
}

/* These spinlock assertion predicates are defined here because having
 * them in spinlock.h is a giant header ordering headache.
 */
#ifdef CONFIG_SPIN_VALIDATE
bool z_spin_lock_valid(struct k_spinlock *l)
{
	uintptr_t thread_cpu = l->thread_cpu;

	if (thread_cpu) {
		if ((thread_cpu & 3) == _current_cpu->id) {
			return false;
		}
	}
	return true;
}

bool z_spin_unlock_valid(struct k_spinlock *l)
{
	if (l->thread_cpu != (_current_cpu->id | (uintptr_t)_current)) {
		return false;
	}
	l->thread_cpu = 0;
	return true;
}

void z_spin_lock_set_owner(struct k_spinlock *l)
{
	l->thread_cpu = _current_cpu->id | (uintptr_t)_current;
}
#endif /* CONFIG_SPIN_VALIDATE */

int z_impl_k_float_disable(struct k_thread *thread)
{
#if defined(CONFIG_FPU) && defined(CONFIG_FP_SHARING)
	return arch_float_disable(thread);
#else
	return -ENOSYS;
#endif /* CONFIG_FPU && CONFIG_FP_SHARING */
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_k_float_disable(struct k_thread *thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	return z_impl_k_float_disable(thread);
}
#include <syscalls/k_float_disable_mrsh.c>
#endif /* CONFIG_USERSPACE */

#ifdef CONFIG_IRQ_OFFLOAD
static K_SEM_DEFINE(offload_sem, 1, 1);

void irq_offload(irq_offload_routine_t routine, void *parameter)
{
	k_sem_take(&offload_sem, K_FOREVER);
	arch_irq_offload(routine, parameter);
	k_sem_give(&offload_sem);
}
#endif

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
#ifdef CONFIG_STACK_GROWS_UP
#error "Unsupported configuration for stack analysis"
#endif

int z_impl_k_thread_stack_space_get(const struct k_thread *thread,
				    size_t *unused_ptr)
{
	const u8_t *start = (u8_t *)thread->stack_info.start;
	size_t size = thread->stack_info.size;
	size_t unused = 0;
	const u8_t *checked_stack = start;
	/* Take the address of any local variable as a shallow bound for the
	 * stack pointer.  Addresses above it are guaranteed to be
	 * accessible.
	 */
	const u8_t *stack_pointer = (const u8_t *)&start;

	/* If we are currently running on the stack being analyzed, some
	 * memory management hardware will generate an exception if we
	 * read unused stack memory.
	 *
	 * This never happens when invoked from user mode, as user mode
	 * will always run this function on the privilege elevation stack.
	 */
	if ((stack_pointer > start) && (stack_pointer <= (start + size)) &&
	    IS_ENABLED(CONFIG_NO_UNUSED_STACK_INSPECTION)) {
		/* TODO: We could add an arch_ API call to temporarily
		 * disable the stack checking in the CPU, but this would
		 * need to be properly managed wrt context switches/interrupts
		 */
		return -ENOTSUP;
	}

	if (IS_ENABLED(CONFIG_STACK_SENTINEL)) {
		/* First 4 bytes of the stack buffer reserved for the
		 * sentinel value, it won't be 0xAAAAAAAA for thread
		 * stacks.
		 *
		 * FIXME: thread->stack_info.start ought to reflect
		 * this!
		 */
		checked_stack += 4;
		size -= 4;
	}

	for (size_t i = 0; i < size; i++) {
		if ((checked_stack[i]) == 0xaaU) {
			unused++;
		} else {
			break;
		}
	}

	*unused_ptr = unused;

	return 0;
}

#ifdef CONFIG_USERSPACE
int z_vrfy_k_thread_stack_space_get(const struct k_thread *thread,
				    size_t *unused_ptr)
{
	size_t unused;
	int ret;

	ret = Z_SYSCALL_OBJ(thread, K_OBJ_THREAD);
	CHECKIF(ret != 0) {
		return ret;
	}

	ret = z_impl_k_thread_stack_space_get(thread, &unused);
	CHECKIF(ret != 0) {
		return ret;
	}

	ret = z_user_to_copy(unused_ptr, &unused, sizeof(size_t));
	CHECKIF(ret != 0) {
		return ret;
	}

	return 0;
}
#include <syscalls/k_thread_stack_space_get_mrsh.c>
#endif /* CONFIG_USERSPACE */
#endif /* CONFIG_INIT_STACKS && CONFIG_THREAD_STACK_INFO */

#ifdef CONFIG_USERSPACE
static inline k_ticks_t z_vrfy_k_thread_timeout_remaining_ticks(
						    struct k_thread *t)
{
	Z_OOPS(Z_SYSCALL_OBJ(t, K_OBJ_THREAD));
	return z_impl_k_thread_timeout_remaining_ticks(t);
}
#include <syscalls/k_thread_timeout_remaining_ticks_mrsh.c>

static inline k_ticks_t z_vrfy_k_thread_timeout_expires_ticks(
						  struct k_thread *t)
{
	Z_OOPS(Z_SYSCALL_OBJ(t, K_OBJ_THREAD));
	return z_impl_k_thread_timeout_expires_ticks(t);
}
#include <syscalls/k_thread_timeout_expires_ticks_mrsh.c>
#endif
