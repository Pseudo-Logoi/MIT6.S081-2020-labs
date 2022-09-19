#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

void find(char *path, char *filename)
{
    // 打开目录，目录也是文件
    int fd;
    if ((fd = open(path, 0)) < 0)
    {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    // 获取目录的stat
    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    // 检查是否是一个目录
    if (st.type != T_DIR)
    {
        fprintf(2, "find: %s is not a directory\n", path);
        close(fd);
        return;
    }

    // 构建当前目录文件路径前缀：path/
    char buf[512], *p;
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf))
    {
        fprintf(1, "find: path too long\n");
        return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    // 获取当前目录内容
    struct dirent de;
    while (read(fd, &de, sizeof(de)) == sizeof(de))
    {
        // 获取子文件的stat
        if (de.inum == 0)
            continue;

        // 跳过当前目录
        if (strcmp(de.name, ".") == 0)
            continue;

        // 跳过上级目录
        if (strcmp(de.name, "..") == 0)
            continue;

        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;

        if (stat(buf, &st) < 0)
        {
            fprintf(1, "find: cannot stat %s\n", buf);
            continue;
        }

        if (st.type == T_FILE)
        {
            if (strcmp(de.name, filename) == 0)
                fprintf(1, "%s\n", buf);
        }
        else if (st.type == T_DIR)
            find(buf, filename);
    }

    return;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(1, "usage: find path filename\n");
        exit(0);
    }

    find(argv[1], argv[2]);

    exit(0);
}