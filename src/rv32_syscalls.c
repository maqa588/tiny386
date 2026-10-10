#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

static struct _reent global_reent;
struct _reent *__getreent(void) {
	return &global_reent;
}

long sys_ecall(long a0, long a1, long a2, long a3, long a7) {
	register long r_a0 asm("a0") = a0;
	register long r_a1 asm("a1") = a1;
	register long r_a2 asm("a2") = a2;
	register long r_a3 asm("a3") = a3;
	register long r_a7 asm("a7") = a7;
	asm volatile("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a7) : "memory");
	return r_a0;
}

long sys_ecall6(long a0, long a1, long a2, long a3, long a4, long a5, long a7) {
	register long r_a0 asm("a0") = a0;
	register long r_a1 asm("a1") = a1;
	register long r_a2 asm("a2") = a2;
	register long r_a3 asm("a3") = a3;
	register long r_a4 asm("a4") = a4;
	register long r_a5 asm("a5") = a5;
	register long r_a7 asm("a7") = a7;
	asm volatile("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5), "r"(r_a7) : "memory");
	return r_a0;
}

static int newlib_to_linux_flags(int flags) {
	int linux_flags = 0;
	int acc = flags & 3;
	if (acc == 0) linux_flags |= 0; /* O_RDONLY */
	else if (acc == 1) linux_flags |= 1; /* O_WRONLY */
	else if (acc == 2) linux_flags |= 2; /* O_RDWR */

	if (flags & 0x0200) linux_flags |= 0x40;  /* O_CREAT */
	if (flags & 0x0400) linux_flags |= 0x200; /* O_TRUNC */
	if (flags & 0x0008) linux_flags |= 0x400; /* O_APPEND */
	if (flags & 0x0800) linux_flags |= 0x80;  /* O_EXCL */
	if (flags & 0x4000) linux_flags |= 0x800; /* O_NONBLOCK */
	return linux_flags;
}

int _open_r(struct _reent *r, const char *file, int flags, int mode) {
	int lflags = newlib_to_linux_flags(flags);
	long ret = sys_ecall(-100, (long)file, lflags, mode, 56); /* __NR_openat */
	if (ret < 0) {
		r->_errno = -ret;
		return -1;
	}
	return (int)ret;
}

int _write_r(struct _reent *r, int fd, const void *buf, size_t count) {
	long ret = sys_ecall(fd, (long)buf, count, 0, 64); /* __NR_write */
	if (ret < 0) {
		r->_errno = -ret;
		return -1;
	}
	return (int)ret;
}

int _read_r(struct _reent *r, int fd, void *buf, size_t count) {
	long ret = sys_ecall(fd, (long)buf, count, 0, 63); /* __NR_read */
	if (ret < 0) {
		r->_errno = -ret;
		return -1;
	}
	return (int)ret;
}

int _close_r(struct _reent *r, int fd) {
	long ret = sys_ecall(fd, 0, 0, 0, 57); /* __NR_close */
	if (ret < 0) {
		r->_errno = -ret;
		return -1;
	}
	return 0;
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t ptr, int dir) {
	uint64_t res = 0;
	uint32_t off_hi = (uint32_t)((uint64_t)ptr >> 32);
	uint32_t off_lo = (uint32_t)ptr;
	long ret = sys_ecall6(fd, off_hi, off_lo, (long)&res, dir, 0, 62); /* __NR_llseek */
	if (ret < 0) {
		r->_errno = -ret;
		return (_off_t)-1;
	}
	return (_off_t)res;
}

int _fstat_r(struct _reent *r, int fd, struct stat *st) {
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG;
	return 0;
}

int _isatty_r(struct _reent *r, int fd) {
	return 1;
}

void *_sbrk_r(struct _reent *r, ptrdiff_t incr) {
	static long cur_brk = 0;
	if (cur_brk == 0) {
		cur_brk = sys_ecall(0, 0, 0, 0, 214); /* __NR_brk */
	}
	long prev_brk = cur_brk;
	long new_brk = cur_brk + incr;
	long res = sys_ecall(new_brk, 0, 0, 0, 214);
	if (res < new_brk && incr > 0) {
		r->_errno = ENOMEM;
		return (void *)-1;
	}
	cur_brk = res;
	return (void *)prev_brk;
}

int _kill_r(struct _reent *r, int pid, int sig) {
	return sys_ecall(pid, sig, 0, 0, 129); /* __NR_kill */
}

int _getpid_r(struct _reent *r) {
	return sys_ecall(0, 0, 0, 0, 172); /* __NR_getpid */
}

void _exit(int status) {
	sys_ecall(status, 0, 0, 0, 93); /* __NR_exit */
	while (1);
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return (void *)sys_ecall6((long)addr, length, prot, flags, fd, offset, 222); /* __NR_mmap */
}

int sys_ftruncate(int fd, off_t length) {
	uint32_t len_lo = (uint32_t)length;
	uint32_t len_hi = (uint32_t)((uint64_t)length >> 32);
	return sys_ecall(fd, len_lo, len_hi, 0, 46); /* __NR_ftruncate */
}

struct timespec64 {
	long long tv_sec;
	long long tv_nsec;
};

int sys_clock_gettime(int clk_id, struct timespec64 *ts) {
	long ret = sys_ecall(clk_id, (long)ts, 0, 0, 403); /* __NR_clock_gettime64 */
	if (ret != 0) {
		ret = sys_ecall(clk_id, (long)ts, 0, 0, 113); /* fallback __NR_clock_gettime */
	}
	return ret;
}

/* Install/query a signal action. sigsetsize is 8 bytes on rv32. */
int sys_rt_sigaction(int sig, void *act, void *oldact) {
	return (int)sys_ecall(sig, (long)act, (long)oldact, 8, 134); /* __NR_rt_sigaction */
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
	struct timespec64 lreq, lrem;
	if (!req) return -1;
	lreq.tv_sec = req->tv_sec;
	lreq.tv_nsec = req->tv_nsec;
	long ret = sys_ecall6(0 /* CLOCK_REALTIME */, 0, (long)&lreq, (long)&lrem, 0, 0, 407); /* __NR_clock_nanosleep_time64 */
	if (rem && ret == 0) {
		rem->tv_sec = lrem.tv_sec;
		rem->tv_nsec = lrem.tv_nsec;
	}
	return ret;
}

int _fcntl_r(struct _reent *r, int fd, int cmd, int arg) {
	return sys_ecall(fd, cmd, arg, 0, 25); /* __NR_fcntl */
}

int fcntl(int fd, int cmd, ...) {
	va_list va;
	va_start(va, cmd);
	int arg = va_arg(va, int);
	va_end(va);
	return sys_ecall(fd, cmd, arg, 0, 25); /* __NR_fcntl */
}

int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) {
	if (tv) {
		struct timespec64 ts;
		sys_clock_gettime(0 /* CLOCK_REALTIME */, &ts);
		tv->tv_sec = (time_t)ts.tv_sec;
		tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
	}
	return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
	return _gettimeofday_r(&global_reent, tv, tz);
}

void rv32_syscalls_init(void) {
	_REENT_INIT_PTR(&global_reent);

	/* Ignore SIGPIPE so broken socket/pipe does not terminate process */
	struct {
		void *handler;
		unsigned long flags;
		void *restorer;
		uint32_t mask[2];
	} sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = (void *)1; /* SIG_IGN */
	sys_ecall6(13 /* SIGPIPE */, (long)&sa, 0, 8 /* sizeof mask */, 0, 0, 134 /* __NR_rt_sigaction */);
}
