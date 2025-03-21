////////////////////////////////////////////////////////////////////////////////////////////
// Detect platform

#if defined(_WIN32)
#define OS_LINUX   0
#define OS_WINDOWS 1
#elif defined(__linux__)
#define OS_LINUX   1
#define OS_WINDOWS 0
#else
#error "Only Linux and Windows are supported"
#endif

////////////////////////////////////////////////////////////////////////////////////////////
// Inclusions

#include <stdio.h>

#if OS_WINDOWS
#define WIN32_MEAN_AND_LEAN
#include <windows.h>
#endif

#if OS_LINUX
#include <sys/mman.h>
#include <pthread.h>
#endif

#include "http.h"
#include <cozyfs.h>

////////////////////////////////////////////////////////////////////////////////////////////
// Types

typedef unsigned int       u32;
typedef unsigned long long u64;

typedef struct {
	char *ptr;
	int   len;
} string;

#define S(X) ((string) { (X), sizeof(X)-1 })

#if OS_WINDOWS
typedef HANDLE Thread;
typedef DWORD  TReturn;
#endif

#if OS_LINUX
typedef pthread_t     Thread;
typedef unsigned long TReturn;
#endif

typedef struct {
#if OS_WINDOWS
	HANDLE hFile;
	HANDLE hMapFile;
#else
	int fd;
#endif
	void *ptr;
	u64   len;
} SharedMemory;

////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

static Thread thread_spawn         (TReturn (*func)(void*), void *arg);
static void   thread_join          (Thread thread);

static int    shared_memory_flush  (SharedMemory shm);
static void   shared_memory_delete (SharedMemory shm);
static int    shared_memory_create (SharedMemory *shm, const char *name, u64 len, int is_file);

static void   run_shell            (CozyFS *fs);

////////////////////////////////////////////////////////////////////////////////////////////
// Utilities

static int streq(string a, string b)
{
	if (a.len != b.len)
		return 0;
	for (int i = 0; i < a.len; i++)
		if (a.ptr[i] != b.ptr[i])
			return 0;
	return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Entry point

static void http(void *ptr)
{
	CozyFS *fs = ptr;
	cozyfs_http_serve("127.0.0.1", 8080, fs);
}

static void fuse(void *ptr)
{
	CozyFS *fs = ptr;
	cozyfs_fuse();
}

static void usage(char *self, FILE *stream)
{
	fprintf(stream, "Usage: %s ..options..\n", self);
	fprintf(stream, "OPTIONS:\n"
		"  --shared   Map the state into shared memory\n"
		"  --persist  Map the state to a file\n"
		"  --http     Expose the state over HTTP\n"
		"  --shell    Start a shell into cozyfs\n");
}

int main(int argc, char **argv)
{
	// OPTIONS:
	//   --shared   Map the state into shared memory
	//   --persist  Map the state to a file
	//   --http     Expose the state over HTTP
	//   --shell    Start a shell into cozyfs

	int shared  = 0;
	int persist = 0;
	int http    = 0;
	int shell   = 0;
	int fuse    = 0;

	for (int i = 1; i < argc; i++) {

		if (!strcmp("-h", argv[i]) || !strcmp("--help", argv[i])) {
			usage(argv[0], stdout);
			return 0;
		}

		if (!strcmp("--shared", argv[i])) {
			shared = 1;
		} else if (!strcmp("--persist", argv[i])) {
			persist = 1;
		} else if (!strcmp("--http", argv[i])) {
			http = 1;
		} else if (!strcmp("--shell", argv[i])) {
			shell = 1;
		} else if (!strcmp("--fuse", argv[i])) {
			fuse = 1;
		} else {
			usage(argv[0], stderr);
			return -1;
		}
	}

	CozyFS fs;
	SharedMemory shm;
	Thread http_thread;
	Thread fuse_thread;

	int code = shared_memory_create(&shm, name, len, persist);
	if (code < 0) {
		// TODO
		return -1;
	}

	// TODO: prepare the cozyfs instance
	cozyfs_attach(&fs, shm.ptr, ???, cozyfs_callback_impl, NULL);

	if (http) http_thread = thread_spawn(http, &fs);
	if (fuse) fuse_thread = thread_spawn(fuse, &fs);

	if (shell) run_shell(&fs);

	if (http) thread_join(http_thread);
	if (fuse) thread_join(fuse_thread);

	shared_memory_delete(shm);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////77
// Threading

static Thread thread_spawn(TReturn (*func)(void*), void *arg)
{
#if defined(_WIN32)
	HANDLE thread = CreateThread(NULL, 0, func, arg, 0, NULL);
	if (thread == INVALID_HANDLE_VALUE)
		abort();
	return thread;
#else
	pthread_t thread;
	int r = pthread_create(&thread, NULL, (void*) func, arg);
	if (r)
		abort();
	return thread;
#endif
}

static void thread_join(Thread thread)
{
#if defined(_WIN32)
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	pthread_join(thread, NULL);
#endif
}

static int shared_memory_create(SharedMemory *shm, const char *name, u64 len, int is_file)
{
#if OS_WINDOWS
	STATIC_ASSERT(sizeof(DWORD) == 4);
	u32 len_hi = len >> 32;
	u32 len_lo = len & 0xFFFFFFFF;

	HANDLE hFile;
	HANDLE hMapFile;
	void *ptr;
	if (is_file) {
		hFile = CreateFile(
			name,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);
		if (hFile == INVALID_HANDLE_VALUE)
			return -1;

		DWORD dwFileSize = len;
		SetFilePointer(hFile, dwFileSize, NULL, FILE_BEGIN);
		SetEndOfFile(hFile);

		hMapFile = CreateFileMapping(
			INVALID_HANDLE_VALUE, // Use paging file
			NULL, // Default security attributes
			PAGE_READWRITE, // Access mode
			len_hi, len_lo,
			name
		);
		if (hFile == NULL) {
			CloseHandle(hFile);
			CloseHandle(hMapFile);
			return -1;
		}

		ptr = MapViewOfFile(
			hMapFile,
			FILE_MAP_ALL_ACCESS,
			0, 0,
			0
		);

		if (ptr == NULL) {
			CloseHandle(hFile);
			CloseHandle(hMapFile);
			return -1;
		}

	} else {

		hFile = INVALID_HANDLE_VALUE;

		hMapFile = CreateFileMapping(
			INVALID_HANDLE_VALUE, // Use paging file
			NULL, // Default security attributes
			PAGE_READWRITE, // Access mode
			len_hi, len_lo,
			name
		);
		if (hFile == NULL)
			return -1;

		ptr = MapViewOfFile(
			hMapFile,
			FILE_MAP_ALL_ACCESS,
			0, 0,
			len
		);

		if (ptr == NULL) {
			CloseHandle(hMapFile);
			return -1;
		}
	}

	shm->hFile = hFile;
	shm->hMapFile = hMapFile;
	shm->ptr = ptr;
	shm->len = len;
	return 0;
#elif OS_LINUX
	int fd;
	
	if (is_file) {
		fd = open(name, O_CREAT | O_RDWR, 0666);
		if (fd == -1)
			return -1;
		if (lseek(fd, FILE_SIZE-1, SEEK_SET) == -1) {
			close(fd);
			return -1;
		}
		if (write(fd, "", 1) == -1) {
			close(fd);
			return -1;
		}
	} else {
		fd = shm_open(name, O_CREAT | O_RDWR, 0666);
		if (fd == -1)
			return -1;
		if (ftruncate(fd, len) == -1) {
			close(fd);
			return -1;
		}
	}

	void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		close(fd);
		return -1;
	}

	shm->fd = fd;
	shm->ptr = ptr;
	shm->len = len;
	return 0;
#else
	return -1;
#endif
}

static void shared_memory_delete(SharedMemory shm)
{
#if OS_WINDOWS
	UnmapViewOfFile(shm.ptr);
	if (shm.hFile != INVALID_HANDLE_VALUE)
		CloseHandle(shm.hFile);
	CloseHandle(shm.hMapFile);
#elif OS_LINUX
	munmap(shm.ptr, shm.len);
	close(shm.fd);
#endif
}

static int shared_memory_flush(SharedMemory shm)
{
#if OS_WINDOWS
	if (!FlushViewOfFile(shm.ptr, 0))
		return -1;
	return 0;
#elif OS_LINUX
	if (msync(shm.ptr, shm.len, MS_SYNC) == -1)
		return -1;
	return 0;
#else
	return -1;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////77
// Shell

static void run_ls(string args, CozyFS *fs)
{
	printf("running ls\n");
	// TODO
}

static void run_cat(string args, CozyFS *fs)
{
	printf("running cat\n");
	// TODO
}

static void run_shell(CozyFS *fs)
{
	for (;;) {

		char cmd[1<<13];
		int  len;

		for (;;) {
			char c = getc(stdin);
			if (c == '\n') break;

			if (len < cmd)
				cmd[len] = c;
			len++;
		}

		if (len > sizeof(cmd)) {
			printf("Error: Command too long\n");
			continue;
		}

		string args[32];
		int num_args = 0;

		int i = 0;
		for (;;) {

			while (i < len && (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\r' || cmd[i] == '\n'))
				i++;

			if (i == len)
				break;

			int off = i;
			while (i < len && cmd[i] != ' ' && cmd[i] != '\t' &&  cmd[i] != '\r' || cmd[i] != '\n')
				i++;

			if (num_args == COUNT(args)) {
				// TODO
			}
			args[num_args++] = (string) { cmd + off, i - off };
		}

		if (num_args == 0)
			continue;

		struct {
			string str;
			void (*fun)(string*);
		} table[] = {
			{ S("ls"),  run_ls  },
			{ S("cat"), run_cat }
		};

		int found = 0;
		for (int i = 0; i < COUNT(table); i++)
			if (streq(args[0], table[i].str)) {
				table[i].fun(args, fs);
				found = 1;
				break;
			}
		if (!found)
			printf("Error: Unknown command '%.*s'\n",
				args[i].len, args[i].ptr);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////77
// Bye!