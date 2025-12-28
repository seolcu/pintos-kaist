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

환경 설정 방법을 [프로젝트 README](../../README.md)에 기록했습니다.

## PROJECT 1: THREADS

