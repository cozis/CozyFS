#include <stdio.h>
#include "cozyfs_core.h"

#define TEST_START do { char mem[1<<16]; CozyFS fs; cozyfs_init(mem, sizeof(mem), 0, 0); cozyfs_attach(&fs, mem, (void*) 0, cozyfs_callback_impl, (void*) 0);
#define TEST_END } while (0);
#define TEST_RESTART TEST_END; TEST_START;

#define TEST_ASSERT(X) if (!(X)) { printf("Test failed at %s:%d\n", __FILE__, __LINE__); break; }

static void test_mkdir(void)
{
	TEST_START;
	TEST_ASSERT(cozyfs_mkdir(&fs, "???") == COZYFS_EBADF);
	TEST_RESTART;
	TEST_ASSERT(cozyfs_mkdir(&fs, "???") == COZYFS_EBADF);
	TEST_END;
}

int main(void)
{
	test_mkdir();
	return 0;
}

/*

int  cozyfs_init   (void *mem, unsigned long len, int backup, int refresh);
void cozyfs_attach (CozyFS *fs, void *mem, const char *user, cozyfs_callback callback, void *userptr);
void cozyfs_idle   (CozyFS *fs);

int  cozyfs_link   (CozyFS *fs, const char *oldpath, const char *newpath);
int  cozyfs_unlink (CozyFS *fs, const char *path);

int  cozyfs_mkdir  (CozyFS *fs, const char *path);
int  cozyfs_rmdir  (CozyFS *fs, const char *path);

int  cozyfs_mkusr  (CozyFS *fs, const char *name);
int  cozyfs_rmusr  (CozyFS *fs, const char *name);

int  cozyfs_chown  (CozyFS *fs, const char *path, const char *newowner);
int  cozyfs_chmod  (CozyFS *fs, const char *path, int mode);

int  cozyfs_open   (CozyFS *fs, const char *path);
int  cozyfs_close  (CozyFS *fs, int fd);

int  cozyfs_read   (CozyFS *fs, int fd, void       *dst, int max);
int  cozyfs_write  (CozyFS *fs, int fd, const void *src, int len);

int  cozyfs_transaction_begin    (CozyFS *fs, int lock);
int  cozyfs_transaction_commit   (CozyFS *fs);
int  cozyfs_transaction_rollback (CozyFS *fs);
*/