#include "types.h"
#include "stat.h"
#include "user.h"

int i = 3;
int
main(void)
{
    int pid;
    pid = fork();
    if(pid == 0)
    {
        printf(1, "Number of free pages in child 1 before changing variable is: %d\n", getNumberOfFreePages());
        i = 4;
        printf(1, "Number of free pages in child 1 after changing variable is: %d\n", getNumberOfFreePages());
    }
    else
    {
        wait();
        pid = fork();
        if(pid == 0)
        {
            printf(1, "Number of free pages in child 2 before changing variable is: %d\n", getNumberOfFreePages());
            i = 4;
            printf(1, "Number of free pages in child 2 after changing variable is: %d\n", getNumberOfFreePages());
        }
        else
        {
            wait();
            printf(1, "Number of free pages in parent is: %d\n", getNumberOfFreePages());
        }
        
    }
    exit();
    return 0;
}