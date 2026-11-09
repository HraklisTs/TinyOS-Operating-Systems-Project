
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include <assert.h>
#include "symposium.h"
#include "kernel_streams.h"

/* 
 The process thread table and related system calls:
 - CreateThread
 - ThreadJoin
 - ThreadSelf
 - ThreadDetach
 - ThreadExit

 */

/*
  This function is provided as an argument to spawn,
  to execute the main thread of a process.
*/

void pthread_start()
{
  PTCB* random = (PTCB *)sys_ThreadSelf();

  Task call = random->main_task;
  int argl = random->argl;
  void* args = random->args;

  int exitVal;
  exitVal = call(argl, args);

  ThreadExit(exitVal);
}

/** 
  @brief Create a new thread in the current process.
  */

Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  CURPROC->th_count++;

  /*Allocate memory for PTCB*/
  PTCB* ptcb = malloc(sizeof(PTCB));

  ptcb->Detached = 0;
  ptcb->wait_list = 0;  
  ptcb->cv_exit = COND_INIT;
  ptcb->Exited = 0;

  /*Pass the task and its arguments to the new PTCB for the thread*/
  ptcb->main_task = task;
  ptcb->argl = argl;

  if(args == NULL)
  {
    ptcb->args = NULL;
  }
  else
  {
    ptcb->args = args;
  }

  rlnode *ptcbnode = rlnode_init(&ptcb->pNode,ptcb);
  rlist_push_back(&CURPROC->thread_list,ptcbnode);

  /*Spawn the thread*/
  if(task != NULL){
    ptcb->tcb = spawn_thread(CURPROC,pthread_start);
    wakeup(ptcb->tcb);      //Wake up the thread
  }

  return (Tid_t) ptcb->tcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
  PTCB* curr = NULL; /*Current ptcb*/
  int lengthPtcb = rlist_len(&CURPROC->thread_list);
  int count = 0;
  int flag = 0; /* Found or not specific thread */
  rlnode *pnode = (&CURPROC->thread_list)->next;

  while(count < lengthPtcb || flag == 0)
  {
    if(pnode->ptcb->tcb == (TCB*)CURTHREAD) /*If is stored*/
    {
      curr = pnode->ptcb; //copy
      flag = 1;
    }
    pnode = pnode->next;
    count++;
  }

     return (Tid_t) curr;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  int count = 0;
  int flag = 0;
  int listLen = rlist_len(&CURPROC->thread_list);
  PTCB* object = NULL;
  rlnode *node = (&CURPROC->thread_list)->next;
  
  //Search the PTCB for the Tid given as argument
  while( count < listLen || flag == 0)
  {
    if(node->ptcb->tcb == (TCB*)tid)
    {
      object = node->ptcb;
      flag = 1;
    }

    node = node->next;
    count++;
  }

  /*Return if the given thread doesnt exist or it is Detached*/
  if( (object == NULL) || (object->Detached == 1) || ( CURTHREAD == (TCB*)tid) )
   {
		return -1;   	
   } 
   else
   {
   	 /*Increase the wait list for the thread*/
  	 object->wait_list++;
  	
  	 /*Wait until the given thread sleeps*/
  	 while(object->Exited == 0 && object->Detached != 1)
   	{
    	 kernel_wait(&object->cv_exit,SCHED_USER);
   	}

 	 /*Decrease the wait list and pass the exit value in the thread's PTCB*/
  		object->wait_list--;

  	if(object->Detached == 1)
  	{
    	return -1;
  	}
  	else
  	{
    	if(object->Exited == 1)
    	{
      		if(exitval != NULL) {*exitval = object->exitvalue;}
    	}
  	}

 	 /*Release the node*/
  	if(object->wait_list == 0)
  	{
    	rlist_remove(&object->pNode);
    	free(object);
  	}  	
   }
 

  return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  int listLen = rlist_len(&CURPROC->thread_list);
  int count = 0;

  PTCB* random = NULL;

  rlnode *node = (&CURPROC->thread_list)->next;

  while(count < listLen)
  {
    if(node->ptcb->tcb == (TCB *) tid)
    {
      random = node->ptcb;
    }

    node = node->next;
    count++;
  
  /*Return if unable to detach a Null thread or a zombie thread.*/
    if(random == NULL || random->Exited != 0)
    {
      return -1;
    }
    else   
    {
      random->Detached = 1;
      kernel_broadcast(&random->cv_exit);
    }

  }

  return 0;

}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{ 
  PTCB* ptcb = (PTCB*)sys_ThreadSelf();
  CURPROC->th_count--;
  ptcb->Exited = 1; /* About to exit. */

  if(ptcb->exitFlag == 0) 
  {
    ptcb->exitvalue = exitval;
  }

  //broadcast condVariables to kernel
  kernel_broadcast(&ptcb->cv_exit);

  //Check if current thread is the last one, inside a list linked with the current procedure (PCB)
  if(CURPROC->th_count == 0)
  {

      PCB *curproc=CURPROC; // make a local copy - cache for higher efficiency level

      //Clean up whatever sysExit would clean but also for a "regular" thread as well.
      //Lines cropped and adjusted from sysExit (kernel_proc.c), no need for them to be there any more

      /* Do all the other cleanup we want here, close files etc. */
      
      if(curproc->args) 
      {
        free(curproc->args);  //clean the argument stream
        curproc->args = NULL;
      }

        /* Clean up FIDT */
      for(int i=0;i<MAX_FILEID;i++) 
      {
        if(curproc->FIDT[i] != NULL) 
        {
          FCB_decref(curproc->FIDT[i]);
          curproc->FIDT[i] = NULL;
        }
      }

      while(!is_rlist_empty(&curproc->thread_list))
      {

        rlnode *currNode=rlist_pop_front(&curproc->thread_list);
        free(currNode->ptcb);


      }

       /* Reparent any children of the exiting process to the 
          initial task */
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) 
      {
        rlnode* child = rlist_pop_front(& curproc->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(& initpcb->children_list, child);
      }

      /* Add exited children to the initial task's exited list 
         and signal the initial task */
      if(!is_rlist_empty(& curproc->exited_list)) 
      {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }

      /* Put me into my parent's exited list */
      if(curproc->parent != NULL)
       {   /* Maybe this is init */
        rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
        kernel_broadcast(& curproc->parent->child_exit);
      }

      //whole process is now exited, or more like a ZOMBIE (@vsam said that zombie thing, not me)

      curproc->pstate = ZOMBIE;
  }
 //goodbye my lover, goodbye my friend..
 kernel_sleep(EXITED, SCHED_USER);
}