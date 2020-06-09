// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#define MAX_PSYC_PAGES    16 // In any given time only 16 pages must be in the main memory
#define MAX_TOTAL_PAGES   32 // maximum number of pages for a process, in main memory and swapFile

struct page {
  char* va;
  int isTaken;
  pte_t* pgdir;
  uint age;
  struct page* next;
  struct page* prev;
};

struct pageInFile{
  char* va;
  //uint offset;
};

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  //Swap file. must initiate with create swap file
  struct file *swapFile;      //page file
  int numOfPagesMem;          // number of pages in main memory
  int numOfPagesFile;          // number of pages in swapFile
  int numOfPageFaults;          // number of pagesFaults
  int numOfSwaps;                // number of times we swapped pages
  struct page mainMemPages[MAX_PSYC_PAGES];     // array for the pages in mainMemory
  struct pageInFile swapFilePages[MAX_PSYC_PAGES];    // array for the pages in swapFile
  struct page* head;            // head of the list
  struct page* tail;            // tail of the list
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

#define NONE 0
#define NFUA 1
#define LAPA 2
#define SCFIFO 3 
#define AQ 4

#define FALSE 0 
#define TRUE 1