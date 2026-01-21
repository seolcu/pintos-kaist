# 4주차 진행기록

## PROJECT 2: USER PROGRAMS

저번에 다 구현하지 못한 PROJECT 2를 마무리하겠습니다.

### System Calls

#### exec-once

`exec-once`, `exec-arg`, `exec-boundary`, `exec-missing`, `exec-read`에서 아래 패닉이 반복적으로 발생했습니다.

```
PANIC at ../../threads/thread.c:551 in schedule(): assertion 'is_thread(next)' failed.
```

원인은 이전 주차에 `lock_release()` 끝에서 `thread_yield()`를 호출하도록 수정했던 것인데, 이 호출이 syscall 중간(예: `exec` 경로의 `filesys_open` 내부)에서 발생하면서 스케줄러가 아직 준비되지 않은 스레드를 잡아 깨진 것으로 보였습니다.

따라서 `lock_release()`에서 강제 `thread_yield()`를 제거하고, 대신 `sema_up()`에서 깨운 스레드가 현재보다 우선순위가 높을 때만 양보하도록 조정했습니다.

```C
if (unblocked != NULL && !intr_context() &&
    unblocked->priority > thread_current()->priority)
  thread_yield();
```

#### fork/exec/wait

fork/wait는 부모가 자식의 종료 상태를 한 번만 회수할 수 있고, 자식이 먼저 종료해도 wait가 블록/해제되어야 합니다.
그래서 부모-자식 사이 상태를 담는 구조를 따로 두고, 세마포어로 종료 시점을 동기화했습니다.

```C
struct child_info {
  tid_t tid;
  int exit_status;
  bool exited;
  bool waited;
  bool parent_alive;
  struct semaphore exit_sema;
  struct list_elem elem;
};
```

`process_fork()`에서는 자식 정보를 만들고, 자식 스레드가 `__do_fork()`에서 복제를 끝낼 때까지 기다리도록 했습니다.

```C
tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, args);
child->tid = tid;
list_push_back(&parent->children, &child->elem);
sema_down(&args->done);
```

`__do_fork()`에서는 부모의 `pml4`/FD 테이블을 복제하고, 성공 여부를 부모에게 알려준 뒤 `do_iret()`로 유저로 복귀시켰습니다.

```C
memcpy(&if_, &args->parent_if, sizeof(struct intr_frame));
if_.R.rax = 0;
current->pml4 = pml4_create();
if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
  goto error;
for (int fd = 2; fd < 128; fd++) {
  if (parent->fd_table[fd] != NULL) {
    struct file *dup = file_duplicate(parent->fd_table[fd]);
    if (dup == NULL)
      goto error;
    current->fd_table[fd] = dup;
  }
}
args->success = true;
sema_up(&args->done);
do_iret(&if_);
```

`wait`은 부모의 child list에서 tid를 찾고, 한 번만 wait 가능하도록 막은 뒤, 자식 종료를 기다렸다가 상태를 반환하도록 했습니다.

```C
struct child_info *child = find_child_info(curr, child_tid);
if (child == NULL || child->waited)
  return -1;
child->waited = true;
if (!child->exited)
  sema_down(&child->exit_sema);
list_remove(&child->elem);
free(child);
return child->exit_status;
```

exec의 경우, 인자 배열이 커널 스택을 넘쳐 `thread_current()`가 깨지는 문제가 있어 `argv/argv_addrs`를 페이지로 분리했습니다.

```C
char **argv = palloc_get_page(0);
char **argv_addrs = palloc_get_page(0);
...
palloc_free_page(argv);
palloc_free_page(argv_addrs);
```

#### 자식 프로그램 test_name 문제

`child-read`, `multi-oom` 등에서 `msg("begin")` 이전에 `test_name`이 설정되지 않아 `((null)) begin`이 출력되는 문제가 있었습니다.
따라서 user entry에서 `argv[0]`로 초기화하도록 추가했습니다.

```C
extern const char *test_name __attribute__((weak));
if (&test_name != NULL && test_name == NULL && argc > 0)
  test_name = argv[0];
```

### Process Termination Messages

프로세스 종료 메시지(`prog: exit(status)`) 요구를 맞추기 위해 `process_exit()`에서 exit status를 출력하도록 했습니다.

```C
if (curr->pml4 != NULL)
  printf("%s: exit(%d)\n", curr->name, curr->exit_status);
```

또한 `exec`로 바뀌기 전 실행 파일의 write deny를 풀기 위해 `exec_file`을 닫고, 종료 시에도 파일을 닫도록 했습니다.

```C
if (curr->exec_file != NULL) {
  file_close(curr->exec_file);
  curr->exec_file = NULL;
}
```

### multi-oom 실패 (커널 페이지 누수)

`make check` 결과 대부분의 userprog 테스트는 통과했지만 `multi-oom`이 남았습니다.

```
FAIL tests/userprog/no-vm/multi-oom
run: should have forked at least 230 times, but 225 times forked: FAILED
```

225번 이상 fork가 진행되지 않았습니다. 테스트 파일 `/tests/userprog/no-vm/multi-oom.c`를 살펴보면, 의도적으로 잘못된 포인터를 넘겨 비정상 종료를 유발합니다.

```C
static int NO_INLINE consume_some_resources_and_die(void) {
  consume_some_resources();
  int *KERN_BASE = (int *)0x8004000000;

  switch (random_ulong() % 5) {
  case 4:
    open((char *)KERN_BASE);
    exit(-1);
    break;
  ...
  }
  return 0;
}

int main(int argc UNUSED, char *argv[] UNUSED) {
  int first_run_depth = make_children();
  for (int i = 0; i < EXPECTED_REPETITIONS; i++) {
    int current_run_depth = make_children();
    if (current_run_depth < first_run_depth)
      fail("should have forked at least %d times, but %d times forked",
           first_run_depth, current_run_depth);
  }
}
```

`open((char *)KERN_BASE)`에서 `copy_in_string()`이 유저 주소 검증에 실패하면 `sys_exit()`로 바로 빠져서, 그 전에 잡은 커널 페이지가 해제되지 않는 누수가 생겼습니다. 아래처럼 실패 경로에서 `palloc_free_page()` 후 `sys_exit()`하도록 바꿨습니다.

```C
static char *copy_in_string(const char *ustr) {
  if (ustr == NULL)
    sys_exit(-1);

  struct thread *curr = thread_current();
  char *kstr = palloc_get_page(0);
  if (kstr == NULL)
    sys_exit(-1);

  for (size_t i = 0; i < PGSIZE; i++) {
    const void *uaddr = (const void *)(ustr + i);
    if (uaddr == NULL || !is_user_vaddr(uaddr) || curr->pml4 == NULL) {
      palloc_free_page(kstr);
      sys_exit(-1);
    }
    const char *kaddr = pml4_get_page(curr->pml4, uaddr);
    if (kaddr == NULL) {
      palloc_free_page(kstr);
      sys_exit(-1);
    }

    kstr[i] = *kaddr;
    if (kstr[i] == '\0')
      return kstr;
  }

  palloc_free_page(kstr);
  sys_exit(-1);
}
```

수정 이후 `multi-oom`도 통과했습니다.

## PROJECT 3: VIRTUAL MEMORY

### Memory Management

#### initd

VM 빌드에서 `make check`를 돌려보니 userprog 테스트들이 시작부터 전부 커널 패닉으로 실패했습니다.

```
Kernel panic in run: PANIC at ../../userprog/process.c:141 in initd(): Fail to launch initd
...
0x...: spt_find_page (vm/vm.c:...)
0x...: vm_alloc_page_with_initializer (vm/vm.c:...)
0x...: load_segment (userprog/process.c:...)
0x...: load (userprog/process.c:...)
0x...: process_exec (userprog/process.c:...)
0x...: initd (userprog/process.c:...)
```

콜스택을 보면 `load_segment()`가 `vm_alloc_page_with_initializer()`를 호출하는 과정에서 SPT의 hash를 건드리다가 깨진 상황이었습니다.

원인은 `process_exec()`에서 `process_cleanup()`로 기존 주소공간/테이블을 정리한 뒤, 다시 `load()`를 호출하면서 SPT를 초기화하지 않았기 때문입니다.

따라서 `process_cleanup()` 직후 SPT를 다시 초기화하도록 수정했습니다.

```C
process_cleanup();
#ifdef VM
supplemental_page_table_init(&thread_current()->spt);
#endif

success = load(argv[0], &_if);
```

#### SPT를 hash로 구현

SPT는 간단하게 가상 페이지 주소(va) -> struct page를 hash로 매핑하도록 구현했습니다.
그래서 `struct page`에 `hash_elem`과 `writable` 플래그를 추가했습니다.

```C
struct page {
  ...
  struct hash_elem spt_elem;
  bool writable;
  ...
};
```

또한 page fault 처리에서 SPT를 이용해 페이지를 찾아 claim하도록 `vm_try_handle_fault()`를 구현했습니다.

```C
page = spt_find_page(spt, addr);
if (page == NULL)
  return false;
if (write && !page->writable)
  return false;
if (!not_present)
  return false;

return vm_do_claim_page(page);
```

`vm_do_claim_page()`에서는 `palloc_get_page(PAL_USER)`로 frame을 얻고, `pml4_set_page()`로 매핑한 뒤, `swap_in(page, frame->kva)`를 호출해 실제 초기화를 이어가도록 구성했습니다.

#### double free

이후 `args-none` 같은 테스트를 개별로 돌려보니 다른 패닉이 터졌습니다.

```
Kernel panic in run: PANIC at ../../threads/palloc.c:321 in palloc_free_multiple():
assertion `bitmap_all (pool->used_map, page_idx, page_cnt)' failed.
...
0x...: pml4_destroy (threads/mmu.c:199)
0x...: process_cleanup (userprog/process.c:...)
```

원인은 프로세스 종료 경로에서 `pml4_destroy()`가 user page frame을 `palloc_free_page()`로 정리하는데,
제가 `anon_destroy()`에서 frame을 또 `palloc_free_page()`해서 double free가 발생한 것입니다.

따라서 `anon_destroy()`에서는 frame을 해제하지 않고, pml4 정리에 맡기도록 수정했습니다.

### Anonymous Page

#### lazy_load_segment

페이지별로 어느 파일의 어느 offset에서 몇 바이트를 읽을지에 대한 정보가 필요해서 `segment_aux` 구조를 만들어 aux로 넘기고, page fault로 claim된 이후에 실제 read를 하도록 했습니다.

```C
struct segment_aux *aux = malloc(sizeof *aux);
aux->file = file;
aux->ofs = ofs;
aux->read_bytes = page_read_bytes;
aux->zero_bytes = page_zero_bytes;
vm_alloc_page_with_initializer(VM_ANON, upage, writable,
                               lazy_load_segment, aux);
```

```C
static bool lazy_load_segment(struct page *page, void *aux) {
  struct segment_aux *args = aux;
  file_read_at(args->file, page->frame->kva, args->read_bytes, args->ofs);
  memset(page->frame->kva + args->read_bytes, 0, args->zero_bytes);
  free(args);
  return true;
}
```

#### fork 시 SPT copy에서 file_reopen 처리

fork 시 자식도 `VM_UNINIT` 페이지를 그대로 물려받는데, 이때 aux의 `file`을 그대로 복사하면 부모가 먼저 종료되어 파일이 닫힐 수 있습니다.
따라서 `supplemental_page_table_copy()`에서 aux를 복제한 뒤, `file_reopen()`으로 자식이 독립적인 file 포인터를 갖도록 했습니다.

```C
if (dst_aux->file != NULL)
  dst_aux->file = file_reopen(dst_aux->file);
```

### 테스트 결과

통과한 결과는 다음과 같습니다.

```
pass tests/userprog/args-none
pass tests/userprog/args-single
pass tests/userprog/halt
pass tests/userprog/exec-once
pass tests/userprog/create-normal
```
