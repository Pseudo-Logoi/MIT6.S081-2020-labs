#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    // 将参数列表向前移动一位，并将最后一个参数设置为lastArg（buf）
    char lastArg[512];
    // char *firstArg = argv[0];
    for(int i = 0; i < argc - 1; ++i)
    {
        argv[i] = argv[i + 1];
    }
    argv[argc - 1] = lastArg;
    
    int i = 0;
    char c;
    int n;
    while (1)
    {
        n = read(0, &c, sizeof(char));
        if (n < 0)
        {
            fprintf(2, "xargs: read error\n");
            exit(1);
        }

        // 检查是否结束
        if (n == 0)
            break;

        // 参数过长
        if (i >= 512)
        {
            fprintf(2, "xargs: too long\n");
            exit(1);
        }

        // 构造最后一个参数
        if (c != '\n')
        {
            lastArg[i++] = c;
            continue;
        }
        lastArg[i] = 0;

        // 使用子进程执行
        if(fork() == 0)
            exec(argv[0], argv);

        wait(0);

        i = 0;
    }

    exit(0);
}