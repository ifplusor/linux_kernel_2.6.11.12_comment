# Linux Kernel 2.6.11.12 with Comments

This project is the comment version for linux 2.6.11.12,
based on book 'Understanding The Linux Kernel 3rd',
please feel free to modify.

## Prepare to compile kernel on macOS

> https://github.com/nsabovic/homebrew-public

Install all the GNU tools and make sure they're used:

```sh
brew install findutils coreutils gnu-tar gnu-sed make

export PATH="/usr/local/opt/findutils/libexec/gnubin:/usr/local/opt/coreutils/libexec/gnubin:/usr/local/opt/gnu-tar/libexec/gnubin:/usr/local/opt/gnu-sed/libexec/gnubin:/usr/local/opt/make/libexec/gnubin:$PATH"
```

For ```make menuconfig``` you need ```ncurses```:

```sh
brew install ncurses

export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:/usr/local/opt/ncurses/lib/pkgconfig"
```

Install the ```elf.h``` header:

```sh
brew install nsabovic/linuxonmac/elf-header
```

## Generate compile_commands.json

> https://github.com/gniuk/linux-compile-commands

Config the kernel as you want:

```sh
make ARCH=i386 menuconfig
```

Generate build log:

```sh
LANGUAGE=en make V=1 ARCH=i386 -j1 --dry-run |& tee build-log.txt
```

Generate ```compile_commands.json```:

```sh
compiledb < build-log.txt
```
