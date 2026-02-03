# 6주차 진행기록

## PROJECT 3: VIRTUAL MEMORY

### Copy-on-Write (Extra)

#### 요구사항 정리

- fork 시점에 부모/자식이 물리 페이지를 복사하지 않고, 같은 frame을 공유하도록 함
- 대신 write-protect(read-only)로 걸어두고, 처음 write가 발생할 때만 해당 프로세스가 새 frame을 할당해서 내용을 복사
- write-protected 페이지도 eviction 대상이 될 수 있음

#### cow-simple 분석

`tests/vm/cow/cow-simple.c`는 fork 직후(아직 write 전) 부모/자식이 동일한 물리 주소를 보고 있어야 한다는 걸 확인합니다.

```c
pa_parent = get_phys_addr((void*)large);
child = fork("child");
...
pa_child = get_phys_addr((void*)large);
CHECK (pa_parent == pa_child, "two phys addrs should be the same.");

large[0] = '@';
pa_child = get_phys_addr((void*)large);
CHECK (pa_parent != pa_child, "two phys addrs should not be the same.");
```

현재 상태에서는 fork 시 `supplemental_page_table_copy()`가 frame을 새로 잡고 `memcpy()`로 내용을 복사하는 방식이라,
fork 직후부터 물리 주소가 달라져서 첫 번째 CHECK에서 실패하는 것으로 보입니다.

즉, fork에서 해야 할 일은 대략 아래 흐름입니다.

1. (이미 claim된 페이지라면) 부모/자식이 같은 frame을 가리키게 만들고
2. 부모/자식의 PTE에서 PTE_W를 내려서(read-only로) write fault가 나도록 만들고
3. page fault 핸들러에서 present + write fault를 COW 케이스로 처리해서
   - 새 frame 할당
   - 기존 frame 내용 복사
   - 내 프로세스의 매핑만 새 frame으로 교체 + writable 복구

#### COW 구현

기존 코드는 frame이 `frame->page`로 1:1 관계라고 가정하고 있어서, fork 시 같은 frame 공유를 표현할 수 없었습니다.
그래서 COW를 위해 VM 쪽 구조를 조금 갈아엎었습니다.

##### 1) frame을 여러 page가 공유하도록 변경

`include/vm/vm.h`에서 `struct frame`이 여러 page를 가질 수 있도록 `pages` 리스트를 추가하고,
page가 frame의 리스트에 들어갈 수 있도록 `frame_elem`을 추가했습니다. COW 상태를 표현하기 위해 `cow` 플래그도 추가했습니다.

```C
struct page {
  ...
  bool writable;
  bool cow;
  struct list_elem frame_elem;
  ...
};

struct frame {
  void *kva;
  struct list pages;
  struct list_elem elem;
};
```

그에 맞춰 `vm_get_frame()`/`vm_get_victim()`/`vm_evict_frame()`도 frame 단위로 동작하도록 바꿨습니다.
특히 eviction에서는 victim frame의 pages 리스트를 전부 `swap_out()`으로 비우고, 비워진 frame을 재사용하도록 했습니다.

##### 2) fork에서 eager copy 대신 공유 + write-protect

핵심은 `supplemental_page_table_copy()`입니다.

- src_page가 이미 frame을 가지고 있으면(dst에 새 frame을 만들지 않고)
  - dst_page가 src_page와 같은 frame을 가리키도록 붙이고
  - child의 매핑은 read-only로 깔고
  - writable 페이지는 parent의 PTE도 write bit를 내려서 둘 다 write fault가 나도록 만들었습니다.

또 한 가지 실수 포인트가 있었는데, `vm_alloc_page()`로 만든 dst_page는 기본이 `VM_UNINIT` 상태라 destroy/swap_out 경로가 깨집니다.
그래서 공유하는 경우에는 dst_page를 src_page와 같은 concrete type(anon/file)로 변환해주었습니다.

```C
dst_page->operations = src_page->operations;
switch (VM_TYPE(src_page->operations->type)) {
case VM_ANON:
  dst_page->anon = src_page->anon;
  break;
case VM_FILE:
  dst_page->file = src_page->file;
  break;
}
```

##### 3) page fault에서 COW(write-protected) 처리

기존 `vm_try_handle_fault()`는 not-present fault만 처리하고(`if (!not_present) return false;`) rights-violation fault를 전부 거절하고 있었는데,
COW는 대표적으로 present + write fault로 들어오기 때문에 이 경로를 새로 추가했습니다.

```C
if (not_present) {
  ...
  return vm_do_claim_page(page);
}

if (write)
  return vm_handle_wp(page);
return false;
```

`vm_handle_wp()`에서는 공유 refcnt를 보고,

- refcnt == 1: 그냥 PTE_W만 다시 켜고 cow 해제
- refcnt > 1: 새 frame을 할당하고 `memcpy`로 복사한 뒤, 현재 프로세스의 매핑만 새 frame으로 교체

하도록 구현했습니다.

##### 4) syscall의 writable buffer 검증과 COW

read 같은 syscall은 커널이 user buffer에 쓰게 됩니다.
그런데 COW 페이지는 PTE_W가 꺼져있기 때문에, 기존 `validate_user_writable_buffer()`가 그대로면
COW로 정상 처리되어야 하는 write도 syscall 진입 단계에서 `exit(-1)`로 죽게 됩니다.

그래서 pte가 non-writable이더라도 SPT에서 해당 페이지가 `page->writable == true`이면 통과시키도록 수정했습니다.

#### 결과

`cow-simple`이 통과했고, 전체 테스트도 다시 확인했습니다.

```
pass tests/vm/cow/cow-simple
All 141 tests passed.
```
