# 2주차 진행기록

## Priority Scheduling

### priority-change

다음 실행될 스레드를 결정하는 `next_thread_to_run` 함수가 실행될 때마다 `ready_list`를 priority 기준으로 sort하도록 했습니다.

```C
bool thread_priority_less(const struct list_elem *a, const struct list_elem *b,
						  void *aux UNUSED)
{
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);
	return ta->priority > tb->priority;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
	{
		list_sort(&ready_list, (list_less_func *)thread_priority_less, NULL);
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
	}
}
```

이때 기본적으로 `list_sort` 함수가 오름차순으로 정렬을 수행하기 때문에, 우선순위가 높은 스레드가 앞으로 오도록 `thread_priority_less` 함수를 작성할 때 `>` 연산자를 사용했습니다.

이후 `make check`을 해보니 다음과 같은 결과가 나왔습니다.

```
FAIL tests/threads/priority-change
Test output failed to match any acceptable form.

Acceptable output:
  (priority-change) begin
  (priority-change) Creating a high-priority thread 2.
  (priority-change) Thread 2 now lowering priority.
  (priority-change) Thread 2 should have just lowered its priority.
  (priority-change) Thread 2 exiting.
  (priority-change) Thread 2 should have just exited.
  (priority-change) end
Differences in `diff -u' format:
  (priority-change) begin
  (priority-change) Creating a high-priority thread 2.
- (priority-change) Thread 2 now lowering priority.
  (priority-change) Thread 2 should have just lowered its priority.
- (priority-change) Thread 2 exiting.
  (priority-change) Thread 2 should have just exited.
  (priority-change) end
```

아무래도 우선순위가 변경되는 상황에서 스레드가 제대로 스케줄링되지 않는 것 같아, `thread_set_priority` 함수에서 우선순위가 변경된 후에 `thread_yield` 함수를 호출하도록 수정했습니다.

```C
/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
	thread_current()->priority = new_priority;
	thread_yield();
}
```

이후 `make check`을 다시 해보니 이런 결과가 나왔습니다.

```
FAIL tests/threads/priority-change
Test output failed to match any acceptable form.

Acceptable output:
  (priority-change) begin
  (priority-change) Creating a high-priority thread 2.
  (priority-change) Thread 2 now lowering priority.
  (priority-change) Thread 2 should have just lowered its priority.
  (priority-change) Thread 2 exiting.
  (priority-change) Thread 2 should have just exited.
  (priority-change) end
Differences in `diff -u' format:
  (priority-change) begin
  (priority-change) Creating a high-priority thread 2.
- (priority-change) Thread 2 now lowering priority.
  (priority-change) Thread 2 should have just lowered its priority.
+ (priority-change) Thread 2 now lowering priority.
  (priority-change) Thread 2 exiting.
  (priority-change) Thread 2 should have just exited.
  (priority-change) end
```

우선순위 변경에는 성공하는 것 같지만, 출력 순서에 문제가 있습니다.

테스트 파일 `/tests/threads/priority-change.c`를 살펴보았습니다.

```C
/* Verifies that lowering a thread's priority so that it is no
   longer the highest-priority thread in the system causes it to
   yield immediately. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/thread.h"

static thread_func changing_thread;

void
test_priority_change (void)
{
  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  msg ("Creating a high-priority thread 2.");
  thread_create ("thread 2", PRI_DEFAULT + 1, changing_thread, NULL);
  msg ("Thread 2 should have just lowered its priority.");
  thread_set_priority (PRI_DEFAULT - 2);
  msg ("Thread 2 should have just exited.");
}

static void
changing_thread (void *aux UNUSED)
{
  msg ("Thread 2 now lowering priority.");
  thread_set_priority (PRI_DEFAULT - 1);
  msg ("Thread 2 exiting.");
}
```

`test_priority_change` 함수는 PRI_DEFAULT + 1 우선순위를 가진 스레드를 생성합니다. 따라서 이 스레드가 생성되자 마자 현재 실행중인 스레드보다 우선순위가 높아져서 스케줄링됩니다. 따라서 바로 "Thread 2 now lowering priority." 메시지가 출력되어야 합니다.

하지만 현재 구현에서는 `thread_set_priority` 함수가 호출된 후에야 스케줄링이 일어나기 때문에, "Thread 2 now lowering priority." 메시지가 출력된 후에야 우선순위가 변경되고 스케줄링이 일어나게 됩니다. 따라서, 스레드가 생성되지마자 스케줄링 되도록 `thread_create` 함수에서도 `thread_yield` 함수를 호출하도록 수정했습니다.

```C
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);

	thread_yield();

	return tid;
}
```

그러자 priority-change 테스트가 통과하는 것을 확인할 수 있었습니다.

```
pass tests/threads/priority-change
```

### priority-donate-one

이제 priority donation을 구현하기로 했습니다. 우선 priority-donate-one 테스트 결과는 다음과 같았습니다.

```
FAIL tests/threads/priority-donate-one
Test output failed to match any acceptable form.

Acceptable output:
  (priority-donate-one) begin
  (priority-donate-one) This thread should have priority 32.  Actual priority: 32.
  (priority-donate-one) This thread should have priority 33.  Actual priority: 33.
  (priority-donate-one) acquire2: got the lock
  (priority-donate-one) acquire2: done
  (priority-donate-one) acquire1: got the lock
  (priority-donate-one) acquire1: done
  (priority-donate-one) acquire2, acquire1 must already have finished, in that order.
  (priority-donate-one) This should be the last line before finishing this test.
  (priority-donate-one) end
Differences in `diff -u' format:
  (priority-donate-one) begin
- (priority-donate-one) This thread should have priority 32.  Actual priority: 32.
- (priority-donate-one) This thread should have priority 33.  Actual priority: 33.
- (priority-donate-one) acquire2: got the lock
- (priority-donate-one) acquire2: done
- (priority-donate-one) acquire1: got the lock
- (priority-donate-one) acquire1: done
+ (priority-donate-one) This thread should have priority 32.  Actual priority: 31.
+ (priority-donate-one) This thread should have priority 33.  Actual priority: 31.
  (priority-donate-one) acquire2, acquire1 must already have finished, in that order.
  (priority-donate-one) This should be the last line before finishing this test.
  (priority-donate-one) end
```

`/tests/threads/priority-donate-one.c` 코드는 다음과 같습니다.

```C
/* The main thread acquires a lock.  Then it creates two
   higher-priority threads that block acquiring the lock, causing
   them to donate their priorities to the main thread.  When the
   main thread releases the lock, the other threads should
   acquire it in priority order.

   Based on a test originally submitted for Stanford's CS 140 in
   winter 1999 by Matt Franklin <startled@leland.stanford.edu>,
   Greg Hutchins <gmh@leland.stanford.edu>, Yu Ping Hu
   <yph@cs.stanford.edu>.  Modified by arens. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

static thread_func acquire1_thread_func;
static thread_func acquire2_thread_func;

void
test_priority_donate_one (void)
{
  struct lock lock;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&lock);
  lock_acquire (&lock);
  thread_create ("acquire1", PRI_DEFAULT + 1, acquire1_thread_func, &lock);
  msg ("This thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 1, thread_get_priority ());
  thread_create ("acquire2", PRI_DEFAULT + 2, acquire2_thread_func, &lock);
  msg ("This thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 2, thread_get_priority ());
  lock_release (&lock);
  msg ("acquire2, acquire1 must already have finished, in that order.");
  msg ("This should be the last line before finishing this test.");
}

static void
acquire1_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("acquire1: got the lock");
  lock_release (lock);
  msg ("acquire1: done");
}

static void
acquire2_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("acquire2: got the lock");
  lock_release (lock);
  msg ("acquire2: done");
}
```

메인 스레드가 우선순위가 더 높은 두 스레드에게 priority donation을 받아야 합니다.

우선, `struct thread`에 두 개의 필드를 추가했습니다.

```C
int original_priority;
struct lock *wait_on_lock;
```

`init_thread`에서 이 필드들을 초기화하고, `thread_set_priority`에서도 `original_priority`를 함께 업데이트하도록 수정했습니다.

그 다음, `/threads/synch.c`의 `lock_acquire`에서 lock holder의 우선순위보다 현재 스레드의 우선순위가 높으면 donation을 수행하도록 했습니다.

```C
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	struct thread *curr = thread_current();

	/* Priority donation */
	if (lock->holder != NULL)
	{
		curr->wait_on_lock = lock;
		if (lock->holder->priority < curr->priority)
			lock->holder->priority = curr->priority;
	}

	sema_down(&lock->semaphore);

	curr->wait_on_lock = NULL;
	lock->holder = curr;
}
```

`lock_release`에서는 원래 우선순위로 복원하도록 했습니다.

```C
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	lock->holder = NULL;
	sema_up(&lock->semaphore);

	/* Restore original priority after release */
	thread_current()->priority = thread_current()->original_priority;
	thread_yield();
}
```

마지막으로, `sema_up`에서 waiter 중 가장 높은 우선순위를 가진 스레드를 먼저 깨우도록 수정했습니다.

```C
void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (!list_empty(&sema->waiters))
	{
		/* Find highest priority waiter */
		struct list_elem *max_elem = list_begin(&sema->waiters);
		struct list_elem *e;
		for (e = list_begin(&sema->waiters); e != list_end(&sema->waiters); e = list_next(e))
		{
			struct thread *t = list_entry(e, struct thread, elem);
			struct thread *max_t = list_entry(max_elem, struct thread, elem);
			if (t->priority > max_t->priority)
				max_elem = e;
		}
		list_remove(max_elem);
		thread_unblock(list_entry(max_elem, struct thread, elem));
	}
	sema->value++;
	intr_set_level(old_level);
}
```

이후 `make check`을 해보니 priority-donate-one 테스트가 통과했습니다.

```
pass tests/threads/priority-donate-one
```