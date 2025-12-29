# 1주차 진행기록

## INTRODUCTION

우선 [첫 페이지](https://casys-kaist.github.io/pintos-kaist/introduction/getting_started.html)의 안내에 따라 [비공개 fork](https://github.com/seolcu/pintos-kaist)를 생성했습니다.

또한 `pintos` 명령어 사용을 위해 `~/.bashrc`에 활성화 명령어를 추가했습니다.

```bash
echo "source $(pwd)/activate" >> ~/.bashrc
```

그리고 `Building Pintos`에 나와있듯 빌드를 진행해봤습니다.

```bash
cd threads/
make
```

그랬더니 아래와 같이 컴파일 에러가 발생했습니다.

```
In file included from ../../threads/mmu.c:5:
../../threads/mmu.c: In function ‘pgdir_destroy’:
../../include/threads/pte.h:29:41: error: passing argument 1 of ‘pt_destroy’ makes pointer from integer without a cast [-Wint-conversion]
   29 | #define PTE_ADDR(pte) ((uint64_t) (pte) & ~0xFFF)
      |                       ~~~~~~~~~~~~~~~~~~^~~~~~~~~
      |                                         |
      |                                         long long unsigned int
../../threads/mmu.c:173:37: note: in expansion of macro ‘PTE_ADDR’
  173 |                         pt_destroy (PTE_ADDR (pte));
      |                                     ^~~~~~~~
../../threads/mmu.c:159:23: note: expected ‘uint64_t *’ {aka ‘long long unsigned int *’} but argument is of type ‘long long unsigned int’
  159 | pt_destroy (uint64_t *pt) {
      |             ~~~~~~~~~~^~
make[1]: *** [../../Make.config:33: threads/mmu.o] 오류 1
make[1]: 디렉터리 '/home/seolcu/문서/코드/pintos-kaist/threads/build' 나감
make: *** [../Makefile.kernel:10: all] 오류 2
```

제 시스템(Fedora)의 gcc 버전은 `15.2.1`이고, 페이지에서 사용되었다고 명시된 시스템은 `Ubuntu 16.04.6 LTS with gcc (gcc (Ubuntu 7.4.0-1ubuntu1~16.04~ppa1) 7.4.0) and qemu-system-x86_64 (QEMU emulator version 2.5.0 (Debian 1:2.5+dfsg-5ubuntu10.43))` 이므로, gcc 버전에 큰 차이가 있습니다.

예견된 문제였습니다. 따라서 distrobox(docker)를 이용해 ubuntu 16.04 서브시스템을 구축해 컴파일했습니다.

```bash
distrobox create -i ubuntu:16.04
distrobox enter ubuntu-16-04
```

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install gcc make qemu-system
```

```bash
cd threads
make
```

그러나, 이번에는 다른 컴파일 에러가 발생했습니다.

```bash
gcc: error: unrecognized command line option ‘-fno-plt’
../../Make.config:33: recipe for target 'threads/init.o' failed
make[1]: *** [threads/init.o] Error 1
make[1]: Leaving directory '/home/seolcu/문서/코드/pintos-kaist/threads/build'
../Makefile.kernel:10: recipe for target 'all' failed
make: *** [all] Error 2
```

gcc가 `-fno-plt` 옵션을 알아보지 못하는 모습입니다. 뭔가 이상해서 gcc 버전을 확인해보니, `5.4.0` 버전이었습니다.

페이지에서 명시된 시스템의 버전은 Ubuntu 16.04였으나, PPA로 gcc를 설치해 버전은 `7.4.0`이었습니다.

그동안 PPA가 제공하는 GCC의 버전도 바뀌었을테니 똑같은 PPA를 찾아 설치하는 건 좋은 생각이 아닌 것 같습니다.

따라서 [Ubuntu - Available GCC versions](https://documentation.ubuntu.com/ubuntu-for-developers/reference/availability/gcc/) 문서를 참고한 결과, gcc 7을 default로 제공하는 배포판은 Ubuntu 18.04라는 것을 알아냈습니다. 따라서 이전과 같은 방법으로 Ubuntu 18.04 시스템을 구축했습니다. (gcc 7.5.0)

그러자, 성공적으로 컴파일되었습니다.

```
$ pintos
qemu-system-x86_64: warning: TCG doesn't support requested feature: CPUID.01H:ECX.vmx [bit 5]
Kernel command line:
0 ~ 9fc00 1
100000 ~ ffe0000 1
Pintos booting with:
        base_mem: 0x0 ~ 0x9fc00 (Usable: 639 kB)
        ext_mem: 0x100000 ~ 0xffe0000 (Usable: 260,992 kB)
Calibrating timer...  261,734,400 loops/s.
Boot complete.
```

환경 설정 방법을 [프로젝트 README](../../README.md)에 기록했습니다.

## PROJECT 1: THREADS

### Alarm Clock

우선 `threads/timer.c`를 보았습니다.

```C
/* Suspends execution for approximately TICKS timer ticks. */
void timer_sleep(int64_t ticks)
{
	int64_t start = timer_ticks();

	ASSERT(intr_get_level() == INTR_ON);
	while (timer_elapsed(start) < ticks)
		thread_yield();
}
```

```C
/* Timer interrupt handler. */
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;
	thread_tick();
}
```

여기서 `timer_sleep` 함수는 틱을 다 채울때까지 `thread_yield`를 반복합니다. 이로 인해 스레드가 실제로 중지되는 것이 아닌, while문이 돌며 순서만 계속 밀리는 busy wait을 하게 됩니다. 따라서 이를 위해선 `thread_block`을 통해 실제로 스레드를 정지해야합니다.

그러나, block을 해도 while문으로 반복하면서 틱을 채우는 방식인 것은 여전합니다. 따라서, 깨우는 작업을 인터럽트 핸들러에게 맡기는 것이 좋아보입니다. 그러려면, `timer_sleep`이 언제 어떤 스레드를 깨울지를 어딘가에 기록해두고, `timer_interrupt`가 이를 참고해 시간이 맞다면 해당 스레드를 깨우면 됩니다. 기록하는 도중 인터럽트가 발생해 race condition이 발생할 수 있으므로 (발생하는 것이 거의 당연하므로) 기록하는 동안에는 인터럽트를 disable하고, 기록이 끝나면 원래의 상태로 되돌려야 합니다.

여러 스레드가 동시에 sleep할 수 있기에, 깨울 스레드를 기록하기 위해서는 리스트가 필요합니다. [Introduction](https://casys-kaist.github.io/pintos-kaist/project1/introduction.html)에서 볼 수 있듯, [list.c](/lib/kernel/list.c)와 [list.h](/include/lib/kernel/list.h)에 이미 이중 연결 리스트가 구현되어 있습니다.

코드를 보니 사용 방법이 다소 복잡해 보였습니다. 따라서 주석을 읽으며 방법을 익혔습니다.

```C
/* Doubly linked list.
 *
 * This implementation of a doubly linked list does not require
 * use of dynamically allocated memory.  Instead, each structure
 * that is a potential list element must embed a struct list_elem
 * member.  All of the list functions operate on these `struct
 * list_elem's.  The list_entry macro allows conversion from a
 * struct list_elem back to a structure object that contains it.

 * For example, suppose there is a needed for a list of `struct
 * foo'.  `struct foo' should contain a `struct list_elem'
 * member, like so:

 * struct foo {
 *   struct list_elem elem;
 *   int bar;
 *   ...other members...
 * };

 * Then a list of `struct foo' can be be declared and initialized
 * like so:

 * struct list foo_list;

 * list_init (&foo_list);

 * Iteration is a typical situation where it is necessary to
 * convert from a struct list_elem back to its enclosing
 * structure.  Here's an example using foo_list:

 * struct list_elem *e;

 * for (e = list_begin (&foo_list); e != list_end (&foo_list);
 * e = list_next (e)) {
 *   struct foo *f = list_entry (e, struct foo, elem);
 *   ...do something with f...
 * }
```

```C
/* List element. */
struct list_elem {
	struct list_elem *prev;     /* Previous list element. */
	struct list_elem *next;     /* Next list element. */
};

/* List. */
struct list {
	struct list_elem head;      /* List head. */
	struct list_elem tail;      /* List tail. */
};

/* Converts pointer to list element LIST_ELEM into a pointer to
   the structure that LIST_ELEM is embedded inside.  Supply the
   name of the outer structure STRUCT and the member name MEMBER
   of the list element.  See the big comment at the top of the
   file for an example. */
#define list_entry(LIST_ELEM, STRUCT, MEMBER)           \
	((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next     \
		- offsetof (STRUCT, MEMBER.next)))
```

이를 보아, 특정 구조체 리스트를 만드려고 하는 경우, 그 구조체의 멤버로 list_elem이 있어야 합니다. list_elem이 이중 연결 리스트의 원소이며, list_elem을 다시 구조체 형태로 변환하고 싶은 경우 list_entry 매크로를 사용해야 하는 것으로 보입니다.

#### 첫번째 시도

```C
static struct sleeping_thread
{
	struct list_elem elem;
	int64_t wake_tick;
	struct thread *thread;
};

static struct list sleeping_thread_list;
```

처음으로는 block된 스레드를 저장하는 sleeping_thread 구조체를 만들고, 이 sleeping_thread들을 관리하는 sleeping_thread_list를 만들었습니다.

```C
void timer_sleep(int64_t ticks)
{
	struct sleeping_thread *st = malloc(sizeof(struct sleeping_thread));
	st->wake_tick = timer_ticks() + ticks;
	st->thread = thread_current();

	enum intr_level old_level = intr_disable();

	list_push_back(&sleeping_thread_list, &st->elem);
	thread_block();

	intr_set_level(old_level);
}
```

```C
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;
	thread_tick();

	struct list_elem *e;
	for (e = list_begin(&sleeping_thread_list); e != list_end(&sleeping_thread_list);)
	{
		struct sleeping_thread *st = list_entry(e, struct sleeping_thread, elem);
		if (st->wake_tick < ticks)
		{
			e = list_remove(e);
			thread_unblock(st->thread);
			free(st);
		}
		else
		{
			e = list_next(e);
		}
	}
}
```

하지만 실제로 실행해보니, `timer_inturrupt`의 free 부분에서 커널 패닉이 발생했습니다.

```
Kernel panic in run: PANIC at ../../threads/synch.c:188 in lock_acquire(): assertion `!intr_context ()' failed.
```

알고보니, free가 lock을 사용하기 때문이었습니다. 따라서 구조체를 새로 만들고 동적 할당을 하는 것보다는, 기존 스레드 구조체에 wake_tick와 list_elem을 추가하는 것이 더 낫겠다는 생각을 했습니다.

#### 두번째 시도

## 배운것

```
Some external interrupts cannot be postponed, even by disabling interrupts. These interrupts, called non-maskable interrupts (NMIs), are supposed to be used only in emergencies, e.g. when the computer is on fire. Pintos does not handle non-maskable interrupts.
```

```
세마포어에서 P / V는 원래 네덜란드어 약자에서 온 고전 표기예요(디익스트라가 썼던 표기).

P: Proberen (시도하다 / 검사하다)
→ 보통 wait, down, acquire 라고도 부름
→ 의미: “토큰 하나 가져가고(카운터 -1), 없으면 기다려”

V: Verhogen (증가시키다)
→ 보통 signal, up, release 라고도 부름
→ 의미: “토큰 하나 돌려주고(카운터 +1), 기다리는 애 있으면 깨워”

간단히 외우면:

P = 들어가려는 동작(획득/대기)

V = 나오는 동작(반납/깨우기)

참고로 어떤 문서에서는 V를 Vrijgeven(해제하다)로 설명하기도 하는데, 핵심 동작은 똑같아요.
```
