SHELL := /bin/bash
# sudo pacman -S --needed riscv64-linux-gnu-gcc riscv64-elf-gcc bc dtc
# sudo pacman -S clang lld llvm-libs

MUSL_VER    := musl-1.2.6
LUA_VER     := lua-5.4.9
BUSYBOX_VER := busybox-1.38.0
LINUX_VER   := linux-7.3
THIRD_PARTY := third_party
SYSROOT     := $(CURDIR)/build/sysroot

.PHONY: all clean distclean sysroot lua busybox linux emu dtb

all: build/riscv_emu build/device_tree.dtb build/linux.image
	@echo "Build completed successfully."
	@ls -lh build

sysroot: $(SYSROOT)/.installed
lua: build/lua
busybox: build/busybox
linux: build/linux.image
emu: build/riscv_emu
dtb: build/device_tree.dtb

build:
	mkdir -p build

build/riscv_emu: riscv_emu.c | build
	gcc -O3 $< -o $@

build/device_tree.dtb: res/device_tree.dts | build
	dtc -I dts -O dtb $< -o $@

$(THIRD_PARTY)/$(MUSL_VER)/.extracted: $(THIRD_PARTY)/$(MUSL_VER).tar.gz
	tar -xf $< -C $(THIRD_PARTY)
	touch $@

$(THIRD_PARTY)/$(LUA_VER)/.extracted: $(THIRD_PARTY)/$(LUA_VER).tar.gz
	tar -xf $< -C $(THIRD_PARTY)
	touch $@

$(THIRD_PARTY)/$(BUSYBOX_VER)/.extracted: $(THIRD_PARTY)/$(BUSYBOX_VER).tar.bz2
	tar -xf $< -C $(THIRD_PARTY)
	touch $@

$(THIRD_PARTY)/$(LINUX_VER)/.extracted: $(THIRD_PARTY)/$(LINUX_VER).tar.xz
	tar -xf $< -C $(THIRD_PARTY)
	touch $@

$(SYSROOT)/.installed: $(THIRD_PARTY)/$(MUSL_VER)/.extracted | build
	LIBGCC=$$(riscv64-elf-gcc -march=rv32im -mabi=ilp32 -print-libgcc-file-name) && \
	LIBGCC_DIR=$$(dirname "$$LIBGCC") && \
	cd $(THIRD_PARTY)/$(MUSL_VER) && \
	CC="riscv64-linux-gnu-gcc -march=rv32ima -mabi=ilp32" \
	CROSS_COMPILE="riscv64-linux-gnu-" \
	LIBCC="$$LIBGCC" \
	./configure --target=riscv32-linux-musl --prefix=$(SYSROOT) --disable-shared --enable-wrapper=gcc && \
	$(MAKE) -j$$(nproc) > /dev/null && \
	$(MAKE) install && \
	cp "$$LIBGCC" $(SYSROOT)/lib/libgcc.a && \
	cp "$$LIBGCC_DIR"/crtbegin.o $(SYSROOT)/lib/crtbeginT.o && \
	cp "$$LIBGCC_DIR"/crtbegin.o $(SYSROOT)/lib/crtbeginS.o && \
	cp "$$LIBGCC_DIR"/crtend.o $(SYSROOT)/lib/crtend.o && \
	cp "$$LIBGCC_DIR"/crtend.o $(SYSROOT)/lib/crtendS.o
	touch $@

build/lua: $(THIRD_PARTY)/$(LUA_VER)/.extracted $(SYSROOT)/.installed | build
	clang --target=riscv32-unknown-linux-musl \
		-march=rv32ima -mabi=ilp32 \
		--sysroot=$(SYSROOT) \
		-fuse-ld=lld \
		--rtlib=libgcc \
		--unwindlib=none \
		-Os -flto -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -s \
		-static -DLUA_USE_POSIX -DLUA_32BITS=1 \
		-Wno-macro-redefined \
		$$(ls $(THIRD_PARTY)/$(LUA_VER)/src/*.c | grep -v luac.c) -lm -o $@

build/busybox: $(THIRD_PARTY)/$(BUSYBOX_VER)/.extracted res/busybox.config.toml $(SYSROOT)/.installed | build
	cd $(THIRD_PARTY)/$(BUSYBOX_VER) && \
	$(MAKE) ARCH=riscv allnoconfig && \
	grep -v -w -F -f <(grep -o '^CONFIG_[^=]*' $(CURDIR)/res/busybox.config.toml) .config > .config.tmp && \
	cat .config.tmp $(CURDIR)/res/busybox.config.toml > .config && rm -f .config.tmp && \
	$(MAKE) ARCH=riscv oldconfig && \
	sed -i 's/#define AUTOCONF_TIMESTAMP .*/#define AUTOCONF_TIMESTAMP "hallo"/' include/autoconf.h && \
	$(MAKE) -j$$(nproc) \
		ARCH=riscv \
		CROSS_COMPILE="riscv64-linux-gnu-" \
		HOSTCC=gcc \
		CC="clang --target=riscv32-unknown-linux-musl -march=rv32ima -mabi=ilp32 --sysroot=$(SYSROOT) -fuse-ld=lld --rtlib=libgcc --unwindlib=none -Os -Qunused-arguments" \
		busybox
	riscv64-linux-gnu-strip -s $(THIRD_PARTY)/$(BUSYBOX_VER)/busybox -o $@

build/linux.image: $(THIRD_PARTY)/$(LINUX_VER)/.extracted res/linux.config.toml build/busybox build/lua res/init.sh res/initramfs.txt | build
	KCONFIG_ALLCONFIG="$(CURDIR)/res/linux.config.toml" $(MAKE) -C $(THIRD_PARTY)/$(LINUX_VER) ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- allnoconfig
	KBUILD_BUILD_USER="yume" \
	KBUILD_BUILD_HOST="hallo" \
	KBUILD_BUILD_TIMESTAMP=" " \
	KBUILD_BUILD_VERSION="1" \
	$(MAKE) -C $(THIRD_PARTY)/$(LINUX_VER) ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j$$(nproc) Image
	cp $(THIRD_PARTY)/$(LINUX_VER)/arch/riscv/boot/Image $@

clean:
	rm -rf build
	-$(MAKE) -C $(THIRD_PARTY)/$(BUSYBOX_VER) clean 2>/dev/null || true
	-$(MAKE) -C $(THIRD_PARTY)/$(LINUX_VER) clean 2>/dev/null || true

distclean: clean
	rm -rf $(THIRD_PARTY)/$(MUSL_VER) $(THIRD_PARTY)/$(LUA_VER) $(THIRD_PARTY)/$(BUSYBOX_VER) $(THIRD_PARTY)/$(LINUX_VER)
