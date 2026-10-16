Q = @
CC_ = gcc
CC_win32 = i686-w64-mingw32-gcc
CC = ${CC_${PLAT}}
LD_ = ld
LD_win32 = i686-w64-mingw32-ld
LD = ${LD_${PLAT}}
OBJCOPY_ = objcopy
OBJCOPY_win32 = i686-w64-mingw32-objcopy
OBJCOPY = ${OBJCOPY_${PLAT}}
HOSTCC = ${CC}

# Host OS detection: macOS (Darwin) needs different linker flags and libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
DARWIN = y
endif
ifeq ($(DARWIN),y)
# Apple ld64 has no --gc-sections (use -dead_strip); libdl lives in libSystem;
# OpenGL is a framework instead of -lGL.
CFLAGS_GCSECTIONS = -Wl,-dead_strip
LIBS_DL =
LIBS_GL = -framework OpenGL
else
CFLAGS_GCSECTIONS = -Wl,--gc-sections
LIBS_DL = -ldl
LIBS_GL = -lGL
endif

# All emulator sources live under src/; object files go to build/obj (gitignored)
INCDIR = src
OBJDIR = build/obj

SDL_CONFIG ?= $(shell which sdl2-config 2>/dev/null || which sdl-config 2>/dev/null || echo sdl-config)
# libslirp: the upstream pkg-config module is named "slirp" (Debian/Hombrew),
# while some distributions install it as "libslirp"; probe both.  pkg-config
# reports -I<prefix>/include/slirp, but ne2000.c includes "slirp/libslirp.h",
# so strip the trailing /slirp from the include path.
SLIRP_PKG ?= $(shell pkg-config --exists libslirp 2>/dev/null && echo libslirp || (pkg-config --exists slirp 2>/dev/null && echo slirp))
SLIRP_INC ?= $(shell pkg-config --cflags ${SLIRP_PKG} 2>/dev/null | sed -e 's|-I\([^ ]*\)/slirp|-I\1|g')
SLIRP_LIB ?= $(shell pkg-config --libs ${SLIRP_PKG} 2>/dev/null)

ifeq ($(ENABLE_ASAN),y)
CFLAGS_ = -I ${INCDIR} -Wall -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS_ += -fsanitize=address,undefined
else
CFLAGS_ = -I ${INCDIR} -Wall -O3 -ffunction-sections -fdata-sections -g ${CFLAGS_GCSECTIONS}
endif
CFLAGS_win32 = -I ${INCDIR} -Wall -O3 -ffunction-sections -g -Wl,--gc-sections
CFLAGS = ${CFLAGS_${PLAT}}
CFLAGS += -DI386_ENABLE_FPU
CFLAGS += -DI386_ENABLE_MMX -DI386_ENABLE_SSE -DI386_ENABLE_SSE2 -DI386_ENABLE_SSE3
CFLAGS += -DI386_ENABLE_SSSE3
CFLAGS_SDL = ${CFLAGS} `${SDL_CONFIG} --cflags`

LDFLAGS_ =
LDFLAGS_win32 = -mconsole
ifeq ($(ENABLE_ASAN),y)
LDFLAGS_ += -fsanitize=address,undefined
endif
LDFLAGS = ${LDFLAGS_${PLAT}}

LIBS = -lm ${LIBS_DL}

SRCS =

#USE_GLIBC_FIX = y/n
#build with recent glibc, can run with older glibc (experimental)
USE_GLIBC_FIX = n
CFLAGS_GLIBC_FIX_y = -include ${INCDIR}/glibc-fix/glibc-fix.h
CFLAGS_GLIBC_FIX = ${CFLAGS_GLIBC_FIX_${USE_GLIBC_FIX}}
SRCS_GLIBC_FIX_y = ${INCDIR}/glibc-fix/glibc-fix.c
SRCS_GLIBC_FIX = ${SRCS_GLIBC_FIX_${USE_GLIBC_FIX}}
LIBS_GLIBC_FIX_y = -Wl,--no-as-needed -l:libdl.so.2 -l:libpthread.so.0 -Wl,--as-needed
LIBS_GLIBC_FIX = ${LIBS_GLIBC_FIX_${USE_GLIBC_FIX}}
SRCS += ${SRCS_GLIBC_FIX}
CFLAGS += ${CFLAGS_GLIBC_FIX}
LIBS += ${LIBS_GLIBC_FIX}

LIBS_SDL_ = `${SDL_CONFIG} --libs` ${LIBS_GL} ${LIBS}
LIBS_SDL_win32 = `${SDL_CONFIG} --libs` ${LIBS}
LIBS_SDL = ${LIBS_SDL_${PLAT}}

#USE_FMOPL = y/n
USE_FMOPL = y
FMOPL_y = ${INCDIR}/fmopl.c
CFLAGS_FMOPL_y = -DUSE_FMOPL
CFLAGS += ${CFLAGS_FMOPL_${USE_FMOPL}}

#USE_CPUABS = y/n
USE_CPUABS = n
CPUABS_y = ${INCDIR}/kvm.c
CFLAGS_CPUABS_y = -DUSE_CPUABS
CFLAGS += ${CFLAGS_CPUABS_${USE_CPUABS}}

#USE_SLIRP = y/n
USE_SLIRP ?= $(if $(strip ${SLIRP_PKG}),y,n)
CFLAGS_SLIRP_y = -DUSE_SLIRP ${SLIRP_INC}
LIBS_SLIRP_y_ = ${SLIRP_LIB}
LIBS_SLIRP_y_win32 = ${SLIRP_LIB} -lws2_32 -liphlpapi
CFLAGS += ${CFLAGS_SLIRP_${USE_SLIRP}}
LIBS += ${LIBS_SLIRP_${USE_SLIRP}_${PLAT}}

#USE_TUNTAP = y/n
USE_TUNTAP_ = y
USE_TUNTAP_win32 = n
USE_TUNTAP = ${USE_TUNTAP_${PLAT}}
CFLAGS_TUNTAP_y = -DUSE_TUNTAP
CFLAGS += ${CFLAGS_TUNTAP_${USE_TUNTAP}}

#ENABLE_CISO = n/zlib/miniz
ENABLE_CISO = n
CFLAGS_zlib = -DIDE_ENABLE_CISO -include zlib.h
LIBS_zlib = -lz
MINIZ_PATH = build/miniz
CFLAGS_miniz = -DIDE_ENABLE_CISO -include ${MINIZ_PATH}/miniz.h
SRCS_miniz = ${MINIZ_PATH}/miniz.c
CFLAGS += ${CFLAGS_${ENABLE_CISO}}
LIBS += ${LIBS_${ENABLE_CISO}}
SRCS += ${SRCS_${ENABLE_CISO}}

# The emulator core is the i386 cpu emulator plus the optional x87 fpu.
SRCS += ${INCDIR}/i386.c ${INCDIR}/fpu.c

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
SRCS += ${INCDIR}/jit_arm64.c
else ifeq ($(UNAME_M),aarch64)
SRCS += ${INCDIR}/jit_arm64.c
endif

ifeq ($(DARWIN),y)
PROGS_ = tiny386 tiny386_headless
else
PROGS_ = tiny386 tiny386_headless initnet
endif
PROGS_win32 = tiny386 tiny386_headless
PROGS = ${PROGS_${PLAT}}

SRCS += ${INCDIR}/ini.c ${INCDIR}/i8259.c ${INCDIR}/i8254.c ${INCDIR}/ide.c ${INCDIR}/isofs.c ${INCDIR}/vga.c ${INCDIR}/vgafont.c ${INCDIR}/cirrus_vga.c ${INCDIR}/cirrus_opencl.c ${INCDIR}/cirrus_ppa.c ${INCDIR}/i8042.c ${INCDIR}/misc.c ${INCDIR}/adlib.c ${INCDIR}/ne2000.c ${INCDIR}/i8257.c ${INCDIR}/sb16.c ${INCDIR}/pcspk.c
SRCS += ${FMOPL_${USE_FMOPL}}
SRCS += ${CPUABS_${USE_CPUABS}}
SRCS += ${INCDIR}/pc.c
SRCS += ${INCDIR}/pci.c
SRCS += ${INCDIR}/win32.c
SRCS += ${INCDIR}/term.c
# OSD
SRCS += ${INCDIR}/osd/microui.c ${INCDIR}/osd/osd.c

OBJS = $(patsubst %.c,${OBJDIR}/%.o,${SRCS})

.PHONY: all clean dep prepare

${OBJDIR}/%.o: %.c
	@mkdir -p $(dir $@)
	@/bin/echo -e " \e[1;32mCC\e[0m\t\e[1;37m$<\e[0m \e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}${CC} ${CFLAGS} -c $< -o $@

all: ${PROGS}

win32:
	make -C . PLAT=win32

clean:
	rm -rf ${OBJDIR} .depends ${PROGS}

prepare: ${INCDIR}/fmopl.inc

${INCDIR}/fmopl.inc: ${INCDIR}/fmopl.c
	${HOSTCC} -DGENTABLE $^$> -o fmoplgen -lm && ./fmoplgen > $@ && rm -f ./fmoplgen

tiny386: ${INCDIR}/sdl/main.c ${OBJS}
	@/bin/echo -e " \e[1;32mCCLD\e[0m\t\e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}${CC} ${LDFLAGS} ${CFLAGS_SDL} -o $@ $^$> ${LIBS_SDL}

fuzz_cpu: tests/fuzz_cpu.c ${OBJDIR}/${INCDIR}/i386.o ${OBJDIR}/${INCDIR}/fpu.o
	@/bin/echo -e " \e[1;32mCCLD\e[0m\t\e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}${CC} ${LDFLAGS} ${CFLAGS} -o $@ $^$> ${LIBS}

tiny386_headless: ${INCDIR}/main.c ${OBJS}
	@/bin/echo -e " \e[1;32mCCLD\e[0m\t\e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}${CC} ${LDFLAGS} ${CFLAGS} -o $@ $^$> ${LIBS}

initnet: tools/initnet.c ${SRCS_GLIBC_FIX}
	@/bin/echo -e " \e[1;32mCCLD\e[0m\t\e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}${CC} ${CFLAGS_GLIBC_FIX} -o $@ $^$> ${LIBS_GLIBC_FIX}

.depends: ${SRCS}
	@/bin/echo -e " \e[1;32mDEP\e[0m\t\e[1;37m$^$>\e[0m \e[1;32m->\e[0m \e[1;37m$@\e[0m"
	${Q}mkdir -p $(dir $@)
	${Q}rm -f $@
	${Q}for i in $^$>; do ${CC} ${CFLAGS} -MT ${OBJDIR}/$${i%.c}.o -MM $$i 2> /dev/null >> $@ || exit 0; done
	# Apple clang records the absolute path of the active Xcode SDK (SDKSettings.json)
	# as a prerequisite.  That path is machine-specific, so if Xcode is later moved
	# (e.g. to an external volume) the stale entry makes make abort with
	# "No rule to make target ...SDKSettings.json".  Strip it from the dep file.
	${Q}grep -v 'SDKSettings.json' $@ > $@.tmp; mv $@.tmp $@

dep: .depends
-include .depends
