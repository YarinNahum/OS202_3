#include "types.h"
#include "stat.h"
#include "user.h"

int num = 0;

void
test1(void)
{
    int *a = malloc(sizeof(int));
    *a = 3;
    int pid = fork();
    if(pid != 0)
    {
        wait();
        if(*a == 3)
        {
            printf(1, "test1: malloc and free work\n");
        }
        else
        {
            printf(1, "test1: unexpected value\n\n");
        }
        free(a);
        printf(1, "test1: parent freed successfully\n\n");
        return;
    }
    if(pid < 0)
    {
        printf(1, "test1: fork failed\n\n");
        exit();
    }
    *a = 5;
    if(*a != 5)
    {
        printf(1, "test1: unexpected value\n\n");
    }
    free(a);
    printf(1, "test1: child freed successfully\n");
    exit();
}

void
test2(void)
{
    int pid;
    pid = fork();
    if(pid != 0)
    {
        wait();
        pid = fork();
        if(pid != 0)
        {
            wait();
            printf(1, "test2: Number of free pages in parent is: %d\n", getNumberOfFreePages());
            printf(1, "test2: double-fork works\n\n");
        }
        else if(pid < 0)
        {
            printf(1, "test2: fork failed\n");
            exit();
        }
        else
        {
            printf(1, "test2: Number of free pages in child 2 before changing num is: %d\n", getNumberOfFreePages());
            num++;
            printf(1, "test2: Number of free pages in child 2 after changing num is: %d\n", getNumberOfFreePages());
        }
    }
    else if(pid < 0)
    {
        printf(1, "test2: fork failed\n");
        exit();
    }
    else
    {
        pid = fork();
        if(pid != 0)
        {
            wait();
            printf(1, "test2: Number of free pages in child 1 is: %d\n", getNumberOfFreePages());
        }
        else if(pid < 0)
        {
            printf(1, "test2: fork failed\n");
            exit();
        }
        else
        {
            printf(1, "test2: Number of free pages in child 1 of child 1 before changing num is: %d\n", getNumberOfFreePages());
            num++;
            printf(1, "test2: Number of free pages in child 1 of child 1 after changing num is: %d\n", getNumberOfFreePages());
        }
    }
    exit();
}

int
main(void)
{
    test1(); // for checking cow mechanism
    test2(); // for checking fork mechanism
    exit();
    return 0;
}