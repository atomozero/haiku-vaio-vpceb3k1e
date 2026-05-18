typedef int status_t;
typedef struct {} mutex;
status_t mutex_lock(mutex* l, void* lo) { return 0; }
void mutex_unlock(mutex* l) { }
