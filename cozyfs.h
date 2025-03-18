/*
Copyright (c) 2025 Francesco Cozzuto

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#ifndef COZYFS_H
#define COZYFS_H

// How many pages a process is allowed to touch in a transaction
#define COZYFS_MAX_PATCHES 128

enum {
	COZYFS_OK,
	COZYFS_EINVAL,
	COZYFS_ENOMEM,
	COZYFS_ENOENT,
	COZYFS_EPERM,
	COZYFS_EBUSY,
	COZYFS_EISDIR,
	COZYFS_ENFILE,
	COZYFS_EBADF,
	COZYFS_ETIMEDOUT,
	COZYFS_ECORRUPT,
	COZYFS_ESYSFREE,
	COZYFS_ESYSSYNC,
	COZYFS_ESYSTIME,
};

enum {
	COZYFS_FCONSUME = 1 << 0,
};

enum {
	COZYFS_SYSOP_MALLOC,
	COZYFS_SYSOP_FREE,
	COZYFS_SYSOP_WAIT,
	COZYFS_SYSOP_WAKE,
	COZYFS_SYSOP_SYNC,
	COZYFS_SYSOP_TIME,
};

enum {
	COZYFS_SYSRES_OK,
	COZYFS_SYSRES_ERROR,
	COZYFS_SYSRES_UNDEFINED,
};

typedef unsigned long long (*cozyfs_callback)(int sysop, void *userptr, void *p, int n);

typedef struct {
	const void*        mem;
	void*              userptr;
	cozyfs_callback    callback;
	unsigned long long ticket;
	int                transaction;
	int                patch_count;
	unsigned int       patch_offs[COZYFS_MAX_PATCHES];
	void*              patch_ptrs[COZYFS_MAX_PATCHES];
} CozyFS;

void cozyfs_init(void *mem, unsigned long len, int backup);
void cozyfs_attach(CozyFS *fs, void *mem, cozyfs_callback callback, void *userptr);
void cozyfs_idle(CozyFS *fs);

int  cozyfs_link(CozyFS *fs, const char *oldpath, const char *newpath);
int  cozyfs_unlink(CozyFS *fs, const char *path);

int  cozyfs_mkdir(CozyFS *fs, const char *path);
int  cozyfs_rmdir(CozyFS *fs, const char *path);

int  cozyfs_transaction_begin(CozyFS *fs, int lock);
int  cozyfs_transaction_commit(CozyFS *fs);
int  cozyfs_transaction_rollback(CozyFS *fs);

int  cozyfs_open(CozyFS *fs, const char *path);
int  cozyfs_close(CozyFS *fs, int fd);

int  cozyfs_read(CozyFS *fs, int fd, void *dst, int max, int flags);

#endif // COZYFS_H