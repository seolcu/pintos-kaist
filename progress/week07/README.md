# 7주차 진행기록

## PROJECT 4: FILE SYSTEM

### Indexed and Extensible Files

#### 요구사항 정리

- 기존 extent 기반(연속 할당) inode 구조를 제거하고, FAT(File Allocation Table) 기반으로 block indexing을 구현해야 합니다.
  - multi-level indexing(FFS 스타일)은 사용하면 안 된다고 명시되어 있습니다.
- `filesys/fat.c`에 주어진 skeleton(`fat_fs_init`, `fat_create_chain`, `fat_remove_chain`, `fat_put/get`, `cluster_to_sector`)을 채우고,
  이를 `inode.c`/`filesys.c`에서 실제 파일 할당에 사용해야 합니다.
- File Growth(확장 파일)을 구현해야 합니다.
  - `seek`로 EOF 바깥으로 이동은 가능하지만, seek만으로 파일 길이가 늘어나면 안 됨
  - EOF 바깥에 `write`가 들어오면 파일을 늘리고, 중간 hole은 0으로 채워야 함
  - root directory도 이제 16 엔트리 제한 없이 확장 가능해야 함

#### 초기 make check 결과

처음에는 테스트가 전부 다음 panic으로 실패했습니다.

```
Kernel panic in run: PANIC at ../../filesys/fat.c:125 in fat_create(): FAT creation failed
Call stack:
  ...
0x...: fat_create (filesys/fat.c:128)
0x...: do_format (filesys/filesys.c:113)
0x...: filesys_init (filesys/filesys.c:32)
```

즉, `-f`로 포맷하는 단계에서 FAT 초기화가 깨져서, 파일시스템 자체가 부팅 중에 죽는 상태였습니다.

#### 원인

`filesys/fat.c`의 핵심 함수들이 TODO 상태라서(`fat_fs_init` 등), `fat_length`가 0으로 남아 `fat_create()`에서 `calloc()`이 실패하면서 panic이 났습니다.

또한 FAT만 채워도 끝이 아니라, 실제 파일 할당/확장 로직이 기존 contiguous inode 구현(`inode.c`)에 박혀 있어서 FAT을 실사용하도록 수정이 필요했습니다.

#### 수정 사항

##### 1) FAT skeleton 구현

`filesys/fat.c`에서 FAT의 기본 메타데이터(`fat_length`, `data_start`)를 계산하고, free cluster를 찾아 chain을 늘리고/지우는 함수들을 구현했습니다.

특히 cluster size가 1 sector로 고정이라서, cluster -> sector 변환은 단순히 `data_start + (clst - 1)` 형태로 처리했습니다.

```c
/* filesys/fat.c */
fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;
fat_fs->fat_length = (fat_fs->bs.total_sectors - fat_fs->data_start)
                   / fat_fs->bs.sectors_per_cluster + 1;

disk_sector_t
cluster_to_sector (cluster_t clst) {
  return fat_fs->data_start + (clst - ROOT_DIR_CLUSTER) * fat_fs->bs.sectors_per_cluster;
}
```

`fat_create_chain()`은 FAT에서 0(빈 엔트리)을 찾아 할당하고, 새 cluster는 기본적으로 0으로 초기화(disk write)하도록 했습니다.

##### 2) inode를 FAT 기반으로 변경

디자인은 단순하게 잡았습니다.

- inode의 “번호/위치”는 disk sector가 아니라 FAT cluster 번호로 취급
  - 실제 disk I/O는 `cluster_to_sector()`로 변환해서 접근
- on-disk inode(`struct inode_disk`)는 파일 메타데이터만 들고,
  - `start`: 파일 데이터 chain의 첫 cluster
  - `length`: 파일 길이

그래서 `inode_open()`/`inode_create()`에서 inode 자체를 `cluster_to_sector(sector)`에 읽고/쓰도록 바꿨습니다.

```c
/* filesys/inode.c */
disk_read (filesys_disk, cluster_to_sector (inode->sector), &inode->data);

disk_write (filesys_disk, cluster_to_sector (sector), disk_inode);
```

그리고 `byte_to_sector()`는 contiguous 계산 대신 FAT chain을 따라가서 해당 offset의 cluster를 찾도록 수정했습니다.

##### 3) File Growth 구현

`inode_write_at()`에서 `offset + size`가 현재 `inode->data.length`를 넘어가면,

1. 필요한 cluster 수만큼 `fat_create_chain()`으로 chain을 늘리고
2. inode length를 갱신한 뒤 inode를 disk에 sync

하는 `inode_grow()`를 추가했습니다.

```c
/* filesys/inode.c */
if (end_pos > inode->data.length)
  inode_grow (inode, end_pos);
```

cluster를 할당할 때 sector를 0으로 채워두었기 때문에(`fat_create_chain()`에서 disk write),
EOF 바깥으로 write할 때 생기는 hole 구간도 read 시 0으로 보이도록 맞췄습니다.

##### 4) filesys_create / format 경로 수정

`filesys_create()`에서 기존 `free_map_allocate()` 대신 inode용 cluster를 `fat_create_chain(0)`으로 하나 할당해서 inode를 만들도록 수정했습니다.

또, 포맷(`do_format`) 시 FAT만 만들고 끝내면 root directory가 없어서 이후 파일 작업이 불가능하므로,
FAT 생성 후 `dir_create(ROOT_DIR_SECTOR, 16)`로 root dir inode를 생성하도록 했습니다.

#### 수정 후 테스트 결과

`make check` 기준으로,

- threads/userprog/filesys base 테스트는 전부 PASS
- file growth 관련 테스트도 PASS

를 확인했습니다.

```
pass tests/filesys/extended/grow-create
pass tests/filesys/extended/grow-seq-sm
pass tests/filesys/extended/grow-seq-lg
pass tests/filesys/extended/grow-sparse
pass tests/filesys/extended/grow-two-files
pass tests/filesys/extended/grow-tell
pass tests/filesys/extended/grow-file-size
pass tests/filesys/extended/grow-root-sm
pass tests/filesys/extended/grow-root-lg
pass tests/filesys/extended/syn-rw
```

남아있는 FAIL은 대부분 subdirectories/symlink/persistence 쪽이라서, 다음 파트에서 진행해야 합니다.

```
FAIL tests/filesys/extended/dir-*
FAIL tests/filesys/extended/grow-dir-lg
FAIL tests/filesys/extended/symlink-*
FAIL tests/filesys/extended/*-persistence
...
41 of 146 tests failed.
```

---

### Subdirectories and Soft Links

#### 요구사항 정리

Subdirectories 파트에서는 “root 디렉토리만 있는 단일 namespace”를

- `/a/b/c` 같은 hierarchical path가 동작하도록 확장하고
- 프로세스마다 독립적인 current working directory(cwd)를 유지

하게 만드는 것이 핵심입니다.

추가로, 아래 syscall들도 구현해야 합니다.

- `chdir(const char *dir)`
- `mkdir(const char *dir)`
- `readdir(int fd, char *name)`
- `isdir(int fd)`
- `inumber(int fd)`

그리고 Soft Link 파트에서는

- `symlink(const char *target, const char *linkpath)`로 symlink 파일을 만들고
- open/create 등 path resolution에서 symlink를 따라가도록

구현해야 합니다.

#### 초기 테스트 결과

테스트를 돌려보면, `dir-*`/`symlink-*` 관련 테스트들이 begin 이후 진행이 끊기고 `exit(-1)`로 끝났습니다.

예를 들어 `dir-mkdir`는 다음처럼 `mkdir "a"`까지만 찍히고 종료됩니다.

```
(dir-mkdir) begin
(dir-mkdir) mkdir "a"
dir-mkdir: exit(-1)
```

`symlink-file`도 `symlink()` 호출 시점에서 그대로 `exit(-1)`로 종료되었습니다.

#### 원인

1. `userprog/syscall.c`에 Project 4 syscall (`SYS_CHDIR`, `SYS_MKDIR`, `SYS_READDIR`, `SYS_ISDIR`, `SYS_INUMBER`, `SYS_SYMLINK`) 처리가 없어서, default로 떨어져 `exit(-1)`.
2. `filesys/filesys.c`의 `filesys_create/open/remove`가 root 디렉토리만 보고 동작해서, `/a/b` 같은 path를 처리할 수 없음.
3. directory inode와 symlink inode를 구분할 메타데이터가 없어서 `isdir()`/symlink-following을 구현할 근거가 부족함.

#### 수정 사항

##### 1) inode 타입 필드 추가 (file/dir/symlink)

`filesys/inode.c`의 on-disk inode(`struct inode_disk`)에 `type` 필드를 추가하고,

- `inode_is_dir()`
- `inode_is_symlink()`

같은 helper를 만들었습니다.

또한 `inode_create_dir()`, `inode_create_symlink()`를 추가해서 directory/symlink 생성 시 타입을 저장하도록 했습니다.

##### 2) 디렉토리 생성 시 "."/".." 엔트리 추가

`filesys/directory.c`의 `dir_create()`를 확장해서

- 디렉토리 inode를 `inode_create_dir()`로 만들고
- 내부에 `.` 과 `..` 엔트리를 생성

하도록 수정했습니다. (root는 `..`가 자기 자신을 가리키게 했습니다.)

또한 `readdir()` 요구사항 때문에 `dir_readdir()`는 `.`/`..`을 반환하지 않도록 skip 처리했습니다.

##### 3) 디렉토리 remove 정책 + empty check

`dir_remove()`에서

- root 디렉토리 삭제 금지
- `.`/`..` 삭제 금지
- 디렉토리는 empty일 때만 삭제 가능
- 디렉토리가 open 상태면 삭제 불가(테스트 `dir-rm-cwd` 케이스)

를 체크하도록 추가했습니다.

##### 4) cwd 추가 + fork inheritance

thread 단위로 cwd를 유지해야 해서

- `include/threads/thread.h`에 `struct dir *cwd` 추가
- `userprog/process.c`
  - initd 시작 시 cwd를 `/`로 초기화
  - fork 시 child가 parent의 cwd를 `dir_reopen()`으로 상속
  - exit 시 cwd `dir_close()`

를 넣었습니다.

##### 5) path resolution 구현 (absolute/relative, '.', '..', symlink follow)

`filesys/filesys.c`에 path parsing helper를 추가해서

- `/`로 시작하면 root부터
- 아니면 현재 thread의 cwd부터

component 단위로 `dir_lookup()`을 하면서 내려가도록 구현했습니다.

symlink는 inode type이 `INODE_SYMLINK`면 target string을 읽어서 다시 resolve하도록 했고, loop 방지를 위해 depth limit을 뒀습니다.

이 로직을 기반으로

- `filesys_create/open/remove`를 path 기반으로 변경
- `filesys_chdir`, `filesys_mkdir`, `filesys_symlink` 추가

를 했습니다.

##### 6) syscall 구현

`userprog/syscall.c`에 아래 케이스를 추가했습니다.

- `SYS_CHDIR` -> `filesys_chdir`
- `SYS_MKDIR` -> `filesys_mkdir`
- `SYS_READDIR` -> directory fd에 대해 엔트리 읽기(`.`/`..` skip)
- `SYS_ISDIR` / `SYS_INUMBER`
- `SYS_SYMLINK` -> `filesys_symlink`

추가로, 기존 syscall 중 `read/write/filesize`는 directory fd에 대해 `-1`이 나오도록 막아서 `dir-open` 테스트(디렉토리에 write 하면 -1)도 맞췄습니다.

#### 수정 후 테스트 결과

먼저 관련 테스트를 일부만 골라서 확인했습니다.

```
pass tests/filesys/extended/dir-mkdir
pass tests/filesys/extended/dir-open
pass tests/filesys/extended/dir-vine
pass tests/filesys/extended/grow-dir-lg
pass tests/filesys/extended/symlink-file
pass tests/filesys/extended/symlink-dir
pass tests/filesys/extended/symlink-link
```

전체 `make check`를 다시 돌렸고, 최종적으로

중간에 `open("")`이 cwd를 열어버리는 바람에 `tests/userprog/open-empty`가 한번 FAIL이 났는데, `filesys_open()`에서 빈 문자열은 바로 실패(NULL) 처리하도록 수정해서 해결했습니다.

```
All 146 tests passed.
```

까지 확인했습니다.
