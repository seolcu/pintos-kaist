# 3주차 진행기록

## PROJECT 2: USER PROGRAMS

### Argument Passing

#### args-none

처음 테스트를 실행했을 때 다음과 같은 커널 패닉이 발생했습니다.

```
Kernel PANIC at ../../threads/mmu.c:206 in pml4_activate(): assertion `is_kernel_vaddr(pml4 ? pml4 : base_pml4)' failed.
```

backtrace를 분석해보니, 이전 주차에서 `lock_release()` 함수 끝에 추가한 `thread_yield()` 호출이 문제였습니다. 커널 초기화 과정에서 인터럽트가 비활성화된 상태에서 `thread_yield()`가 호출되면서 아직 초기화되지 않은 스레드의 `pml4`에 접근하는 것이 원인이었습니다.

이 문제를 해결하기 위해 `threads/synch.c`의 `lock_release()` 함수에서 인터럽트가 활성화된 경우에만 `thread_yield()`를 호출하도록 수정했습니다.

```C
curr->priority = max_priority;
if (intr_get_level() == INTR_ON)
	thread_yield();
```

이후 다시 테스트를 실행하니 "system call!"이라는 메시지가 출력되면서 프로그램이 종료되었습니다. 이는 아직 argument passing과 system call이 구현되지 않았기 때문이었습니다.

먼저 `userprog/process.c`의 `process_exec()` 함수에서 명령줄을 파싱하여 인자들을 분리하고, 스택에 x86-64 calling convention에 맞게 설정하도록 구현했습니다. [System V AMD64 ABI](https://wiki.osdev.org/System_V_ABI)에 따르면 함수의 첫 번째 인자는 `%rdi`, 두 번째 인자는 `%rsi` 레지스터에 전달됩니다. Pintos의 사용자 프로그램은 `_start()` 함수에서 `main(argc, argv)`를 호출하므로, `%rdi`에는 `argc`, `%rsi`에는 `argv` 배열의 주소를 설정해야 합니다. 또한 스택은 8바이트 단위로 정렬되어야 하고, `argv` 배열의 마지막에는 NULL 포인터가 있어야 합니다. (참고: [x86 Calling Conventions - Wikipedia](https://en.wikipedia.org/wiki/X86_calling_conventions#System_V_AMD64_ABI))

```C
char *argv[64];
int argc = 0;
char *token;
char *save_ptr;

token = strtok_r(file_name, " ", &save_ptr);
while (token != NULL) {
  argv[argc++] = token;
  token = strtok_r(NULL, " ", &save_ptr);
}

success = load(argv[0], &_if);

if (success) {
  char *argv_addrs[64];
  int i;

  for (i = argc - 1; i >= 0; i--) {
    int len = strlen(argv[i]) + 1;
    _if.rsp = _if.rsp - len;
    memcpy((void *)_if.rsp, argv[i], len);
    argv_addrs[i] = (char *)_if.rsp;
  }

  while (_if.rsp % 8 != 0)
    _if.rsp--;

  _if.rsp = _if.rsp - 8;
  *(char **)_if.rsp = 0;

  for (i = argc - 1; i >= 0; i--) {
    _if.rsp = _if.rsp - 8;
    memcpy((void *)_if.rsp, &argv_addrs[i], 8);
  }

  _if.R.rsi = _if.rsp;
  _if.R.rdi = argc;

  _if.rsp = _if.rsp - 8;
  *(void **)_if.rsp = 0;
}
```

그리고 `userprog/syscall.c`에서 `SYS_EXIT`와 `SYS_WRITE` system call을 간단히 구현했습니다.

```C
void syscall_handler(struct intr_frame *f) {
  int syscall_num = f->R.rax;

  if (syscall_num == SYS_EXIT) {
    thread_exit();
  } else if (syscall_num == SYS_WRITE) {
    int fd = f->R.rdi;
    char *buffer = (char *)f->R.rsi;
    int size = f->R.rdx;

    if (fd == 1) {
      putbuf(buffer, size);
      f->R.rax = size;
    }
  } else {
    printf("system call! %d\n", syscall_num);
    thread_exit();
  }
}
```

이제 테스트를 실행하니 출력은 정상적으로 나왔지만, 타임아웃이 발생했습니다. 이는 `process_wait()` 함수가 무한 루프로 되어 있어서 프로세스가 종료되어도 pintos가 종료되지 않았기 때문이었습니다.

이 문제를 해결하기 위해 `include/threads/thread.h`에 세마포어와 부모 스레드 포인터를 추가했습니다.

```C
struct semaphore wait_sema;
int exit_status;
struct thread *parent;
```

`threads/thread.c`의 `init_thread()`에서 세마포어를 초기화하고, `thread_create()`에서 부모를 설정하도록 했습니다. 그리고 `userprog/process.c`에서 `process_wait()`는 부모 스레드의 세마포어를 기다리도록, `process_exit()`는 부모의 세마포어를 up하도록 수정했습니다.

```C
int process_wait(tid_t child_tid UNUSED) {
  struct thread *curr = thread_current();
  sema_down(&curr->wait_sema);
  return curr->exit_status;
}

void process_exit(void) {
  struct thread *curr = thread_current();
  if (curr->pml4 != NULL)
    printf("%s: exit(%d)\n", curr->name, curr->exit_status);
  if (curr->parent != NULL)
    sema_up(&curr->parent->wait_sema);
  process_cleanup();
}
```

`pml4 != NULL` 조건은 사용자 프로세스인 경우에만 exit 메시지를 출력하기 위함입니다. 이렇게 하지 않으면 이전 주차의 threads 테스트에서도 exit 메시지가 출력되어 테스트가 실패합니다.

마지막으로 `args-none` 테스트를 통과했습니다.

#### args-single

`args-single` 테스트를 실행하니 다음과 같이 스레드 이름이 잘못 출력되었습니다.

```
args-single one: exit(0)
```

원래는 `args-single: exit(0)`가 출력되어야 합니다. 문제는 `process_create_initd()` 함수에서 `thread_create()`에 전체 명령줄을 스레드 이름으로 전달하고 있었기 때문이었습니다.

이를 해결하기 위해 스레드 이름으로는 첫 번째 단어(프로그램 이름)만 사용하도록 수정했습니다.

```C
char thread_name[16];
strlcpy(thread_name, file_name, sizeof thread_name);
char *space = strchr(thread_name, ' ');
if (space != NULL)
  *space = '\0';

tid = thread_create(thread_name, PRI_DEFAULT, initd, fn_copy);
```

이후 모든 args 테스트가 통과했습니다.

```
pass tests/userprog/args-none
pass tests/userprog/args-single
pass tests/userprog/args-multiple
pass tests/userprog/args-many
pass tests/userprog/args-dbl-space
```

### User Memory

#### bad-read

`bad-read` 테스트를 실행하니 출력이 acceptable output과 일치하지 않았습니다.

```
FAIL tests/userprog/bad-read
Test output failed to match any acceptable form.

Acceptable output:
  (bad-read) begin
  bad-read: exit(-1)
Differences in `diff -u' format:
  (bad-read) begin
+ Interrupt 0x0e (#PF Page-Fault Exception) at rip=4000f1
+ rax ...
  bad-read: exit(-1)
```

테스트 스크립트(`tests/tests.pm`)를 보면 `IGNORE_USER_FAULTS` 옵션으로 "유저 폴트 메시지"를 일부 무시해주는데, 해당 정규식이 x86(32-bit) 포맷(`eip/eax/...`) 기준으로 되어 있어서 현재 코드에서 출력되는 x86-64 포맷(`rip/rax/...`)의 레지스터 덤프는 필터링되지 않는 것이 원인이었습니다.

실제로 `tests/tests.pm`에서 무시하는 라인은 이런 형태였습니다.

```perl
@output = grep (!/^Page fault at.*in user context\.$/
                && !/: dying due to interrupt 0x0e \(.*\).$/
                && !/^Interrupt 0x0e \(.*\) at eip=/
                && !/^ cr2=.* error=.*/
                && !/^ eax=.* ebx=.* ecx=.* edx=.*/
                ...
                && !/^ cs=.* ds=.* es=.* ss=.*/, @output);
```

즉, page fault 메시지와 "dying" 메시지는 걸러지는데 레지스터 dump는 `eip/eax/...` 패턴만 잡고 있어서, 우리 커널이 출력하는 `rip/rax/...` 덤프가 그대로 남아 diff에 나타난 상황입니다.

따라서 사용자 모드에서 발생한 예외/페이지 폴트는 레지스터 덤프를 출력하지 않고, 조용히 `exit(-1)`로 종료하도록 수정했습니다.

`userprog/exception.c`의 `kill()`에서 user segment인 경우 바로 `exit_with_status(-1)`을 호출하도록 했고,

```C
case SEL_UCSEG:
  exit_with_status(-1);
```

`page_fault()`도 user context이면 동일하게 처리하도록 변경했습니다. (유저 폴트는 "유저가 잘못했으니 종료"가 기대 동작이고, 커널 쪽에서 디버그 로그까지 출력하는 것은 테스트 출력에 방해가 되었습니다.)

```C
if (user)
  exit_with_status(-1);
```

이후 아래 테스트들이 통과하는 것을 확인했습니다.

```
pass tests/userprog/bad-read
pass tests/userprog/bad-read2
pass tests/userprog/bad-write
pass tests/userprog/bad-write2
pass tests/userprog/bad-jump
pass tests/userprog/bad-jump2
```

#### exit

여기서 한 가지 더 중요한 점은, 유저 프로그램이 잘못된 접근으로 죽더라도 테스트는 `exit(-1)`을 기대한다는 것입니다. 즉, page fault handler에서 `thread_exit()`로만 끝내면 `exit_status`가 초기값(0)으로 남아 테스트가 실패하게 됩니다.

따라서 `SYS_EXIT` 뿐 아니라, 유저 예외로 종료되는 경로에서도 `exit_status`를 먼저 세팅한 뒤 `thread_exit()`로 빠져야 합니다.

`userprog/syscall.c`에 `sys_exit(status)`를 추가하고, x86-64 calling convention에 맞게 `rdi`에서 status를 읽어 `thread_current()->exit_status`에 저장한 뒤 종료하도록 수정했습니다.

```C
static void sys_exit(int status) {
  struct thread *curr = thread_current();
  curr->exit_status = status;
  thread_exit();
}
```

#### write-bad-fd

`write()`는 사용자 포인터(`buffer`)를 커널에서 역참조하게 되므로, 잘못된 주소가 들어오면 커널 페이지 폴트가 날 수 있습니다. 특히 `write()`는 `putbuf()`를 호출하면서 커널이 사용자 버퍼를 그대로 읽게 되므로, 유저가 넘긴 포인터를 먼저 검증해야 합니다.

이를 막기 위해 `userprog/syscall.c`에 `validate_user_buffer()`를 추가하고, 아래를 모두 만족하는지 확인한 뒤에만 접근하도록 했습니다.

- `buffer`가 user address space인지 (`is_user_vaddr()`)
- 현재 프로세스의 페이지 테이블에 매핑되어 있는지 (`pml4_get_page()`)
- `buffer..buffer+size` 범위가 페이지 경계를 넘을 수 있으니, 범위 전체가 걸치는 모든 페이지에 대해 위 검사를 수행하는지
- `start + size - 1` 계산에서 overflow가 나지 않는지

또한 디버그용으로 출력하던 `"system call!"` 메시지는 테스트 출력에 섞여 실패 원인이 될 수 있어서 제거하고, 아직 구현하지 않은 syscall은 `exit(-1)`로 처리하도록 했습니다.

`write-bad-fd` 테스트도 통과했습니다.

```
pass tests/userprog/write-bad-fd
```

### System Calls

이제부터는 본격적으로 system call을 구현했습니다. syscall 번호 분기/인자 처리의 중심은 `userprog/syscall.c`이고, fd 테이블과 종료 시 정리 같은 주변 처리를 위해 `include/threads/thread.h`, `threads/thread.c`, `userprog/process.c`도 함께 수정했습니다.

Pintos-KAIST는 x86-64의 `syscall` instruction을 사용하므로, syscall 번호는 `rax`, 인자들은 calling convention대로 `rdi/rsi/rdx/...` 레지스터에서 꺼내옵니다.

```C
/* userprog/syscall.c */
void syscall_handler(struct intr_frame *f) {
  int syscall_num = f->R.rax;
  ...
}
```

이번에는 분량이 큰 관계로 절반 정도(프로세스 종료 + 파일 관련 syscall)만 구현하고, `fork/exec/wait`는 다음 단계로 미뤘습니다.

또한 파일 시스템은 글로벌 상태를 많이 공유하므로, 여러 스레드가 동시에 syscall로 파일 시스템에 들어가면 race condition이 생길 수 있습니다. 그래서 `filesys_lock`을 두고 파일 관련 연산은 lock으로 감쌌습니다.

```C
/* userprog/syscall.c */
static struct lock filesys_lock;

void syscall_init(void) {
  lock_init(&filesys_lock);
  ...
}
```

#### exit

`SYS_EXIT`에서 `rdi`로 넘어오는 status를 `thread_current()->exit_status`에 저장한 뒤 `thread_exit()`로 종료하도록 구현했습니다.

```C
/* userprog/syscall.c */
static void sys_exit(int status) NO_RETURN;
static void sys_exit(int status) {
  struct thread *curr = thread_current();
  curr->exit_status = status;
  thread_exit();
}
```

```C
/* userprog/syscall.c */
case SYS_EXIT:
  sys_exit((int)f->R.rdi);
```

아래 테스트를 통과했습니다.

```
pass tests/userprog/exit
```

#### halt

`SYS_HALT`는 `power_off()`를 호출하도록 구현했습니다.

```C
/* userprog/syscall.c */
case SYS_HALT:
  power_off();
```

```
pass tests/userprog/halt
```

#### create-null

파일 관련 syscall은 거의 전부 유저 문자열 포인터를 받습니다. (예: `create(const char *file, ...)`, `open(const char *file)`)

파일 이름 포인터가 NULL이거나, user 영역이 아니거나, 매핑되지 않은 주소면 커널에서 문자열을 읽는 순간 커널 페이지 폴트가 날 수 있습니다. 따라서 "유저 문자열을 커널로 복사하면서 검증"하는 루틴이 필요합니다.

그래서 `copy_in_string()`으로 유저 문자열을 커널 페이지로 복사하면서(바이트 단위로) 매 주소를 검증하고, 실패 시 `exit(-1)`로 처리하도록 했습니다.

주의할 점은, 문자열 길이에 제한이 없으면 유저가 `'\0'`을 끝까지 안 주고 페이지를 계속 넘겨서 커널이 무한히 읽게 만들 수 있다는 것입니다. 그래서 한 페이지(`PGSIZE`)까지만 복사하고, 그 안에 `'\0'`이 없으면 실패 처리했습니다.

```C
/* userprog/syscall.c */
static void validate_user_address(const void *uaddr) {
  struct thread *curr = thread_current();
  if (uaddr == NULL || !is_user_vaddr(uaddr))
    sys_exit(-1);
  if (curr->pml4 == NULL || pml4_get_page(curr->pml4, uaddr) == NULL)
    sys_exit(-1);
}

static char *copy_in_string(const char *ustr) {
  if (ustr == NULL)
    sys_exit(-1);

  struct thread *curr = thread_current();
  char *kstr = palloc_get_page(0);
  if (kstr == NULL)
    sys_exit(-1);

  for (size_t i = 0; i < PGSIZE; i++) {
    const void *uaddr = (const void *)(ustr + i);
    validate_user_address(uaddr);

    const char *kaddr = pml4_get_page(curr->pml4, uaddr);
    kstr[i] = *kaddr;
    if (kstr[i] == '\0')
      return kstr;
  }

  palloc_free_page(kstr);
  sys_exit(-1);
}
```

```
pass tests/userprog/create-null
```

#### create-normal

`SYS_CREATE`는 `filesys_create(file, initial_size)`를 호출해 파일을 만들고, 성공 여부를 `rax`로 반환하도록 구현했습니다.

```C
/* userprog/syscall.c */
case SYS_CREATE: {
  char *file = copy_in_string((const char *)f->R.rdi);
  unsigned initial_size = (unsigned)f->R.rsi;

  lock_acquire(&filesys_lock);
  bool ok = filesys_create(file, initial_size);
  lock_release(&filesys_lock);

  palloc_free_page(file);
  f->R.rax = ok;
  return;
}
```

```
pass tests/userprog/create-normal
pass tests/userprog/create-empty
pass tests/userprog/create-exists
pass tests/userprog/create-long
```

#### open-normal

`SYS_OPEN`을 구현하면서 프로세스별 파일 디스크립터 테이블이 필요했습니다.

`struct thread`에 `fd_table[128]`/`next_fd`를 추가해서 간단한 fd 테이블을 만들었습니다.

- `0`/`1`은 stdin/stdout으로 예약되어 있으니 일반 파일은 `2`부터 할당했습니다.
- `SYS_OPEN`에서 `filesys_open()`으로 `struct file *`을 얻고, 빈 슬롯을 찾아 fd를 할당해서 반환했습니다.
- fd가 꽉 찬 경우엔 `file_close()` 후 `-1`을 반환합니다.

```C
/* include/threads/thread.h */
struct thread {
  ...
#ifdef USERPROG
  struct file *fd_table[128];
  int next_fd;
#endif
  ...
};
```

```C
/* threads/thread.c */
static void init_thread(struct thread *t, const char *name, int priority) {
  ...
#ifdef USERPROG
  t->next_fd = 2;
#endif
  ...
}
```

```C
/* userprog/syscall.c */
static int allocate_fd(struct file *file) {
  struct thread *curr = thread_current();
  for (int fd = curr->next_fd; fd < 128; fd++) {
    if (curr->fd_table[fd] == NULL) {
      curr->fd_table[fd] = file;
      curr->next_fd = fd + 1;
      return fd;
    }
  }
  for (int fd = 2; fd < curr->next_fd; fd++) {
    if (curr->fd_table[fd] == NULL) {
      curr->fd_table[fd] = file;
      return fd;
    }
  }
  return -1;
}
```

```C
/* userprog/syscall.c */
case SYS_OPEN: {
  char *file_name = copy_in_string((const char *)f->R.rdi);

  lock_acquire(&filesys_lock);
  struct file *file = filesys_open(file_name);
  lock_release(&filesys_lock);

  palloc_free_page(file_name);
  if (file == NULL) {
    f->R.rax = -1;
    return;
  }

  int fd = allocate_fd(file);
  if (fd < 0) {
    file_close(file);
    f->R.rax = -1;
    return;
  }
  f->R.rax = fd;
  return;
}
```

```
pass tests/userprog/open-normal
pass tests/userprog/close-normal
pass tests/userprog/close-twice
pass tests/userprog/close-bad-fd
```

#### read-normal

`SYS_READ`는 `fd == 0`이면 `input_getc()`로 stdin을 읽고, 파일 fd면 `file_read()`를 호출하도록 했습니다.

특히 `read()`는 유저 버퍼에 쓰기를 해야하므로, `validate_user_writable_buffer()`로 아래 조건을 확인한 뒤에만 진행하도록 했습니다.

- user address space인지
- 매핑되어 있는지
- writable인지 (PTE_W)

또한 `buffer`가 페이지 경계를 넘을 수 있으므로, `file_read()`/`file_write()` 호출도 페이지 단위로 잘라서 처리하도록 구현했습니다. (한 번에 큰 덩어리로 읽다가 페이지 경계를 넘어가면, 중간에 접근 실패로 커널이 죽을 수 있습니다.)

```C
/* userprog/syscall.c */
static void validate_user_writable_buffer(void *buffer, size_t size) {
  if (size == 0)
    return;

  validate_user_buffer(buffer, size);

  struct thread *curr = thread_current();
  uintptr_t start = (uintptr_t)buffer;
  uintptr_t end = start + size - 1;

  for (uintptr_t page = (uintptr_t)pg_round_down((const void *)start);
       page <= (uintptr_t)pg_round_down((const void *)end); page += PGSIZE) {
    uint64_t *pte = pml4e_walk(curr->pml4, page, 0);
    if (pte == NULL || ((*pte & PTE_P) == 0) || !is_user_pte(pte) ||
        !is_writable(pte))
      sys_exit(-1);
  }
}
```

```C
/* userprog/syscall.c */
case SYS_READ: {
  int fd = (int)f->R.rdi;
  void *buffer = (void *)f->R.rsi;
  unsigned size = (unsigned)f->R.rdx;

  validate_user_writable_buffer(buffer, size);

  if (fd == 0) {
    for (unsigned i = 0; i < size; i++) {
      uint8_t *dst = pml4_get_page(thread_current()->pml4,
                                   (const void *)((uint8_t *)buffer + i));
      *dst = input_getc();
    }
    f->R.rax = (int)size;
    return;
  }
  if (fd == 1) {
    f->R.rax = -1;
    return;
  }

  struct file *file = get_file(fd);
  if (file == NULL) {
    f->R.rax = -1;
    return;
  }

  off_t bytes_read = 0;
  lock_acquire(&filesys_lock);
  while (size > 0) {
    size_t page_left = PGSIZE - pg_ofs(buffer);
    size_t chunk = size < page_left ? size : page_left;
    void *kaddr = pml4_get_page(thread_current()->pml4, buffer);
    if (kaddr == NULL) {
      lock_release(&filesys_lock);
      sys_exit(-1);
    }

    off_t n = file_read(file, kaddr, (off_t)chunk);
    if (n <= 0)
      break;
    bytes_read += n;
    buffer = (uint8_t *)buffer + n;
    size -= (unsigned)n;
    if ((size_t)n < chunk)
      break;
  }
  lock_release(&filesys_lock);
  f->R.rax = (int)bytes_read;
  return;
}
```

```
pass tests/userprog/read-normal
pass tests/userprog/read-bad-ptr
pass tests/userprog/read-bad-fd
pass tests/userprog/read-stdout
pass tests/userprog/read-boundary
```

#### write-normal

`SYS_WRITE`는 `fd == 1`이면 `putbuf()`로 stdout에 출력하고, 파일 fd면 `file_write()`를 호출하도록 했습니다. (stdin으로 쓰기는 `-1` 반환)

`write()`는 유저 버퍼를 읽기만 하므로 writable까지는 요구하지 않고, `validate_user_buffer()`로 매핑 여부만 확인했습니다.

```C
/* userprog/syscall.c */
case SYS_WRITE: {
  int fd = (int)f->R.rdi;
  const void *buffer = (const void *)f->R.rsi;
  unsigned size = (unsigned)f->R.rdx;

  validate_user_buffer(buffer, size);

  if (fd == 1) {
    putbuf(buffer, size);
    f->R.rax = (int)size;
    return;
  }
  if (fd == 0) {
    f->R.rax = -1;
    return;
  }

  struct file *file = get_file(fd);
  if (file == NULL) {
    f->R.rax = -1;
    return;
  }

  off_t bytes_written = 0;
  lock_acquire(&filesys_lock);
  while (size > 0) {
    size_t page_left = PGSIZE - pg_ofs(buffer);
    size_t chunk = size < page_left ? size : page_left;
    const void *kaddr = pml4_get_page(thread_current()->pml4, buffer);
    if (kaddr == NULL) {
      lock_release(&filesys_lock);
      sys_exit(-1);
    }

    off_t n = file_write(file, kaddr, (off_t)chunk);
    if (n <= 0)
      break;
    bytes_written += n;
    buffer = (const uint8_t *)buffer + n;
    size -= (unsigned)n;
    if ((size_t)n < chunk)
      break;
  }
  lock_release(&filesys_lock);
  f->R.rax = (int)bytes_written;
  return;
}
```

```
pass tests/userprog/write-normal
pass tests/userprog/write-bad-ptr
pass tests/userprog/write-bad-fd
pass tests/userprog/write-stdin
pass tests/userprog/write-boundary
```

#### create-bound

문자열이 페이지 경계에 걸쳐있는 케이스도 `copy_in_string()`이 바이트 단위로 검증/복사하므로 통과했습니다. 즉, `file_name`이 어떤 페이지의 끝에 걸려 있더라도, 다음 페이지가 매핑되어 있고 접근 가능하면 계속 복사됩니다.

```
pass tests/userprog/create-bound
pass tests/userprog/open-boundary
```

추가로, `SYS_CLOSE`는 fd 테이블에서 엔트리를 제거하고 `file_close()`를 호출하도록 구현했고, 프로세스 종료 시에도 열린 파일을 정리하도록 `process_exit()`에서 전부 닫아주었습니다.

```C
/* userprog/syscall.c */
static void close_fd(int fd) {
  struct thread *curr = thread_current();
  if (fd < 2 || fd >= 128)
    return;
  if (curr->fd_table[fd] == NULL)
    return;
  file_close(curr->fd_table[fd]);
  curr->fd_table[fd] = NULL;
  if (fd < curr->next_fd)
    curr->next_fd = fd;
}

case SYS_CLOSE: {
  int fd = (int)f->R.rdi;
  lock_acquire(&filesys_lock);
  close_fd(fd);
  lock_release(&filesys_lock);
  return;
}
```

```C
/* userprog/process.c */
for (int fd = 2; fd < 128; fd++) {
  if (curr->fd_table[fd] != NULL) {
    file_close(curr->fd_table[fd]);
    curr->fd_table[fd] = NULL;
  }
}
```
