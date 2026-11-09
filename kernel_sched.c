
#include <assert.h>
#include <sys/mman.h>

#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_sched.h"
#include "tinyos.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

#define LVL 5

/*
   The thread layout.
  --------------------

  On the x86 (Pentium) architecture, the stack grows upward. Therefore, we
  can allocate the TCB at the top of the memory block used as the stack.

  +-------------+
  |   TCB       |
  +-------------+
  |             |
  |    stack    |
  |             |
  |      ^      |
  |      |      |
  +-------------+
  | first frame |
  +-------------+

  Advantages: (a) unified memory area for stack and TCB (b) stack overrun will
  crash own thread, before it affects other threads (which may make debugging
  easier).

  Disadvantages: The stack cannot grow unless we move the whole TCB. Of course,
  we do not support stack growth anyway!
 */

/*
  A counter for active threads. By "active", we mean 'existing',
  with the exception of idle threads (they don't count).
 */
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;

/* This is specific to Intel Pentium! */
#define SYSTEM_PAGE_SIZE (1 << 12)

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE \
	(((sizeof(TCB) + SYSTEM_PAGE_SIZE - 1) / SYSTEM_PAGE_SIZE) * SYSTEM_PAGE_SIZE)

#define THREAD_SIZE (THREAD_TCB_SIZE + THREAD_STACK_SIZE)

//#define MMAPPED_THREAD_MEM
#ifdef MMAPPED_THREAD_MEM

/*
  Use mmap to allocate a thread. A more detailed implementation can allocate a
  "sentinel page", and change access to PROT_NONE, so that a stack overflow
  is detected as seg.fault.
 */
void free_thread(void* ptr, size_t size) { CHECK(munmap(ptr, size)); }

void* allocate_thread(size_t size)
{
	void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	CHECK((ptr == MAP_FAILED) ? -1 : 0);

	return ptr;
}
#else
/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but
  cannot be made easily to 'detect' stack overflow.
 */
void free_thread(void* ptr, size_t size) { free(ptr); }

void* allocate_thread(size_t size)
{
	void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
	CHECK((ptr == NULL) ? -1 : 0);
	return ptr;
}
#endif

/*
  This is the function that is used to start normal threads.
*/

void gain(int preempt); /* forward */

static void thread_start()
{
	gain(1);
	CURTHREAD->thread_func();

	/* We are not supposed to get here! */
	assert(0);
}

/*
  Initialize and return a new TCB
*/

TCB* spawn_thread(PCB* pcb, void (*func)())
{
	/* The allocated thread size must be a multiple of page size */
	TCB* tcb = (TCB*)allocate_thread(THREAD_SIZE);

	/* Set the owner */
	tcb->owner_pcb = pcb;

	/* USER */
	tcb->priority=LVL/2;  /*Initializing the priority : first thread in the first list*/
	tcb->invprior = 0;
	tcb->last_ex = 0;

	/* Initialize the other attributes */
	tcb->type = NORMAL_THREAD;
	tcb->state = INIT;
	tcb->phase = CTX_CLEAN;
	tcb->thread_func = func;
	tcb->wakeup_time = NO_TIMEOUT;
	rlnode_init(&tcb->sched_node, tcb); /* Intrusive list node */

	tcb->its = QUANTUM;
	tcb->rts = QUANTUM;
	tcb->last_cause = SCHED_IDLE;	/* Cause for priorities calculation */
	tcb->curr_cause = SCHED_IDLE;

	/* Compute the stack segment address and size */
	void* sp = ((void*)tcb) + THREAD_TCB_SIZE;

	/* Init the context */
	cpu_initialize_context(&tcb->context, sp, THREAD_STACK_SIZE, thread_start);

#ifndef NVALGRIND
	tcb->valgrind_stack_id = VALGRIND_STACK_REGISTER(sp, sp + THREAD_STACK_SIZE);
#endif

	/* increase the count of active threads */
	Mutex_Lock(&active_threads_spinlock);
	active_threads++;
	Mutex_Unlock(&active_threads_spinlock);

	return tcb;
}

/*
  This is called with sched_spinlock locked !
 */
void release_TCB(TCB* tcb)
{
#ifndef NVALGRIND
	VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);
#endif

	free_thread(tcb, THREAD_SIZE);

	Mutex_Lock(&active_threads_spinlock);
	active_threads--;
	Mutex_Unlock(&active_threads_spinlock);
}

/*
 *
 * Scheduler
 *
 */

/*
 *  Note: the scheduler routines are all in the non-preemptive domain.
 */

/* Core control blocks */
CCB cctx[MAX_CORES];

/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in  SCHED.

  Also, the scheduler contains a linked list of all the sleeping
  threads with a timeout.

  Both of these structures are protected by @c sched_spinlock.
*/

rlnode SCHED[LVL]; /* The scheduler queue */
rlnode TIMEOUT_LIST; /* The list of threads with a timeout */
unsigned long num_ex = 0; /* Number (or round) of execution */
Mutex sched_spinlock = MUTEX_INIT; /* spinlock for scheduler queue */

/* Interrupt handler for ALARM */
void yield_handler() 
{	
	yield(SCHED_QUANTUM); 
}


/* Interrupt handle for inter-core interrupts */
void ici_handler()
{ /* noop for now... */
}

/*
  Possibly add TCB to the scheduler timeout list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_register_timeout(TCB* tcb, TimerDuration timeout)
{
	if (timeout != NO_TIMEOUT) {
		/* set the wakeup time */
		TimerDuration curtime = bios_clock();
		tcb->wakeup_time = (timeout == NO_TIMEOUT) ? NO_TIMEOUT : curtime + timeout;

		/* add to the TIMEOUT_LIST in sorted order */
		rlnode* n = TIMEOUT_LIST.next;
		for (; n != &TIMEOUT_LIST; n = n->next)
			/* skip earlier entries */
			if (tcb->wakeup_time < n->tcb->wakeup_time)
				break;
		/* insert before n */
		rl_splice(n->prev, &tcb->sched_node);
	}
}

/*
  Add TCB to the end of the scheduler list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_queue_add(TCB* tcb)
{
	/* Insert at the end of the scheduling list */
	rlist_push_back(&SCHED[tcb->priority], &tcb->sched_node);

	/* Restart possibly halted cores */
	cpu_core_restart_one();
}

/*
	Adjust the state of a thread to make it READY.

	*** MUST BE CALLED WITH sched_spinlock HELD ***
 */
static void sched_make_ready(TCB* tcb)
{
	assert(tcb->state == STOPPED || tcb->state == INIT);

	/* Possibly remove from TIMEOUT_LIST */
	if (tcb->wakeup_time != NO_TIMEOUT) {
		/* tcb is in TIMEOUT_LIST, fix it */
		assert(tcb->sched_node.next != &(tcb->sched_node) && tcb->state == STOPPED);
		rlist_remove(&tcb->sched_node);
		tcb->wakeup_time = NO_TIMEOUT;
	}

	/* Mark as ready */
	tcb->state = READY;

	/* Possibly add to the scheduler queue */
	if (tcb->phase == CTX_CLEAN)
		sched_queue_add(tcb);
}

/*
  Scan the \c TIMEOUT_LIST for threads whose timeout has expired, and
  wake them up.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_wakeup_expired_timeouts()
{
	/* Empty the timeout list up to the current time and wake up each thread */
	TimerDuration curtime = bios_clock();

	while (!is_rlist_empty(&TIMEOUT_LIST)) {
		TCB* tcb = TIMEOUT_LIST.next->tcb;
		if (tcb->wakeup_time > curtime)
			break;
		sched_make_ready(tcb);
	}
}

/*
	This function checks for all the threads that are stalled
	and calls a function to boost them  OR checks if a thread is 
	waiting for a locked mutex and boosts lower priority threads
	to unlock it.(Priority inversion)
*/


void check_delayed_threads(enum SCHED_CAUSE cause,int priority)
{
	if (cause == SCHED_MUTEX)	
	{  /*Temporarily decrease the priority of a LOCKED THREAD*/

    	TCB* current = CURTHREAD;
  		rlist_push_back(& SCHED[current->priority],rlist_remove(& current->sched_node));
	}
	else	 /*Temporarily increase the priority of a delayed THREAD */
	{                    
   		rlnode *node,*next;
		
    	for(int i=(LVL/2);i<LVL-1;i++)
    	{
			node = SCHED[i].next;	/*Check if it completes the circle*/

			while(node!=NULL && node->next != SCHED[i].next)
			{
				next = node->next; /*Keep the node in case of removal*/
				if((num_ex - node->tcb->last_ex > 10 && node->tcb->state == READY && node->tcb->phase==CTX_CLEAN))
					rlist_push_front(& SCHED[node->tcb->priority-1],rlist_remove(node));
		        
		        node = next;
			}
		}
  	}

  return;
}

/*
  Remove the head of the scheduler list, if any, and
  return it. Return NULL if the list is empty.
*/

static TCB* sched_queue_select(TCB* current)
{
	int i;
	/* Get the head of the SCHED list */
	rlnode * sel;

	for (i=0;i<LVL;i++)	/*Search the lists until a not null list is found*/
	{
		sel = rlist_pop_front(&SCHED[i]); 
  		if (sel->tcb != NULL)
  			break;
  	}

	TCB* next_thread = sel->tcb;

	if(next_thread == NULL)
		next_thread = (current->state == READY) ? current : &CURCORE.idle_thread;

	next_thread->its = QUANTUM;

	if(next_thread != NULL)
	{
		next_thread->last_ex = num_ex;

  	/*Checking for stalled threads to promote them every 20 executions*/
		if((num_ex >= 20) & (num_ex%20 == 0))
			check_delayed_threads(SCHED_USER,0);
	}

	return next_thread;	/* When the list is empty, this is NULL */
}

/*
  Make the process ready.
 */
int wakeup(TCB* tcb)
{
	int ret = 0;

	/* Preemption off */
	int oldpre = preempt_off;

	/* To touch tcb->state, we must get the spinlock. */
	Mutex_Lock(&sched_spinlock);

	if (tcb->state == STOPPED || tcb->state == INIT) {
		sched_make_ready(tcb);
		ret = 1;
	}

	Mutex_Unlock(&sched_spinlock);

	/* Restore preemption state */
	if (oldpre)	preempt_on;

	return ret;
}

/*
  Atomically put the current process to sleep, after unlocking mx.
 */
void sleep_releasing(Thread_state state, Mutex* mx, enum SCHED_CAUSE cause,
	TimerDuration timeout)
{
	assert(state == STOPPED || state == EXITED);

	TCB* tcb = CURTHREAD;

  /* 
    The tcb->state_spinlock guarantees atomic sleep-and-release.
    But, to access it safely, we need to go into the non-preemptive
    domain.
   */

	int preempt = preempt_off;
	Mutex_Lock(&sched_spinlock);

	/* mark the thread as stopped or exited */
	tcb->state = state;

	/* register the timeout (if any) for the sleeping thread */
	if (state != EXITED)
		sched_register_timeout(tcb, timeout);

	/* Release mx */
	if (mx != NULL)
		Mutex_Unlock(mx);

	/* Release the schduler spinlock before calling yield() !!! */
	Mutex_Unlock(&sched_spinlock);

	/* call this to schedule someone else */
	yield(cause);

	/* Restore preemption state */
	if (preempt) preempt_on;
}

/* This function is the entry point to the scheduler's context switching */

void yield(enum SCHED_CAUSE cause)
{
	/* Reset the timer, so that we are not interrupted by ALARM */
	TimerDuration remaining = bios_cancel_timer();

	/* We must stop preemption but save it! */
	int preempt = preempt_off;

	TCB* current = CURTHREAD; /* Make a local copy of current process, for speed */

	Mutex_Lock(&sched_spinlock);

	/* Update CURTHREAD state */
	switch(current->state)
	{
		case RUNNING:
			current->state = READY;
		case READY:		
			break;
		case STOPPED:
		case EXITED:
			break;
    	
    	default:
      		fprintf(stderr, "BAD STATE for current thread %p in yield: %d\n", current, current->state);
      		assert(0);  /* It should not be READY or EXITED ! */

	}

	/* Update CURTHREAD scheduler data */
	switch(cause)
	{
		case SCHED_QUANTUM: /* Expired quantum => priority -1 */
			if(current->priority < LVL - 1 )
				current->priority++;			
			break;
		
		case SCHED_IO:	/* I/O Thread => priority +1 */
			if( current->priority != 0 )
				current->priority--;
			break;

		case SCHED_MUTEX:
			if(current->priority < LVL - 1)
			{
				current->invprior = current->priority;
				current->priority ++;
				check_delayed_threads(cause,current->priority);
			}
			break;

		default:
			break;
	}


	num_ex++;

	/* Wake up threads whose sleep timeout has expired */
	sched_wakeup_expired_timeouts();

	/* Get next */
	TCB* next = sched_queue_select(current);
	assert(next != NULL);

	current->rts = remaining;
	current->last_cause = current->curr_cause;
	current->curr_cause = cause;
	current->prev_prior = current->priority;

	/* Save the current TCB for the gain phase */
	CURCORE.previous_thread = current;
	

	Mutex_Unlock(&sched_spinlock);

	/* Switch contexts */
	if (current != next) {
		CURTHREAD = next;
		cpu_swap_context(&current->context, &next->context);
	}

	/* This is where we get after we are switched back on! A long time
	   may have passed. Start a new timeslice...
	  */
	gain(preempt);
}

/*
  This function must be called at the beginning of each new timeslice.
  This is done mostly from inside yield().
  However, for threads that are executed for the first time, this
  has to happen in thread_start.

  The 'preempt' argument determines whether preemption is turned on
  in the new timeslice. When returning to threads in the non-preemptive
  domain (e.g., waiting at some driver), we need to not turn preemption
  on!
*/

void gain(int preempt)
{
	Mutex_Lock(&sched_spinlock);

	TCB* current = CURTHREAD;

	/* Mark current state */
	current->state = RUNNING;
	current->phase = CTX_DIRTY;
	current->rts = current->its;

	/* Take care of the previous thread */
	TCB* prev = CURCORE.previous_thread;
	if (current != prev) 
	{
		prev->phase = CTX_CLEAN;
		switch (prev->state) 
		{
			case READY:
				if (prev->type != IDLE_THREAD)
					sched_queue_add(prev);
			break;
			
			case EXITED:
				release_TCB(prev);
			break;
			
			case STOPPED:
			break;
		
			default:
				assert(0); /* prev->state should not be INIT or RUNNING ! */
		}
	}

	Mutex_Unlock(&sched_spinlock);

	/* Reset preemption as needed */
	if (preempt) preempt_on;

	/* Set a 1-quantum alarm */
	bios_set_timer(current->rts);
}

static void idle_thread()
{
	/* When we first start the idle thread */
	yield(SCHED_IDLE);

	/* We come here whenever we cannot find a ready thread for our core */
	while (active_threads > 0) {
		cpu_core_halt();
		yield(SCHED_IDLE);
	}

	/* If the idle thread exits here, we are leaving the scheduler! */
	bios_cancel_timer();
	cpu_core_restart_all();
}

/*
  Initialize the scheduler queue
 */
void initialize_scheduler()
{	
	int i;

	for(i = 0 ; i < LVL ; i++)
	{
		rlnode_init(&SCHED[i], NULL);
	} 
	rlnode_init(&TIMEOUT_LIST, NULL);
}

void run_scheduler()
{
	CCB* curcore = &CURCORE;

	/* Initialize current CCB */
	curcore->id = cpu_core_id;

	curcore->current_thread = &curcore->idle_thread;

	curcore->idle_thread.owner_pcb = get_pcb(0);
	curcore->idle_thread.type = IDLE_THREAD;
	curcore->idle_thread.state = RUNNING;
	curcore->idle_thread.phase = CTX_DIRTY;
	curcore->idle_thread.wakeup_time = NO_TIMEOUT;
	rlnode_init(&curcore->idle_thread.sched_node, &curcore->idle_thread);

	curcore->idle_thread.its = QUANTUM;
	curcore->idle_thread.rts = QUANTUM;

	curcore->idle_thread.curr_cause = SCHED_IDLE;
	curcore->idle_thread.last_cause = SCHED_IDLE;

	/* Initialize interrupt handler */
	cpu_interrupt_handler(ALARM, yield_handler);
	cpu_interrupt_handler(ICI, ici_handler);

	/* Run idle thread */
	preempt_on;
	idle_thread();

	/* Finished scheduling */
	assert(CURTHREAD == &CURCORE.idle_thread);
	cpu_interrupt_handler(ALARM, NULL);
	cpu_interrupt_handler(ICI, NULL);
}
