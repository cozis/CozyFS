
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
