#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"
#include <stdbool.h>

/* This is the wait queue structure */
struct wait_queue {
	struct thread* wait_q[THREAD_MAX_THREADS+1];
	int str_id;
	int run_id;
};

/* This is the thread control block */
struct thread {
	Tid tid;
	int state; // 0 nothing, 1 run, 2 ready, 3 killed
	ucontext_t mycontext;
	bool fflag;
	void* st_addr;
	struct wait_queue* who_is_waiting;
};

// Global Variables
// store is the last stored index, store+1 is where to store
// numThr to record how many threads are added
// q_id is where the queue is at
// tidQueue stores all available zeros
struct thread* contextQueue[THREAD_MAX_THREADS+1];
struct thread* thread_lock_cv[THREAD_MAX_THREADS+1];
int tidQueue[THREAD_MAX_THREADS] = {0};
int exitTable[THREAD_MAX_THREADS+1] = {0};
bool firstWaitTable[THREAD_MAX_THREADS+1];
bool lock_cv_Table[THREAD_MAX_THREADS+1];
bool process_lock_cv;
int store, numThr, q_id, highThreadNum;
void* stack_to_free;
ucontext_t main_uc;

// helper funcs:
// update str_id to next available
void update_str_id(struct wait_queue *queue){
	for (int i = queue->str_id+1; i < queue->str_id + THREAD_MAX_THREADS+2; i++){
		int str_res = i;
		if (i >= THREAD_MAX_THREADS+1) str_res = i - 1 - THREAD_MAX_THREADS;
		if (!queue->wait_q[str_res]) {
			queue->str_id = str_res; // find empty slot
			return;
		}
		if (str_res == queue->str_id){ // error
			printf("No valid slot for wait queue\n");
			return;
		}
	}
}

struct thread* th_copy(struct thread* target){
	struct thread* th = (struct thread*) malloc (sizeof(struct thread));
	th->tid = target->tid;
	th->mycontext = target->mycontext;
	th->state = target->state;
	th->st_addr = target->st_addr;
	th->who_is_waiting = target->who_is_waiting;
	return th;
}

void
thread_init(void)
{
	//register_interrupt_handler(0);
	interrupts_off();
	getcontext(&main_uc);
	struct thread* init_th = (struct thread*) malloc (sizeof(struct thread));
	init_th->tid = 0;
	init_th->mycontext = main_uc;
	init_th->state = 1;
	init_th->fflag = true;
	init_th->who_is_waiting = wait_queue_create();
	contextQueue[0] = init_th;

	q_id = 1;
	store = 0;
	numThr = 1;
	highThreadNum = 0;
	tidQueue[0] = 1;
	firstWaitTable[0] = true;
	stack_to_free = NULL;
	process_lock_cv = false;

	for (int i = 1; i < THREAD_MAX_THREADS+1; i++)
		firstWaitTable[i] = false;
	for (int i = 0; i < THREAD_MAX_THREADS+1; i++)
		lock_cv_Table[i] = false;
	
	interrupts_on();
}

Tid
thread_id()
{
	int status = interrupts_enabled();
	interrupts_off();
	for (int i = 0; i < THREAD_MAX_THREADS+1; i++){
		if (contextQueue[i] && contextQueue[i]->state == 1){
			int res = contextQueue[i]->tid;
			return res;
		}
	}
	if (status) interrupts_on();
	return THREAD_INVALID;
}

int
thread_index()
{
	for (int i = 0; i < THREAD_MAX_THREADS+1; i++){
		if (contextQueue[i] && contextQueue[i]->state == 1){
			return i;
		}
	}
	return -1;
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
	interrupts_off();
	getcontext(&main_uc);
	if (numThr >= THREAD_MAX_THREADS){
		interrupts_on();
		return THREAD_NOMORE;
	}
	else numThr++;
	
	if (++store == THREAD_MAX_THREADS+1)
		store = 0;
	struct thread* th = (struct thread*) malloc (sizeof(struct thread));
	// find the first available tid
	for (int x = highThreadNum+1; x < THREAD_MAX_THREADS + highThreadNum+1; x++){
		int i = x % THREAD_MAX_THREADS;
		if (tidQueue[i] == 0){
			th->tid = i;
			tidQueue[i] = 1;
			firstWaitTable[i] = true;
			break;
		}
	}
	highThreadNum += 1;
	th->mycontext = main_uc;
	th->state = 2;
	th->who_is_waiting = wait_queue_create();

	th->mycontext.uc_link = &main_uc;
	void* stack = (void*) malloc (THREAD_MIN_STACK);
	if (stack == NULL) {
		interrupts_on();
		return THREAD_NOMEMORY;
	}
	th->st_addr = stack;
	unsigned long staddr = (unsigned long) stack + (unsigned long) THREAD_MIN_STACK;
	staddr = staddr - staddr%16 - 8;
	th->mycontext.uc_mcontext.gregs[REG_RSP] = (greg_t) staddr;
	th->mycontext.uc_mcontext.gregs[REG_RIP] = (greg_t) thread_stub;
	th->mycontext.uc_mcontext.gregs[REG_RDI] = (greg_t) fn;
	th->mycontext.uc_mcontext.gregs[REG_RSI] = (greg_t) parg;
	contextQueue[store] = th;

	int res = th->tid;
	interrupts_on();
	return res;
}

Tid
thread_yield(Tid want_tid)
{
	interrupts_off();
	if (stack_to_free){
		free(stack_to_free);
		stack_to_free = NULL;
	}
	bool goQueue = (want_tid == THREAD_ANY);
	// loop to find the first ready state
	// if out of scale, then reset to zero
	// if no ready, return THREAD_NONE
	if (goQueue){
		for (int i = q_id; i < q_id + THREAD_MAX_THREADS+1; i++){
			int index = i;
			if (i >= THREAD_MAX_THREADS+1) index = i - 1 - THREAD_MAX_THREADS;
			if (contextQueue[index] && (contextQueue[index]->state == 2 || contextQueue[index]->state == 3)){
				want_tid = index;
				goto thread_processing;
			}
		}
		interrupts_on();
		return THREAD_NONE;
	}
	else if (want_tid == THREAD_SELF || want_tid == thread_id()){
		int res = thread_id();
		interrupts_on();
		return res;
	}
	else if (want_tid < 0){
		interrupts_on();
		return THREAD_INVALID;
	} 
	// if a number is input, loop to find where its tid is at
	// if not found, then return INVALID
	// make want_tid be the index in queue
	else {
		for (int i = q_id; i < q_id + THREAD_MAX_THREADS+1; i++){
			int index = i;
			if (i >= THREAD_MAX_THREADS+1) index = i - 1 - THREAD_MAX_THREADS;
			if (contextQueue[index] && contextQueue[index]->tid == want_tid){
				if (contextQueue[index]->state == 2 || contextQueue[index]->state == 3){
					want_tid = index;
					goto thread_processing;
				}
				else if (contextQueue[index]->state == 1){
					int res = contextQueue[index]->tid;
					interrupts_on();
					return res;
				}
			}
		}
		interrupts_on();
		return THREAD_INVALID;
	}
	thread_processing:
	contextQueue[want_tid]->fflag = true;
	int res = contextQueue[want_tid]->tid;
	getcontext(&main_uc);

	if (contextQueue[want_tid] && contextQueue[want_tid]->fflag){
		// deal with thread kill state
		if (contextQueue[want_tid]->state == 3){
			Tid threadID = thread_id();
			int th_idx = thread_index();
			contextQueue[th_idx]->state = 0;
			contextQueue[want_tid]->state = 0;
			if (++store == THREAD_MAX_THREADS+1) store = 0;
			struct thread* th = (struct thread*) malloc (sizeof(struct thread));
			th->tid = threadID;
			th->mycontext = main_uc;
			th->state = 2;
			th->st_addr = contextQueue[th_idx]->st_addr;
			th->who_is_waiting = contextQueue[th_idx]->who_is_waiting;

			free(contextQueue[th_idx]);
			contextQueue[th_idx] = NULL;
			free(contextQueue[store]);
			contextQueue[store] = th;

			// wakeup
			exitTable[contextQueue[want_tid]->tid] = 9;
			thread_wakeup(contextQueue[want_tid]->who_is_waiting,1);

			// delete
			numThr--;
			tidQueue[contextQueue[want_tid]->tid] = 0;
						
			wait_queue_destroy(contextQueue[want_tid]->who_is_waiting);
			free(contextQueue[want_tid]->st_addr);
			free(contextQueue[want_tid]);
			contextQueue[want_tid] = NULL;

			for (int i = q_id; i < q_id + THREAD_MAX_THREADS+1; i++){
				int index = i;
				if (i >= THREAD_MAX_THREADS+1) index = i - 1 - THREAD_MAX_THREADS;
				if (contextQueue[index] && contextQueue[index]->state == 2){
					want_tid = index;
					res = contextQueue[want_tid]->tid;
					break;
				}
			}
			contextQueue[want_tid]->state = 1;
			ucontext_t temp = contextQueue[want_tid]->mycontext;
			if (goQueue){
				if (want_tid == THREAD_MAX_THREADS) q_id = 0;
				else q_id = want_tid+1;
			}
			contextQueue[want_tid]->fflag = false;
			setcontext(&temp);
		}
		else if (contextQueue[want_tid]->state == 2){
			// dequeue and enqueue
			Tid threadID = thread_id();
			int th_idx = thread_index();

			contextQueue[th_idx]->state = 0;
			contextQueue[want_tid]->state = 1;
			if (++store == THREAD_MAX_THREADS+1) store = 0;
		
			struct thread* th = (struct thread*) malloc (sizeof(struct thread));
			th->tid = threadID;
			th->mycontext = main_uc;
			th->state = 2;
			th->st_addr = contextQueue[th_idx]->st_addr;
			th->who_is_waiting = contextQueue[th_idx]->who_is_waiting;

			free(contextQueue[th_idx]);
			contextQueue[th_idx] = NULL;
			free(contextQueue[store]);
			contextQueue[store] = th;

			// swapcontext(&contextQueue[curr]->mycontext, &contextQueue[want_tid]->mycontext);
			ucontext_t temp = contextQueue[want_tid]->mycontext;
			if (goQueue){
				if (want_tid == THREAD_MAX_THREADS) q_id = 0;
				else q_id = want_tid+1;
			}
			contextQueue[want_tid]->fflag = false;
			setcontext(&temp);
		}
	}

	interrupts_on();
	return res;
	
	// return THREAD_FAILED;
}

void
thread_exit(int exit_code)
{
	interrupts_off();
	int th_id = thread_index();

	if (stack_to_free){
		free(stack_to_free);
		stack_to_free = NULL;
	}
	
	// wakeup
	exitTable[contextQueue[th_id]->tid] = exit_code;
	thread_wakeup(contextQueue[th_id]->who_is_waiting,1);

	// remove and free
	contextQueue[th_id]->state = 0;
	numThr--;
	tidQueue[contextQueue[th_id]->tid] = 0;

	wait_queue_destroy(contextQueue[th_id]->who_is_waiting);
	stack_to_free = contextQueue[th_id]->st_addr;
	// free(contextQueue[th_id]->st_addr);
	free(contextQueue[th_id]);
	contextQueue[th_id] = NULL;
	
	for (int i = q_id; i < q_id + THREAD_MAX_THREADS+1; i++){
		int index = i;
		if (i >= THREAD_MAX_THREADS+1) index = i - 1 - THREAD_MAX_THREADS;
		if (contextQueue[index] && contextQueue[index]->state == 2){
			if (index == THREAD_MAX_THREADS) q_id = 0;
			else q_id = index+1;
			contextQueue[index]->state = 1;
			ucontext_t temp = contextQueue[index]->mycontext;
			// interrupts_on();
			setcontext(&temp);
			break; // not necessary
		}
	}

	interrupts_on();
	exit(0);
}

Tid
thread_kill(Tid tid)
{
	interrupts_off();
	for (int i = 0; i < THREAD_MAX_THREADS+1; i++){
		if (contextQueue[i] && contextQueue[i]->tid == tid){
			if (contextQueue[i]->state == 1 || contextQueue[i]->state == 3){
				interrupts_on();
				return THREAD_INVALID;
			}
			if (contextQueue[i]->state == 2){
				contextQueue[i]->state = 3;
				interrupts_on();
				return tid;
			}
		}
		else if (contextQueue[i] && contextQueue[i]->state != 0){
			// try to find in the wait q from other threads
			// loop through until last thread has no waiting thread
			if (contextQueue[i]->who_is_waiting->run_id == contextQueue[i]->who_is_waiting->str_id)
				continue;
			struct thread* temp = contextQueue[i]->who_is_waiting->wait_q[contextQueue[i]->who_is_waiting->run_id];
			while (temp){
				if (temp->tid == tid){
					if (temp->state == 2){
						temp->state = 3;
						exitTable[tid] = 9;
						interrupts_on();
						return tid;
					}
					else {
						interrupts_on();
						return THREAD_INVALID;
					}
				}
				if (!temp->who_is_waiting) break;
				if (temp->who_is_waiting->str_id == temp->who_is_waiting->run_id) break;
				temp = temp->who_is_waiting->wait_q[temp->who_is_waiting->run_id];
			}	
		}
	}
	interrupts_on();
	return THREAD_INVALID;
	//return THREAD_FAILED;
}

/*******************************************************************
 * Important: The rest of the code should be implemented in Lab 3. *
 *******************************************************************/

/* make sure to fill the wait_queue structure defined above */

struct wait_queue *
wait_queue_create()
{
	int status = interrupts_enabled();
	interrupts_off();
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	wq->str_id = 0;
	wq->run_id = 0;

	if (status) interrupts_on();
	return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	int status = interrupts_enabled();
	interrupts_off();

	free(wq);
	wq = NULL;

	if (status) interrupts_on();
}

Tid
thread_sleep(struct wait_queue *queue)
{
	int status = interrupts_enabled();
	interrupts_off();
	if (queue == NULL){
		if (status) interrupts_on();
		return THREAD_INVALID;
	}
	int index = q_id;
	int th_id = thread_index();
	bool onlyOnce = true;
	int result;

	// check and find the next thread to run
	for (int i = q_id; i < q_id + THREAD_MAX_THREADS+1; i++){
		index = i;
		if (i >= THREAD_MAX_THREADS+1) index = i - 1 - THREAD_MAX_THREADS;
		if (contextQueue[index] && contextQueue[index]->state == 2){
			goto success;
		}
	}
	if (status) interrupts_on();
	return THREAD_NONE;

	success:
	// add to wait queue as ready state
	getcontext(&main_uc);

	if (onlyOnce){
		onlyOnce = false;
		contextQueue[th_id]->mycontext = main_uc;
		free(queue->wait_q[queue->str_id]); // free its pos before storing to it

		queue->wait_q[queue->str_id] = th_copy(contextQueue[th_id]);
		queue->wait_q[queue->str_id]->state = 2; // store as ready state

		if (process_lock_cv){
			lock_cv_Table[thread_id()] = true;
			thread_lock_cv[thread_id()] = queue->wait_q[queue->str_id];
			process_lock_cv = false;
		}

		update_str_id(queue);

		// delete from running queue
		free(contextQueue[th_id]);
		contextQueue[th_id] = NULL;
		
		// run next available thread
		contextQueue[index]->state = 1;
		ucontext_t temp = contextQueue[index]->mycontext;
		result = contextQueue[index]->tid;

		setcontext(&temp);	
	}

	if (status) interrupts_on();
	return result;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	int status = interrupts_enabled();
	interrupts_off();
	// first check if return 0 invalid
	if (queue == NULL){
		if (status) interrupts_on();
		return 0;
	}

	if (queue->run_id == queue->str_id) { 	// if empty
		if (status) interrupts_on();
		return 0;
	}
	// if all is zero, get one to running queue
	// delete from wait queue
	// add to running queue
	if (all == 0){
		if (++store == THREAD_MAX_THREADS+1) store = 0; // find where to store in run q
		contextQueue[store] = th_copy(queue->wait_q[queue->run_id]);
		if (process_lock_cv) {
			lock_cv_Table[contextQueue[store]->tid] = false;
			thread_lock_cv[contextQueue[store]->tid] = NULL;
			process_lock_cv = false;
		}
		free(queue->wait_q[queue->run_id]);
		queue->wait_q[queue->run_id] = NULL;
		if (++queue->run_id == THREAD_MAX_THREADS+1) queue->run_id = 0;
		if (status) interrupts_on();
		return 1;
	}
	// if all is one, then get all to running queue
	// delete from wait queue
	// add to running queue
	// repeat till run_id goes to store_id
	else if (all == 1){
		int count = 0;
		while(queue->run_id != queue->str_id){
			count++;
			if (++store == THREAD_MAX_THREADS+1) store = 0; // find where to store in run q
			contextQueue[store] = th_copy(queue->wait_q[queue->run_id]);
			if (process_lock_cv) {
				lock_cv_Table[contextQueue[store]->tid] = false;
				thread_lock_cv[contextQueue[store]->tid] = NULL;
				process_lock_cv = false;
			}
			free(queue->wait_q[queue->run_id]);
			queue->wait_q[queue->run_id] = NULL;
			if (++queue->run_id == THREAD_MAX_THREADS+1) queue->run_id = 0;
		}

		if (status) interrupts_on();
		return count;
	}

	if (status) interrupts_on();
	return 0;
}

/* suspend current thread until Thread tid exits */
Tid
thread_wait(Tid tid, int *exit_code)
{
	int status = interrupts_enabled();
	interrupts_off();

	// in the run queue find that tid thread
	for (int i = 0; i < THREAD_MAX_THREADS+1; i++){
		if (contextQueue[i] && contextQueue[i]->state != 0){
			if (contextQueue[i]->tid == tid){ // if found in the running queue
				// if is the running thread
				if (contextQueue[i]->state == 1)
					goto return_invalid;
				// if wait queue is not empty
				if (contextQueue[i]->who_is_waiting->run_id != contextQueue[i]->who_is_waiting->str_id)
					goto return_invalid;
				// else valid
				firstWaitTable[tid] = false;
				thread_sleep(contextQueue[i]->who_is_waiting);
				if (exit_code) *exit_code = exitTable[tid];
				if (status) interrupts_on();
				return tid;
			}
			else{ // try to find in the wait q from other threads
				// loop through until last thread has no waiting thread
				if (contextQueue[i]->who_is_waiting->run_id == contextQueue[i]->who_is_waiting->str_id)
					continue;
				struct thread* temp = contextQueue[i]->who_is_waiting->wait_q[contextQueue[i]->who_is_waiting->run_id];
				while (temp){
					if (temp->tid == tid){
						// if is the running thread
						if (temp->state == 1)
							goto return_invalid;
						if (temp->who_is_waiting->run_id != temp->who_is_waiting->str_id)
							goto return_invalid;
						// else valid
						firstWaitTable[tid] = false;
						thread_sleep(temp->who_is_waiting);
						if (exit_code) *exit_code = exitTable[tid];
						if (status) interrupts_on();
						return tid;
					}
					temp = temp->who_is_waiting->wait_q[temp->who_is_waiting->run_id];
				}	
			}
		}
	}
	
	if (lock_cv_Table[tid]){
		firstWaitTable[tid] = false;
		thread_sleep(thread_lock_cv[tid]->who_is_waiting);
		if (exit_code) *exit_code = exitTable[tid];
		if (status) interrupts_on();
		return tid;
	}
	
	if (firstWaitTable[tid]){
		firstWaitTable[tid] = false;
		if (exit_code) *exit_code = exitTable[tid];
		if (status) interrupts_on();
		return tid;
	}

	return_invalid:
	// if not found
	if (status) interrupts_on();
	return THREAD_INVALID;
}

struct lock {
	struct wait_queue* wq;
	bool acquired;
	int tid;
};

struct lock *
lock_create()
{
	int status = interrupts_enabled();
	interrupts_off();
	struct lock *lock;

	lock = malloc(sizeof(struct lock));
	assert(lock);

	lock->wq = wait_queue_create();
	lock->acquired = false;
	lock->tid = -1;

	if (status) interrupts_on();
	return lock;
}

void
lock_destroy(struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(lock != NULL);

	wait_queue_destroy(lock->wq);
	free(lock);
	lock = NULL;

	if (status) interrupts_on();
}

void
lock_acquire(struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(lock != NULL);

	while(lock->acquired){ // if lock is acquired
		process_lock_cv = true;
		thread_sleep(lock->wq);
	}

	if (!lock->acquired){ // if not acquired
		lock->acquired = true;
		lock->tid = thread_id();
	}

	if (status) interrupts_on();
}

void
lock_release(struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(lock != NULL);

	if (lock->acquired && lock->tid == thread_id()){
		lock->acquired = false;
		lock->tid = -1;
		process_lock_cv = true;
		thread_wakeup(lock->wq,1);
	}
	if (status) interrupts_on();
}

struct cv {
	struct wait_queue* wq;
};

struct cv *
cv_create()
{
	int status = interrupts_enabled();
	interrupts_off();
	
	struct cv *cv;

	cv = malloc(sizeof(struct cv));
	assert(cv);

	cv->wq = wait_queue_create();

	if (status) interrupts_on();
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	int status = interrupts_enabled();
	interrupts_off();

	assert(cv != NULL);

	wait_queue_destroy(cv->wq);
	free(cv);
	cv = NULL;

	if (status) interrupts_on();
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if (lock->acquired && lock->tid == thread_id()){
		lock_release(lock);
		thread_sleep(cv->wq);
	}
	else
		printf("Lock is not acquired or is not acquired by the running thread\n");
	
	lock_acquire(lock);  // reacquire lock
	if (status) interrupts_on();
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if (lock->acquired && lock->tid == thread_id()){
		thread_wakeup(cv->wq,0);
	}
	else
		printf("Lock is not acquired or is not acquired by the running thread\n");

	if (status) interrupts_on();
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	int status = interrupts_enabled();
	interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if (lock->acquired && lock->tid == thread_id()){
		thread_wakeup(cv->wq,1);
	}
	else
		printf("Lock is not acquired or is not acquired by the running thread\n");

	if (status) interrupts_on();
}
