#if defined(__linux__)
#	define OS_LINUX   1
#	define OS_WINDOWS 0
#elif defined(_WIN32)
#	define OS_LINUX   0
#	define OS_WINDOWS 1
#endif

#if OS_LINUX
#	include <unistd.h>
#	include <sys/mman.h>
#	include <sys/syscall.h>
#	define CLOCK_REALTIME 0
#elif OS_WINDOWS
#	define WIN32_MEAN_AND_LEAN
#	include <windows.h>
#endif

#include "cozyfs.h"

static int wait(u64 *word, u64 old_word, int timeout_ms)
{
#if defined(_WIN32)

	if (!WaitOnAddress((volatile VOID*) word, (PVOID) &old_word, sizeof(u64), timeout_ms < 0 ? INFINITE: (DWORD) timeout_ms))
		return -EAGAIN;
	return 0;

#elif defined(__linux__)

	struct timespec ts;
	struct timespec *tsptr;

	if (timeout_ms < 0)
		tsptr = NULL;
	else {
		ts.tv_sec = timeout_ms * / 1000;
		ts.tv_nsec = (timeout_ms % 1000) * 1000000;
	}

	errno = 0;
	long ret = futex((unsigned int*) word, FUTEX_WAIT, (unsigned int) old_word, tsptr, NULL, 0);
	if (ret == -1 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT)
		return -errno;
	return 0;

#endif
}

static int wake(u64 *word)
{
#if defined(_WIN32)
	WakeByAddressAll((PVOID) word);
	return 0;
#elif defined(__linux__)
	errno = 0;
	long ret = futex((unsigned int*) word, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
	if (ret < 0) return -errno;
	// TODO: Don't return an error on EAGAIN or EINTR
	return 0;
#endif
}

static unsigned long long
callback(int sysop, void *userptr, void *p, int n);

int main()
{
	char mem[1<<20];
	cozyfs_init(mem, sizeof(mem), 0);

	CozyFS fs;
	cozyfs_attach(&fs, mem, callback, NULL);

	// TODO

	return 0;
}

static unsigned long long
callback(int sysop, void *userptr, void *p, int n)
{
	switch (sysop) {

		case COZYFS_SYSOP_MALLOC:
		{
#if OS_LINUX
			void *addr = mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
			if (*addr == MAP_FAILED)
				return NULL;
			return addr;
#elif OS_WINDOWS
			return VirtualAlloc(NULL, n, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
#else
			return NULL;
#endif
		}
		break;

		case COZYFS_SYSOP_FREE:
		{
			// Note that "n" contains the length of the region to free,
			// and always matches the number of bytes initially requested.
#if OS_LINUX
			return !munmap(p, n);
#elif OS_WINDOWS
			return VirtualFree(p, n, MEM_RELEASE);
#else
			return 0;
#endif
		}
		break;

		case COZYFS_SYSOP_WAIT:
		???
		break;

		case COZYFS_SYSOP_WAKE:
		???
		break;

		case COZYFS_SYSOP_SYNC:
		// We aren't backing the file system with a file, so we don't need this
		break;

		case COZYFS_SYSOP_TIME:
		{
#if OS_LINUX
			struct timespec ts;
			int result = syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
			if (result)
				return 0;
			return ts.tv_sec;
#elif OS_WINDOWS
			FILETIME ft;
			GetSystemTimeAsFileTime(&ft);

			ULARGE_INTEGER uli;
			uli.LowPart = ft.dwLowDateTime;
			uli.HighPart = ft.dwHighDateTime;
					
			// Convert Windows file time (100ns since 1601-01-01) to 
			// Unix epoch time (seconds since 1970-01-01)
			// 116444736000000000 = number of 100ns intervals from 1601 to 1970
			return (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
#else
			return 0;
#endif
		}
		break;
	}

	/* unreachable */
	return -1;
}
