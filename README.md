# CozyFS
CozyFS is a tiny in-memory file system.

# Features
* Self-contained: It's only 1K lines of C with no dependencies (not even libc)
* Serialization-friendly: The file system is position independant. You can `memmove` it around and it will keep working. This allows you to share it with processes at different virtual addresses or dump it to a file
* Crash recovery: If a process dies while operating on CozyFS, its state won't be corrupted.
* Transaction support (with rollback and everything)
* By using `mmap` it becomes an ACID file system

# Limitations
* Concurrent access is managed by a single lock
* Crash recovery (which can be turned off) doubles memory usage
* Size limit of 4GB (8GB if you use backup mode)
