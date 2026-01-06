# Private Fork of pintos-kaist

학부인턴 Pintos 과제 기록

## 환경설정

```bash
distrobox create -i ubuntu:18.04
distrobox enter ubuntu-18-04
```

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install gcc make qemu-system python3
```

## IDE 설정 (clangd)

IDE에서 코드 자동완성 및 에러 표시를 위해 clangd 설정이 필요합니다:

```bash
./setup-clangd.sh
```

> **Note:** `.clangd` 파일은 `.gitignore`에 포함되어 있습니다.
