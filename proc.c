//TODOS OS COMENTARIOS EM PORTUGUÊS SÃO REFERENTES ÁS MODIFICAÇÕES. OS COMENTARIOS EM INGLES SÃO COMENTARIOS ORIGINAIS DOS ARQUIVOS OBTIDOS DE: 		 http://pdos.csail.mit.edu/6.828/2012/xv6.html


#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
// Must hold ptable.lock.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  acquire(&ptable.lock);

  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S
  p->tickets = DEFAULT_TICKETS; // da a quantidade padrao pro init

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;

  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(int tickets)
{
  int i, pid;
  struct proc *np;

  acquire(&ptable.lock);

  // Allocate process.
  if((np = allocproc()) == 0){
    release(&ptable.lock);
    return -1;
  }

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    release(&ptable.lock);
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

//**************************************************************************************************************
//														*
//Aqui entra a parte de inserção dos tickets.									*
//apos copiar os status do processo pai, tribui-se ao np(novo processo) os tickets informados			*
//conforme as especificações em proc.h (variaveis globais MIN_TCKTS, MAX_TCKTS, D_TCKTS)			*

  if (tickets == 0) {											     				
	np->tickets = DEFAULT_TICKETS;
  }else if (tickets < 0 || tickets < MIN_TICKETS) {
	np->tickets = MIN_TICKETS;
  }else if (tickets > MAX_TICKETS){
	np->tickets = MAX_TICKETS;
  }else {
	np->tickets = tickets; //isso so acontece quando os tickets repassados estão dentro dos parametros
  }

	cprintf("O processo %d foi forkado com o seguinte valor de tickets: %d\n", np->pid, np->tickets);
//														*
//aqui termina a atribuição dos tikets										*
// tudo foi separado por esse bloco para melhor visualização							*
//														*	
//**************************************************************************************************************

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;

}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//**************************************************************************************************************
//						FUNÇÃO RANDOMICA						*
//Função random encontrada em usertest.c									*
//

unsigned long randstate = 1;
unsigned int
rand(int total)
{
  randstate = (randstate * 1664525 + 1013904223)%total;
  return randstate;
}

// aqui termina a função randomica										*
// tudo foi separado por esse bloco para melhor visualização							*
//														*	
//**************************************************************************************************************

//**************************************************************************************************************
//					FUNÇÃO DE CONTAGEM DE TOTAL DE TICKETS					*
//aqui fica a parte onde eu conto os tickets dos processosem estado RUNNABLE					*
//para poder em seguid fazer o sorteio do processo que detentor do bilhete vencedor				*
//(ps) me disseram posteriormente a eu ja ter feito que havia outro metodo mais eficaz de realizar o sorteio	*
//sem ser atraves de contagem direta, por modulo;								*

int totaltickets(void) { //função que sera chamada para saber o total de tickets
	struct proc *p; //cria uma nova estrutura de processo para acessar a tabela de processos
	int total = 0; //contador de tickets
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) { //for que realiza a rotação dentro da tabela de procs
		if (p->state == RUNNABLE) { //verificando se o estado do processo é RUNNABLE, para poder incrementar os seus tikets na contagem
			total += p->tickets; //se o processo for RUNNABLE, os seus tickets são incrementados no total
		}
	}
	//cprintf("Total de tickets aptos a participar do sorteio: %d ", total);
	return total; //retorna o total de tickets
}

//														*
// aqui termina a contagem dos tickets										*
// tudo foi separado por esse bloco para melhor visualização							*
//														*	
//**************************************************************************************************************
//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  int sorteado;
  int qttickets;

  for(;;){
    
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
//**************************************************************************************************************
//						ESCALONADOR							*
//aqui fica a aprte  onde o escalonador faz o trabalho de procurar na tabela o processo que foi sorteado	*
//														*

	qttickets = totaltickets();
	if (qttickets > 0) {
		sorteado = rand(qttickets); //rand e a função randomica utilizada mais acima, a mesma doarquivo usertest.c
		if (sorteado < 0) { // nao pode ter um total negativo
			sorteado = sorteado * -1;
			cprintf("rand negativo\n");
		}
		if (qttickets < sorteado) { //se o numero sorteado e maior que a quantidade de tickets
			sorteado = sorteado % qttickets; //e feito um mod pra escolher um numero que esta no alcance do total de tickets
		}
			
			for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){//aqui, a variavel p percorre novamente a tabela de processos
				if (p->state == RUNNABLE) {//verificando se o provesso e valido (RUNNABLE) e caso seja,
					sorteado = sorteado - p->tickets;// desconta o valor de tickets do processo em questão da variável que representa os sorteados
				} 
				if(p->state != RUNNABLE || sorteado >= 0) {//aqui, se o valor de sorteado for inferior ou igual a 0, quer dizer que o ticket vencedor
					continue;// esta entre os tickets do processo analisado, portanto, este é o processo vencedor e pode continuar
				}


//cprintf("p name %s\n", p->name);
// Switch to chosen process.  It is the process's job
// to release ptable.lock and then reacquire it
// before jumping back to us.
proc = p;
switchuvm(p);
p->state = RUNNING;
swtch(&cpu->scheduler, p->context);
switchkvm();

// Process is done running for now.
// It should have changed its p->state before coming back.
proc = 0;
		}
	}
	release(&ptable.lock);

  }
}

//														*
// aqui termina a seleção do processo detentor do ticket sorteado						*
// tudo foi separado por esse bloco para melhor visualização							*
//														*	
//**************************************************************************************************************

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s tickets: %d", p->pid, state, p->name, p->tickets);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++){}
        //cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
