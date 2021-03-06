#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"


extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

struct spinlock ref_lock;
char ref_counters[PHYSTOP/PGSIZE];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);

  acquire(&ref_lock);
  ref_counters[V2P(mem)/PGSIZE] = ref_counters[V2P(mem)/PGSIZE] + 1;
  release(&ref_lock);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);

  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }

    acquire(&ref_lock);
    ref_counters[V2P(mem)/PGSIZE] = ref_counters[V2P(mem)/PGSIZE] + 1;
    release(&ref_lock);

    #if SELECTION!=NONE 
    struct proc* p = myproc();
    if(p->pid > 2) // not touching the init and sh processes
    {
      if(p->numOfPagesFile == MAX_PSYC_PAGES)
        panic("allocuvm: more than 32 pages allocated");
      int i =0;
      if(p->numOfPagesMem < MAX_PSYC_PAGES)
      {
        for(; i< MAX_PSYC_PAGES; i++)
        {
          if(p->mainMemPages[i].isTaken == 0)
            {
              createNewPageMainMemory((char*)a,i,pgdir);
              break;
            }
        }
      }
      else
      {
        int index = removePageToSwapFile(pgdir);
        createNewPageMainMemory((char*)a , index,pgdir);
      }
    }
    #endif
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      
      acquire(&ref_lock);
      ref_counters[pa/PGSIZE] = ref_counters[pa/PGSIZE] - 1;
      if(ref_counters[pa/PGSIZE] == 0)
      {
        char *v = P2V(pa);
        kfree(v);
      }
      release(&ref_lock);
      
      #if SELECTION==NONE
      goto end;
      #endif
      struct proc* p = myproc();
      int i = 0;
      for(; i < MAX_PSYC_PAGES; i++)
      {
        if((p->mainMemPages[i].va == (char*)a) && (p->pgdir == pgdir))
          {
            p->numOfPagesMem -= 1;
            p->mainMemPages[i].va = (char*)0xffffffff;
            if(p->head == &p->mainMemPages[i])
              p->head = p->mainMemPages[i].next;
            if(p->tail == &p->mainMemPages[i])
              p->tail = p->mainMemPages[i].prev;
            p->mainMemPages[i].prev = 0;
            p->mainMemPages[i].next = 0;
            p->mainMemPages[i].isTaken = 0;
            p->mainMemPages[i].pgdir = 0;
          }
      }
      goto end;
      end:;
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  //char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    #if SELECTION==NONE
    goto endcopy;
    #endif
    if((myproc()->pid > 2) && (*pte & PTE_PG)){
      pte_t * pte1 = walkpgdir(d,(char*)i,1);
      *pte1 &= ~PTE_P;
      *pte1 |= PTE_PG;
      *pte1 &= ~PTE_W;
      lcr3(V2P(d));
      continue;
      goto endcopy;
    }
    endcopy:;
    if(!(*pte & PTE_P)){
      if(!(*pte & PTE_PG))
        panic("copyuvm: page not present");
    }
    *pte &= ~PTE_W;
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if(mappages(d, (void*)i, PGSIZE, pa, flags) < 0)
      goto bad;
    acquire(&ref_lock);
    ref_counters[pa/PGSIZE] = ref_counters[pa/PGSIZE] + 1;
    release(&ref_lock);
  }
  lcr3(V2P(myproc()->pgdir));
  return d;

bad:
  freevm(d);
  lcr3(V2P(myproc()->pgdir));
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// create a new page to main memory, put the page in 
//mainMemPages[index] with virtual address va in directory pgdir
// 
void
createNewPageMainMemory(char* va, int index,pde_t* pgdir)
{
  if(index == -1)
    return;
  #if SELECTION == SCFIFO
    createPageSCFIFO(va,index,pgdir);
  #endif
  #if SELECTION == NFUA
    createPageNFUA(va, index,pgdir);
  #endif
  #if SELECTION == LAPA
    createPageLAPA(va,index,pgdir);
  #endif
  #if SELECTION == AQ
    createPageAQ(va,index,pgdir);
  #endif
}

void
createPageSCFIFO(char* va, int index, pde_t* pgdir)
{
  struct proc* p = myproc();
  p->numOfPagesMem += 1;
  p->mainMemPages[index].va = va;
  p->mainMemPages[index].age = 0;
  p->mainMemPages[index].isTaken = 1;
  p->mainMemPages[index].pgdir = pgdir;
  if(p->head == 0){
    p->head = &p->mainMemPages[index];
    p->tail = p->head;
    p->mainMemPages[index].next = p->tail;
    p->mainMemPages[index].prev = p->head;
  }
  else
  {
    p->mainMemPages[index].next = p->head;
    p->mainMemPages[index].prev = p->tail;
    p->tail->next = &p->mainMemPages[index];
    p->head->prev = &p->mainMemPages[index];
    p->head = &p->mainMemPages[index];
  }
  pte_t * t = walkpgdir(pgdir, va, 0);
  *t |= PTE_P;
  *t &= ~PTE_PG;
  lcr3(V2P(pgdir));

  // struct page* headCur = p->head;
  // int i = 0;
  // for(; i < 16 ; i++)
  //   {
  //     if(headCur == 0)
  //       break;
  //     cprintf("parent va is %d\n",(uint)headCur->va);
  //     headCur = headCur->next;
  //   }
  //   cprintf("done\n");

}


void
createPageNFUA(char* va, int index, pde_t* pgdir)
{
  struct proc* p = myproc();
  p->numOfPagesMem += 1;
  p->mainMemPages[index].age = 0;
  p->mainMemPages[index].isTaken = 1;
  p->mainMemPages[index].va = va;
  p->mainMemPages[index].pgdir = pgdir;
  p->mainMemPages[index].prev = 0;
  p->mainMemPages[index].next = 0;
  pte_t * pte = walkpgdir(pgdir,va,0);
  *pte |= PTE_P;
  *pte &= ~PTE_PG;
  lcr3(V2P(pgdir));
  return; 
}

void
createPageLAPA(char* va, int index, pde_t* pgdir)
{
  struct proc* p = myproc();
  p->numOfPagesMem += 1;
  p->mainMemPages[index].age = 0xffffffff;
  p->mainMemPages[index].isTaken = 1;
  p->mainMemPages[index].va = va;
  p->mainMemPages[index].pgdir = pgdir;
  p->mainMemPages[index].prev = 0;
  p->mainMemPages[index].next = 0;
  pte_t * pte = walkpgdir(pgdir,va,0);
  *pte |= PTE_P;
  *pte &= ~PTE_PG;
  lcr3(V2P(pgdir));
  return;   
}

void
createPageAQ(char* va, int index, pde_t* pgdir)
{
    struct proc* p = myproc();
  p->numOfPagesMem += 1;
  p->mainMemPages[index].age = 0xffffffff;
  p->mainMemPages[index].isTaken = 1;
  p->mainMemPages[index].va = va;
  p->mainMemPages[index].pgdir = pgdir;
  p->mainMemPages[index].prev = 0;
  p->mainMemPages[index].next = 0;
  pte_t * pte = walkpgdir(pgdir,va,0);
  *pte |= PTE_P;
  *pte &= ~PTE_PG;
  lcr3(V2P(pgdir));

  fixAQ(index);
  return;   
}

void
fixAQ(int index)
{
  if (index == 0)
    return;
  int i = index;
  for(; i > 0; i--)
  {
    switchPages(i, i-1);
  }
  //cprintf("done fixing\n");
  return;
}


int
removePageToSwapFile(pde_t* pgdir)
{
  #if SELECTION == NONE
  goto end;
  #endif
  struct proc* p = myproc();
  int index = findPageToRemove();
  int j = 0;
  int found = 0;
  for(; j < MAX_PSYC_PAGES ; j++){
    if(p->swapFilePages[j].va == (char*)0xffffffff)
    {
      found = 1;
      p->swapFilePages[j].va = p->mainMemPages[index].va;
      break;
    }
  }
  if(!found)
  {
    panic("removePageToSwapFile: didnt found empty pace in pages in swap file array");
  }
  
  if(writeToSwapFile(p,p->mainMemPages[index].va,j*PGSIZE, PGSIZE) == -1)
    {
    panic("removePageToSwapFile: write to buffer");
    goto end;
    }
  p->numOfPagesMem -= 1;
  p->numOfPagesFile += 1;
  p->numOfSwaps +=1;
  p->mainMemPages[index].isTaken = 0;
  #if SELECTION==SCFIFO
  p->head = p->head->next;
  p->tail->next = p->head;
  p->head->prev = p->tail;
  #endif
  p->mainMemPages[index].pgdir = 0;
  pte_t *pte = walkpgdir(pgdir, p->mainMemPages[index].va, 0);
  if(pte == 0)
    panic("pte is 0");
  p->mainMemPages[index].va = (char*)0xffffffff;
  *pte |= PTE_PG;
  *pte &= (~PTE_P);
  acquire(&ref_lock);
  ref_counters[PTE_ADDR(*pte)/PGSIZE] = ref_counters[PTE_ADDR(*pte)/PGSIZE] - 1;
  if(ref_counters[PTE_ADDR(*pte)/PGSIZE] < 1)
    kfree((char*)P2V(PTE_ADDR(*pte)));
  release(&ref_lock);
  lcr3(V2P(pgdir));
  return index;
  end:
  return -1;
}



void
checkSegFault(char* va)
{
  struct proc* p = myproc();
  p->numOfPageFaults += 1;
  char* va1 = (char*)PGROUNDDOWN((uint)va);
  pte_t* t1 = walkpgdir(p->pgdir, va1, 0);
  if((*t1 & PTE_PG) == 0){
    copyOnWrite(va);
    return;
  }
  #if SELECTION == NONE
  return;
  #endif
  char* mem = kalloc();
  if (mem == 0)
    panic("no more memory");
  int j = 0;
  p->numOfPageFaults +=1;
  uint flags = PTE_FLAGS(*t1);
  *t1 = 0;
  flags |= PTE_P | PTE_W | PTE_U;
  flags &= ~PTE_PG;
  *t1 |= PTE_ADDR(V2P(mem)) | flags;

  acquire(&ref_lock);
  ref_counters[V2P(mem)/PGSIZE] = ref_counters[V2P(mem)/PGSIZE] + 1;
  release(&ref_lock);
  // for(; i < MAX_PSYC_PAGES; i++)
  // {
  //   if(p->mainMemPages[i].isTaken == 0)
  //   {
  //     for(; j< MAX_PSYC_PAGES; j++){
  //       if(p->swapFilePages[j].va == va1)
  //       {
  //         cprintf("am i even getting here?\n");
  //         if(readFromSwapFile(p,mem,j*PGSIZE,PGSIZE) == -1)
  //           panic("read from file in segfault");
  //         p->numOfPagesMem += 1;
  //         p->numOfPagesFile -= 1;
  //         p->mainMemPages[i].va = p->swapFilePages[j].va;
  //         p->mainMemPages[i].isTaken = 1;
  //         p->mainMemPages[i].pgdir = p->pgdir;
  //         p->swapFilePages[j].va = (char*)0xffffffff;

  //         #if SELECTION == NFUA
  //           p->mainMemPages[i].age = 0;
  //         #else
  //         #if SELECTION == LAPA
  //         p->mainMemPages[i].age = 0xffffffff;
  //         #else
  //         p->mainMemPages[i].next = p->head;
  //         p->mainMemPages[i].prev = p->tail;
  //         p->head = &p->mainMemPages[i];
  //         #endif
  //         #endif
  //         lcr3(V2P(p->pgdir));
  //         goto end;
  //       }
  //     }
  //       panic("checkSegFault: should never happen"); 
  //   }
  // }
  // didn't found any place in main memory, need to switch with a page in swap file
  int index = findPageToRemove();
  for(; j<MAX_PSYC_PAGES;j++)
  {
    if(p->swapFilePages[j].va == va1)
    {
      if(readFromSwapFile(p, mem, j*PGSIZE, PGSIZE) == -1)
        panic("read from file in segfault");
      //buffer contains the data of the page we want to put in main memory
      p->swapFilePages[j].va = p->mainMemPages[index].va;
      if (writeToSwapFile(p,p->mainMemPages[index].va, j*PGSIZE, PGSIZE) == -1)
        panic("checkSegFault: failed to write to memory");
      p->mainMemPages[index].isTaken = 1;
      pte_t* tToRemove = walkpgdir(p->pgdir,p->mainMemPages[index].va,0);
      *tToRemove |= PTE_PG;
      *tToRemove &= ~PTE_P;
      acquire(&ref_lock);
      ref_counters[PTE_ADDR(*tToRemove)/PGSIZE] = ref_counters[PTE_ADDR(*tToRemove)/PGSIZE] - 1;
      if(ref_counters[PTE_ADDR(*tToRemove)/PGSIZE] < 1)
        kfree((char*)(P2V(PTE_ADDR(*tToRemove))));
      release(&ref_lock);
      lcr3(V2P(p->pgdir));
      p->mainMemPages[index].va = va1;
      #if SELECTION == NFUA
      p->mainMemPages[index].age = 0;
      #else
      #if SELECTION == LAPA
      p->mainMemPages[index].age = 0xffffffff;
      #else 
      #if SELECTION == AQ
      fixAQ(15);          // put the new page in the head of the Q
      #endif
      #endif
      #endif
      p->numOfSwaps +=1;
      j = 16;;
    }
  }

  return;
}


// find the index of the page in main memory to remove according to the algorithem
// and remove it, return the index of the page in the main memory array
int
findPageToRemove()
{
  #if SELECTION == SCFIFO
    return findPageToRemoveSCFIFO();
  #endif
  #if SELECTION == NFUA
    //cprintf("im here\n");
    return findPageToRemoveNFUA();
  #endif
  #if SELECTION == AQ
    return findPageToRemoveAQ();
  #endif
  #if SELECTION == LAPA
    return findPageToRemoveLAPA();
  #endif
  return -1;
}

int
findPageToRemoveSCFIFO()
{
  int index = -1;
  struct proc* p = myproc();
  int found =0;
  while(!found)
  {
    struct page* current = p->head;
    if(current->isTaken)
    {
      pte_t * t = walkpgdir(current->pgdir,current->va, 0);
      if((*t & PTE_A) == 0)
      {
        int i = 0;
        for(; i<MAX_PSYC_PAGES; i++)
        {
          if(p->mainMemPages[i].isTaken && p->mainMemPages[i].va == current->va)
          {
            //cprintf("SCFIFO %d\n",(uint)current->va);
            index = i;
            found = 1;
          }
        }
      }
      else
      {
        *t = *t & (~PTE_A);
      }
    }
    if(!found)
    {
    p->head = p->head->next;
    p->tail = p->tail->next;
    current = p->head;
    }
  }
  return index;
}


int
findPageToRemoveNFUA(){
  struct proc* p = myproc();
  int index = -1;
  uint maxValue = (uint)0xffffffff;
  for(int i = 0 ; i < MAX_PSYC_PAGES; i++)
  {
    if(p->mainMemPages[i].isTaken)
    {
      if(p->mainMemPages[i].age <= maxValue)
      {
        maxValue = p->mainMemPages[i].age;
        index = i;
      }
    }
  }
  //cprintf("removing page number %d\n", index);
  return index;
}


int
findPageToRemoveLAPA()
{
  struct proc* p = myproc();
  int index = -1;
  int maxValue = 32;
  for(int i = 0; i < MAX_PSYC_PAGES ; i++)
  {
    if(p->mainMemPages[i].isTaken)
    {
      int count = 0;
      uint age = p->mainMemPages[i].age;
      for(int j = 0; j < 32 ; j++)
      {
        count += (age & 0x1);
        age = age >> 1;
      }
      if(count <= maxValue)
      {
        maxValue = count;
        index = i;
      }
    }
  }
  //cprintf("removing page numnber %d\n",index);
  return index;
}


int
findPageToRemoveAQ()
{
  struct proc* p = myproc();
  int j = 15 ; 
  for(; j > 0 ; j--)
  {
    if(p->mainMemPages[j].isTaken)
    {
      pte_t * pte = walkpgdir(p->mainMemPages[j].pgdir, p->mainMemPages[j].va, 0);
      if(*pte & PTE_A)
      {
        *pte &= ~PTE_A;
        if(p->mainMemPages[j-1].isTaken){
          pte_t * pte_next = walkpgdir(p->mainMemPages[j-1].pgdir,p->mainMemPages[j-1].va,0);
          if((*pte_next & PTE_A) == 0) // need to switch places
          {
            switchPages(j,j-1);
            j--; // 
          }
          else
          {
            *pte_next &= ~PTE_A;
          }
        }
      }
    }
  }

  return 15; // always return the last index of the array
}

void
switchPages(int j, int i)
{
  struct proc* p = myproc();
  char* va = p->mainMemPages[j].va;
  p->mainMemPages[j].va = p->mainMemPages[i].va;
  p->mainMemPages[i].va = va;
  return;
}


void
updateAGE()
{
  if(myproc()->pid > 2)
  {
    struct proc* p = myproc();
    int i = 0;
    for(; i< MAX_PSYC_PAGES; i++)
    {
      if(p->mainMemPages[i].isTaken)
      {
        pte_t * pte = walkpgdir(p->mainMemPages[i].pgdir, p->mainMemPages[i].va, 0);
        if((*pte & PTE_A) == 0)
        {
          uint val = p->mainMemPages[i].age >> 1;
          val &= ~0x80000000;
          p->mainMemPages[i].age = val;
        }
        else
        {
          *pte &= ~PTE_A;
          uint val = p->mainMemPages[i].age >> 1;
          val |= 0x80000000;
          p->mainMemPages[i].age = val;
        }
      }
    }
  }
}

void
copyOnWrite(char *va)
{
  uint pa;
  pte_t *pte;
  char *mem;
  if((uint)va >= KERNBASE || (pte = walkpgdir(myproc()->pgdir, (void*)va, 0)) == 0)
    panic("Invalid virtual address\n");
  if(!(*pte & PTE_P) && !(*pte && PTE_U))
    panic("Inaccessable page\n");
  
  if(*pte & PTE_W)
    panic("COW on writable page\n");
  
  pa = PTE_ADDR(*pte);
  acquire(&ref_lock);
  if(ref_counters[pa/PGSIZE] == 1)
  {
    release(&ref_lock);
    *pte |= PTE_W;
    lcr3(V2P(myproc()->pgdir));
  }
  else 
  {
    if(ref_counters[pa/PGSIZE] > 1)
    {
      release(&ref_lock);
      if((mem = kalloc()) == 0)
      {
        myproc()->killed = 1;
        cprintf("Out of memory\n");
        return;
      }
      memmove(mem, (char*)P2V(pa), PGSIZE);
      acquire(&ref_lock);
      ref_counters[pa/PGSIZE] = ref_counters[pa/PGSIZE] - 1;
      ref_counters[V2P(mem)/PGSIZE] = ref_counters[V2P(mem)/PGSIZE] + 1;
      release(&ref_lock);
      *pte = V2P(mem) | PTE_P | PTE_W | PTE_U;
    }
    else
    {
      release(&ref_lock);
      panic("Non-positive refcount\n");
    }
    lcr3(V2P(myproc()->pgdir));
  }
}