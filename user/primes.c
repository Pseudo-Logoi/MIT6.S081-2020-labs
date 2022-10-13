#include "kernel/types.h"
#include "user/user.h"

int findPrimes(int inputfd)
{
    int prime;
    read(inputfd, &prime, sizeof(int));
    fprintf(1, "prime %d\n", prime);

    int p[2] = {0, 0};
    int num;
    int n;
    while (1)
    {
        n = read(inputfd, &num, sizeof(int));

        if (n == 0)
            break;

        if (num % prime != 0)
        {
            if (p[0] == 0)
            {
                pipe(p);

                if (fork() == 0)
                {
                    // 子进程
                    close(p[1]);
                    close(inputfd);
                    findPrimes(p[0]);
                }

                // 父进程
                close(p[0]);
            }

            write(p[1], &num, sizeof(num));
        }
    }
    close(p[1]);
    wait(0);
    exit(0);
}

int main()
{
    // close(0); // 关闭标准输入
    // close(2); // 关闭标准错误

    int p[2];
    pipe(p); // p[0]读端，p[1]写端

    if (fork() == 0)
    {
        // 子进程
        close(p[1]);
        findPrimes(p[0]);
    }

    // 父进程
    close(p[0]);
    for (int i = 2; i <= 35; ++i)
    {
        write(p[1], &i, sizeof(int));
    }
    close(p[1]);
    wait(0);
    exit(0);
}
