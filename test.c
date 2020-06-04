// #include "param.h"
// #include "types.h"
// #include "stat.h"
// #include "user.h"
// #include "fs.h"
// #include "fcntl.h"
// #include "syscall.h"
// #include "traps.h"
// #include "memlayout.h"

// unsigned long randstate = 1;
// unsigned int
// rand()
// {
//   randstate = randstate * 1664525 + 1013904223;
//   return randstate;
// }


// void testSimplest(){
//   printf(1,"~~~~~~~~~~~~Test simplest start\n");
//   // int size = (4096*1);
//   int* ptr = (int*)malloc(sizeof(int));
//   // printf(1,"~~~~~~~~~~~~pointer is %d\n",ptr);
//   *ptr = 7;
//   // printf(1,"~~~~~~~~~~~~value is %d\n",*ptr);
  
//   // for(int i = 0; i<size;i+=sizeof(int) )
//   //   *(ptr+i) = i;
  
//   // for(int i = 0; i<size;i+=4096 )
//   //   if(*(ptr+i) != i)
//   //     printf(1,"~~~~~~~~~~~~Failed testSwapOut Test");

//   free(ptr);
//   printf(1,"~~~~~~~~~~~~Test simplest OK\n");
// }

// void testArray(){
//   printf(1,"~~~~~~~~~~~~Test array start\n");
//   int size = (4096*1);
//   int* ptr = (int*)malloc(size);
//   // printf(1,"~~~~~~~~~~~~pointer is %d\n",ptr);

//   for(int i = 0; i<size/4;i+=1 )
//     *(ptr+i) = i;
  

//   if(*(ptr) != 0)
//     printf(1,"~~~~~~~~~~~~Failed array Test\n");

//   free(ptr);
//   printf(1,"~~~~~~~~~~~~Test array OK\n");
// }


// void testSwapOut(){
//   printf(1,"~~~~~~~~~~~~Test swapout start\n");
//   int size = (4096*15);
//   int* ptr = (int*)malloc(size);
//   for(int i = 0; i<size/4;i+=1 )
//     *(ptr+i) = i;
  
//   for(int i = 0; i<size/4;i+=1024 )
//     if(*(ptr+i) != i)
//       printf(1,"~~~~~~~~~~~~Failed testSwapOut Test\n");

//   free(ptr);
//   printf(1,"~~~~~~~~~~~~Test swapout OK\n");
// }

// void testCOW(){
//   printf(1,"~~~~~~~~~~~~Test COW start pid=%d\n",getpid());
//   int size = (16*4);
//   char failed = 0;
//   int* ptr = (int*)malloc(size);
//   for(int i = 0; i<size/4;i+=1 )
//     *(ptr+i) = i;
  
//   int pid = fork();
//   if(pid == 0){
//     printf(1,"~~~~~~~~~~~~child do stuff\n");
//     *(ptr+10)= 0;
//     exit();
//   }else if(pid < 0){
//     printf(1, "fork failed\n");
//     exit();
//   }else{
//     //parent
//     sleep(10);
//     printf(1,"~~~~~~~~~~~~parent do stuff\n");
//     if(*(ptr+10) != 10){
//       printf(1,"~~~~~~~~~~~~Failed COW Test\n");
//       failed = 1;
//     }
//     wait();
    
//   }
//   if(!failed)
//     printf(1,"~~~~~~~~~~~~Test COW OK\n");
//   printf(1,"~~~~~~~~~~~~Test COW end\n");
// }

// void testCOWSwap(){
//   printf(1,"~~~~~~~~~~~~Test COWSwap start\n");
//   int size = (4096*12);
//   char failed = 0;
//   int* ptr = (int*)malloc(size);
//   for(int i = 0; i<size/4;i+=1 )
//     *(ptr+i) = i;
  

//   printf(1,"~~~~~~~~~~~~Test COWSwap forking\n");

//   int pid = fork();
//   if(pid == 0){
//     printf(1,"~~~~~~~~~~~~child do stuff\n");

//     *(ptr+10)= 0;
//     exit();
//   }else if(pid < 0){
//     printf(1, "fork failed\n");
//     exit();
//   }else{
//     //parent
//      printf(1,"~~~~~~~~~~~~COWSwap parent do stuff\n");
//     sleep(10);
//     if(*(ptr+10) != 10){
//       printf(1,"~~~~~~~~~~~~Failed COWSwap Test\n");
//       failed = 1;
//     }
//     wait();
    
//   }
//   if(!failed)
//     printf(1,"~~~~~~~~~~~~Test COWSwap OK\n");
// }

// //run test in a clean enviroment
// void runTest(void (*func)(void) ){
//   int pid = fork();
//   if(pid == 0){
//     func();
//     exit();
//   }else if(pid < 0){
//     printf(1, "fork failed\n");
//     exit();
//   }
//   wait();
// }


// int
// main(int argc, char *argv[])
// {
//     printf(1, "ass3Tests starting\n");

//     runTest(testSimplest);
//     runTest(testArray);
//     runTest(testSwapOut);
//     //runTest(testCOW);
//     //runTest(testCOWSwap);

//     // testSimplest();
//     // testArray();
//     //  testSwapOut();
//     // testCOW();
//     // testCOWSwap();
    
//     printf(1, "ass3Tests finished\n");
//     exit();
// }

#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
    int i = 0;
    uint c = 21;
    uint pointers[c];
    int pid = 0;
    // making sure I have more than 16 pages on RAM
    printf(1, "im here\n");
    for (i = 0 ; i < c ; i++){
        pointers[i] = (uint)sbrk(4096);
        * (char *) pointers[i] = (char) ('a' + i);
    }

    pid = fork();

    if(pid == 0){
    printf(1,"SON : \n");
    for (i = 0 ; i < c ; i++){

        printf(1,"%c", *(char * )pointers[i]);

    }


    printf(1, " \n DONE \n");

    }
    

    if(pid != 0){
        wait();
    }
    
    if(pid != 0){
    printf(1,"FATHER : \n");
    for (i = 0 ; i < c ; i++){
    printf(1,"%c", *(char * )pointers[i]);

    }
    printf(1, " \n DONE \n");

    }


    exit();
}