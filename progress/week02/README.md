# 2주차 진행기록

## PROJECT 1: THREADS

### Priority Scheduling

#### priority-change

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

#### priority-donate-one

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

#### priority-donate-multiple

하지만 `priority-donate-multiple` 테스트는 여전히 실패했습니다.

테스트 코드를 확인했습니다.

```C
/* The main thread acquires locks A and B, then it creates two
   higher-priority threads.  Each of these threads blocks
   acquiring one of the locks and thus donate their priority to
   the main thread.  The main thread releases the locks in turn
   and relinquishes its donated priorities.

   Based on a test originally submitted for Stanford's CS 140 in
   winter 1999 by Matt Franklin <startled@leland.stanford.edu>,
   Greg Hutchins <gmh@leland.stanford.edu>, Yu Ping Hu
   <yph@cs.stanford.edu>.  Modified by arens. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

static thread_func a_thread_func;
static thread_func b_thread_func;

void
test_priority_donate_multiple (void)
{
  struct lock a, b;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&a);
  lock_init (&b);

  lock_acquire (&a);
  lock_acquire (&b);

  thread_create ("a", PRI_DEFAULT + 1, a_thread_func, &a);
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 1, thread_get_priority ());

  thread_create ("b", PRI_DEFAULT + 2, b_thread_func, &b);
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 2, thread_get_priority ());

  lock_release (&b);
  msg ("Thread b should have just finished.");
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 1, thread_get_priority ());

  lock_release (&a);
  msg ("Thread a should have just finished.");
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT, thread_get_priority ());
}

static void
a_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("Thread a acquired lock a.");
  lock_release (lock);
  msg ("Thread a finished.");
}

static void
b_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("Thread b acquired lock b.");
  lock_release (lock);
  msg ("Thread b finished.");
}
```

실패 메시지는 다음과 같습니다.

```
FAIL tests/threads/priority-donate-multiple
Test output failed to match any acceptable form.

Acceptable output:
  (priority-donate-multiple) begin
  (priority-donate-multiple) Main thread should have priority 32.  Actual priority: 32.
  (priority-donate-multiple) Main thread should have priority 33.  Actual priority: 33.
  (priority-donate-multiple) Thread b acquired lock b.
  (priority-donate-multiple) Thread b finished.
  (priority-donate-multiple) Thread b should have just finished.
  (priority-donate-multiple) Main thread should have priority 32.  Actual priority: 32.
  (priority-donate-multiple) Thread a acquired lock a.
  (priority-donate-multiple) Thread a finished.
  (priority-donate-multiple) Thread a should have just finished.
  (priority-donate-multiple) Main thread should have priority 31.  Actual priority: 31.
  (priority-donate-multiple) end
Differences in `diff -u' format:
  (priority-donate-multiple) begin
  (priority-donate-multiple) Main thread should have priority 32.  Actual priority: 32.
  (priority-donate-multiple) Main thread should have priority 33.  Actual priority: 33.
  (priority-donate-multiple) Thread b acquired lock b.
  (priority-donate-multiple) Thread b finished.
  (priority-donate-multiple) Thread b should have just finished.
- (priority-donate-multiple) Main thread should have priority 32.  Actual priority: 32.
+ (priority-donate-multiple) Main thread should have priority 32.  Actual priority: 31.
  (priority-donate-multiple) Thread a acquired lock a.
  (priority-donate-multiple) Thread a finished.
  (priority-donate-multiple) Thread a should have just finished.
  (priority-donate-multiple) Main thread should have priority 31.  Actual priority: 31.
  (priority-donate-multiple) end
```

메인 스레드가 락 두개를 잡고 있고, b 락이 먼저 해제됩니다. 이 과정에서 lock_release가 priority를 original_priority로 복원하기 때문에, a로부터 donate받은 priority가 무시되는 문제가 발생합니다.

이 문제를 해결하기 위해, 스레드가 보유한 락들에 대한 donation을 추적하도록 했습니다.

우선 `struct thread`에 `donations` 리스트와 `donation_elem`을 추가했습니다.

```C
struct list donations;
struct list_elem donation_elem;
```

`init_thread`에서 donations 리스트를 초기화했습니다.

```C
list_init(&t->donations);
```

`lock_acquire`에서 donation 시 holder의 donations 리스트에 현재 스레드를 추가했습니다.

```C
if (lock->holder != NULL)
{
	curr->wait_on_lock = lock;
	list_push_back(&lock->holder->donations, &curr->donation_elem);
	if (lock->holder->priority < curr->priority)
		lock->holder->priority = curr->priority;
}
```

`lock_release`에서는 해당 락을 기다리던 스레드들을 donations 리스트에서 제거하고, 남은 donations 중 최대 priority를 계산하도록 수정했습니다.

```C
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	struct thread *curr = thread_current();

	/* Remove donations for this lock */
	struct list_elem *e = list_begin(&curr->donations);
	while (e != list_end(&curr->donations))
	{
		struct thread *t = list_entry(e, struct thread, donation_elem);
		struct list_elem *next = list_next(e);
		if (t->wait_on_lock == lock)
			list_remove(e);
		e = next;
	}

	lock->holder = NULL;
	sema_up(&lock->semaphore);

	/* Recalculate priority from remaining donations */
	int max_priority = curr->original_priority;
	for (e = list_begin(&curr->donations); e != list_end(&curr->donations); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, donation_elem);
		if (t->priority > max_priority)
			max_priority = t->priority;
	}
	curr->priority = max_priority;
	thread_yield();
}
```

이후 `make check`을 해보니 priority-donate-multiple 테스트가 통과했습니다. 동시에 priority-donate-multiple2 테스트도 통과했습니다.

```
pintos -v -k -T 60 -m 20   -- -q   run priority-donate-multiple < /dev/null 2> tests/threads/priority-donate-multiple.errors > tests/threads/priority-donate-multiple.output
perl -I../.. ../../tests/threads/priority-donate-multiple.ck tests/threads/priority-donate-multiple tests/threads/priority-donate-multiple.result
pass tests/threads/priority-donate-multiple
pintos -v -k -T 60 -m 20   -- -q   run priority-donate-multiple2 < /dev/null 2> tests/threads/priority-donate-multiple2.errors > tests/threads/priority-donate-multiple2.output
perl -I../.. ../../tests/threads/priority-donate-multiple2.ck tests/threads/priority-donate-multiple2 tests/threads/priority-donate-multiple2.result
pass tests/threads/priority-donate-multiple2
```

#### priority-donate-nest

테스트 코드는 다음과 같습니다.

```C
/* Low-priority main thread L acquires lock A.  Medium-priority
   thread M then acquires lock B then blocks on acquiring lock A.
   High-priority thread H then blocks on acquiring lock B.  Thus,
   thread H donates its priority to M, which in turn donates it
   to thread L.

   Based on a test originally submitted for Stanford's CS 140 in
   winter 1999 by Matt Franklin <startled@leland.stanford.edu>,
   Greg Hutchins <gmh@leland.stanford.edu>, Yu Ping Hu
   <yph@cs.stanford.edu>.  Modified by arens. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

struct locks
  {
    struct lock *a;
    struct lock *b;
  };

static thread_func medium_thread_func;
static thread_func high_thread_func;

void
test_priority_donate_nest (void)
{
  struct lock a, b;
  struct locks locks;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&a);
  lock_init (&b);

  lock_acquire (&a);

  locks.a = &a;
  locks.b = &b;
  thread_create ("medium", PRI_DEFAULT + 1, medium_thread_func, &locks);
  thread_yield ();
  msg ("Low thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 1, thread_get_priority ());

  thread_create ("high", PRI_DEFAULT + 2, high_thread_func, &b);
  thread_yield ();
  msg ("Low thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 2, thread_get_priority ());

  lock_release (&a);
  thread_yield ();
  msg ("Medium thread should just have finished.");
  msg ("Low thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT, thread_get_priority ());
}

static void
medium_thread_func (void *locks_)
{
  struct locks *locks = locks_;

  lock_acquire (locks->b);
  lock_acquire (locks->a);

  msg ("Medium thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 2, thread_get_priority ());
  msg ("Medium thread got the lock.");

  lock_release (locks->a);
  thread_yield ();

  lock_release (locks->b);
  thread_yield ();

  msg ("High thread should have just finished.");
  msg ("Middle thread finished.");
}

static void
high_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("High thread got the lock.");
  lock_release (lock);
  msg ("High thread finished.");
}
```

실패 메시지는 다음과 같습니다.

```
Acceptable output:
  (priority-donate-nest) begin
  (priority-donate-nest) Low thread should have priority 32.  Actual priority: 32.
  (priority-donate-nest) Low thread should have priority 33.  Actual priority: 33.
  (priority-donate-nest) Medium thread should have priority 33.  Actual priority: 33.
  (priority-donate-nest) Medium thread got the lock.
  (priority-donate-nest) High thread got the lock.
  (priority-donate-nest) High thread finished.
  (priority-donate-nest) High thread should have just finished.
  (priority-donate-nest) Middle thread finished.
  (priority-donate-nest) Medium thread should just have finished.
  (priority-donate-nest) Low thread should have priority 31.  Actual priority: 31.
  (priority-donate-nest) end
Differences in `diff -u' format:
  (priority-donate-nest) begin
  (priority-donate-nest) Low thread should have priority 32.  Actual priority: 32.
- (priority-donate-nest) Low thread should have priority 33.  Actual priority: 33.
+ (priority-donate-nest) Low thread should have priority 33.  Actual priority: 32.
  (priority-donate-nest) Medium thread should have priority 33.  Actual priority: 33.
  (priority-donate-nest) Medium thread got the lock.
  (priority-donate-nest) High thread got the lock.
  (priority-donate-nest) High thread finished.
  (priority-donate-nest) High thread should have just finished.
  (priority-donate-nest) Middle thread finished.
  (priority-donate-nest) Medium thread should just have finished.
  (priority-donate-nest) Low thread should have priority 31.  Actual priority: 31.
  (priority-donate-nest) end
```

high 스레드가 medium 스레드가 가지고 있는 lock b를 기다리고, medium 스레드는 low 스레드가 lock a를 release할 때까지 기다립니다. 이 경우에 high 스레드의 priority가 medium 스레드에 donate 되면서, 새롭게 업데이트된 medium의 priority가 low 스레드에도 넘어가야 하는데, 현재 구현에서는 priority donation이 딱 하위 스레드에게만 이루어지기 때문에 문제가 발생합니다.

이 문제를 해결하기 위해, `lock_acquire`에서 donation 시 `wait_on_lock` 체인을 따라가면서 모든 관련 스레드에 priority를 전파하도록 수정했습니다.

```C
/* Nested donation */
struct thread *t = lock->holder;
while (t != NULL && t->priority < curr->priority)
{
	t->priority = curr->priority;
	if (t->wait_on_lock == NULL)
		break;
	t = t->wait_on_lock->holder;
}
```

이후 `make check`을 해보니 priority-donate-nest 테스트에 성공한 것을 볼 수 있었습니다.

```
pass tests/threads/priority-donate-nest
```

#### priority-donate-sema

테스트 코드는 다음과 같습니다.

```C
/* Low priority thread L acquires a lock, then blocks downing a
   semaphore.  Medium priority thread M then blocks waiting on
   the same semaphore.  Next, high priority thread H attempts to
   acquire the lock, donating its priority to L.

   Next, the main thread ups the semaphore, waking up L.  L
   releases the lock, which wakes up H.  H "up"s the semaphore,
   waking up M.  H terminates, then M, then L, and finally the
   main thread.

   Written by Godmar Back <gback@cs.vt.edu>. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

struct lock_and_sema
  {
    struct lock lock;
    struct semaphore sema;
  };

static thread_func l_thread_func;
static thread_func m_thread_func;
static thread_func h_thread_func;

void
test_priority_donate_sema (void)
{
  struct lock_and_sema ls;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&ls.lock);
  sema_init (&ls.sema, 0);
  thread_create ("low", PRI_DEFAULT + 1, l_thread_func, &ls);
  thread_create ("med", PRI_DEFAULT + 3, m_thread_func, &ls);
  thread_create ("high", PRI_DEFAULT + 5, h_thread_func, &ls);
  sema_up (&ls.sema);
  msg ("Main thread finished.");
}

static void
l_thread_func (void *ls_)
{
  struct lock_and_sema *ls = ls_;

  lock_acquire (&ls->lock);
  msg ("Thread L acquired lock.");
  sema_down (&ls->sema);
  msg ("Thread L downed semaphore.");
  lock_release (&ls->lock);
  msg ("Thread L finished.");
}

static void
m_thread_func (void *ls_)
{
  struct lock_and_sema *ls = ls_;

  sema_down (&ls->sema);
  msg ("Thread M finished.");
}

static void
h_thread_func (void *ls_)
{
  struct lock_and_sema *ls = ls_;

  lock_acquire (&ls->lock);
  msg ("Thread H acquired lock.");

  sema_up (&ls->sema);
  lock_release (&ls->lock);
  msg ("Thread H finished.");
}
```

실패 메시지는 다음과 같습니다.

```
Acceptable output:
  (priority-donate-sema) begin
  (priority-donate-sema) Thread L acquired lock.
  (priority-donate-sema) Thread L downed semaphore.
  (priority-donate-sema) Thread H acquired lock.
  (priority-donate-sema) Thread H finished.
  (priority-donate-sema) Thread M finished.
  (priority-donate-sema) Thread L finished.
  (priority-donate-sema) Main thread finished.
  (priority-donate-sema) end
Differences in `diff -u' format:
  (priority-donate-sema) begin
  (priority-donate-sema) Thread L acquired lock.
- (priority-donate-sema) Thread L downed semaphore.
+ (priority-donate-sema) (priority-donate-sema) Thread L downed semaphore.
  (priority-donate-sema) Thread H acquired lock.
  (priority-donate-sema) Thread H finished.
  (priority-donate-sema) Thread M finished.
  (priority-donate-sema) Thread L finished.
- (priority-donate-sema) Main thread finished.
+ Main thread finished.
  (priority-donate-sema) end
```

로직은 맞는 것 같은데, 출력 형식이 안 맞습니다. 마지막 줄에 나왔어야 할 `(priority-donate-sema)`가 위쪽으로 올라왔습니다.

이건 왜 그런건지 잘 모르겠습니다. 실제 코드에서 문제가 있을 수도 있고, 환경 문제일 수도 있고, 이후 구현에 따라 해결될 지도 모른다는 생각이 들어서 우선 넘어가기로 했습니다.

#### priority-donate-lower

테스트 코드는 다음과 같습니다.

```C
/* The main thread acquires a lock.  Then it creates a
   higher-priority thread that blocks acquiring the lock, causing
   it to donate their priorities to the main thread.  The main
   thread attempts to lower its priority, which should not take
   effect until the donation is released. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

static thread_func acquire_thread_func;

void
test_priority_donate_lower (void)
{
  struct lock lock;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&lock);
  lock_acquire (&lock);
  thread_create ("acquire", PRI_DEFAULT + 10, acquire_thread_func, &lock);
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 10, thread_get_priority ());

  msg ("Lowering base priority...");
  thread_set_priority (PRI_DEFAULT - 10);
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 10, thread_get_priority ());
  lock_release (&lock);
  msg ("acquire must already have finished.");
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT - 10, thread_get_priority ());
}

static void
acquire_thread_func (void *lock_)
{
  struct lock *lock = lock_;

  lock_acquire (lock);
  msg ("acquire: got the lock");
  lock_release (lock);
  msg ("acquire: done");
}
```

에러 메시지는 다음과 같았습니다.

```
Acceptable output:
  (priority-donate-lower) begin
  (priority-donate-lower) Main thread should have priority 41.  Actual priority: 41.
  (priority-donate-lower) Lowering base priority...
  (priority-donate-lower) Main thread should have priority 41.  Actual priority: 41.
  (priority-donate-lower) acquire: got the lock
  (priority-donate-lower) acquire: done
  (priority-donate-lower) acquire must already have finished.
  (priority-donate-lower) Main thread should have priority 21.  Actual priority: 21.
  (priority-donate-lower) end
Differences in `diff -u' format:
  (priority-donate-lower) begin
  (priority-donate-lower) Main thread should have priority 41.  Actual priority: 41.
  (priority-donate-lower) Lowering base priority...
- (priority-donate-lower) Main thread should have priority 41.  Actual priority: 41.
+ (priority-donate-lower) Main thread should have priority 41.  Actual priority: 21.
  (priority-donate-lower) acquire: got the lock
  (priority-donate-lower) acquire: done
  (priority-donate-lower) acquire must already have finished.
  (priority-donate-lower) Main thread should have priority 21.  Actual priority: 21.
  (priority-donate-lower) end
```

메인 스레드에서 priority를 낮추더라도, donation이 끝나야 priority가 낮아져야 합니다.

이 문제를 해결하기 위해, `thread_set_priority`에서 donations 리스트를 확인하여 priority를 재계산하도록 수정했습니다.

```C
void thread_set_priority(int new_priority)
{
	struct thread *curr = thread_current();
	curr->original_priority = new_priority;

	int max_priority = new_priority;
	struct list_elem *e;
	for (e = list_begin(&curr->donations); e != list_end(&curr->donations); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, donation_elem);
		if (t->priority > max_priority)
			max_priority = t->priority;
	}
	curr->priority = max_priority;
	thread_yield();
}
```

`original_priority`는 항상 새 값으로 설정하되, `priority`는 `new_priority`와 donations 중 최대값으로 설정합니다. 이렇게 하면 donation 없을 때는 priority가 정상적으로 변경되고, donation 중에는 donated priority가 유지됩니다.

이후 `make check`을 해보니 priority-donate-lower 테스트가 통과했습니다. 동시에 priority-fifo와 priority-preempt 테스트도 통과했습니다.

```
pintos -v -k -T 60 -m 20   -- -q   run priority-donate-lower < /dev/null 2> tests/threads/priority-donate-lower.errors > tests/threads/priority-donate-lower.output
perl -I../.. ../../tests/threads/priority-donate-lower.ck tests/threads/priority-donate-lower tests/threads/priority-donate-lower.result
pass tests/threads/priority-donate-lower
pintos -v -k -T 60 -m 20   -- -q   run priority-fifo < /dev/null 2> tests/threads/priority-fifo.errors > tests/threads/priority-fifo.output
perl -I../.. ../../tests/threads/priority-fifo.ck tests/threads/priority-fifo tests/threads/priority-fifo.result
pass tests/threads/priority-fifo
pintos -v -k -T 60 -m 20   -- -q   run priority-preempt < /dev/null 2> tests/threads/priority-preempt.errors > tests/threads/priority-preempt.output
perl -I../.. ../../tests/threads/priority-preempt.ck tests/threads/priority-preempt tests/threads/priority-preempt.result
pass tests/threads/priority-preempt
```

#### priority-sema

```
Acceptable output:
  (priority-sema) begin
  (priority-sema) Thread priority 30 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 29 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 28 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 27 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 26 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 25 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 24 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 23 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 22 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) Thread priority 21 woke up.
  (priority-sema) Back in main thread.
  (priority-sema) end
Differences in `diff -u' format:
  (priority-sema) begin
- (priority-sema) Thread priority 30 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 29 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 28 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 27 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 26 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 25 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 24 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 23 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 22 woke up.
- (priority-sema) Back in main thread.
- (priority-sema) Thread priority 21 woke up.
- (priority-sema) Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 30 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 29 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 28 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 27 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 26 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 25 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 24 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 23 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 22 woke up.
+ Back in main thread.
+ (priority-sema) (priority-sema) Thread priority 21 woke up.
+ Back in main thread.
  (priority-sema) end
```

이전의 priority-donate-sema 테스트에서 겪었던 것과 같은 출력 밀림 현상이 다시 발생했습니다. 우선 원인을 파악하지 못하니 다음 테스트로 바로 넘어갔습니다.

#### priority-condvar

```C
/* Tests that cond_signal() wakes up the highest-priority thread
   waiting in cond_wait(). */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static thread_func priority_condvar_thread;
static struct lock lock;
static struct condition condition;

void
test_priority_condvar (void)
{
  int i;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  lock_init (&lock);
  cond_init (&condition);

  thread_set_priority (PRI_MIN);
  for (i = 0; i < 10; i++)
    {
      int priority = PRI_DEFAULT - (i + 7) % 10 - 1;
      char name[16];
      snprintf (name, sizeof name, "priority %d", priority);
      thread_create (name, priority, priority_condvar_thread, NULL);
    }

  for (i = 0; i < 10; i++)
    {
      lock_acquire (&lock);
      msg ("Signaling...");
      cond_signal (&condition, &lock);
      lock_release (&lock);
    }
}

static void
priority_condvar_thread (void *aux UNUSED)
{
  msg ("Thread %s starting.", thread_name ());
  lock_acquire (&lock);
  cond_wait (&condition, &lock);
  msg ("Thread %s woke up.", thread_name ());
  lock_release (&lock);
}
```

```
Acceptable output:
  (priority-condvar) begin
  (priority-condvar) Thread priority 23 starting.
  (priority-condvar) Thread priority 22 starting.
  (priority-condvar) Thread priority 21 starting.
  (priority-condvar) Thread priority 30 starting.
  (priority-condvar) Thread priority 29 starting.
  (priority-condvar) Thread priority 28 starting.
  (priority-condvar) Thread priority 27 starting.
  (priority-condvar) Thread priority 26 starting.
  (priority-condvar) Thread priority 25 starting.
  (priority-condvar) Thread priority 24 starting.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 30 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 29 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 28 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 27 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 26 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 25 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 24 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 23 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 22 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 21 woke up.
  (priority-condvar) end
Differences in `diff -u' format:
  (priority-condvar) begin
  (priority-condvar) Thread priority 23 starting.
  (priority-condvar) Thread priority 22 starting.
  (priority-condvar) Thread priority 21 starting.
  (priority-condvar) Thread priority 30 starting.
  (priority-condvar) Thread priority 29 starting.
  (priority-condvar) Thread priority 28 starting.
  (priority-condvar) Thread priority 27 starting.
  (priority-condvar) Thread priority 26 starting.
  (priority-condvar) Thread priority 25 starting.
  (priority-condvar) Thread priority 24 starting.
  (priority-condvar) Signaling...
+ (priority-condvar) Thread priority 23 woke up.
+ (priority-condvar) Signaling...
+ (priority-condvar) Thread priority 22 woke up.
+ (priority-condvar) Signaling...
+ (priority-condvar) Thread priority 21 woke up.
+ (priority-condvar) Signaling...
  (priority-condvar) Thread priority 30 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 29 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 28 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 27 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 26 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 25 woke up.
  (priority-condvar) Signaling...
  (priority-condvar) Thread priority 24 woke up.
- (priority-condvar) Signaling...
- (priority-condvar) Thread priority 23 woke up.
- (priority-condvar) Signaling...
- (priority-condvar) Thread priority 22 woke up.
- (priority-condvar) Signaling...
- (priority-condvar) Thread priority 21 woke up.
  (priority-condvar) end
```

주석에서 명시되었듯이, `cond_signal` 함수는 우선순위가 높은 순서대로 스레드들을 깨워야 하는데, 현재 구현에서는 FIFO 순서로 깨우고 있습니다.

이 문제를 해결하기 위해, `cond_signal`에서 waiters 리스트를 순회하며 가장 높은 priority를 가진 waiter를 찾아서 signal하도록 수정했습니다.

```C
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
	{
		struct list_elem *max_elem = list_begin(&cond->waiters);
		struct list_elem *e;
		for (e = list_begin(&cond->waiters); e != list_end(&cond->waiters); e = list_next(e))
		{
			struct semaphore_elem *se = list_entry(e, struct semaphore_elem, elem);
			struct semaphore_elem *max_se = list_entry(max_elem, struct semaphore_elem, elem);
			struct thread *t = list_entry(list_begin(&se->semaphore.waiters), struct thread, elem);
			struct thread *max_t = list_entry(list_begin(&max_se->semaphore.waiters), struct thread, elem);
			if (t->priority > max_t->priority)
				max_elem = e;
		}
		list_remove(max_elem);
		sema_up(&list_entry(max_elem, struct semaphore_elem, elem)->semaphore);
	}
}
```

이후 `make check`을 해보니 priority-condvar 테스트가 통과했습니다. 동시에 priority-donate-chain 테스트도 통과했습니다.

```
pintos -v -k -T 60 -m 20   -- -q   run priority-condvar < /dev/null 2> tests/threads/priority-condvar.errors > tests/threads/priority-condvar.output
perl -I../.. ../../tests/threads/priority-condvar.ck tests/threads/priority-condvar tests/threads/priority-condvar.result
pass tests/threads/priority-condvar
pintos -v -k -T 60 -m 20   -- -q   run priority-donate-chain < /dev/null 2> tests/threads/priority-donate-chain.errors > tests/threads/priority-donate-chain.output
perl -I../.. ../../tests/threads/priority-donate-chain.ck tests/threads/priority-donate-chain tests/threads/priority-donate-chain.result
pass tests/threads/priority-donate-chain
```

---

Advanced Scheduler는 패스하므로, 이렇게 1단원을 마무리했습니다. 해결 못한 테스트는 출력 형식에 문제가 있던 `priority-donate-sema`, `priority-sema` 입니다.
