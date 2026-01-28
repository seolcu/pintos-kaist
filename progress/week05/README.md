# 5주차 진행기록

## PROJECT 3: VIRTUAL MEMORY

### Stack Growth

#### 요구사항 정리

Project 2에서는 `USER_STACK`에 1페이지짜리 스택만 두고 시작했는데, 이제 스택이 더 내려가면 필요한 만큼 페이지를 추가로 할당해야 합니다.

- 스택 접근처럼 보이는 page fault일 때만 스택을 늘려야 함
- x86-64 `push`는 접근 권한을 먼저 체크해서 `rsp`를 내리기 전에 `rsp-8`에서 page fault가 날 수 있음
- `vm_try_handle_fault()`에서 stack growth 케이스를 식별하고, 맞으면 `vm_stack_growth()`를 호출해야 함
- 스택 최대 크기는 1MB로 제한
- syscall 중 커널 모드에서 page fault가 나면 `intr_frame->rsp`가 유효하지 않을 수 있으니, 유저 `rsp`를 다른 곳에 저장해 두는 방식이 필요함

처음 `vm/vm.c`를 확인해보면 stack growth 관련 로직이 비어있었습니다. 즉, SPT에 이미 있는 page만 처리하고, 스택이 내려가서 아직 SPT에 없는 주소로 접근하는 케이스는 그대로 `false`로 빠져서 프로세스가 `exit(-1)`로 죽는 상황입니다.

#### make check 결과

Ubuntu 18.04 distrobox에서 `vm/build`에서 `make check`를 돌렸습니다.

Stack Growth 관련으로 바로 확인되는 실패는 아래 3개였습니다.

```
FAIL tests/vm/pt-grow-stack
FAIL tests/vm/pt-big-stk-obj
FAIL tests/vm/pt-grow-stk-sc
```

`pt-grow-stack.output`를 보면 begin 이후 바로 `exit(-1)`로 종료되고, page fault가 1회 발생한 상태였습니다.

```
(pt-grow-stack) begin
pt-grow-stack: exit(-1)
...
Exception: 1 page faults
```

`pt-big-stk-obj`, `pt-grow-stk-sc`도 동일하게 begin 직후 `exit(-1)` + page fault 1회 패턴이었습니다.

#### 수정 사항

`vm/vm.c`에서 아래 2가지를 추가 구현했습니다.

1. `vm_try_handle_fault()`에서 `spt_find_page()`가 NULL일 때 stack growth로 볼 수 있는지 heuristic을 추가했습니다.

```C
uint8_t *rsp = (uint8_t *) f->rsp;
uint8_t *uaddr = (uint8_t *) addr;

bool is_stack =
	not_present &&
	uaddr < (uint8_t *) USER_STACK &&
	uaddr >= (uint8_t *) (USER_STACK - (1 << 20)) &&
	uaddr >= rsp - 8;

page = spt_find_page (spt, addr);
if (page == NULL && is_stack) {
	vm_stack_growth (addr);
	page = spt_find_page (spt, addr);
}
```

2. `vm_stack_growth()`에서 `pg_round_down(addr)`부터 `USER_STACK` 방향으로 익명 페이지를 할당/claim 하도록 구현했습니다.

```C
static void
vm_stack_growth (void *addr) {
	uint8_t *va = pg_round_down (addr);
	uint8_t *limit = (uint8_t *) USER_STACK - (1 << 20);

	if (va < limit)
		return;

	for (uint8_t *p = va; p < (uint8_t *) USER_STACK; p += PGSIZE) {
		if (spt_find_page (&thread_current ()->spt, p) != NULL)
			break;
		if (!vm_alloc_page (VM_ANON, p, true))
			return;
		if (!vm_claim_page (p))
			return;
	}
}
```

#### 수정 후 테스트 결과

전체 `make check` 대신, Stack Growth에 해당하는 테스트 3개만 골라서 다시 실행했습니다.

```
PASS tests/vm/pt-grow-stack
PASS tests/vm/pt-big-stk-obj
PASS tests/vm/pt-grow-stk-sc
```

---

### Memory Mapped Files

#### 요구사항 정리

- `mmap(addr, length, writable, fd, offset)`
  - 성공 시 `addr`를 리턴, 실패 시 `NULL`/`MAP_FAILED`
  - `addr`는 page-aligned여야 하고, `addr == 0`이면 실패
  - `length == 0`이면 실패
  - mapping이 기존 매핑(스택/코드/데이터/기존 mmap 포함)과 겹치면 실패
  - file-backed page는 lazy하게 로딩
  - 마지막 page에서 파일 길이를 넘어가는 부분은 0으로 채움
- `munmap(addr)`
  - mmap이 리턴했던 시작 주소로만 unmap
  - dirty한 페이지는 파일에 writeback
  - 프로세스 종료 시에도 자동으로 munmap 처리가 되어야 함
  - `file_reopen()`으로 mapping마다 독립적인 file reference를 가져야 함

#### 테스트 결과

```
FAIL tests/vm/mmap-read
FAIL tests/vm/mmap-null
pass tests/vm/mmap-zero-len
FAIL tests/vm/mmap-misalign
FAIL tests/vm/mmap-write
```

먼저 `mmap-read`는 다음과 같이 `(mmap-read) end`가 출력되지 않고 끝났습니다.

```
FAIL tests/vm/mmap-read
Test output failed to match any acceptable form.

Acceptable output:
  (mmap-read) begin
  (mmap-read) open "sample.txt"
  (mmap-read) mmap "sample.txt"
  (mmap-read) end
Differences in `diff -u' format:
  (mmap-read) begin
  (mmap-read) open "sample.txt"
  (mmap-read) mmap "sample.txt"
- (mmap-read) end
```

`mmap-null`, `mmap-misalign`도 동일하게 `try to mmap ...` 메시지까지 찍고 `end` 없이 종료되었습니다.

특이하게 `mmap-zero-len`만 PASS였는데, 이 테스트는 `length == 0`인 mmap이 `MAP_FAILED`로 실패해야 하므로(= 성공하면 안 됨) 현재 구현/동작이 우연히 통과 조건을 만족한 것으로 보입니다.

#### 원인 추정

현재 `userprog/syscall.c`에는 `SYS_MMAP`, `SYS_MUNMAP` 처리가 없습니다. 그래서 `mmap()` 호출이 들어오면 syscall handler의 `default:`로 떨어져서 `sys_exit(-1)`로 프로세스가 바로 종료되는 것으로 보입니다.

```C
/* userprog/syscall.c */
switch (syscall_num) {
  ...
  case SYS_CLOSE:
    ...
    return;

  default:
    sys_exit(-1);
}
```

또한 `vm/file.c`의 file-backed page와 `do_mmap/do_munmap`도 전부 비어있습니다.

```C
/* vm/file.c */
void *
do_mmap (void *addr, size_t length, int writable,
         struct file *file, off_t offset) {
}

void
do_munmap (void *addr) {
}
```

즉, 지금 단계에서 해야 할 것은:

1. `userprog/syscall.c`에 `SYS_MMAP`, `SYS_MUNMAP` 케이스 추가
2. `vm/file.c`에 file-backed page 구현 + `do_mmap/do_munmap` 구현

#### 수정 사항

1. `userprog/syscall.c`에 `SYS_MMAP`, `SYS_MUNMAP`을 추가했습니다. `syscall5` 호출 규약에 맞게 `r10`, `r8` 레지스터도 사용합니다.

```C
case SYS_MMAP: {
  void *addr = (void *) f->R.rdi;
  size_t length = (size_t) f->R.rsi;
  int writable = (int) f->R.rdx;
  int fd = (int) f->R.r10;
  off_t offset = (off_t) f->R.r8;

  struct file *file = get_file (fd);
  if (file == NULL) {
    f->R.rax = (uint64_t) NULL;
    return;
  }
  f->R.rax = (uint64_t) do_mmap (addr, length, writable, file, offset);
  return;
}

case SYS_MUNMAP: {
  void *addr = (void *) f->R.rdi;
  do_munmap (addr);
  return;
}
```

2. `vm/file.c`에서 file-backed page와 `do_mmap/do_munmap`을 구현했습니다.

`do_mmap()`은 인자 검증(정렬/길이/offset/겹침)을 하고, mapping마다 `file_reopen()`으로 독립적인 file reference를 만든 다음, 각 페이지를 `vm_alloc_page_with_initializer(VM_FILE, ...)`로 SPT에 lazy하게 등록합니다.

```C
/* vm/file.c */
if (addr == NULL)
  return NULL;
if (pg_ofs (addr) != 0)
  return NULL;
if (length == 0)
  return NULL;
if (offset < 0 || pg_ofs (offset) != 0)
  return NULL;

/* overlap check */
for (size_t i = 0; i < page_cnt; i++) {
  void *va = (uint8_t *) addr + i * PGSIZE;
  if (spt_find_page (spt, va) != NULL)
    return NULL;
  if (pml4_get_page (t->pml4, va) != NULL)
    return NULL;
}

struct mmap_region *mr = malloc (sizeof *mr);
*mr = (struct mmap_region) {
  .addr = addr,
  .length = length,
  .page_cnt = page_cnt,
  .file = file_reopen (file),
  .offset = offset,
  .writable = writable != 0,
};
list_push_back (&t->mmap_list, &mr->elem);

/* lazy page allocation */
if (!vm_alloc_page_with_initializer (VM_FILE, va, writable != 0,
    lazy_load_mmap, aux)) {
  ...
}
```

`lazy_load_mmap()`에서 fault 시 `file_read_at()`로 내용을 채우고, 마지막 page의 나머지는 0으로 채웁니다.

```C
/* vm/file.c */
if (file_read_at (file_page->file, page->frame->kva, file_page->read_bytes,
    file_page->ofs) != (int) file_page->read_bytes)
  return false;
memset ((uint8_t *) page->frame->kva + file_page->read_bytes, 0,
    file_page->zero_bytes);
```

`file_backed_destroy()`는 dirty 페이지면 writeback 하고, pml4 매핑과 frame을 해제합니다.

```C
/* vm/file.c */
if (page->frame != NULL && page->writable && pml4_is_dirty (t->pml4, page->va)) {
  file_write_at (file_page->file, page->frame->kva,
      file_page->read_bytes, file_page->ofs);
  pml4_set_dirty (t->pml4, page->va, false);
}
if (page->frame != NULL) {
  pml4_clear_page (t->pml4, page->va);
  palloc_free_page (page->frame->kva);
  free (page->frame);
  page->frame = NULL;
}
```

`do_munmap()`은 mmap 당시 저장해둔 region 정보(`mmap_list`)를 찾은 뒤, 해당 범위의 페이지들을 `spt_remove_page()`로 제거하고 mapping의 file을 close합니다.

```C
/* vm/file.c */
for (size_t i = 0; i < mr->page_cnt; i++) {
  void *va = (uint8_t *) mr->addr + i * PGSIZE;
  struct page *page = spt_find_page (spt, va);
  if (page != NULL)
    spt_remove_page (spt, page);
}
list_remove (&mr->elem);
file_close (mr->file);
free (mr);
```

3. 프로세스 종료 시에도 자동으로 unmap 되도록 `supplemental_page_table_kill()`에서 `mmap_list`를 비울 때까지 `do_munmap()`을 호출하도록 했습니다.

```C
/* vm/vm.c */
while (!list_empty (&thread_current ()->mmap_list)) {
  struct mmap_region *mr = list_entry (list_front (&thread_current ()->mmap_list),
                                      struct mmap_region, elem);
  do_munmap (mr->addr);
}
```

4. fork 시 mmap이 상속되지 않도록(테스트 `mmap-inherit` 요구사항) `supplemental_page_table_copy()`에서 `VM_FILE` 페이지는 skip하도록 했습니다.

```C
/* vm/vm.c */
enum vm_type type = page_get_type (src_page);

/* Memory-mapped file pages are not inherited. */
if (type == VM_FILE)
  continue;
```

#### 수정 후 테스트 결과

mmap 관련 테스트 일부를 다시 돌려서 확인했습니다.

```
pass tests/vm/mmap-null
pass tests/vm/mmap-misalign
pass tests/vm/mmap-read
pass tests/vm/mmap-write
pass tests/vm/mmap-unmap
pass tests/vm/mmap-clean
pass tests/vm/mmap-close
pass tests/vm/mmap-off
pass tests/vm/mmap-overlap
```

---

### Swap In/Out

#### 요구사항 정리

물리 메모리가 부족할 때 frame을 eviction 해서

- anonymous page는 swap disk로 내보냈다가(swap out) 다시 가져오고(swap in)
- file-backed page는 dirty면 파일에 writeback 한 뒤 매핑을 끊고(evict)

다시 접근 시 page fault를 통해 복구할 수 있어야 합니다.

핵심은 2가지입니다.

1. swap disk + swap table(bitmap)로 swap slot을 관리
2. frame table + eviction policy로 palloc이 실패할 때 victim frame을 골라 swap out

#### 테스트 결과

먼저 swap 관련 테스트를 골라서 실행했습니다.

```
pass tests/vm/swap-anon
pass tests/vm/swap-file
pass tests/vm/swap-iter
pass tests/vm/swap-fork
```

추가로, swap과 연동되는 `page-merge-par`, `page-merge-mm`은 처음에 FAIL이 났습니다.

```
FAIL tests/vm/page-merge-par
FAIL tests/vm/page-merge-mm
```

`page-merge-par.output`를 보면 child 프로세스들이 `exec("child-sort ...")`로 들어갈 때 간헐적으로 executable open이 실패했습니다.

```
load: child-sort: open failed
child-sort: exit(-1)
...
(page-merge-par) wait for child 1: FAILED
```

#### 수정 사항

1. swap disk와 swap table(bitmap) 초기화 (`vm/anon.c`)

swap disk는 `pintos` 실행 시 drive index 3으로 붙고, Pintos에서는 `hd1:1`로 접근 가능합니다.

```C
/* vm/anon.c */
swap_disk = disk_get (1, 1);
size_t sectors_per_page = PGSIZE / DISK_SECTOR_SIZE;
size_t slot_cnt = disk_size (swap_disk) / sectors_per_page;
swap_table = bitmap_create (slot_cnt);
bitmap_set_all (swap_table, false);
```

2. anon page swap in/out 구현 (`vm/anon.c`)

swap out 시 bitmap에서 빈 slot을 잡고(frame 내용을 disk로 write), swap in 시 disk에서 다시 read한 뒤 slot을 반납합니다.

```C
/* vm/anon.c */
size_t slot = bitmap_scan_and_flip (swap_table, 0, 1, false);
disk_sector_t base = (disk_sector_t) (slot * sectors_per_page);
for (size_t i = 0; i < sectors_per_page; i++)
  disk_write (swap_disk, base + i, (uint8_t *) page->frame->kva + i * DISK_SECTOR_SIZE);

/* swap in */
disk_sector_t base = (disk_sector_t) (anon_page->slot * sectors_per_page);
for (size_t i = 0; i < sectors_per_page; i++)
  disk_read (swap_disk, base + i, (uint8_t *) kva + i * DISK_SECTOR_SIZE);
bitmap_reset (swap_table, anon_page->slot);
anon_page->slot = BITMAP_ERROR;
```

3. frame table + eviction policy(clock) 구현 (`vm/vm.c`, `include/vm/vm.h`)

`vm_get_frame()`에서 user pool이 부족하면 eviction을 수행하도록 했습니다.

```C
/* vm/vm.c */
void *kva = palloc_get_page (PAL_USER);
if (kva == NULL)
  frame = vm_evict_frame();
```

victim 선택은 accessed bit를 기반으로 한 simple clock 방식으로 구현했습니다.

```C
/* vm/vm.c */
if (pml4_is_accessed (pml4, p->va)) {
  pml4_set_accessed (pml4, p->va, false);
  continue;
}
```

4. page-merge-par/mm 실패 수정: `filesys_open()` 동기화

여러 child가 동시에 `exec()`로 들어가면 `load()` 내부에서 `filesys_open()`이 동시에 호출되는데, Pintos file system은 thread-safe가 아니라서 간헐적으로 open 실패가 발생했습니다.

그래서 `filesys_lock`을 전역 lock으로 만들고(`userprog/syscall.c`), `load()`/`lazy_load_segment()`에서 파일 I/O 구간을 lock으로 감쌌습니다.

```C
/* userprog/process.c */
lock_acquire (&filesys_lock);
file = filesys_open (file_name);
lock_release (&filesys_lock);

lock_acquire (&filesys_lock);
int n = file_read_at (file, page->frame->kva, read_bytes, ofs);
lock_release (&filesys_lock);
```

#### 수정 후 테스트 결과

```
pass tests/vm/swap-anon
pass tests/vm/swap-file
pass tests/vm/swap-iter
pass tests/vm/swap-fork
pass tests/vm/page-merge-par
pass tests/vm/page-merge-mm
```
