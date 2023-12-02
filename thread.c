#include <assert.h>
#include <stdlib.h>
// #define __USE_GNU
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"

typedef struct wait_queue wait_queue;
typedef struct lock lock;

struct thread_t {
	Tid tid;
	ucontext_t context;
	int running;
	int dead;
	int sleeping;
	void* stack_malloc_pointer;
	wait_queue* wq;
	int* exit_code_ptr;
} typedef thread_t;

thread_t* thread_t_init(Tid tid, void* stack_malloc_pointer) {
	thread_t* thread = malloc(sizeof(thread_t));
	thread->tid = tid;
	getcontext(&thread->context);
	thread->running = 0;
	thread->dead = 0;
	thread->sleeping = 0;
	thread->stack_malloc_pointer = stack_malloc_pointer;
	thread->wq = wait_queue_create();
	thread->exit_code_ptr = NULL;
	return thread;
}

void thread_t_destroy(thread_t* thread) {
	free(thread->stack_malloc_pointer);
	wait_queue_destroy(thread->wq);
	free(thread);
}

typedef struct node_t node_t;
struct node_t {
	node_t* next;
	thread_t* thread;
};

/* This is the wait queue structure */
struct wait_queue {
	node_t* queue;
};

node_t* node_t_init(thread_t* thread) {
	node_t* node = malloc(sizeof(node));
	node->next = NULL;
	node->thread = thread;
	return node;
}

node_t* next_node(node_t* node) {
	return node->next;
}

void add_node(node_t* start, node_t* node) {
	while (start->next) {
		start = start->next;
	}
	start->next = node;
}

node_t* threads_list;
Tid free_ids[THREAD_MAX_THREADS];
int free_ids_idx = 1;
int runnable_threads = 1;

void
thread_init(void)
{
	threads_list = node_t_init(thread_t_init(-1, malloc(sizeof(int))));
	
	thread_t* main_thread = thread_t_init(0, malloc(sizeof(int)));
	main_thread->running = 1;

	add_node(threads_list, node_t_init(main_thread));

	for (int i = 1; i < THREAD_MAX_THREADS; ++i) {
		free_ids[i] = i;
	}

	interrupts_on();
}

Tid
thread_id()
{
	int old = interrupts_off();

	node_t* node = threads_list;
	while ((node = next_node(node))) {
		if (node->thread->running) {
			Tid ret = node->thread->tid;

			interrupts_set(old);
			return ret;
		}
	}
	fprintf(stderr, "thread_id() (line %d): 1 thread should always be running. The function should not have reached here.\n", __LINE__);
	exit(1);
}

void destroy_dead_threads() {
	node_t* node = threads_list->next;
	node_t* previous = threads_list;
	while (node) {
		if (node->thread->dead && !node->thread->sleeping) {
			node_t* next = node->next;
			previous->next = next;
			thread_t_destroy(node->thread);
			free(node);
			node = next;
		} else {
			previous = node;
			node = node->next;
		}
	}
}

/* New thread starts by calling thread_stub. The arguments to thread_stub are
 * the thread_main() function, and one argument to the thread_main() function. 
 */
void
thread_stub(void (*thread_main)(void *), void *arg)
{
	interrupts_on();
    thread_main(arg); // call thread_main() function with arg
    thread_exit(0);
}

Tid
thread_create(void (*fn) (void *), void *parg)
{
	int old = interrupts_off();

	destroy_dead_threads();

	Tid ret;

	if (free_ids_idx == THREAD_MAX_THREADS) {
		ret = THREAD_NOMORE;
	} else {
		const long stack_sz = THREAD_MIN_STACK + 32;
		void* stack_malloc_pointer = malloc(stack_sz);
		if (stack_malloc_pointer == NULL) {
			ret = THREAD_NOMEMORY;	
		} else {
			ret = free_ids[free_ids_idx++];

			thread_t* new_thread = thread_t_init(ret, stack_malloc_pointer);

			// Handle stack alignment at 16 bytes.
			void* stack = stack_malloc_pointer + stack_sz - ((long)(stack_malloc_pointer + stack_sz) % 16L) - 8;

			// Set up context for stub function call when thread is called the first time.
			new_thread->context.uc_mcontext.gregs[REG_RIP] = (long)thread_stub;
			new_thread->context.uc_mcontext.gregs[REG_RDI] = (long)fn;
			new_thread->context.uc_mcontext.gregs[REG_RSI] = (long)parg;
			new_thread->context.uc_mcontext.gregs[REG_RSP] = (long)stack;

			add_node(threads_list, node_t_init(new_thread));

			runnable_threads++;
		}
	}

	interrupts_set(old);
	
	return ret;
}

node_t* find_before_node_by_tid_runnable(Tid tid) {
	if (tid == THREAD_SELF) tid = thread_id();

	node_t* node = threads_list;
	node_t* previous = threads_list;
	int found = 0;
	while ((node = next_node(node))) {
		if (!node->thread->dead && !node->thread->sleeping && (node->thread->tid == tid || tid == THREAD_ANY)) {
			found = 1;
			break;
		}
		if (node->thread->tid == tid && node->thread->running) {
			found = 1;
			break;
		}
		previous = node;
	}
	return found ? previous : NULL;
}

Tid
thread_yield(Tid want_tid)
{
	int old = interrupts_off();

	Tid ret;

	if (want_tid == THREAD_ANY && free_ids_idx == 1) {
		ret = THREAD_NONE;
	} else {
		node_t* before_node = find_before_node_by_tid_runnable(want_tid);
		if (before_node == NULL) {
			ret = THREAD_INVALID;
		} else {
			node_t* want_node = before_node->next;
			before_node->next = want_node->next;
			want_node->next = NULL;
			add_node(threads_list, want_node);

			thread_t* want_thread = want_node->thread;
			ret = want_thread->tid;
			thread_t* from_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;

			volatile int cycle_cnt = 0;
			getcontext(&from_thread->context);
			if (cycle_cnt == 0) {
				cycle_cnt++;
				from_thread->running = 0;
				want_thread->running = 1;
				setcontext(&want_thread->context);
			}	
		}
	}

	interrupts_set(old);

	return ret;
}

void destroy_threads_list() {
	node_t* node = threads_list;
	node_t* previous = threads_list;
	while ((node = next_node(node))) {
		thread_t_destroy(previous->thread);
		free(previous);
		previous = node;
	}
	thread_t_destroy(previous->thread);
	free(previous);
}

void update_wq_exit_codes(wait_queue* wq, int exit_code) {
	node_t* node = wq->queue;
	while ((node = next_node(node))) {
		int* ptr = node->thread->exit_code_ptr;
		if (ptr) {
			*ptr = exit_code;
		}
	}
}

void
thread_exit(int exit_code)
{
	interrupts_off();

	destroy_dead_threads();

	thread_t* want_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	want_thread->running = 0;
	want_thread->dead = 1;
	free_ids[--free_ids_idx] = want_thread->tid;

	runnable_threads--;

	update_wq_exit_codes(want_thread->wq, exit_code);

	thread_wakeup(want_thread->wq, 1);

	if (free_ids_idx == 0) {
		destroy_threads_list();
		exit(exit_code);
	} else {
		node_t* before_node = find_before_node_by_tid_runnable(THREAD_ANY);
		node_t* to_node = before_node->next;
		before_node->next = to_node->next;
		to_node->next = NULL;
		add_node(threads_list, to_node);
		to_node->thread->running = 1;
		setcontext(&to_node->thread->context);
	}
}

node_t* find_before_node_by_tid_all(Tid tid) {
	if (tid == THREAD_SELF) tid = thread_id();

	node_t* node = threads_list;
	node_t* previous = threads_list;
	int found = 0;
	while ((node = next_node(node))) {
		if (node->thread->tid == tid) {
			found = 1;
			break;
		}
		previous = node;
	}
	return found ? previous : NULL;
}

Tid
thread_kill(Tid tid)
{
	int old = interrupts_off();

	Tid ret;

	if (tid == thread_id() || tid < 0) {
		 ret = THREAD_INVALID;
	} else {
		node_t* before_node = find_before_node_by_tid_all(tid);
		if (before_node == NULL) {
			ret = THREAD_INVALID;
		} else {
			ret = tid;
			thread_t* want_thread = before_node->next->thread;
			if (want_thread->dead == 1) {
				ret = THREAD_INVALID;
			} else {
				want_thread->dead = 1;
				free_ids[--free_ids_idx] = tid;

				runnable_threads--;
				thread_wakeup(want_thread->wq, 1);
			}
		}
	}

	interrupts_set(old);

	return ret;
}

/**************************************************************************
 * Important: The rest of the code should be implemented in Assignment 2. *
 **************************************************************************/

/* make sure to fill the wait_queue structure defined above */
struct wait_queue *
wait_queue_create()
{
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));

	wq->queue = node_t_init(malloc(sizeof(int)));

	return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	node_t* node = wq->queue;
	node_t* previous = node;
	while ((node = next_node(node))) {
		free(previous);
		previous = node;
	}
	free(previous);
	free(wq);
}

Tid
thread_sleep(struct wait_queue *queue)
{
	int old = interrupts_off();

	Tid ret;

	if (!queue) {
		ret = THREAD_INVALID;
	} else if (runnable_threads == 1) {
		ret = THREAD_NONE;
	} else {
		thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
		current_thread->sleeping = 1;
		add_node(queue->queue, node_t_init(current_thread));
		ret = thread_yield(THREAD_ANY);
	}

	interrupts_set(old);

	return ret;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	int old = interrupts_off();

	int ret = 0;

	if (queue) {
		node_t* node = queue->queue->next;
		if (all) {
			while (node) {
				node_t* next = node->next;
				node->thread->sleeping = 0;
				free(node);
				node = next;
				ret++;
			}
			queue->queue->next = NULL;
		} else {
			if (node) {
				ret = 1;
				queue->queue->next = node->next;
				node->thread->sleeping = 0;
				free(node);
			} else {
				ret = 0;
			}
		}
	}

	runnable_threads += ret;

	interrupts_set(old);

	return ret;
}

/* suspend current thread until Thread tid exits */
Tid
thread_wait(Tid tid, int *exit_code)
{
	int old = interrupts_off();

	Tid ret;

	if (tid == thread_id() || tid < 0) {
		ret = THREAD_INVALID;
	} else {
		node_t* before_node = find_before_node_by_tid_all(tid);
		if (!before_node) {
			ret = THREAD_INVALID;
		} else {
			thread_t* to_thread = before_node->next->thread;
			if (to_thread->dead) {
				ret = THREAD_INVALID;
			} else {
				ret = tid;
				thread_t* from_thread = find_before_node_by_tid_all(thread_id())->next->thread;
				from_thread->exit_code_ptr = exit_code;
				thread_sleep(to_thread->wq);
				from_thread->exit_code_ptr = NULL;		
			}
		}
	}

	interrupts_set(old);

	return ret;
}

struct lock {
	thread_t* user;
	wait_queue* wq;
};

struct lock *
lock_create()
{
	struct lock *lock;

	lock = malloc(sizeof(struct lock));

	lock->user = NULL;
	lock->wq = wait_queue_create();

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	assert(lock != NULL);
	if (!lock->user) {
		wait_queue_destroy(lock->wq);
		free(lock);
	}
}

void
lock_acquire(struct lock *lock)
{
	int old = interrupts_off();

	assert(lock != NULL);

	thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	while (lock->user && lock->user != current_thread) {
		thread_sleep(lock->wq);
	} 
	
	lock->user = current_thread;

	interrupts_set(old);
}

void
lock_release(struct lock *lock)
{
	int old = interrupts_off();

	assert(lock != NULL);

	thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	if (lock->user == current_thread) {
		lock->user = NULL;
		thread_wakeup(lock->wq, 1);
	}

	interrupts_set(old);
}

struct cv {
	wait_queue* wq;
};

struct cv *
cv_create()
{
	struct cv *cv;

	cv = malloc(sizeof(struct cv));

	cv->wq = wait_queue_create();

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);

	if (!cv->wq->queue->next) {
		wait_queue_destroy(cv->wq);
		free(cv);
	}
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	int old = interrupts_off();

	thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	if (lock->user == current_thread) {
		lock_release(lock);
		thread_sleep(cv->wq);
		lock_acquire(lock);
	}

	interrupts_set(old);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	int old = interrupts_off();

	thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	if (lock->user == current_thread) {
		thread_wakeup(cv->wq, 0);
	}

	interrupts_set(old);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	int old = interrupts_off();

	thread_t* current_thread = find_before_node_by_tid_runnable(thread_id())->next->thread;
	if (lock->user == current_thread) {
		thread_wakeup(cv->wq, 1);
	}

	interrupts_set(old);
}
