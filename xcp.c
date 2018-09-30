#define _FILE_OFFSET_BITS 64  
  
#include <unistd.h>  
#include <stdio.h>  
#include <stdlib.h>  
#include <sys/stat.h>  
#include <sys/types.h>  
#include <dirent.h>  
#include <string.h>  
#include <time.h>  
#include <signal.h>  
#include <sys/time.h>  
#include <stdarg.h>  
#include <errno.h>  
  
/* 
* 复制文件可以显示进度 
*  
* 两个思路:遍历文件一次，把文件名记录在一个列表，后续操作直接从列表中得到文件名 
* 或者遍历两遍，第一次统计，第二次执行 
*  
* 关于进度条  
* 1. 用定时器每隔1秒刷新一次要注意函数重入的问题 
* 2. 两个线程工作线程统计/拷贝主线程刷新状态，似乎小题大做了 
* 3. 一个线程有变化时刷新，这样就无法现实动画 
* 
* TODO 
* 2013-10-22 
* 1. 添加命令行选项的处理。 
* 2. 添加文件无法访问/目录无法创建或者文件/目录已经存在的情况的处理。 
* 3. 如果没有任何文件成功复制时的提示信息BUG（在有文件detected的情况下）。 
* 4. 复制文件，目标是已经存在的目录名时自动添加文件名而不是直接复制。 
* 5. 结束时用human_time 来显示用去的时间。 
*  
* 2013-10-23 
* 1. 统计阶段也要显示动画 
*  
* 2013-10-24 
* 1. overwrite 提示后等待用户输入和定时器冲突的问题 
*  
* 2013-10-29 
* 1. 多源拷贝在主函数做个循环，都要补齐文件名，判断是否存在等. 
* 
*/  
  
#define BOOL int  
#define TRUE 1  
#define FALSE 0  
#define MAX_FMTSTR_LENGTH 2048  /*传递给print_message函数的格式字符串最大长度*/  
#define COPY_BUF_SIZE 4096 /*复制文件时每次读取的长度*/  
#define MAX_PATH_LENGTH (PATH_MAX + 1)  /*路径的最大长度*/  
#define GBYTES (1024 * 1024 * 1024)  
#define MBYTES (1024 * 1024)  
#define KBYTES 1024  
#define HOUR (60 * 60)  
#define MINUTE 60  
#define OPP_CONTINUE 0  
#define OPP_SKIP 1  
#define OPP_CANCEL 2 /*walk 函数终止遍历退出*/  
  
#define MSGT_PROMPT 0  
#define MSGT_WARNING 1  
#define MSGT_ERROR 2  
#define MSGT_VERBOSE 3  
#define MSGT_DEMO 4  
  
/*启用大文件支持*/  
//#define _LARGEFILE64_SOURCE  
//#define _FILE_OFFSET_BITS 64  
  
//#ifdef _LARGEFILE64_SOURCE  
//#define stat stat64  
//#define fopen fopen64  
//#define fread fread64  
//#define fwrite fwrite64  
//#endif  
  
typedef int (*each_opp_t)(const char*, const char*, const char*, const struct stat* st);  
typedef void (*sig_handler_t)(int);  
  
/* 全局变量 */  
int sum_file = 0;  
int sum_dir = 0;  
long long sum_size = 0;  
int copied_file = 0;  
int copied_dir = 0;  
long long copied_size = 0;  
time_t copy_start_time = 0;  
BOOL status_pause = FALSE;  
BOOL opt_d = FALSE;  
BOOL opt_f = FALSE;  
BOOL opt_q = FALSE;  
BOOL opt_r = FALSE;  
BOOL opt_v = FALSE;  
  
/*显示为可读数字*/  
char* human_size(long long s, char *hs)  
{  
    if(s >= GBYTES)  
    {  
        sprintf(hs, "%.2fGB", (s * 1.0) / GBYTES);  
    }  
    else if(s >= 1024 * 1024)  
    {  
        sprintf(hs, "%.2fMB", (s * 1.0) / MBYTES);  
    }  
    else if(s > 1024)  
    {  
        sprintf(hs, "%.2fKB", (s * 1.0) / KBYTES);  
    }  
    else  
    {  
        sprintf(hs, "%lldB", s);  
    }  
  
    return hs;  
}  
  
/* human readable time */  
char* human_time(time_t t, char *text)  
{  
    int h,m,s;  
    h = (int)(t / HOUR);  
    m = (int)((t % HOUR) / MINUTE);  
    s = (int)(t % HOUR % MINUTE);  
  
    if(h > 0)  
    {  
        sprintf(text, "%dh %dm %ds", h, m, s);  
    }  
    else if(m > 0)  
    {  
        sprintf(text, "%dm %ds", m, s);  
    }  
    else  
    {  
        sprintf(text, "%ds", s);  
    }  
    return text;  
}  
  
/* 
* 先清除状态文字然后在输出信息 
* 1. 状态文字总是在当前行输出不换行 
* 2. printerror只能在状态文字被显示之后输出，即定时器被安装之后使用。 
*/  
void print_message(int t, const char* fmt, ...)  
{  
    char real_fmt[MAX_FMTSTR_LENGTH];  
    va_list args;  
    va_start(args, fmt);  
  
    if(opt_q && (t == MSGT_WARNING || t == MSGT_ERROR))  
    {  
        /*quiet, don't output warning nor error message*/  
    }  
    else  
    {  
        sprintf(real_fmt, "\r\033[K%s", fmt);  
        vprintf(real_fmt, args);  
    }  
}  
  
/*连接目录字符串,主要处理末尾/的问题,frt snd 两个参数不能同时为空那样没有意义*/  
char* make_path(char *dest, const char *frt, const char *snd)  
{  
    if(NULL == frt || strlen(frt) == 0)  
    {  
        sprintf(dest, "%s", snd);  
    }  
    else if(NULL == snd || strlen(snd) == 0)  
    {  
        sprintf(dest, "%s", frt);  
    }  
    else  
    {  
        if(frt[strlen(frt) - 1] == '/')  
        {  
            sprintf(dest, "%s%s", frt, snd);  
        }  
        else  
        {  
            sprintf(dest, "%s/%s", frt, snd);  
        }  
    }  
    return dest;  
}  
  
/*显示进度条*/  
void show_status(BOOL finish)  
{  
    int percent,i;  
    char animate[4];  
    static int animate_pos = -1;  
    time_t cur_time;  
    char speed[512];  
    char hs[512];  
    long long sp = 0;  
    char ht[512];  
  
    animate[0] = '-';  
    animate[1] = '/';  
    animate[2] = '|';  
    animate[3] = '\\';  
  
    time(&cur_time);  
    if(sum_size == 0)  
    {  
        percent = 0;  
    }  
    else  
    {  
        percent = (copied_size * 1.0 / sum_size) * 100;  
    }  
  
    if(cur_time > copy_start_time)  
    {  
        sp = copied_size / (cur_time - copy_start_time);  
        sprintf(speed, "%s/s", human_size(sp, hs));  
    }  
    else  
    {  
        sprintf(speed, "-");  
    }  
  
    human_size(copied_size, hs);  
    if(finish)  
    {  
        printf("\r\033[K%d directories %d files %s copied, %s, %s.\n",\
            copied_dir, copied_file, hs, human_time(cur_time - copy_start_time, ht), speed);  
    }  
    else  
    {  
        printf("\r\033[K%d directories %d files %s copied, %d%%, %s %c ", \
            copied_dir, copied_file, hs, percent, speed, animate[animate_pos = (animate_pos + 1) % 4]);  
    }  
    fflush(stdout);  
}  
  
/*定时器处理函数*/  
void timer_handler(int signum)  
{  
    if(!status_pause)  
    {  
        show_status(FALSE);  
    }     
}  
  
/*安装/删除定时器*/  
void install_timer(size_t sec, sig_handler_t  handler_func)  
{  
    struct sigaction act;  
    struct itimerval tick;  
  
    if(sec > 0)  
    {  
        act.sa_handler = handler_func;  
    }  
    else  
    {  
        act.sa_handler = SIG_DFL;  
    }  
    sigemptyset(&act.sa_mask);  
    act.sa_flags = 0;  
    sigaction(SIGALRM, &act, 0);  
      
    memset(&tick, 0, sizeof(tick));  
    tick.it_value.tv_sec = sec;  
    tick.it_value.tv_usec = 0;  
    tick.it_interval.tv_sec = sec;  
    tick.it_interval.tv_usec = 0;  
  
    setitimer(ITIMER_REAL, &tick, 0);  
}  
  
/*  
* 遍历函数 
* 遍历函数只保证源文件/文件夹的每一项都调用一次opp函数 
* 由opp函数的返回值决定是否继续扫描 
* 采用“串烧”式程序风格 
* 只有一种情况下返回值为FALSE：opp 函数返回OPP_CANCEL 
*/  
int walk(const char* path_from, const char* path_to, const char* path_tree, each_opp_t opp)  
{  
    struct stat st;  
    DIR* dir = NULL;  
    struct dirent *entry = NULL;  
    char path_tree_new[MAX_PATH_LENGTH];  
    char path_from_full[MAX_PATH_LENGTH];  
    int ret_val = OPP_CONTINUE;  
  
    /*获得源的属性*/  
    make_path(path_from_full, path_from, path_tree);  
    if(-1 == stat(path_from_full, &st))  
    {  
        print_message(MSGT_ERROR, "can't access \"%s\".\n", path_from_full);  
        return OPP_SKIP;  
    }  
  
    /*调用一次处理函数，处理当前项*/  
    if((ret_val = opp(path_from, path_to, path_tree, &st)) != OPP_CONTINUE)  
    {  
        return ret_val;  
    }  
              
    /*如果是目录，则浏览目录，否则结束*/  
    if(!S_ISDIR(st.st_mode))  
    {  
        return OPP_CONTINUE;  
    }  
  
    /*打开目录*/  
    if(!(dir = opendir(path_from_full)))  
    {  
        print_message(MSGT_ERROR, "can't open directory \"%s\".\n", path_from_full);  
        return OPP_SKIP;  
    }  
  
    /*浏览目录*/  
    while((entry = readdir(dir)) != NULL)  
    {  
        /*构建path_tree_new*/  
        make_path(path_tree_new, path_tree, entry->d_name);  
        make_path(path_from_full, path_from, path_tree_new);  
      
        /*无法访问 skip*/  
        if(-1 == stat(path_from_full, &st))  
        {  
            print_message(MSGT_ERROR, "skip, can't access \"\".\n", path_from_full);  
            continue;  
        }  
  
        /* 忽略 . 和 .. */  
        if(S_ISDIR(st.st_mode) && (strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0))  
        {  
            continue;  
        }  
  
        if(S_ISDIR(st.st_mode) && opt_r)  
        {  
          /*递归处理子目录*/  
            if(walk(path_from, path_to, path_tree_new, opp) == OPP_CANCEL)  
            {  
                ret_val = OPP_CANCEL;  
                break;  
            }  
        }  
        else  
        {  
            /*处理函数处理一个子项*/  
            if(opp(path_from, path_to, path_tree_new, &st) == OPP_CANCEL)  
            {  
                ret_val = OPP_CANCEL;  
                break;  
            }  
        }  
    }  
    closedir(dir);  
    return ret_val;  
}  
  
/* 统计函数 */  
int sum_up(const char* path_from, const char* path_to, const char* path_tree, const struct stat* st)  
{  
    if(S_ISREG(st->st_mode))  
    {  
        sum_file++;  
        sum_size += st->st_size;  
    }  
    else if(S_ISDIR(st->st_mode))  
    {  
        sum_dir++;  
    }  
    else  
    {  
        print_message(MSGT_WARNING, "skip:%s\n", path_tree);  
    }  
    return OPP_CONTINUE;  
}  
  
/*demo*/  
int demo(const char* path_from, const char* path_to, const char* path_tree, const struct stat* st)  
{  
    char path_from_full[MAX_PATH_LENGTH];  
    char path_to_full[MAX_PATH_LENGTH];  
      
    make_path(path_from_full, path_from, path_tree);  
    make_path(path_to_full, path_to, path_tree);  
  
    if(S_ISREG(st->st_mode))  
    {  
        print_message(MSGT_DEMO, "cp \"%s\" -> \"%s\".\n", path_from_full, path_to_full);  
    }  
    else if(S_ISDIR(st->st_mode))  
    {  
        print_message(MSGT_DEMO, "mkdir \"%s\".\n", path_to_full);  
    }  
    else  
    {  
        print_message(MSGT_WARNING, "skip \"%s\"\n", path_tree);  
    }  
    return OPP_CONTINUE;  
  
}  
  
/* 操作 */  
int action(const char* path_from, const char* path_to, const char* path_tree, const struct stat* st)  
{  
    int ret_val = OPP_CONTINUE;  
    char path_from_full[MAX_PATH_LENGTH];  
    char path_to_full[MAX_PATH_LENGTH];  
    size_t rd, wr, swr;  
    char buf[COPY_BUF_SIZE];  
    FILE *src_file, *dest_file;   
    BOOL over_write = FALSE;  
    int cmd;  
    BOOL skip = FALSE;  
    struct stat st_dest;  
      
    make_path(path_from_full, path_from, path_tree);  
    make_path(path_to_full, path_to, path_tree);  
  
    if(S_ISREG(st->st_mode))  
    {  
        /* regular file */  
        if(opt_v)  
        {  
            print_message(MSGT_VERBOSE, "cp \"%s\" -> \"%s\".\n", path_from_full, path_to_full);  
        }  
  
        if(strcmp(path_from_full, path_to_full) == 0)  
        {  
            ret_val = OPP_SKIP;  
            print_message(MSGT_ERROR, "skip, \"%s\" and \"%s\" are the same.\n", path_from_full, path_to_full);  
        }  
        else if(src_file = fopen(path_from_full, "rb"))  
        {  
            do  
            {  
                /*询问是否可以覆盖*/  
                if(!opt_f && 0 == access(path_to_full, F_OK))   
                {  
                    /* 应该先停止计时器，否则在等待用户输入时如果有定时器被触发，会导致 getchar()停止等待并返回 EOF*/  
                    status_pause = TRUE;  
                    print_message(MSGT_PROMPT, "overwrite \"%s\"? ([y] yes,[n] no, [a] all, [c] cancel)", path_to_full);  
                    while(1)  
                    {  
                        cmd = getchar();  
  
                        /*中断重启 由于有一个定时器正在运行，在等待用户输入的过程中getchar会被中断返回*/  
                        if(-1 == cmd) continue;  
  
                        /*skip useless chars of inputed line*/  
                        if(cmd != '\n')  
                        {  
                            while(getchar() != '\n');  
                        }  
  
                        if('y' == cmd)  
                        {  
                            break;  
                        }  
                        else if('n' == cmd)  
                        {  
                            skip = TRUE;  
                            ret_val = OPP_SKIP;  
                            break;  
                        }  
                        else if('a' == cmd)  
                        {  
                            opt_f = TRUE;  
                            break;  
                        }  
                        else if('c' == cmd)  
                        {  
                            /* skip current file and cancel walk progress */  
                            skip = TRUE;  
                            ret_val = OPP_CANCEL;  
                            break;  
                        }  
                        else  
                        {  
                            /* unknown command */  
                        }  
                    }  
                    status_pause = FALSE;  
                  
                    /* ship current file */  
                    if(skip) break;  
                }  
                  
                /* open target file for write */  
                if(dest_file = fopen(path_to_full, "wb"))  
                {  
                    while((rd = fread(buf, 1, COPY_BUF_SIZE, src_file)) > 0)  
                    {  
                        wr = 0;  
                        do  
                        {  
                            swr = fwrite(buf + wr, 1, rd - wr, dest_file);  
                            wr += swr;  
                        }  
                        while(swr > 0 && wr < rd);  
                        copied_size += rd;  
                      
                        if(wr != rd)  
                        {  
                            /*只有部分文件被复制也视为成功因为文件系统中已经有这个文件的记录了*/  
                            print_message(MSGT_ERROR, "write file error %s.\n", path_to_full);  
                            break;  
                        }  
                    }  
                    fclose(dest_file);  
                    chmod(path_to_full, st->st_mode);  
                    copied_file++;  
                }  
                else  
                {  
                    ret_val = OPP_SKIP;  
                    print_message(MSGT_ERROR, "skip, can't open target file \"%s\"\n", path_to_full);  
                }  
            }while(0);  
  
            fclose(src_file);  
        }  
        else  
        {  
            ret_val = OPP_SKIP;  
            print_message(MSGT_ERROR, "skip, can't open source file \"%s\"\n", path_from_full);  
        }  
    }  
    else if(S_ISDIR(st->st_mode))  
    {  
        /* directories */  
        if(opt_v)  
        {  
            print_message(MSGT_VERBOSE, "mkdir \"%s\"\n", path_to_full);  
        }  
  
        if(0 == stat(path_to_full, &st_dest))  
        {  
            /*path_to_full already exist*/  
            if(S_ISDIR(st_dest.st_mode))  
            {  
                copied_dir++;  
            }  
            else  
            {  
                ret_val = OPP_SKIP;  
                print_message(MSGT_WARNING, "skip, \"%s\" exists and it's not a directory.\n", path_to_full);  
            }  
        }  
        else  
        {  
            /*try to make a new directory*/  
            if(0 == mkdir(path_to_full, st->st_mode))  
            {  
                chmod(path_to_full, st->st_mode);  
                copied_dir++;  
            }  
            else  
            {  
                ret_val = OPP_SKIP;  
                print_message(MSGT_ERROR, "skip, \"%s\" mkdir failed.\n", path_to_full);  
            }  
        }  
    }  
    else  
    {  
        ret_val = OPP_SKIP;  
        print_message(MSGT_WARNING, "skip, \"%s\" is not a file nor directory.\n", path_to_full);  
    }  
  
    return ret_val;  
}  
  
/*使用说明*/  
void usage()  
{  
    printf("xcp - by Q++ Studio 2013-11-1\n");  
    printf("description:cp with progress\n");  
    printf("synopsis: xcp [OPTIONS] src1 [src2 ... srcn] dest\n\n");  
    printf("[OPTIONS]\n");  
    printf("-r:recusive copy sub directories.\n");  
    printf("-f:force overwrite without prompt.\n");  
    printf("-q:quiet no warning/error message.\n");  
    printf("-d:demo,do not copy,output message only.\n");  
    printf("-v:verbos output.\n");  
    printf("-h:show usage message.\n");  
}  
  
/*禁止循环复制，即目标文件/文件夹不能包含在源文件/文件夹中*/  
BOOL is_self_copy(const char* src, const char* dest)  
{  
    /*严格的做法应该先把src和dest都转化为绝对路径然后在比较，不过 
     *Linux下的相对路径比较麻烦有 ~ ./ ../ ../../ 等... 
    */  
    char c;  
    char* sub = strstr(dest, src);  
  
    if(sub)  
    {  
        c = sub[strlen(src)];  
        return c == '\0' || c == '/' || src[strlen(src) - 1] == '/';  
    }  
    else  
    {  
        return FALSE;  
    }  
}  
  
/*主函数，做两次遍历*/  
int main(int argc, char* args[])  
{  
    int i = 0;  
    char *path_from = NULL, *path_to = NULL, *file_name = NULL;  
    char path_to_fixed[MAX_PATH_LENGTH];  
    struct stat st_src, st_dest;  
    char human_readable_size[200];  
    int opt;  
    BOOL help = FALSE;  
      
    while((opt = getopt(argc, args, "rfqdhv")) != -1)  
    {  
        switch(opt)  
        {  
            case 'r':  
                opt_r = TRUE;  
                break;  
            case 'f':  
                opt_f = TRUE;  
                break;  
            case 'q':  
                opt_q = TRUE;  
                break;  
            case 'd':  
                opt_d = TRUE;  
                break;  
            case 'h':  
                help = TRUE;  
                break;  
            case 'v':  
                opt_v = TRUE;  
                break;  
            case '?':  
                printf("unknown option: %c\n", optopt);  
                help = TRUE;  
                break;  
            default:  
                break;  
        }  
    }  
      
    if(help || optind + 2 > argc)  
    {  
        usage();  
        return 1;  
    }  
  
    /* 第一次遍历：统计 */  
    sum_file = 0;  
    sum_dir = 0;  
    sum_size = 0;  
  
    path_to = args[argc - 1];  
    for(i = optind; i < argc -1; ++i)  
    {  
        path_from = args[i];  
        walk(path_from, path_to, NULL, sum_up);  
    }  
  
    if(sum_file == 0 && sum_dir == 0)  
    {  
        printf("nothing found.\n");  
    }  
    else  
    {  
        human_size(sum_size, human_readable_size);  
        printf("%d directories %d files %s detected.\n", sum_dir, sum_file, human_readable_size);  
          
        /* 第二次遍历：执行*/  
        copied_file = 0;  
        copied_dir = 0;  
        copied_size = 0;  
  
        // 设置一个定时器，每隔1秒显示一下进度   
        time(&copy_start_time);  
        show_status(FALSE);  
        install_timer(1, timer_handler);  
  
        for(i = optind; i < argc - 1; ++i)  
        {  
            path_from = args[i];  
            path_to = args[argc - 1];  
  
            /*源是否存在*/  
            if(-1 == stat(path_from, &st_src))  
            {  
                    print_message(MSGT_ERROR, "\"%s\" doesn't exist.\n", path_from);  
                    continue;  
            }  
          
            /* 
            * 如果源是文件而且目标是已经存在的目录，则自动补齐文件名 
            * 如果目标是已经存在的文件，先判断是否指向同一个文件 inode number 
            */  
            if(S_ISREG(st_src.st_mode))  
            {  
                if((0 == stat(path_to, &st_dest)) && S_ISDIR(st_dest.st_mode))  
                {  
                    file_name = strrchr(path_from, '/');  
                    path_to = make_path(path_to_fixed, path_to, file_name ? file_name + 1 : path_from);  
                }  
            }  
            else if(S_ISDIR(st_src.st_mode))  
            {  
                if(opt_r && is_self_copy(path_from, path_to))  
                {  
                    /*源是目录时要防止循环复制*/  
                    print_message(MSGT_ERROR, "can't xcp \"%s\" -> \"%s\"\n", path_from, path_to);  
                    continue;  
                }  
            }  
            else  
            {  
                print_message(MSGT_WARNING, "skip \"%s\" not a file nor a directory.\n", path_from);  
                continue;  
            }  
  
            if(opt_d)  
            {  
                walk(path_from, path_to, NULL, demo);   
            }  
            else  
            {  
                walk(path_from, path_to, NULL, action);  
            }  
        }  
        install_timer(0, NULL);  
        show_status(TRUE);  
    }  
  
    return 0;  
}  
