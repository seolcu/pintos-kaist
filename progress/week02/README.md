# 2주차 진행기록

## Priority Scheduling

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
