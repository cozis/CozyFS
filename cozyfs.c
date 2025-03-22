// Copyright (c) 2025 Francesco Cozzuto
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
// to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#include "cozyfs.h"

#if defined(__clang__)
#	define COMPILER_GCC   0
#	define COMPILER_CLANG 1
#	define COMPILER_MSVC  0
#elif defined(__GNUC__)
#	define COMPILER_GCC   1
#	define COMPILER_CLANG 0
#	define COMPILER_MSVC  0
#elif defined(_MSC_VER)
#	define COMPILER_GCC   0
#	define COMPILER_CLANG 0
#	define COMPILER_MSVC  1
#else
#	error "Unknown compiler: we only support gcc, clang, msvc"
#endif

// NOTE: Compiling for an unknown platform is not an error
#if defined(__linux__)
#	define OS_LINUX   1
#	define OS_WINDOWS 0
#elif defined(_WIN32)
#	define OS_LINUX   0
#	define OS_WINDOWS 1
#else
#	define OS_LINUX   0
#	define OS_WINDOWS 0
#endif

typedef unsigned char       u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          s64;

#define NULL ((void*) 0)

#define COUNT(X) ((int) (sizeof(X)/sizeof((X)[0])))

#define OFFSETOF(TYPE, MEMBER) (((TYPE*) 0)->MEMBER)
#define MEMBER_SIZEOF(TYPE, MEMBER) sizeof(((TYPE*) 0)->MEMBER)

#define ASSERT(X) if (!(X)) __builtin_trap();
#define STATIC_ASSERT(X) _Static_assert(X);

typedef struct { u8 *data; int size; } string;

////////////////////////////////////////////////////////////////////////
// TYPES SPECIFIC TO THE FS

typedef u32 Offset;

#define INVALID_OFFSET ((Offset) -1LL)

enum {
	ENTITY_DIR = 1 << 0,
	ENTITY_FILE = 1 << 1,
};

enum {
	TRANSACTION_OFF,
	TRANSACTION_ON,
	TRANSACTION_TIMEOUT,
};

enum {
	BACKUP_NO = -1,
	BACKUP_HALF_ACTIVE = 1,
	BACKUP_HALF_INACTIVE = 0,
};

typedef struct {
	Offset off;
	u8     name[128];
} Link;
STATIC_ASSERT(sizeof(Link) == 132);

#define MAX_NAME MEMBER_SIZEOF(Link, name)

typedef struct {
	u32    refs;
	u32    flags;
	Offset head;
	Offset tail;
	u32    owner;
	u16    head_start;
	u16    tail_end;
} Entity;
STATIC_ASSERT(sizeof(Entity) == 24);

// 4072
// 24+132=156

typedef struct {

	u32 gen;
	u32 flags;

	Offset global_prev;
	Offset global_next;

	// Linked lists of dpages for a directory
	Offset prev;
	Offset next;

	// List of links for this directory
	Link links[26];

	// List of entities. This may or may not be associated
	// to the directory
	Entity ents[26];

	// Make sure the page struct is 4K
	char pad[16];

} DPage;
STATIC_ASSERT(sizeof(DPage) == 4096);

typedef struct {

	u32 gen;

	Offset prev;
	Offset next;

	char data[4084];

} FPage;
STATIC_ASSERT(sizeof(FPage) == 4096);

typedef struct {
	u8     used;
	u16    gen;
	Offset entity;
	Offset cursor;
} Handle;
STATIC_ASSERT(sizeof(Handle) == 12);

typedef struct {
	Offset next;
	Handle handles[341];
} HPage;
STATIC_ASSERT(sizeof(HPage) == 4096);

typedef struct {
	u16    id; // Zero if unused
	char   name[30];
} User;
STATIC_ASSERT(sizeof(User) == 32);

#define MAX_USER_NAME MEMBER_SIZEOF(User, name)

typedef struct {
	u32    gen;
	Offset prev;
	Offset next;
	User   users[127];
	char   pad[20];
} UPage;
STATIC_ASSERT(sizeof(UPage) == 4096);

typedef struct {
	u32 gen;

	volatile u64 lock;
	volatile int backup; // All volatile fields must come before "backup"

	u64 last_backup_time;

	u32 next_account_id;

	Offset dpages;
	Offset hpages;

	Offset head_upage;
	Offset tail_upage;
	Offset tail_upage_used;

	Offset free_pages;

	int tot_pages;
	int num_pages;

	Entity root;

	Handle handles[333];

	char pad[8];
} RPage;
STATIC_ASSERT(sizeof(RPage) == 4096);

typedef struct {
	Offset next;
	char   pad[4092];
} XPage;
STATIC_ASSERT(sizeof(XPage) == 4096);

////////////////////////////////////////////////////////////////////////
// FILE OVERVIEW

// Basic utilities
static void         my_memcpy           (void *dst, const void *src, unsigned long len);
static void         my_memset           (void *dst, char src, unsigned long len);
static unsigned int my_strlen           (const u8 *str);
static int          memeq               (const void *p1, const void *p2, int len);

// Atomic operations
static u64           atomic_load        (volatile u64 *ptr);
static void          atomic_store       (volatile u64 *ptr, u64 val);
static int           atomic_compare_exchange(volatile u64 *ptr, u64 expect, u64 new_value);
static u64           load               (u64 *ptr);
static void          fence              (void);
static int           cmpxchg_acquire    (u64 *word, u64 new_word, u64 old_word);
static int           cmpxchg_release    (u64 *word, u64 new_word, u64 old_word);
static int           cmpxchg_acq_rel    (u64 *word, u64 new_word, u64 old_word);

// User callback wrappers
static void*         sys_malloc         (CozyFS *fs, int len);
static int           sys_free           (CozyFS *fs, void *ptr, int len);
static int           sys_wait           (CozyFS *fs, u64 *word, u64 old_word, int timeout_ms);
static int           sys_wake           (CozyFS *fs, u64 *word);
static int           sys_sync           (CozyFS *fs);
static u64           sys_time           (CozyFS *fs);

// Relative pointer management
static const RPage*  get_root           (CozyFS *fs);
static const void*   off2ptr            (CozyFS *fs, Offset off);
static Offset        ptr2off            (CozyFS *fs, const void *ptr);
static void*         writable_addr      (CozyFS *fs, const void *ptr);

// Directory and file management
static Entity*       find_unused_entity (CozyFS *fs);
static int           free_entity        (CozyFS *fs, const Entity *entity);
static const Entity* find_entity        (CozyFS *fs, const Entity *parent, string name);
static int           create_entity      (CozyFS *fs, const Entity *parent, const Entity *target, string name, u32 flags);
static int           remove_entity      (CozyFS *fs, const Entity *parent, string name, u32 flags);

// User management
static void*         allocate_page      (CozyFS *fs);
static int           create_user        (CozyFS *fs, const char *name);
static int           remove_user        (CozyFS *fs, const char *name);

// File system functions
static int           parse_path         (string path, string *comps, int max);
static int           pack_fd            (CozyFS *fs, const Handle *handle);
static const Handle* unpack_fd          (CozyFS *fs, int fd);
static int           link_              (CozyFS *fs, const char *oldpath, const char *newpath);
static int           unlink_            (CozyFS *fs, const char *path);
static int           mkdir_             (CozyFS *fs, const char *path);
static int           rmdir_             (CozyFS *fs, const char *path);
static int           mkusr              (CozyFS *fs, const char *name);
static int           rmusr              (CozyFS *fs, const char *name);
static int           chown_             (CozyFS *fs, const char *path, const char *new_owner);
static int           chmod_             (CozyFS *fs, const char *path, int mode);
static int           open_              (CozyFS *fs, const char *path);
static int           close_             (CozyFS *fs, int fd);
static string        fpage_bytes        (const Entity *entity, const FPage *fpage);
static int           read_              (CozyFS *fs, int fd, void       *dst, int max);
static int           write_             (CozyFS *fs, int fd, const void *src, int num);

// File system lock
static int           lock               (CozyFS *fs, int wait_timeout_ms, int acquire_timeout_sec, int *crash);
static int           unlock             (CozyFS *fs);
static int           refresh_lock       (CozyFS *fs, int postpone_sec);

// Backup
static void          perform_backup     (CozyFS *fs, int not_before_sec);
static int           restore_backup     (CozyFS *fs);

// Public and thread-safe interface
static int           enter_critical_section(CozyFS *fs, int wait_timeout_ms);
static void          leave_critical_section(CozyFS *fs);
int                  cozyfs_init        (void *mem, unsigned long len, int backup, int refresh);
void                 cozyfs_attach      (CozyFS *fs, void *mem, const char *user, cozyfs_callback callback, void *userptr);
void                 cozyfs_idle        (CozyFS *fs);
int                  cozyfs_link        (CozyFS *fs, const char *oldpath, const char *newpath);
int                  cozyfs_unlink      (CozyFS *fs, const char *path);
int                  cozyfs_mkdir       (CozyFS *fs, const char *path);
int                  cozyfs_rmdir       (CozyFS *fs, const char *path);
int                  cozyfs_mkusr       (CozyFS *fs, const char *name);
int                  cozyfs_rmusr       (CozyFS *fs, const char *name);
int                  cozyfs_open        (CozyFS *fs, const char *path);
int                  cozyfs_close       (CozyFS *fs, int fd);
int                  cozyfs_read        (CozyFS *fs, int fd, void       *dst, int max);
int                  cozyfs_write       (CozyFS *fs, int fd, const void *src, int num);
int                  cozyfs_transaction_begin   (CozyFS *fs);
int                  cozyfs_transaction_commit  (CozyFS *fs);
int                  cozyfs_transaction_rollback(CozyFS *fs);

////////////////////////////////////////////////////////////////////////
// Basic utilities

static void my_memcpy(void *dst, const void *src, unsigned long len)
{
	char *dstc = dst;
	const char *srcc = src;
	for (unsigned long i = 0; i < len; i++)
		dstc[i] = srcc[i];
}

static void my_memset(void *dst, char src, unsigned long len)
{
	char *dstc = dst;
	for (unsigned long i = 0; i < len; i++)
		dstc[i] = src;
}

static unsigned int my_strlen(const u8 *str)
{
	if (str == NULL)
		return 0;
	unsigned int len = 0;
	while (str[len] != '\0')
		len++;
	return len;
}

static int memeq(const void *p1, const void *p2, int len)
{
	for (int i = 0; i < len; i++)
		if (((char*) p1)[i] != ((char*) p2)[i])
			return 0;
	return 1;
}

////////////////////////////////////////////////////////////////////////
// Atomic operations

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
	_InterlockedExchange64((volatile s64*) ptr, val);
#elif COMPILER_GCC || COMPILER_CLANG
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
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

static u64 load(u64 *ptr)
{
#if COMPILER_MSVC
	return = *ptr;
#else
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#endif
}

static void fence(void)
{
	// TODO
}

static int cmpxchg_acquire(u64 *word, u64 new_word, u64 old_word)
{
#if defined(_MSC_VER)
	return (u64) _InterlockedCompareExchange64((volatile s64*) word, new_word, old_word) == old_word;
#else
	return __atomic_compare_exchange_n(word, &old_word, new_word, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif
}

static int cmpxchg_release(u64 *word, u64 new_word, u64 old_word)
{
#if defined(_MSC_VER)
	return (u64) _InterlockedCompareExchange64((volatile s64*) word, new_word, old_word) == old_word
#else
	return __atomic_compare_exchange_n(word, &old_word, new_word, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
#endif
}

static int cmpxchg_acq_rel(u64 *word, u64 new_word, u64 old_word)
{
#if defined(_MSC_VER)
	return (u64) _InterlockedCompareExchange64((volatile s64*) word, new_word, old_word) == old_word
#else
	return __atomic_compare_exchange_n(word, &old_word, new_word, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
#endif
}

////////////////////////////////////////////////////////////////////////
// User callback wrappers

static void *sys_malloc(CozyFS *fs, int len)
{
	return (void*) fs->callback(COZYFS_SYSOP_MALLOC, fs->userptr, NULL, len);
}

static int sys_free(CozyFS *fs, void *ptr, int len)
{
	// We ignore the return value here
	if (!fs->callback(COZYFS_SYSOP_FREE, fs->userptr, ptr, len))
		return -COZYFS_ESYSFREE;
	return COZYFS_OK;
}

static int sys_wait(CozyFS *fs, u64 *word, u64 old_word, int timeout_ms)
{
	int code = fs->callback(COZYFS_SYSOP_WAIT, fs->userptr, ???);
	if (code != COZYFS_SYSRES_OK)
		return -COZYFS_ESYSWAIT;
	return COZYFS_OK;
}

static int sys_wake(CozyFS *fs, u64 *word)
{
	int code = fs->callback(COZYFS_SYSOP_WAKE, fs->userptr, ???);
	if (code != COZYFS_SYSRES_OK)
		return -COZYFS_ESYSWAKE;
	return COZYFS_OK;
}

static int sys_sync(CozyFS *fs)
{
	if (!fs->callback(COZYFS_SYSOP_SYNC, fs->userptr, NULL, 0))
		return -COZYFS_ESYSSYNC;
	return COZYFS_OK;
}

static u64 sys_time(CozyFS *fs)
{
	return fs->callback(COZYFS_SYSOP_TIME, fs->userptr, NULL, 0);
}

////////////////////////////////////////////////////////////////////////
// Relative pointer management

// TODO: Use this instead of accessing the root page directly
static const RPage *get_root(CozyFS *fs)
{
	const RPage *root = fs->mem;
	if (root->backup == BACKUP_HALF_INACTIVE)
		root += root->tot_pages;
	return root;
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
			return (void*) ptr; // Unconst the pointer

		// We need to create a new patch
		if (fs->patch_count == COUNT(fs->patch_offs))
			return NULL; // Path limit reached

		// Ask the user for a new page
		void *page_copy = sys_malloc(fs, 4096);
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

	return (void*) ptr;
}

////////////////////////////////////////////////////////////////////////
// Directory and file management

static Entity *find_unused_entity(CozyFS *fs)
{
	// First, look in the already modified pages
	for (int i = 0; i < fs->patch_count; i++) {
		// TODO
	}
}

static int free_entity(CozyFS *fs, const Entity *entity)
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
			if (name.size == link_name_len && memeq(name.data, dpage->links[i].name, name.size))
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

	DPage *writable_tail = writable_addr(fs, tail);
	if (writable_tail == NULL)
		return -COZYFS_ENOMEM;

	if (target) {

		Entity *writable_target = writable_addr(fs, target);
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
			if (name.size == link_name_len && memeq(name.data, dpage->links[i].name, name.size))
				return off2ptr(fs, dpage->links[i].off);
			i++;
		}

		dpage = off2ptr(fs, dpage->next);
	}

	if (dpage == NULL)
		return -COZYFS_ENOENT;

	const Link *link = &dpage->links[i];
	const Entity *entity = off2ptr(fs, link->off);

	Entity *writable_parent = writable_addr(fs, parent);
	if (writable_parent == NULL)
		return COZYFS_ENOMEM;

	DPage *writable_dpage = writable_addr(fs, dpage);
	if (writable_dpage == NULL)
		return -COZYFS_ENOMEM;

	int code = free_entity(fs, entity);
	if (code != COZYFS_OK)
		return code;

	const DPage *tail = off2ptr(fs, parent->tail);
	writable_dpage->links[i] = tail->links[--writable_parent->tail_end];

	return COZYFS_OK;
}

////////////////////////////////////////////////////////////////////////
// User management

static void *allocate_page(CozyFS *fs)
{
	const RPage *root = get_root(fs);

	if (root->free_pages == INVALID_OFFSET && root->num_pages == root->tot_pages)
		return NULL;

	RPage *writable_root = writable_addr(fs, root);
	if (writable_root == NULL)
		return NULL;

	XPage *xpage;
	if (root->free_pages == INVALID_OFFSET) {
		xpage = (XPage*) get_root(fs) + writable_root->num_pages++;
	} else {
		xpage = writable_addr(fs, off2ptr(fs, writable_root->free_pages));
		writable_root->free_pages = xpage->next;
	}

	// TODO: The page here should be copied if a transaction is happening

	return xpage;
}

static int create_user(CozyFS *fs, const char *name)
{
	const RPage *root = get_root(fs);

	int name_len = my_strlen(name);
	if (name_len >= MAX_USER_NAME)
		return -COZYFS_ENAMETOOLONG;

	UPage *upage;
	if (root->tail_upage_used == COUNT(((UPage*) 0)->users)) {

		RPage *writable_root = writable_addr(fs, root);
		if (writable_root == NULL)
			return -COZYFS_ENOMEM;

		upage = allocate_page(fs);
		if (upage == NULL)
			return -COZYFS_ENOMEM;
		upage->prev = writable_root->tail_upage;
		upage->next = INVALID_OFFSET;
		writable_root->tail_upage = ptr2off(fs, upage);
		writable_root->tail_upage_used = 0;

	} else
		upage = off2ptr(fs, root->tail_upage);

	RPage *writable_root = writable_addr(fs, root);
	if (writable_root == NULL)
		return -COZYFS_ENOMEM;

	User *user = &upage->users[writable_root->tail_upage_used++];
	user->id = writable_root->next_account_id++;
	my_memset(user->name, 0, MAX_USER_NAME);
	my_memcpy(user->name, name, name_len);
	return 0;
}

static int remove_user(CozyFS *fs, const char *name)
{
	const RPage *root = get_root(fs);

	if (name == NULL)
		return -COZYFS_EPERM; // Trying to remove the root user

	const User *user = NULL;
	const UPage *upage = off2ptr(fs, root->head_upage);
	while (upage) {

		int num_users_in_page = COUNT(upage->users);
		if (upage->next == INVALID_OFFSET)
			num_users_in_page = root->tail_upage_used;
 
		for (int i = 0; i < num_users_in_page; i++)
			if (!strcmp(name, upage->users[i].name)) {
				user = &upage->users[i];
				break;
			}
		if (user) break;

		upage = off2ptr(fs, upage->next);
	}

	if (upage == NULL)
		return -COZYFS_ENOENT; // No such user

	const UPage *tail_upage = off2ptr(fs, root->tail_upage);

	// Now apply the change

	User *writable_user = writable_addr(fs, user);
	if (writable_user == NULL) return -COZYFS_ENOMEM;

	UPage *writable_tail_upage = writable_addr(fs, tail_upage);
	if (writable_tail_upage == NULL) return -COZYFS_ENOMEM;

	RPage *writable_root = writable_addr(fs, root);
	if (writable_root == NULL) return -COZYFS_ENOMEM;

	*writable_user = writable_tail_upage->users[writable_root->tail_upage_used--];

	if (writable_root->tail_upage_used == 0) {
		// Tail upage is now unused, making it an xpage
		XPage *writable_xpage = (XPage*) writable_tail_upage;
		writable_xpage->next = writable_root->free_pages;
		writable_root->free_pages = ptr2off(fs, writable_xpage);
	}

	return 0;
}

////////////////////////////////////////////////////////////////////////
// File system functions

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
		return NULL;

	const Handle *handle = &root->handles[idx];
	if (handle->gen != gen)
		return NULL;
	
	return handle;
}

static int link_(CozyFS *fs, const char *oldpath, const char *newpath)
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

static int unlink_(CozyFS *fs, const char *path)
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

static int mkdir_(CozyFS *fs, const char *path)
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

static int rmdir_(CozyFS *fs, const char *path)
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

static int mkusr(CozyFS *fs, const char *name)
{
	// TODO: Check that this user can perform this operation
	return create_user(fs, name);
}

static int rmusr(CozyFS *fs, const char *name)
{
	// TODO: Check that this user can perform this operation
	return remove_user(fs, name);
}

static int chown_(CozyFS *fs, const char *path, const char *new_owner)
{
	// TODO
}

static int chmod_(CozyFS *fs, const char *path, int mode)
{
	// TODO
}

static int open_(CozyFS *fs, const char *path)
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

	// TODO: Handles should have an expiration or crashing processes will fill up the array
	handle->entity = ptr2off(fs, entity);
	handle->used = 1;

	return pack_fd(fs, handle);
}

static int close_(CozyFS *fs, int fd)
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

static int read_(CozyFS *fs, int fd, void *dst, int max)
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

static int write_(CozyFS *fs, int fd, const void *src, int len)
{
	// TODO
}

////////////////////////////////////////////////////////////////////////
// File system lock

static int lock(CozyFS *fs, int wait_timeout_ms, int acquire_timeout_sec, int *crash)
{
	u64 start = sys_time(fs);
	if (start == 0)
		return -COZYFS_ESYSTIME;

	RPage *root = fs->mem;
	u64   *word = root->lock;
	u64 old_word;
	u64 new_word;
	for (;;) {

		u64 now = sys_time(fs);
		if (now == 0)
			return -COZYFS_ESYSTIME;

		old_word = load(word);
		new_word = now + acquire_timeout_sec * 1000;

		if (old_word < now) {
			// Region is unlocked. Try locking it.
			if (cmpxchg_acquire(word, new_word, old_word))
				break; // Lock acquired
			// If we reach this point, probably someone
			// else got the lock. We don't need to wait
			// before trying again.
		} else {
			int code = sys_wait(fs, word, old_word, old_word - now);
			if (code < 0)
				return code;
		}

		// Don't wait more than "wait_timeout_ms"
		if (wait_timeout_ms >= 0 && now - start >= wait_timeout_ms)
			return -COZYFS_ETIMEDOUT;
	}

	fs->ticket = new_word;

	if (old_word > 0) {
		fence(); // If a crash happened, we missed the memory barriers from the unlock operation
		*crash = 1;
	} else
		*crash = 0;
	return 0;
}

static int unlock(CozyFS *fs)
{
	RPage *root = fs->mem;
	u64   *word = root->lock;
	if (!cmpxchg_release(root->lock, 0, fs->ticket))
		return -COZYFS_ETIMEDOUT;
	return sys_wake(fs, word); // TODO: Should I only wake up 1?
}

static int refresh_lock(CozyFS *fs, int postpone_sec)
{
	u64 now = sys_time(fs);
	if (now == 0)
		return -COZYFS_ESYSTIME;

	RPage *root = fs->mem;

	u64 *word = root->lock;
	u64 new_word = now + postpone_sec * 1000;
	u64 old_word = fs->ticket;

	if (!cmpxchg_acq_rel(word, new_word, fs->ticket))
		return -COZYFS_ETIMEDOUT;
	
	fs->ticket = new_word;
	return 0;
}

////////////////////////////////////////////////////////////////////////
// Backup

static void perform_backup(CozyFS *fs, int not_before_sec)
{
	RPage *root = fs->mem;

	int backup = atomic_load(&root->backup);
	if (backup == BACKUP_NO)
		return;

	u64 now = sys_time(fs);
	if (now < root->last_backup_time + not_before_sec * 1000)
		return;

	atomic_store(&root->backup, !backup);

	// TODO
	my_memcpy(???);

	atomic_store(&root->last_backup_time, now);
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

////////////////////////////////////////////////////////////////////////
// Public and thread-safe interface

static int enter_critical_section(CozyFS *fs, int wait_timeout_ms)
{
	if (fs->transaction == TRANSACTION_TIMEOUT)
		return -COZYFS_ETIMEDOUT;

	int code;
	if (fs->transaction == TRANSACTION_ON) {

		code = refresh_lock(fs, 5);
		if (code != COZYFS_OK) {
			fs->transaction = TRANSACTION_TIMEOUT;
			return code;
		}

	} else {

		int crash;
		code = lock(fs, wait_timeout_ms, 5, &crash);
		if (code != COZYFS_OK)
			return code;

		// We entered the critical section because previous
		// acquire timed out. We must assume the state is
		// invalid.
		if (crash) {
			code = restore_backup(fs);
			if (code < 0) {
				unlock(fs);
				return code;
			}
		}
	}

	return COZYFS_OK;
}

static void leave_critical_section(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_TIMEOUT)
		return;

	if (fs->transaction == TRANSACTION_OFF)
		perform_backup(fs, 3);

	if (fs->transaction != TRANSACTION_ON)
		unlock(fs);
}

int cozyfs_init(void *mem, unsigned long len, int backup, int refresh)
{
	// Align to the size of a pointer
	{
		unsigned long pad = -(unsigned long) mem & 7;
		if (len < pad)
			return -COZYFS_ENOMEM;
		mem = (char*) mem + pad;
		len -= pad;
	}

	if (backup)
		len /= 2;

	int tot_pages = len / 4096;
	if (tot_pages == 0)
		return -COZYFS_ENOMEM;

	RPage *root = mem;

	if (refresh)
		atomic_store(&root->lock, 0);
	else {

		atomic_store(&root->lock, 0);
		atomic_store(&root->backup, backup ? BACKUP_HALF_ACTIVE : BACKUP_NO);
		root->dpages = INVALID_OFFSET;
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

	return 0;
}

void cozyfs_attach(CozyFS *fs, void *mem, const char *user, cozyfs_callback callback, void *userptr)
{
	// Align to the size of a pointer
	{
		unsigned long pad = -(unsigned long) mem & 7;
		mem = (char*) mem + pad;
	}

	fs->mem         = mem;
	fs->userptr     = userptr;
	fs->callback    = callback;
	fs->user        = ???;
	fs->ticket      = 0;
	fs->transaction = TRANSACTION_OFF;
	fs->patch_count = 0;
}

void cozyfs_idle(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_ON)
		refresh_lock(fs, 5);
	perform_backup(fs, 3); // TODO: Do this in the critical section
}

int cozyfs_link(CozyFS *fs, const char *oldpath, const char *newpath)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = link_(fs, oldpath, newpath);

	leave_critical_section(fs);
	return code;
}

int cozyfs_unlink(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = unlink_(fs, path);

	leave_critical_section(fs);
	return code;
}

int cozyfs_mkdir(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = mkdir_(fs, path);

	leave_critical_section(fs);
	return code;
}

int cozyfs_rmdir(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = rmdir_(fs, path);

	leave_critical_section(fs);
	return code;
}

int cozyfs_mkusr(CozyFS *fs, const char *name)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = mkusr(fs, name);

	leave_critical_section(fs);
	return code;
}

int cozyfs_rmusr(CozyFS *fs, const char *name)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = rmusr(fs, name);

	leave_critical_section(fs);
	return code;
}

int cozyfs_chown(CozyFS *fs, const char *path, const char *newowner)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = chown_(fs, path, newowner);

	leave_critical_section(fs);
	return code;
}

int cozyfs_chmod(CozyFS *fs, const char *path, int mode)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = chmod_(fs, path, mode);

	leave_critical_section(fs);
	return code;
}

int cozyfs_open(CozyFS *fs, const char *path)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = open_(fs, path);

	leave_critical_section(fs);
	return code;
}

int cozyfs_close(CozyFS *fs, int fd)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = close_(fs, fd);

	leave_critical_section(fs);
	return code;
}

int cozyfs_read(CozyFS *fs, int fd, void *dst, int max)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = read_(fs, fd, dst, max);

	leave_critical_section(fs);
	return code;
}

int cozyfs_write(CozyFS *fs, int fd, const void *src, int len)
{
	int code;
	code = enter_critical_section(fs, -1);
	if (code != COZYFS_OK)
		return code;

	code = write_(fs, fd, src, len);

	leave_critical_section(fs);
	return code;
}

int cozyfs_transaction_begin(CozyFS *fs)
{
	if (fs->transaction != TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	int crash;
	int code = lock(fs, -1, 5, &crash);
	if (code != COZYFS_OK)
		return code;
	if (crash) {
		// TODO
	}

	fs->transaction = TRANSACTION_ON;
	return COZYFS_OK;
}

int cozyfs_transaction_rollback(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	// Discard changes
	for (int i = 0; i < fs->patch_count; i++)
		sys_free(fs, fs->patch_ptrs[i], 4096);
	fs->patch_count = 0;

	unlock(fs);
	fs->transaction = TRANSACTION_OFF;
	return COZYFS_OK;
}

int cozyfs_transaction_commit(CozyFS *fs)
{
	if (fs->transaction == TRANSACTION_OFF)
		return -COZYFS_EINVAL;

	if (fs->transaction == TRANSACTION_TIMEOUT) {

		// Free patches
		for (int i = 0; i < fs->patch_count; i++) {
			sys_free(fs, fs->patch_ptrs[i], 4096);
		}
		fs->patch_count = 0;
		return -COZYFS_ETIMEDOUT;
	}

	// TODO: Verify conflicts

	// Apply changes and free patches
	for (int i = 0; i < fs->patch_count; i++) {
		void *src = fs->patch_ptrs[i];
		void *dst = (void*) (fs->patch_offs[i] + fs->mem);
		my_memcpy(dst, src, 4096);
		sys_free(fs, src, 4096);
	}
	fs->patch_count = 0;

	perform_backup(fs, 0);
	unlock(fs);
	fs->transaction = TRANSACTION_OFF;
	return COZYFS_OK;
}

////////////////////////////////////////////////////////////////////////
// Windows callback
#if OS_WINDOWS

#define WIN32_MEAN_AND_LEAN
#include <windows.h>

static int wait(u64 *word, u64 old_word, int timeout_ms)
{
	if (!WaitOnAddress((volatile VOID*) word, (PVOID) &old_word, sizeof(u64), timeout_ms < 0 ? INFINITE: (DWORD) timeout_ms))
		return -EAGAIN;
	return 0;
}

static int wake(u64 *word)
{
	WakeByAddressAll((PVOID) word);
	return 0;
}

unsigned long long
cozyfs_callback_impl(int sysop, void *userptr, void *p, int n)
{
	(void) userptr;

	switch (sysop) {

		case COZYFS_SYSOP_MALLOC:
		return VirtualAlloc(NULL, n, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);

		case COZYFS_SYSOP_FREE:
		return VirtualFree(p, n, MEM_RELEASE);

		case COZYFS_SYSOP_WAIT:
		break;

		case COZYFS_SYSOP_WAKE:
		break;

		case COZYFS_SYSOP_SYNC:
		// We aren't backing the file system with a file, so we don't need this
		break;

		case COZYFS_SYSOP_TIME:
		{
			FILETIME ft;
			GetSystemTimeAsFileTime(&ft);

			ULARGE_INTEGER uli;
			uli.LowPart = ft.dwLowDateTime;
			uli.HighPart = ft.dwHighDateTime;
					
			// Convert Windows file time (100ns since 1601-01-01) to 
			// Unix epoch time (seconds since 1970-01-01)
			// 116444736000000000 = number of 100ns intervals from 1601 to 1970
			return (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
		}
		break;
	}

	return -1; // unreachable
}

#endif
////////////////////////////////////////////////////////////////////////
// Linux callback
#if OS_LINUX

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define CLOCK_REALTIME 0

static int futex(unsigned int *uaddr,
	int futex_op, unsigned int val,
	const struct timespec *timeout,
	unsigned int *uaddr2, unsigned int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

static int wait(u64 *word, u64 old_word, int timeout_ms)
{
	struct timespec ts;
	struct timespec *tsptr;

	if (timeout_ms < 0)
		tsptr = NULL;
	else {
		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (timeout_ms % 1000) * 1000000;
	}

	errno = 0;
	long ret = futex((unsigned int*) word, FUTEX_WAIT, (unsigned int) old_word, tsptr, NULL, 0);
	if (ret == -1 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT)
		return -errno;
	return 0;
}

static int wake(u64 *word)
{
	errno = 0;
	long ret = futex((unsigned int*) word, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
	if (ret < 0) return -errno;
	// TODO: Don't return an error on EAGAIN or EINTR
	return 0;
}

u64 cozyfs_callback_impl(int sysop, void *userptr, void *p, int n)
{
	(void) userptr;

	switch (sysop) {

		case COZYFS_SYSOP_MALLOC:
		{
			void *addr = mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
			if (*addr == MAP_FAILED)
				return 0;
			return (u64) addr;
		}
		break;

		case COZYFS_SYSOP_FREE:
		return !munmap(p, n);

		case COZYFS_SYSOP_WAIT:
		break;

		case COZYFS_SYSOP_WAKE:
		break;

		case COZYFS_SYSOP_SYNC:
		// We aren't backing the file system with a file, so we don't need this
		break;

		case COZYFS_SYSOP_TIME:
		{
			struct timespec ts;
			int result = syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
			if (result)
				return 0;
			return ts.tv_sec;
		}
		break;
	}

	return -1; // unreachable
}

#endif
////////////////////////////////////////////////////////////////////////