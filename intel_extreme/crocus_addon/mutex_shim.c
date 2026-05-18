typedef int status_t;
typedef struct {} mutex;
extern status_t mutex_lock(mutex* lock, void* locker);
extern void mutex_unlock(mutex* lock);
status_t _mutex_lock(mutex* lock, void* locker) { return mutex_lock(lock, locker); }
void _mutex_unlock(mutex* lock) { mutex_unlock(lock); }
