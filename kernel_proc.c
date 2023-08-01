
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
#include "kernel_sys.h"
#include "util.h"
#include <string.h>

/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;

  //initialize new field
  pcb->thread_count=0;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    newproc->main_thread = spawn_thread(newproc, start_main_thread);
    
  //new thread created so increase thread_count of the new process
  newproc->thread_count++;


  /*Create a new PTCB*/
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

  //connect new_ptcb with the main_thread
  new_ptcb->task = newproc->main_task;
  new_ptcb->argl = newproc->argl;
  new_ptcb->args = newproc->args;
  new_ptcb->tcb = newproc->main_thread;

  newproc->main_thread->ptcb = new_ptcb;
  rlnode_init(&newproc->ptcb_list, NULL);
  rlist_push_back(&newproc->ptcb_list, &new_ptcb->ptcb_list_node);


    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  } 
  sys_ThreadExit(exitval);
}


typedef struct procinfo_cb {
  procinfo info;
  PCB *cursor;
}procinfo_cb;


int procinfo_read(void* this, char *buf, unsigned int size){
  
  if (this == NULL) 
  {
      return -1;
  }

  procinfo_cb *proc_info = (procinfo_cb *)this;// create a processinfo_cb

  // a loop searching for the following not used from PT process
  while(proc_info->cursor->pstate == FREE ) {
    //limits
    if((PT+MAX_PROC-1) > proc_info->cursor ) { 
      // next position of the cursor
      proc_info->cursor++; 
    }else{
      return 0;
    }
  }

  // connecting info from process to proc_info
  proc_info->info.pid = get_pid(proc_info->cursor);
  proc_info->info.ppid = get_pid(proc_info->cursor->parent); 
  proc_info->info.alive = proc_info->cursor->pstate;

  if(proc_info->cursor->pstate == ALIVE){
    proc_info->info.alive = 1;
  }
  else {
    proc_info->info.alive = 0;
  }
  
  proc_info->info.thread_count = proc_info->cursor->thread_count; 
  proc_info->info.main_task = proc_info->cursor->main_task;
  proc_info->info.argl = proc_info->cursor->argl; 
  
  if(proc_info->cursor->args != NULL) {
    //transfer data
    if(proc_info->cursor->argl < PROCINFO_MAX_ARGS_SIZE){ 
      memcpy(proc_info->info.args, proc_info->cursor->args, proc_info->cursor->argl);
    }else{
      memcpy(proc_info->info.args, proc_info->cursor->args, PROCINFO_MAX_ARGS_SIZE);
    }

  } else {
    // no data to transfer
    proc_info->info.args[0] = 0; 
  }
  
  // next process of PT
  proc_info->cursor++;

  // use memcpy to copy the information of process to buffer
  if (sizeof(proc_info->info) > size)
  {
    memcpy(buf, &proc_info->info, size);
    return size;
  }else{
    memcpy(buf, &proc_info->info, sizeof(proc_info->info));
    return sizeof(proc_info->info);
  }

}


int procinfo_close(void* this){

  if (this == NULL)
  {
      return -1;
  }

  procinfo_cb *proc_info = (procinfo_cb *)this;

  free(proc_info);
  return 0;
}

file_ops procinfo_ops = {
  .Open = null_open,
  .Read = procinfo_read,
  .Write = null_write,
  .Close = procinfo_close
};


Fid_t sys_OpenInfo()
{
  Fid_t fid;
  FCB *fcb;
  //return an error for the case in which FID is unaccessible
  if(FCB_reserve(1, &fid, &fcb) == 0){ 
    return NOFILE;
  }

  //connecting fcb info 
  //allocating space to save process info control block(using size of)
  fcb->streamfunc = &procinfo_ops;
  fcb->streamobj = xmalloc(sizeof(procinfo_cb));
  // start from the beginning of PT
  ((procinfo_cb *)fcb->streamobj)->cursor = PT; 

  return fid; 
}

