#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <ucontext.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "lib/heap.h"
#include "lib/tvhelp.h"

#include "uthread.h"


/* Define private directives. ****************************************************/

#define UCONTEXT_STACK_SIZE  16384
#define MAX_NUM_UTHREADS     1000
#define gettid()             (syscall(SYS_gettid))

/* Define custom data structures. ************************************************/

typedef struct {
	void* fst;
	void* snd;
} ptrpair_t;

typedef struct {
	ucontext_t ucontext;
	struct timeval running_time;
	bool active;
} uthread_t;


typedef struct {
	pthread_t pthread;
	struct timeval initial_utime;
	struct timeval initial_stime;
	bool active;
} kthread_t;



/* Declare helper functions. *****************************************************/

int uthread_priority(const void* key1, const void* key2);
void kthread_init(kthread_t* kt);
void uthread_init(uthread_t* ut, void (*run_func)());
int kthread_runner(void* ptr);
int kthread_create(kthread_t* kt, uthread_t* ut);
kthread_t* find_inactive_kthread();
uthread_t* find_inactive_uthread();




/* Define file-global variables. *************************************************/

Heap _waiting_uthreads = NULL;
/*
// DEBUG
int _system_init_with;
pthread_mutex_t _system_mutex = PTHREAD_MUTEX_INITIALIZER;
*/
int _num_kthreads;
int _max_num_kthreads;
int _num_uthreads;
uthread_t* _uthreads;
kthread_t* _kthreads;
pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_attr_t* _default_pthread_attr = NULL;
ucontext_t _creator_context;


/* Define primary public functions. **********************************************/

void system_init(int max_num_kthreads) {
	uthread_system_init(max_num_kthreads);
}


void uthread_system_init(int max_num_kthreads)
{
	assert(1 <= max_num_kthreads && max_num_kthreads <= MAX_NUM_UTHREADS);
	assert(_waiting_uthreads == NULL);  // Function must only be called once.

	/*
	// DEBUG
	_system_init_with = gettid();
	pthread_mutex_lock(&_system_mutex);
	*/

	// The highest priority uthread record (i.e. the on with the lowest running time)
	// will be at top of the `heap`. Thus, the heap is bottom-heavy w.r.t. running time.
	_waiting_uthreads = HEAPinit(uthread_priority, NULL);

	// Allocate memory for each `kthread_t` and mark each as inactive.
	_kthreads = malloc(_max_num_kthreads * sizeof(kthread_t));
	for (int i = 0; i < _max_num_kthreads; i++) {
		_kthreads[i].active = false;
	}

	// Allocate memory for each `uthread_t` and mark each as inactive.
	_uthreads = malloc(MAX_NUM_UTHREADS * sizeof(uthread_t));
	for (int i = 0; i < MAX_NUM_UTHREADS; i++) {
		_uthreads[i].active = false;
	}

	// Initialize other globals.
	_num_uthreads = 0;
	_num_kthreads = 0;
	_max_num_kthreads = max_num_kthreads;
	// TODO: initialize `_default_pthread_attr()`.
}



int uthread_create(void (*run_func)())
{
	int rv;

	puts("uthread_create()");
	pthread_mutex_lock(&_mutex);
	puts("uthread_create(): past lock");

	_num_uthreads += 1;

	assert(_num_uthreads < MAX_NUM_UTHREADS);
	assert(_num_kthreads <= _max_num_kthreads);

	uthread_t* uthread = find_inactive_uthread();
	uthread_init(uthread, run_func);

	if (_num_kthreads == _max_num_kthreads)
	{
		puts("Adding `uthread` to heap.");
		// Add the new uthread record to the heap.
		HEAPinsert(_waiting_uthreads, (const void *) uthread);
	}
	else
	{
		puts("Starting `uthread` on new `kthread`.");
		// Make a pthread to run this function immediately.

		assert(HEAPsize(_waiting_uthreads) == 0);  // There must not be waiting
												   // uthreads if `_num_kthreads` is
												   // less than `_max_num_kthreads`.

		kthread_t* kthread = find_inactive_kthread();
		assert(kthread != NULL);  // There must be an inactive `kthread` if
								  // `_num_kthreads` is less than `_max_num_kthreads`.

		rv = kthread_create(kthread, uthread);
		_num_kthreads += 1;
	}

	pthread_mutex_unlock(&_mutex);
	return rv;
}




void uthread_yield()
{
	pthread_mutex_lock(&_mutex);

	assert(false);  // TODO: not implemented error

	pthread_mutex_unlock(&_mutex);
}


void uthread_exit()
{

	/*
	// DEBUG
	if (gettid() == _system_init_with)
	{
		printf("uthread_exit() was called by a non-kthread.\n");
		// Make the thread which initialized the system have to wait until all threads
		// are finished running.
		pthread_mutex_lock(&_system_mutex);
	}
	else
	{
	*/
		pthread_mutex_lock(&_mutex);
		printf("uthread_exit() was called by a kthread.\n");
		// Check if a uthread can use this kthread. If so, pop the uthread from the
		// heap and use this kthread. Else, destroy the kthread.
		assert(false);  // TODO: not implemented error
		pthread_mutex_unlock(&_mutex);
	/*
	}
	*/

}



/* Define primary helper functions. **********************************************/

void uthread_init(uthread_t* uthread, void (*run_func)())
{
	// Initialize the `ucontext`.
	ucontext_t* ucp = &(uthread->ucontext);
	getcontext(ucp);
	ucp->uc_stack.ss_sp = malloc(UCONTEXT_STACK_SIZE);
	ucp->uc_stack.ss_size = UCONTEXT_STACK_SIZE;
	makecontext(ucp, run_func, 0);

	// Initialize the running time.
	struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
	uthread->running_time = tv;

	// Set as active.
	uthread->active = true;
}



/**
 * This function is expected to be a `start_routine` for `pthread_create()`
 *
 * This interprets the given void pointer as a pointer to a `ptrpair_t`. The `fst`
 * pointer is interpreted as a `kthread_t*` and the second pointer is interpreted
 * as a `uthread_t*`. This will free the object which it is given.
 */
int kthread_runner(void* ptr)
{
	ptrpair_t* pair = ptr;
	kthread_t* kt = pair->fst;
	uthread_t* ut = pair->snd;
	free(pair);

	struct rusage ru;
	const int RUSAGE_THREAD = 1;	// TODO: Fix this hack!
	getrusage(RUSAGE_THREAD, &ru);  // Only available on linux.
	kt->initial_utime = ru.ru_utime;
	kt->initial_stime = ru.ru_stime;

	setcontext(&(ut->ucontext));

	return 0;
}


/**
 * Run the given user thread on the given kernel thread. The kernel thread must
 * not already be active.
 */
int kthread_create(kthread_t* kt, uthread_t* ut)
{
	// TODO: everything!
	assert(kt->active == false);

	// It is the responsibility of the newly created thread to free `pair`.
	ptrpair_t* pair = malloc(sizeof(ptrpair_t));
	pair->fst = kt;
	pair->snd = ut;

	/*
	int err = pthread_create(&(kt->pthread), _default_pthread_attr,
							 kthread_runner, (void*) pair);
	assert (err == 0);  // Cannot handle `pthread` creation errors.
	*/
	// Try using clone instead:
	void *child_stack;
	child_stack=(void *)malloc(16384); child_stack+=16383;
	return clone(kthread_runner, child_stack, CLONE_VM|CLONE_FILES, pair);
}




/* Define minor helper functions. ************************************************/

void kthread_init(kthread_t* kt) {
	kt->active = false;
}


/**
 * Returns a pointer to a `kthread_t` slot which is which is not active. If
 * no such `kthread_t` slot exists, then `NULL` is returned.
 */
kthread_t* find_inactive_kthread()
{
	kthread_t* kthread = NULL;
	for (int idx = 0; idx < _max_num_kthreads; idx++) {
		if (_kthreads[idx].active == false) {
			kthread = _kthreads + idx;
			break;
		}
	}
	return kthread;
}


/**
 * Returns a pointer to a `uthread_t` slot which is which is not active. If
 * no such `uthread_t` slot exists, then `NULL` is returned.
 */
uthread_t* find_inactive_uthread()
{
	uthread_t* uthread = NULL;
	for (int idx = 0; idx < _max_num_kthreads; idx++) {
		if (_uthreads[idx].active == false) {
			uthread = _uthreads + idx;
			break;
		}
	}
	return uthread;
}


/**
 * Interprets `key1` and `key2` as pointers to `uthread_t` objects, and
 * compares them. The comparison is based on the running time of the two records.
 * In particular, given the two records, the record with the smaller running time will
 * have the greater priority.
 */
int uthread_priority(const void* key1, const void* key2)
{
	const uthread_t* rec1 = key1;
	const uthread_t* rec2 = key2;

	int cmp = timeval_cmp(rec1->running_time, rec2->running_time);
	return -cmp;
}
