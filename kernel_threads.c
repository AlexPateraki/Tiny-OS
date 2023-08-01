
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "kernel_sys.h"


void start_thread() {
  int exitval;
  TCB* curthread = cur_thread();
  
  Task call = curthread->ptcb->task;
  int argl = curthread->ptcb->argl;
  void* args = curthread->ptcb->args;

  exitval = call(argl, args);
  ThreadExit(exitval);
}

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{   

  PCB* curproc= CURPROC;
    if(curproc==NULL){
      return NOTHREAD;
    }

  TCB* new_thread = NULL;
  new_thread = spawn_thread(curproc , start_thread);

  if(new_thread==NULL){
    return NOTHREAD;
  }
  

  PTCB* new_ptcb = xmalloc(sizeof(PTCB));

  new_ptcb->tcb=NULL;
  new_ptcb->task=NULL;
  new_ptcb->argl=0;
  new_ptcb->args=NULL;
  new_ptcb->exitval=0;
  new_ptcb->exited=0;
  new_ptcb->detached=0;
  new_ptcb->exit_cv=COND_INIT;
  new_ptcb->refcount = 0;

  rlnode_init(&new_ptcb->ptcb_list_node, new_ptcb);

  new_thread->ptcb = new_ptcb;

  new_ptcb->tcb = new_thread;
  new_ptcb->task=task;
  new_ptcb->argl=argl;
  new_ptcb->args=args;

  rlist_push_front(&curproc->ptcb_list, &new_ptcb->ptcb_list_node);

  curproc->thread_count++;


  wakeup(new_thread);

  return (Tid_t)(new_ptcb);

}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  **/
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

  PTCB* join_ptcb=(PTCB*)tid;
  PCB* curproc=CURPROC;

  /**
   * CURTHREAD TRIES TO JOIN BUT ITS ALREADY IN THE PTCB LIST
   *       OR  TRIES TO JOIN A DETACHED THREAD
   * CURTHEAD AND join_ptcb HAVE DIFFERENT PCB OWNER
  **/
  if(rlist_find(&curproc->ptcb_list, join_ptcb, NULL) == NULL){
    return -1;
  }
  if(tid==sys_ThreadSelf()){
    return -1;
  }
  if(join_ptcb->detached==1){
    return -1;
  }
  
  /*increase refcount*/
  join_ptcb->refcount++;

  while( (join_ptcb->exited == 0) && (join_ptcb->detached==0) ) {
    kernel_wait(&join_ptcb->exit_cv, SCHED_USER);
  }

  // decrease refcount
  join_ptcb->refcount--;

  // if joined thread became detached, return fail value
  if(join_ptcb->detached == 1) {
    return -1;
  }

  if(exitval != NULL){
    *exitval = join_ptcb->exitval;
  }

  // Joined Thread is not useful remove its PTCB
  if(join_ptcb->refcount == 0) {
    rlist_remove(&join_ptcb->ptcb_list_node);
    free(join_ptcb);
  }

  
  return 0;
}

/**
  @brief Detach the given thread.
  **/
int sys_ThreadDetach(Tid_t tid)
{
  PTCB *det_ptcb =(PTCB *)tid;
  PCB* curproc = CURPROC;

  // checking legality of detaching
  if((rlist_find(&curproc->ptcb_list, det_ptcb, NULL) == NULL) ){
    return -1;
  }else if(det_ptcb->exited == 1){
    //delete_PTCB(det_ptcb);
    return -1; 
  }

  det_ptcb->detached = 1;

  // if the input thread is joined by another, signal the joiners
  if(det_ptcb->refcount > 0){
    kernel_broadcast(&det_ptcb->exit_cv);

  }

    // reset possible joins of the given thread
    //det_ptcb->refcount = 0;

  return 0;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval){
  PCB* curproc = CURPROC;
  TCB *curthread = cur_thread();

  // thread count is the number of active threads of the current process
  curproc->thread_count--;
  int remainingThreads = curproc->thread_count;


  if(remainingThreads==0){// cleanup process children if it is not init
    if (get_pid(curproc)!=1){
    /* Reparent any children of the exiting process to the 
    initial task */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Add exited children to the initial task's exited list 
       and signal the initial task */
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit); 
  }

     assert(is_rlist_empty(& curproc->children_list));
     assert(is_rlist_empty(& curproc->exited_list));

  //if(remainingThreads == 0) {
  
    /* Release the args data */
    if(curproc->args) {
      free(curproc->args);
      curproc->args = NULL;
    }

    /* Clean up FIDT */
    for(int i=0;i<MAX_FILEID;i++) {
      if(curproc->FIDT[i] != NULL) {
        FCB_decref(curproc->FIDT[i]);
        curproc->FIDT[i] = NULL;
      }
    }

    /* Disconnect my main_thread */
    curproc->main_thread = NULL;

    /* Now, mark the process as exited. */
    curproc->pstate = ZOMBIE;


    // before exiting of the current process,
    // delete ALL the PTCBs of this process
    
    kernel_broadcast(&curthread->ptcb->exit_cv);

    /* 
      Delete all the PTCBs from current process 
    */
    rlnode* List = &curproc->ptcb_list;
    rlnode* front= List->next; 
    rlnode* previous = front;

    while(front!=List) {
      // free the ptcb
      PTCB* ptcb_to_delete = front->ptcb;
      free(ptcb_to_delete);
      
      previous = front;
      front = front->next;

      rlist_remove(previous);
    
    }
  
}
  else {
    
    // checking when to delete ptcb in order not to lose the exit value of PTCB
    if(curthread->ptcb->detached == 1){

      // delete PTCB of the runnning thread 
      curthread->ptcb->exited = 1;
      kernel_broadcast(&curthread->ptcb->exit_cv);
      //int flag = delete_PTCB(curthread->ptcb);
      //assert(flag==0);
    }
    else{ 
      //store the exit status of running thread in order to be read from the thread which called join
      curthread->ptcb->exited = 1;
      curthread->ptcb->exitval = exitval;
      kernel_broadcast(&curthread->ptcb->exit_cv);
      
    }

  }
  
  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);

}

  
