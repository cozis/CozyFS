/*
=== CozyFS =============================================================================

CozyFS is a serialization-friendly in-memory file system that supports crash recovery,
transactions, and zero-copy operations. You can back it up with a memory-mapped file to
offer ACID guarantees.

    USE CASE
The file sistem will only use the single large memory buffer provided by the user. The
file system state does not depend on its position, so the buffer can be copied somewhere
else and a new handle to it can be created with cozyfs_attach. All the state will stay
the same. A consequence of this is the file system can be backed by a memory mapped file
making it persistent, or shared between processes by using a shared memory buffer, or
both.

    CONCURRENCY
Concurrent access will be managed with a single writer and multiple reader approach.
Since readers don't modify the state they can read concurrently. I think this is a common
pattern.

TODO: We are not using multiple readers single writers anymore

    CRASH RECOVERY
It's possible for processes to crash when using the file system, causing it to stay in
an invalid state. To avoid this, the user-provided memory is divided in two halves, each
with one instance of the file system. One is read-only and used as backup. Once in a while,
when the writeable half is in a consistent state, a flag is toggled atomically which
instantly marks the previously writeable region as backup, inverting the roles of the two
halves. The process then copies the contents of the backup in the newly writeable half
and leaves the critical section. When a process crashes while using the file system
(potentially leaving it in an invalid state) it will leave the lock acquired, causing the
others to wait indefinitely. To avoid this, the lock is made with a timeout. It is free
when either it's not set or it's expired. When a process enters the critical section because
the lock expired, it means the writeable half is invalid and therefore continues from the
backup. I was thinking of making the lock using a 64bit word containing either zero or the
timeout UTC timestamp. The lock is acquired when the word contains a time in the future,
and is released when it's either zero or a time in the past.

    TRANSACTIONS
Internally, the file system splits the buffer into chunks. To make the state position-independant,
all pointers are stored relative to the start of the buffer. Any time a process is reading
or writing to the file system, it must convert the relative pointers to absolute. My idea
is to use chunk indices as pointer, therefore I'll have a function ptr2chnk which converts
the index to pointer. When a process starts a transaction, any chunk that is about to be
modified is instead copied to keep the real one intact. When the transaction bit is set,
the ptr2chnk copies any chunk it would return and returns the pointer to the copy, then the
address to the copy (and the index of the original chunk) is stored in a process-local table.
When the same pointer is resolved, the copy is returned again. This makes it so processes
in a transaction can see their own modifications but others can't. If the process crashes,
all modifications are lost. If the process rolls back the transaction, simply all copies are
deallocated. If the process commits, it copies all chunk copies at once. If someone else
modified the file system in the mean time, the chunks are freed and the transaction fails

    REPLICATION IDEAS
It should be possible to extend CozyFS to be replicated over the network with varying
degrees of consistency. My idea of how one would do so follows.

Processes that want to share state over the network must instanciate a file system in
shared memory, then spawn a daemon which connects to it and makes sure it's synchronized
with remote replicas. One of these daemons behaves as master. The synchronization process
is transparet for users of the file system. Read and write operations procede normally.
Once in a while the daemon sends the state of the file system to master. The master can
either apply the changes or reject them. If they are rejected, the process must apply
the master's state and lose the changes. When users of the file system perform a transaction,
the write goes directly to the master, which may accept the changes or not. If the
transaction succedes, the changes are permanent.

=== LICENSE ============================================================================

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

========================================================================================
*/

#include "cozyfs.h"

#define COMPILER_MSVC  0
#define COMPILER_GCC   1
#define COMPILER_CLANG 0

typedef unsigned char       u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef long long s64;

typedef u32 Offset;

#define INVALID_OFFSET ((Offset) -1LL)

#define COUNT(X) sizeof(X)/sizeof((X)[0])

#define NULL ((void*) 0)

#define OFFSETOF(TYPE, MEMBER) (((TYPE*) 0)->MEMBER)

typedef struct { u8 *data; int size; } string;

enum {
	ENTITY_DIR = 1 << 0,
	ENTITY_FILE = 1 << 1,
};

enum {
	TRANSACTION_OFF,
	TRANSACTION_ON,
	TRANSACTION_ON_WITH_LOCK,
	TRANSACTION_TIMEOUT,
};

enum {
	BACKUP_NO = -1,
	BACKUP_HALF_ACTIVE = 1,
	BACKUP_HALF_INACTIVE = 0,
};

#define MAX_NAME 256 // TODO

typedef struct {
	Offset off;
	u8     name[128];
} Link;

typedef struct {
	u32    refs;
	u32    flags;
	Offset head;
	Offset tail;
	u16    head_start;
	u16    tail_end;
} Entity;

typedef struct {
	
	// Linked lists of dpages for a directory
	Offset prev;
	Offset next;

	// List of links for this directory
	Link links[28];

	// List of entities. This may or may not be associated
	// to the directory
	Entity ents[28];

	// Make sure the page struct is 4K
	char pad[56];

} DPage;
_Static_assert(sizeof(DPage) == 4096);

typedef struct {

	Offset prev;
	Offset next;

	char data[4088];

} FPage;
_Static_assert(sizeof(FPage) == 4096);

typedef struct {
	u8     used;
	u16    gen;
	Offset entity;
	Offset cursor;
} Handle;

typedef struct {
	volatile u64 lock;
	volatile int backup; // All volatile fields must come before "backup"
	u64 last_backup_time;
	Offset free_dpages;
	Offset free_pages;
	int tot_pages;
	int num_pages;
	Entity root;
	Handle handles[8];
} RPage;

static Entity *find_unused_entity(CozyFS *fs)
{
	// First, look in the already modified pages
	for (int i = 0; i < fs->patch_count; i++) {
		// TODO
	}
}

static const void *off2ptr(CozyFS *fs, Offset off)
{
	if (off == INVALID_OFFSET)
		return NULL;

	Offset byte_off = off & (4096-1);
	Offset page_off = off - byte_off;

	for (int i = 0; i < fs->patch_count; i++)
		if (fs->patch_offs[i] == page_off)
			return (char*) fs->patch_ptrs[i] + byte_off;

	return (const char*) fs->mem + off;
}

static Offset ptr2off(CozyFS *fs, const void *ptr)
{
	if (ptr == NULL)
		return INVALID_OFFSET;

	for (int i = 0; i < fs->patch_count; i++) {
		void *page_ptr = fs->patch_ptrs[i];
		if (ptr >= page_ptr && ptr < page_ptr + 4096)
			return fs->patch_offs[i] + (ptr - page_ptr);
	}

	return ptr - (void*) fs->mem;
}

static void *writable_addr(CozyFS *fs, const void *ptr)
{
	if (fs->transaction != TRANSACTION_OFF) {

		// If we already have a patch for this pointer, return it
		int i = 0;
		while (i < fs->patch_count && (fs->patch_ptrs[i] > ptr || fs->patch_ptrs[i] + 4096 <= ptr))
			i++;

		// Patch found. Return the pointer as-is
		if (i < fs->patch_count)
			return ptr;

		// We need to create a new patch
		if (fs->patch_count == COUNT(fs->patch_offs))
			return NULL; // Path limit reached

		// Ask the user for a new page
		void *page_copy = sys_alloc(fs, 4096);
		if (page_copy == NULL)
			return NULL;

		// Copy the page
		Offset byte_off = (ptr - fs->mem) & (4096 - 1);
		void  *page_ptr = ptr - byte_off;

		my_memcpy(page_copy, page_ptr, 4096);

		// Make the patch
		fs->patch_ptrs[fs->patch_count] = page_copy;
		fs->patch_offs[fs->patch_count] = byte_off;
		fs->patch_count++;

		ptr = page_copy + byte_off;
	}

	return ptr;
}

int free_entity(CozyFS *fs, const Entity *entity)
{
	Entity *writable_entity = writable_addr(fs, entity);
	if (writable_entity == NULL)
		return -COZYFS_ENOMEM;

	writable_entity->refs--;
	if (writable_entity->refs > 0)
		return COZYFS_OK;

	// TODO

	return COZYFS_OK;
}

static const Entity *find_entity(CozyFS *fs, const Entity *parent, string name)
{
	const DPage *dpage = off2ptr(fs, parent->head);
	while (dpage) {

		int i = 0;
		while (i < COUNT(dpage->links) && dpage->links[i].off != INVALID_OFFSET) {

			int link_name_len = my_strlen(dpage->links[i].name);
			if (name.size == link_name_len && !my_memcmp(name.data, dpage->links[i].name, name.size))
				return off2ptr(fs, dpage->links[i].off);
			i++;
		}

		dpage = off2ptr(fs, dpage->next);
	}

	return NULL;
}

static int create_entity(CozyFS *fs, const Entity *parent, const Entity *target, string name, u32 flags)
{
	if (name.size > MAX_NAME)
		return -COZYFS_EINVAL;

	// TODO: Check that the entity is a directory

	const Entity *entity = find_entity(fs, parent, name);
	if (entity) {
		// TODO: Entity exists already
	}

	// TODO: Check if the directory is empty

	const DPage *tail = off2ptr(fs, parent->tail);

	int i = 0;
	while (i < COUNT(tail->links) && tail->links[i].off != INVALID_OFFSET)
		i++;
	
	if (i == COUNT(tail->links)) {
		// TODO: Tail is full
	}

	DPage *writable_tail = writable_ptr(fs, tail);
	if (writable_tail == NULL)
		return -COZYFS_ENOMEM;

	if (target) {

		Entity *writable_target = writable_ptr(fs, target);
		if (writable_target == NULL)
			return -COZYFS_ENOMEM;

		writable_target->refs++;
		writable_tail->links[i].off = ptr2off(fs, target);

	} else {
		int j = 0;
		while (j < COUNT(writable_tail->ents) && writable_tail->ents[j].refs > 0)
			j++;

		Entity *ent;
		if (j == COUNT(writable_tail->ents)) {
			// TODO: Current dpage is full of entities. Find them somewhere else
			ent = find_unused_entity(fs);
			if (ent == NULL)
				return -COZYFS_ENOMEM;
		} else {
			ent = &writable_tail->ents[j];
		}

		writable_tail->links[i].off = ptr2off(fs, ent);

		ent->refs = 1;
		ent->head = INVALID_OFFSET;
		ent->tail = INVALID_OFFSET;
		ent->head_start = 0;
		ent->tail_end = 0;
	}

	my_memset(writable_tail->links[i].name, 0, MAX_NAME);
	my_memcpy(writable_tail->links[i].name, name.data, name.size);

	return COZYFS_OK;
}

static int remove_entity(CozyFS *fs, const Entity *parent, string name, u32 flags)
{
	int i;
	const DPage *dpage = off2ptr(fs, parent->head);
	while (dpage) {

		i = 0;
		while (i < COUNT(dpage->links) && dpage->links[i].off != INVALID_OFFSET) {

			int link_name_len = my_strlen(dpage->links[i].name);
			if (name.size == link_name_len && !my_memcmp(name, dpage->links[i].name, name.size))
				return off2ptr(fs, dpage->links[i].off);
			i++;
		}

		dpage = off2ptr(fs, dpage->next);
	}

	if (dpage == NULL)
		return -COZYFS_ENOENT;

	const Link *link = &dpage->links[i];
	const Entity *entity = off2ptr(fs, link->off);

	Entity *writable_parent = writable_ptr(fs, parent);
	if (writable_parent == NULL)
		return COZYFS_ENOMEM;

	DPage *writable_dpage = writable_ptr(fs, dpage);
	if (writable_dpage == NULL)
		return -COZYFS_ENOMEM;

	int code = free_entity(fs, entity);
	if (code != COZYFS_OK)
		return code;

	const DPage *tail = off2ptr(fs, parent->tail);
	writable_dpage->links[i] = tail->links[--writable_parent->tail_end];

	return COZYFS_OK;
}

static u64 atomic_load(volatile u64 *ptr)
{
#if COMPILER_MSVC
	return *ptr; // TODO: Is this read atomic?
#elif COMPILER_GCC || COMPILER_CLANG
	return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#endif
}

static void atomic_store(volatile u64 *ptr, u64 val)
{
#if COMPILER_MSVC
	_InterlockedExchange64((volatile s64*) ptr, 0);
#elif COMPILER_GCC || COMPILER_CLANG
	__atomic_store_n(ptr, 0, __ATOMIC_RELEASE);
#endif
}

static int atomic_compare_exchange(volatile u64 *ptr, u64 expect, u64 new_value)
{
#if COMPILER_MSVC
	return (u64) _InterlockedCompareExchange64((volatile s64*) ptr, new_value, expect) == expect;
#elif COMPILER_GCC || COMPILER_CLANG
	return __atomic_compare_exchange_n(ptr, &expect, new_value, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif
}

static int restore_backup(CozyFS *fs)
{
	const RPage *root = fs->mem;

	int backup = atomic_load(&root->backup);
	if (backup == BACKUP_NO)
		return 0;
	
	// Copy the inactive region into the active
	// (and corrupted) one. Don't copy the volatile
	// fields though.

	char *active = root;
	char *inactive = root + root->tot_pages;

	int skip = OFFSETOF(RPage, backup) + sizeof(root->backup);
	my_memcpy(active + skip, inactive + skip, root->tot_pages * 4096 - skip);

	return 1;
}

static void perform_backup(CozyFS *fs, int not_before_sec)
{
	RPage *root = fs->mem;

	int backup = atomic_load(&root->backup);
	if (backup == BACKUP_NO)
		return;

	u64 now = timestamp_utc(fs);
	if (now < root->last_backup_time + not_before_sec)
		return;

	atomic_store(&root->backup, !backup);

	// TODO
	my_memcpy(???);

	atomic_store(&root->last_backup_time, now);
}

static int trylock(CozyFS *fs, int acquire_timeout_sec, int *timedout)
{
	RPage *root = fs->mem;

	u64 current = atomic_load(&root->lock);

	if (current == 1)
		return -COZYFS_EBUSY; // Locked with no timeout

	u64 now = timestamp_utc();

	// Try acquiring the lock if it timed out
	if (current < now) {
		if (!atomic_compare_exchange(&root->lock, current, now + acquire_timeout_sec))
			return -COZYFS_EBUSY;
		return COZYFS_OK;
	}

	// Lock didn't expire yet
	return -COZYFS_EBUSY;
}

static void unlock(CozyFS *fs)
{
	RPage *root = fs->mem;

	atomic_store(&root->lock, 0);
	fs->timeout = atomic_load(&root->lock);
}

static int refresh_lock(CozyFS *fs, int postpone_sec)
{
	RPage *root = fs->mem;

	u64 now = timestamp_utc(fs);
	if (!atomic_compare_exchange(&root->lock, fs->timeout, now + postpone_sec)) {
		// Timed out
		fs->timeout = 0;
		return -COZYFS_ETIMEOUT;
	}

	// Lock refreshed
	fs->timeout = now + postpone_sec;
	return COZYFS_OK;
}

static int enter_critical_section(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_TIMEOUT)
		return -COZYFS_ETIMEOUT;

	if (fs->transaction == TRANSACTION_ON_WITH_LOCK) {
		int code = refresh_lock(fs, 5);
		if (code != COZYFS_OK) {
			fs->transaction == TRANSACTION_TIMEOUT;
			return code;
		}
	} else {
		int timedout;
		int code = trylock(fs, 5, &timedout);
		if (code != COZYFS_OK)
			return code;

		// We entered the critical section because previous
		// acquire timed out. We must assume the state is
		// invalid.
		if (timedout)
			if (!restore_backup(fs))
				return -COZYFS_ECORRUPT;
	}

	return COZYFS_OK;
}

// TODO: Use this instead of accessing the root page directly
const RPage *get_root(CozyFS *fs)
{
	RPage *root = fs->mem;
	if (root->backup == BACKUP_HALF_INACTIVE)
		root += root->tot_pages;
	return root;
}

static void leave_critical_section(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_TIMEOUT)
		return;

	if (fs->transaction == TRANSACTION_OFF)
		perform_backup(fs, 3);

	if (fs->transaction != TRANSACTION_ON_WITH_LOCK)
		unlock(fs);
}

void cozyfs_init(void *mem, unsigned long len, int backup)
{
	// Align to the size of a pointer
	{
		// TODO
	}

	if (backup)
		len /= 2;

	int tot_pages = len / 4096;
	if (tot_pages == 0)
		return;

	RPage *root = mem;
	root->lock = 0;
	root->backup = backup ? BACKUP_HALF_ACTIVE : BACKUP_NO;
	root->free_dpages = INVALID_OFFSET;
	root->free_pages = INVALID_OFFSET;
	root->tot_pages = tot_pages;
	root->num_pages = 1;

	for (int i = 0; i < COUNT(root->handles); i++) {
		root->handles[i].gen = 1;
		root->handles[i].used = 0;
	}

	if (backup)
		my_memcpy(root + tot_pages, root, tot_pages * 4096);
}

void cozyfs_attach(CozyFS *fs, void *mem, unsigned long len)
{
	// Align to the size of a pointer
	{
		// TODO
	}

	fs->mem = mem;
	fs->transaction = TRANSACTION_OFF;
	fs->patch_count = 0;
}

void cozyfs_idle(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_ON_WITH_LOCK)
		refresh_lock(fs, 5);
	maybe_perform_backup(fs);
}

static int parse_path(string path, string *comps, int max)
{
	if (path.size > 0 && path.data[0] == '/') {
		path.data++;
		path.size--;
		if (path.size == 0)
			return 0; // Absolute paths with no components are allowed
	}

	int num = 0;
	int i = 0;
	for (;;) {

		int off = i;
		while (i < path.size && path.data[i] != '/')
			i++;
		int len = i - off;

		if (len == 0)
			return -COZYFS_EINVAL; // Empty component

		string comp = { path.data + off, len };
		if (comp.size == 2 && comp.data[0] == '.' && comp.data[1] == '.') {
			if (num == 0)
				return -COZYFS_EINVAL; // Path references the parent of the root. TODO: What if the path is absolute?
			num--;
		} else if (comp.size != 1 || comp.data[0] != '.') {
			if (num == max)
				return -COZYFS_ENOMEM; // To many components
			comps[num++] = comp;
		}

		if (i == path.size)
			break;

		ASSERT(path.data[i] == '/');
		i++;

		if (i == path.size)
			break;
	}

	return num;
}

static int link(CozyFS *fs, const char *oldpath, const char *newpath)
{
	string oldpathstr = { oldpath, my_strlen(oldpath) };
	string newpathstr = { newpath, my_strlen(newpath) };

	string oldpathcomps[32];
	int oldpathnum = parse_path(oldpathstr, oldpathcomps, COUNT(oldpathcomps));
	if (oldpathnum < 0) return oldpathnum;

	string newpathcomps[32];
	int newpathnum = parse_path(newpathstr, newpathcomps, COUNT(newpathcomps));
	if (newpathnum < 0) return newpathnum;

	const RPage *root = fs->mem;

	// Resolve the old path
	const Entity *target = &root->root;
	for (int i = 0; i < oldpathnum; i++) {
		target = find_entity(fs, target, oldpathcomps[i]);
		if (target == NULL)
			return -COZYFS_ENOENT;
	}

	if (target->flags & ENTITY_DIR)
		return -COZYFS_EPERM;

	if (newpathnum == 0)
		return -COZYFS_EPERM;

	// Resolve new path up to the last parent
	const Entity *parent = &root->root;
	for (int i = 0; i < newpathnum-1; i++) {
		parent = find_entity(fs, parent, newpathcomps[i]);
		if (parent == NULL)
			return -COZYFS_ENOENT;
	}

	return create_entity(fs, parent, target, newpathcomps[newpathnum-1], ENTITY_FILE);
}

int cozyfs_link(CozyFS *fs, const char *oldpath, const char *newpath)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = link(fs, oldpath, newpath);

	leave_critical_section(fs);
	return code;
}

static int unlink(CozyFS *fs, const char *path)
{
	string pathstr = { path, my_strlen(path) };

	string pathcomps[32];
	int pathnum = parse_path(pathstr, pathcomps, COUNT(pathcomps));
	if (pathnum < 0) return pathnum;

	if (pathnum == 0)
		return -COZYFS_EPERM; // Trying to unlink root

	const RPage *root = fs->mem;

	const Entity *parent = &root->root;
	for (int i = 0; i < pathnum-1; i++) {
		parent = find_entity(fs, parent, pathcomps[i]);
		if (parent == NULL)
			return -COZYFS_ENOENT;
	}

	return remove_entity(fs, parent, pathcomps[pathnum-1], ENTITY_FILE);
}

int cozyfs_unlink(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = unlink(fs, path);

	leave_critical_section(fs);
	return code;
}

static int mkdir(CozyFS *fs, const char *path)
{
	string pathstr = { path, my_strlen(path) };

	string pathcomps[32];
	int pathnum = parse_path(pathstr, pathcomps, COUNT(pathcomps));
	if (pathnum < 0) return pathnum;

	const RPage *root = fs->mem;

	if (pathnum == 0)
		return -COZYFS_EPERM;

	const Entity *parent = &root->root;
	for (int i = 0; i < pathnum-1; i++) {
		parent = find_entity(fs, parent, pathcomps[i]);
		if (parent == NULL)
			return -COZYFS_ENOENT;
	}

	return create_entity(fs, parent, NULL, pathcomps[pathnum-1], ENTITY_DIR);
}

int cozyfs_mkdir(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = mkdir(fs, path);

	leave_critical_section(fs);
	return code;
}

static int rmdir(CozyFS *fs, const char *path)
{
	string pathstr = { path, my_strlen(path) };

	string pathcomps[32];
	int pathnum = parse_path(pathstr, pathcomps, COUNT(pathcomps));
	if (pathnum < 0) return pathnum;

	if (pathnum == 0)
		return -COZYFS_EPERM; // Trying to unlink root

	const RPage *root = fs->mem;

	const Entity *parent = &root->root;
	for (int i = 0; i < pathnum-1; i++) {
		parent = find_entity(fs, parent, pathcomps[i]);
		if (parent == NULL)
			return -COZYFS_ENOENT;
	}

	return remove_entity(fs, parent, pathcomps[pathnum-1], ENTITY_DIR);
}

int cozyfs_rmdir(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = rmdir(fs, path);

	leave_critical_section(fs);
	return code;
}

int cozyfs_transaction_begin(CozyFS *fs, int lock)
{
	if (fs->transaction != TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	if (lock) {
		int code = trylock(fs, 5);
		if (code != COZYFS_OK)
			return code;
	}

	fs->transaction = lock ? TRANSACTION_ON_WITH_LOCK : TRANSACTION_ON;
	return COZYFS_OK;
}

int cozyfs_transaction_commit(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	if (fs->transaction == TRANSACTION_TIMEOUT) {

		// Free patches
		for (int i = 0; i < fs->patch_count; i++) {
			sys_free(fs, fs->patch_ptrs[i]);
		}
		fs->patch_count = 0;
		return -COZYFS_ETIMEOUT;
	}

	if (fs->transaction != TRANSACTION_ON_WITH_LOCK) {
		int code = lock(fs);
		if (code != COZYFS_OK)
			return code;
	}

	// TODO: Verify conflicts

	// Apply changes and free patches
	for (int i = 0; i < fs->patch_count; i++) {
		void *src = fs->patch_ptrs[i];
		void *dst = fs->patch_offs[i] + fs->mem;
		my_memcpy(dst, src, 4096);
		sys_free(fs, src, 4096);
	}
	fs->patch_count = 0;

	perform_backup(fs, 0);
	unlock(fs);
	return COZYFS_OK;
}

int cozyfs_transaction_rollback(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	// Discard changes
	for (int i = 0; i < fs->patch_count; i++)
		sys_free(fs->patch_ptrs[i]);
	fs->patch_count = 0;

	if (fs->transaction == TRANSACTION_ON_WITH_LOCK)
		unlock(fs);
	fs->transaction = TRANSACTION_OFF;

	return COZYFS_OK;
}

static int pack_fd(CozyFS *fs, const Handle *handle)
{
	const RPage *root = fs->mem;
	int i = handle - root->handles;
	int fd = (handle->gen << 16) | i;
	ASSERT(fd >= 0);
	return fd;
}

static const Handle *unpack_fd(CozyFS *fs, int fd)
{
	u32 gen = (u32) fd >> 16;
	u32 idx = fd & 0xFFFF;

	const RPage *root = fs->mem;
	if (idx >= COUNT(root->handles))
		return -COZYFS_EBADF;

	const Handle *handle = &root->handles[idx];
	if (handle->gen != gen)
		return -COZYFS_EBADF;
}

static int open(CozyFS *fs, const char *path)
{
	string pathstr = { path, my_strlen(path) };

	string pathcomps[32];
	int pathnum = parse_path(pathstr, pathcomps, COUNT(pathcomps));
	if (pathnum < 0) return pathnum;

	// Follow the path to the entity
	const RPage *root = fs->mem;
	const Entity *entity = &root->root;
	for (int i = 0; i < pathnum-1; i++) {
		entity = find_entity(fs, entity, pathcomps[i]);
		if (entity == NULL)
			return -COZYFS_ENOENT;
	}

	// "open" only works on files
	if (entity->flags & ENTITY_DIR)
		return -COZYFS_EISDIR;

	// Find an unused handle
	int i = 0;
	while (i < COUNT(root->handles) && root->handles[i].used)
		i++;

	// Handle limit reached
	if (i == COUNT(root->handles) || i >= 1 << 16)
		return -COZYFS_ENFILE;

	Handle *handle = writable_addr(fs, &root->handles[i]);
	if (handle == NULL)
		return -COZYFS_ENOMEM;

	handle->entity = ptr2off(fs, entity);
	handle->used = 1;

	return pack_fd(fs, handle);
}

int cozyfs_open(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = open(fs, path);

	leave_critical_section(fs);
	return code;
}

static int close(CozyFS *fs, int fd)
{
	const Handle *handle = unpack_fd(fs, fd);
	if (handle == NULL)
		return -COZYFS_EBADF;

	const Entity *entity = off2ptr(fs, handle->entity);
	if ((entity->flags & ENTITY_FILE) == 0)
		return -COZYFS_EINVAL;

	Handle *writable_handle = writable_addr(fs, handle);
	if (writable_handle == NULL)
		return -COZYFS_ENOMEM;

	int code = free_entity(fs, entity);
	if (code != COZYFS_OK)
		return code;

	writable_handle->used = 0;
	writable_handle->gen++;
	if (writable_handle->gen == 0 || writable_handle->gen == (u64) -1)
		writable_handle->gen = 1;

	return COZYFS_OK;
}

int cozyfs_close(CozyFS *fs, int fd)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = close(fs, fd);

	leave_critical_section(fs);
	return code;
}

static string fpage_bytes(const Entity *entity, const FPage *fpage)
{
	char *src = fpage->data;
	int   num = sizeof(fpage->data);

	if (fpage->prev == INVALID_OFFSET) {
		src += entity->head_start;
		num -= entity->head_start;
	}

	if (fpage->next == INVALID_OFFSET)
		num -= sizeof(fpage->data) - entity->tail_end;

	return (string) { src, num };
}

static int read(CozyFS *fs, int fd, void *dst, int max, int flags)
{
	const Handle *handle = unpack_fd(fs, fd);
	if (handle == NULL)
		return -COZYFS_EBADF;

	const Entity *entity = off2ptr(fs, handle->entity);
	if ((entity->flags & ENTITY_FILE) == 0)
		return -COZYFS_EINVAL;

	const FPage *fpage = off2ptr(fs, entity->head);
	Offset       start = entity->head_start;

	if ((flags & READ_START) == 0) {

		Offset skipped = 0;
		while (fpage && skipped < handle->cursor) {
			skipped += fpage_bytes(entity, fpage).size;
			fpage = off2ptr(fs, fpage);
		}

		if (skipped < handle->cursor) {
			???
			handle->cursor = skipped;
		}
	}

	int copied = 0;
	while (fpage && copied < max) {

		string src = fpage_bytes(entity, fpage);

		if (src.size > max - copied)
			src.size = max - copied;

		my_memcpy(dst + copied, src.data, src.size);

		copied += src.size;
		fpage = off2ptr(fs, fpage->next);
	}

	if (flags & COZYFS_FCONSUME) {
		if (handle->cursor > 0)
			return -COZYFS_EINVAL;

		// TODO
	}

	return copied;
}

int cozyfs_read(CozyFS *fs, int fd, void *dst, int max, int flags)
{
	int code;
	code = enter_critical_section(fs);
	if (code != COZYFS_OK)
		return code;

	code = read(fs, fd, dst, max, flags);

	leave_critical_section(fs);
	return code;
}