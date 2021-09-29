// #define _FILE_OFFSET_BITS 64

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
#include <assert.h>

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
 1. 多源拷贝在主函数做个循环，都要补齐文件名，判断是否存在等.
*
* 2020-1-10
* v0.2 重写整个程序
* 1. 使之符合 cp 命令的习惯
* 2. 遵循 linux 编码风格
*
* 2020-1-11
* v0.3 添加覆盖时 yes/no to all 选项
* v0.3.1 细节修订
* v0.3.2 细节修订
* v0.3.3 代码格式修订
*
* 2020-01-16
* v0.3.4 只在扫描阶段提示 -r 选项 
* 取消 dry_run() 合并入 copy()
*
* 2021-9-29
* v0.3.5 add to github
*/

#define MAX_FMTSTR_LENGTH			2048/*传递给print_message函数的格式字符串最大长度*/
#define COPY_BUF_SIZE				4096 /*复制文件时每次读取的长度*/
#define MAX_PATH_LENGTH				(PATH_MAX + 1)/*路径的最大长度*/
#define GBYTES						(1024 * 1024 * 1024)
#define MBYTES						(1024 * 1024)
#define KBYTES						1024
#define HOUR						(60 * 60)
#define MINUTE						60
#define OP_CONTINUE					0
#define OP_SKIP						1
#define OP_CANCEL					2 /*walk 函数终止遍历退出*/

#define MSGT_PROMPT					0
#define MSGT_WARNING				1
#define MSGT_ERROR					2
#define MSGT_VERBOSE				3

/*启用大文件支持*/
//#define _LARGEFILE64_SOURCE
//#define _FILE_OFFSET_BITS 64

//#ifdef _LARGEFILE64_SOURCE
//#define stat stat64
//#define fopen fopen64
//#define fread fread64
//#define fwrite fwrite64
//#endif

typedef int (*op_func_t)(const char*, const char*, const struct stat*, const struct stat*);
typedef void (*sig_handler_t)(int);

/* 全局变量 */
int g_sum_file = 0;
int g_sum_dir = 0;
long long g_sum_size = 0;
int g_copied_file = 0;
int g_copied_dir = 0;
long long g_copied_size = 0;
time_t g_copy_start_time = 0;
int g_status_pause = 0;
int g_opt_d = 0;
int g_opt_f = 0;
int g_opt_q = 0;
int g_opt_r = 0;
int g_opt_v = 0;
char g_copy_buf[COPY_BUF_SIZE];
int g_auto_choice = 0;

/*显示为可读数字*/
static char *human_size(long long s, char *hs)
{
	if (s >= GBYTES) {
		sprintf(hs, "%.2fGB", (s * 1.0) / GBYTES);
	} else if (s >= 1024 * 1024) {
		sprintf(hs, "%.2fMB", (s * 1.0) / MBYTES);
	} else if (s > 1024) {
		sprintf(hs, "%.2fKB", (s * 1.0) / KBYTES);
	} else {
		sprintf(hs, "%lldB", s);
	}
	return hs;
}

/* human readable time */
static char *human_time(time_t t, char *text)
{
	int h, m, s;
	h = (int)(t / HOUR);
	m = (int)((t % HOUR) / MINUTE);
	s = (int)(t % HOUR % MINUTE);

	if (h > 0) {
		sprintf(text, "%dh %dm %ds", h, m, s);
	} else if (m > 0) {
		sprintf(text, "%dm %ds", m, s);
	} else {
		sprintf(text, "%ds", s);
	}
	return text;
}

/*
* 先清除状态文字然后在输出信息
* 1. 状态文字总是在当前行输出不换行
* 2. printerror只能在状态文字被显示之后输出，即定时器被安装之后使用。
*/
static void print_message(int t, const char *fmt, ...)
{
	char real_fmt[MAX_FMTSTR_LENGTH];
	va_list args;

	if (t == MSGT_PROMPT || (!g_opt_q && (t != MSGT_VERBOSE || g_opt_v))) {
		va_start(args, fmt);
		sprintf(real_fmt, "\r\033[K%s", fmt);
		vprintf(real_fmt, args);
		va_end(args);
	}
}

/*显示进度条*/
static void show_status(int finish)
{
	static char s_animate[4] = {"-/|\\"};
	static int s_animate_pos = 0;
	int percent,i;
	time_t cur_time;
	char speed[512];
	char hs[512];
	long long sp = 0;
	char ht[512];

	time(&cur_time);
	if (g_sum_size == 0) {
		percent = 0;
	} else {
		percent = (g_copied_size * 1.0 / g_sum_size) * 100;
	}

	if (cur_time > g_copy_start_time) {
		sp = g_copied_size / (cur_time - g_copy_start_time);
		sprintf(speed, "%s/s", human_size(sp, hs));
	} else {
		sprintf(speed, "-");
	}

	human_size(g_copied_size, hs);
	if (finish) {
		printf(
			"\r\033[K%d directories %d files %s copied, %s, %s\n",
			g_copied_dir, g_copied_file, hs, human_time(cur_time - g_copy_start_time, ht), speed
		);
	} else {
		printf(
			"\r\033[K%d directories %d files %s copied, %d%%, %s %c ",
			g_copied_dir, g_copied_file, hs, percent, speed, s_animate[s_animate_pos++ % 4]
		);
	}
	fflush(stdout);
}

/*定时器处理函数*/
static void timer_handler(int signum)
{
	if (!g_status_pause) {
		show_status(0);
	}
}

/*安装/删除定时器*/
static void install_timer(size_t sec, sig_handler_t handler_func)
{
	struct sigaction act;
	struct itimerval tick;

	if (sec > 0) {
		act.sa_handler = handler_func;
	} else {
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

static char *step_in_path(char *p, const char *sub)
{
	if (p[strlen(p) - 1] == '/') {
		strcat(p, sub);
	} else {
		strcat(p, "/");
		strcat(p, sub);
	}
	return p;
}

static char *step_out_path(char *p)
{
	char *r = strrchr(p, '/');
	if (r) {
		r[0] = 0;
	} else {
		p[0] = 0;
	}
	return p;
}

/* 获取路径中最后一项 */
static char *get_last_item(char *p)
{
	if (p[strlen(p) - 1] == '/') {
		p[strlen(p) - 1] = 0;
		return get_last_item(p);
	}

	char *r = strrchr(p, '/');
	if (r) return r + 1;
	return p;
}

/*
* 遍历函数
* 遍历函数只保证源文件/文件夹的每一项都调用一次op函数
* 由op函数的返回值决定是否继续扫描
*/
static int walk(int depth, char* src, char* dest, op_func_t op)
{
	struct stat src_st = {};
	struct stat dest_st = {};
	int src_errno = 0;
	int dest_errno = 0;
	DIR* dir = NULL;
	struct dirent *entry = NULL;
	int ret_val = OP_CONTINUE;

	/* 获取源/目标属性 */
	if (-1 == stat(src, &src_st)) src_errno = errno;
	if (src_errno) {
		print_message(MSGT_ERROR, "can't access \"%s\", errno %d\n", src, src_errno);
		return OP_SKIP;
	}
	if (-1 == stat(dest, &dest_st)) dest_errno = errno;

	/* 第一层调用时做一次有效性检验 */
	if (0 == depth) {
		if (S_ISDIR(src_st.st_mode) && (dest_errno || !S_ISDIR(dest_st.st_mode))) {
			/* 源是目录,目标不是已存在的目录,失败 */
			print_message(MSGT_ERROR, "can't write \"%s\", is not a existing directory\n", dest);
			return OP_SKIP;
		}
		if (!dest_errno && S_ISDIR(dest_st.st_mode)) {
			/* 目标是一个已经存在的目录则自动作为前缀 */
			step_in_path(dest, get_last_item(src));
			if (-1 == stat(dest, &dest_st)) dest_errno = errno;
		}
	}

	/* no self copy */
	if (!src_errno && !dest_errno && src_st.st_ino == dest_st.st_ino) {
		print_message(MSGT_ERROR, "skip, \"%s\" and \"%s\" are the same\n", src, dest);
		return OP_SKIP;
	}

	/* 源是一个文件 */
	if (!S_ISDIR(src_st.st_mode)) {
		if (!dest_errno && S_ISDIR(dest_st.st_mode)) {
			/* 目标是一个已存在的目录 */
			print_message(MSGT_ERROR, "can't write \"%s\", is a existing directory\n", dest);
			return OP_SKIP;
		} else {
			/* 目标不存在,新建 */
			/* 目标是一个已存在的文件,覆盖之 */
			return op(src, dest, &src_st, (dest_errno ? NULL : &dest_st));
		}
	}

	/* 源是一个目录 */
	if (dest_errno || S_ISDIR(dest_st.st_mode)) {
		/* 目标不存在或是一个目录 */
		if ((ret_val = op(src, dest, &src_st, (dest_errno ? NULL : &dest_st))) != OP_CONTINUE) {
			/* skip or cancel */
		} else {
			/* 目录: 递归浏览 */
			if (!(dir = opendir(src))) {
				print_message(MSGT_ERROR, "can't open directory \"%s\"\n", src);
				ret_val = OP_SKIP;
			} else {
				while ((entry = readdir(dir)) != NULL) {
					if (strcmp(".", entry->d_name) && strcmp("..", entry->d_name)) {
						step_in_path(src, entry->d_name);
						step_in_path(dest, entry->d_name);
						if (OP_CANCEL == walk(depth + 1, src, dest, op)) {
							ret_val = OP_CANCEL;
							break;
						}
						step_out_path(src);
						step_out_path(dest);
					}
				}
				closedir(dir);
			}
		}
		return ret_val;
	} else {
		/* 目标是一个文件,失败,不能把目录写入一个已经存在的文件 */
		print_message(MSGT_ERROR, "can't write \"%s\", is a existing file\n", dest);
		return OP_SKIP;
	}
}

/* 统计函数 */
static int sum_up(const char* src, const char* dest, const struct stat *src_st, const struct stat* dest_st)
{
	if (S_ISREG(src_st->st_mode)) {
		g_sum_file++;
		g_sum_size += src_st->st_size;
	} else if (S_ISDIR(src_st->st_mode)) {
		if (g_opt_r) {
			g_sum_dir++;
		} else {
			print_message(MSGT_WARNING, "skip directory \"%s\", -r not specified\n", src);
			return OP_SKIP;
		}
	} else {
		print_message(MSGT_WARNING, "skip \"%s\"\n", src);
	}
	return OP_CONTINUE;
}

static int get_user_choice(const char *dest)
{
	int c;

	/* 如果用户已经设置了自动选项,直接返回 */
	if (g_auto_choice) return g_auto_choice;

	print_message(MSGT_PROMPT, "overwrite \"%s\"? ([y] yes, [Y] yes to all, [n] no, [N] no to all, [c] cancel): ", dest);
	while (1) {
		c = getchar();

		/*中断重启 由于有一个定时器正在运行，在等待用户输入的过程中getchar会被中断返回*/
		if (-1 == c) continue;

		/*skip useless chars of inputed line*/
		if (c != '\n') {
			while(getchar() != '\n');
		}

		if ('y' == c) {
			break;
		} else if ('Y' == c) {
			c = 'y';
			g_auto_choice = 'y';
			break;
		} else if ('n' == c) {
			break;
		} else if ('N' == c) {
			c = 'n';
			g_auto_choice = 'n';
			break;
		} else if ('c' == c) {
			/* skip current file and cancel walk progress */
			break;
		} else {
			/* unknown command */
		}
	}
	return c;
}

/* 实际操作 */
static int copy(const char* src, const char* dest, const struct stat *src_st, const struct stat *dest_st)
{
	int ret_val = OP_CONTINUE;
	size_t rd, wr, swr;
	FILE *src_file, *dest_file;
	int ret_mkdir = 0;
	int user_choice = 0;

	if (S_ISREG(src_st->st_mode)) {
		/* regular file */
		print_message(MSGT_VERBOSE, "cp \"%s\" -> \"%s\"\n", src, dest);

		if (g_opt_d) return OP_CONTINUE;

		/*询问是否可以覆盖*/
		if (!g_opt_f && dest_st) {
			/* 应该先停止计时器，否则在等待用户输入时如果有定时器被触发，会导致 getchar()停止等待并返回 EOF*/
			g_status_pause = 1;
			user_choice = get_user_choice(dest);
			g_status_pause = 0;

			if ('n' == user_choice) {
				return OP_SKIP;
			} else if ('c' == user_choice) {
				return OP_CANCEL;
			}
		}

		if ((src_file = fopen(src, "rb"))) {
			/* open target file for write */
			if ((dest_file = fopen(dest, "wb"))) {
				while ((rd = fread(g_copy_buf, 1, COPY_BUF_SIZE, src_file)) > 0) {
					wr = 0;
					do {
						swr = fwrite(g_copy_buf + wr, 1, rd - wr, dest_file);
						wr += swr;
					} while(swr > 0 && wr < rd);
					
					g_copied_size += rd;
					if (wr != rd) {
						/*只有部分文件被复制也视为成功因为文件系统中已经有这个文件的记录了*/
						print_message(MSGT_ERROR, "write target file error \"%s\"\n", dest);
						break;
					}
				}
				fclose(dest_file);
				chmod(dest, src_st->st_mode);
				g_copied_file++;
			} else {
				ret_val = OP_SKIP;
				print_message(MSGT_ERROR, "skip, can't open target file \"%s\"\n", dest);
			}
			fclose(src_file);
		} else {
			ret_val = OP_SKIP;
			print_message(MSGT_ERROR, "skip, can't open source file \"%s\"\n", src);
		}
	}
	else if (S_ISDIR(src_st->st_mode)) {
		if (g_opt_r) {
			/* directories */
			print_message(MSGT_VERBOSE, "mkdir \"%s\"\n", dest);

			if (g_opt_d) return OP_CONTINUE;

			ret_mkdir = mkdir(dest, src_st->st_mode);
			if (!ret_mkdir || (ret_mkdir == -1 && errno == EEXIST)) {
				g_copied_dir++;
			} else {
				ret_val = OP_SKIP;
				print_message(MSGT_ERROR, "skip, \"%s\" mkdir failed\n", dest);
			}
		} else {
			ret_val = OP_SKIP;
		}
	} else {
		/* not file nor directory */
		ret_val = OP_SKIP;
		print_message(MSGT_WARNING, "skip, \"%s\" is not a file nor directory\n", dest);
	}

	return ret_val;
}

/*使用说明*/
static void usage()
{
	printf("xcp v0.3.5 - by Que's C++ Studio 2020-01-16\n");
	printf("description: cp with progress\n");
	printf("\n");
	printf("synopsis: xcp [OPTIONS] src1 [src2 ... srcn] dest\n");
	printf("\n");
	printf("[OPTIONS]\n");
	printf("-r: recusive copy sub directories\n");
	printf("-f: force overwrite without prompt\n");
	printf("-q: quiet mode\n");
	printf("-d: dry run\n");
	printf("-v: print verbos message\n");
	printf("-h: print usage message\n");
	printf("\n");
}

/*主函数，做两次遍历*/
int main(int argc, char *args[])
{
	int i = 0;
	char path_from[MAX_PATH_LENGTH];
	char path_to[MAX_PATH_LENGTH];
	char human_readable_size[200];
	int opt;
	int help = 0;

	while ((opt = getopt(argc, args, "rfqdhv")) != -1) {
		switch(opt) {
		case 'r':
			g_opt_r = 1;
			break;
		case 'f':
			g_opt_f = 1;
			break;
		case 'q':
			g_opt_q = 1;
			break;
		case 'd':
			g_opt_d = 1;
			break;
		case 'h':
			help = 1;
			break;
		case 'v':
			g_opt_v = 1;
			break;
		case '?':
			printf("unknown option: %c\n", optopt);
			help = 1;
			break;
		default:
			break;
		}
	}

	if (help || optind + 2 > argc) {
		usage();
		return 1;
	}

	/* 第一次遍历：统计 */
	g_sum_file = 0;
	g_sum_dir = 0;
	g_sum_size = 0;

	for (i = optind; i < argc - 1; ++i) {
		strcpy(path_from, args[i]);
		strcpy(path_to, args[argc - 1]);
		if (OP_CANCEL == walk(0, path_from, path_to, sum_up)) break;
	}

	if (g_sum_file == 0 && g_sum_dir == 0) {
		return 1;
	}
	human_size(g_sum_size, human_readable_size);
	printf("%d directories %d files %s detected\n", g_sum_dir, g_sum_file, human_readable_size);

	/* 第二次遍历：执行*/
	g_copied_file = 0;
	g_copied_dir = 0;
	g_copied_size = 0;

	// 设置一个定时器，每隔1秒显示一下进度
	time(&g_copy_start_time);
	show_status(0);
	install_timer(1, timer_handler);

	for (i = optind; i < argc - 1; ++i) {
		strcpy(path_from, args[i]);
		strcpy(path_to, args[argc - 1]);
		if (OP_CANCEL == walk(0, path_from, path_to, copy)) break;
	}

	install_timer(0, NULL);
	show_status(1);
	return 0;
}
