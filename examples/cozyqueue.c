#include "cozyfs.h"

#define COZYQUEUE_MAX_PRIORITIES 8

typedef struct {
	CozyFS fs;
	int num_queues;
	int queues[COZYQUEUE_MAX_PRIORITIES];
} CozyQueue;

char mem[1<<20];

int create_queue(CozyQueue *queue, char *name, int num_prios)
{
	int code;
	cozyfs_transaction_begin(&queue->fs, 1);

	// Create the queue's directory
	char path[1<<10];
	snprintf(path, sizeof(path), "/queues/%s", name);
	code = cozyfs_mkdir(&queue->fs, path);
	if (code < 0) goto error;

	for (int i = 0; i < num_prios; i++) {
		char path[1<<10];
		snprintf(path, sizeof(path), "/queues/%s/prio_%d\n", name, i);
		code = cozyfs_open(&queue->fs, path);
		if (code < 0) goto error;
	}

	code = cozyfs_transaction_commit(&queue->fs);
	if (code < 0) goto error;
	return 0;

error:
	cozyfs_transaction_rollback(&queue->fs);
	return code;
}

int remove_queue(CozyQueue *queue, char *name)
{
	char path[1<<10];
	snprintf(path, sizeof(path), "/queues/%s", name);
	return cozyfs_rmdir(&queue->fs, path);
}

int send_message(CozyFS *fs, char *name, int prio, void *msg, int len)
{
	char path[1<<10];
	snprintf(path, sizeof(path), "/queues/%s/prio_%d", name, prio);
	int fd = cozyfs_open(fs, path);
	if (fd < 0) return fd;
	return cozyfs_write(fs, fd, msg, len);
}

int recv_message(CozyFS *fs, char *name, void *dst, int max)
{
	for (int i = 0; i < 1000; i++) {

		char path[1<<10];
		snprintf(path, sizeof(path), "/queues/%s/prio_%d", name, i);
		int fd = cozyfs_open(fs, path);
		if (fd < 0) {
			if (fd == -COZYFS_ENOENT)
				break;
			return fd;
		}

		cozyfs_transaction_begin(fs, 1);
		unsigned int header;
		int num = cozyfs_read(fs, fd, &header, sizeof(header), COZYFS_FCONSUME);
		if (num == 0) {
			cozyfs_transaction_rollback(fs);
			continue;
		}
		if (num < 0) {
			cozyfs_transaction_rollback(fs);
			return num;
		}
		if (header > max) {
			cozyfs_transaction_rollback(fs);
			return -COZYFS_ENOMEM;
		}
		num = cozyfs_read(fs, fd, dst, max, COZYFS_FCONSUME);
		if (num < 0) {
			cozyfs_transaction_rollback(fs);
			return -COZYFS_ENOMEM;
		}
		cozyfs_transaction_commit(fs);
		return num;
	}
	return 0;
}

int cozyqueue_init(void *mem, int len, int num_priorities)
{
	int code;

	code = cozyfs_init(mem, len, 1, 0);
	if (code < 0) return code;

	CozyFS fs;
	cozyfs_attach(&fs, mem, callback, NULL);
	
}

int cozyqueue_attach(CozyQueue *queue, void *mem)
{
	cozyfs_attach(&queue->fs, mem, callback, NULL);

	queue->num_queues = 0;
	for (;;) {

		char path[1<<10];
		snprintf(path, sizeof(path), "/queues/queue_%d\n", queue->num_queues);

		int fd = cozyfs_open(&queue->fs, path);
		if (fd < 0) {
			if (fd == -COZYFS_ENOENT)
				break;
			for (int i = 0; i < queue->num_queues; i++)
				cozyfs_close(&queue->fs, queue->queues[i]);
			return fd;
		}

		if (queue->num_queues == COUNT(queue->queues)) {
			// TODO: Close handles
			return -COZYFS_ENOMEM;
		}

		queue->queues[queue->num_queues++] = fd;
	}

	return 0;
}

int cozyqueue_send()

int main()
{
	CozyQueue queue;
	cozyqueue_init(&queue, mem, sizeof(mem), 3);
}
