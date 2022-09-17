#include "user/user.h"

int main()
{
    int p1[2];
    int p2[2];
    pipe(p1);
    pipe(p2);

    if(fork() == 0)
    {
        // 子进程
        int pid = getpid();

        char buf[64];
        read(p1[0], buf, sizeof(buf));
        fprintf(1, "%d: received ping", pid);
        write(p2[1], "1", 1);
    }
    else
    {
        // 父进程
        int pid = getpid();

        char buf[64];
        write(p1[1], "1", 1);
        read(p2[0], buf, sizeof(buf));
        fprintf(1, "%d: received pong", pid);
    }

    exit(0);
}