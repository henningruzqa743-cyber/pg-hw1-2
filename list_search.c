/*
 * list_search.c
 *	  双向链表搜索优化 + perf_event_open 统计（任务说明见 link1.md）
 *
 *	  InitLink / AppendNode / FindNode 为优化版本；Naive* 为普通写法，
 *	  用作对照。两者对同一组数据、同一组查询分别统计：
 *	  耗时、指令数、周期数、访存次数、L1 Cache 命中率。
 *
 *	  优化手段（不使用 HASH）：
 *	  1. 冷热分离：搜索时每个节点只需读 next、dataSize，节点结构本身集中
 *	     放在一块连续内存（32 字节/节点，一条 cache line 放 2 个），数据放
 *	     在另一块。1 万个节点的链表只占 320KB，能放进 L2；普通写法每个节点
 *	     连同数据 300 多字节，遍历几乎每一步都是一条新 cache line。
 *	  2. 比较顺序：先比 dataSize（就在节点结构里），相等再比前 8 字节，
 *	     最后才 memcmp。数据长度 32~512 字节随机时，绝大多数节点只做一次
 *	     整数比较。
 *	  3. 节点、数据都从 mmap 的大块内存中顺序分配，并申请透明大页，减少
 *	     malloc 开销和 TLB miss；同一条链表的节点地址递增。
 *	  4. 软件预取：既然节点地址递增，就不必等 next 读回来，直接按地址
 *	     预取后面的节点，打破链表遍历“读到当前节点才知道下一个在哪”的
 *	     串行访存。
 *
 * 编译：gcc -O2 -march=native -o list_search list_search.c
 *	  （不支持 -march=native 时去掉即可）
 * 运行：./list_search [链表条数=100] [每条节点数=10000] [查询次数=2000]
 *	  输出同时写入当前目录下的 MMDD_1.log
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

typedef struct LinkData
{
	struct LinkData *prev;
	struct LinkData *next;
	int			type;
	int			dataSize;
	char	   *data;
} LinkData;

/* ---------------- 顺序分配的内存池 ---------------- */

#define ARENA_CHUNK (64UL << 20)	/* 每次向系统申请 64MB */

typedef struct Arena
{
	char	   *cur;
	char	   *end;
} Arena;

static Arena node_arena;		/* 只放 LinkData，热数据 */
static Arena data_arena;		/* 只放 data，冷数据 */

static void *
arena_alloc(Arena *a, size_t size, size_t align)
{
	char	   *p = (char *) (((uintptr_t) a->cur + align - 1) & ~(uintptr_t) (align - 1));

	if (a->cur == NULL || p + size > a->end)
	{
		size_t		len = size > ARENA_CHUNK ? size : ARENA_CHUNK;
		void	   *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
							 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (m == MAP_FAILED)
		{
			perror("mmap");
			exit(1);
		}
		madvise(m, len, MADV_HUGEPAGE);
		a->cur = m;
		a->end = (char *) m + len;
		p = a->cur;
	}
	a->cur = p + size;
	return p;
}

static LinkData *
NewNode(int size, char *mem)
{
	LinkData   *node = arena_alloc(&node_arena, sizeof(LinkData), sizeof(LinkData));

	node->type = 0;
	node->dataSize = size;
	node->data = arena_alloc(&data_arena, size, 8);
	memcpy(node->data, mem, size);
	return node;
}

/* ---------------- 优化版 ---------------- */

#ifndef PREFETCH_DIST
#define PREFETCH_DIST 8192		/* 预取 256 个节点之后；随 CPU 不同，可用 -DPREFETCH_DIST= 调整 */
#endif

LinkData *
InitLink(int size, char *mem)
{
	LinkData   *firstNode = NewNode(size, mem);

	firstNode->prev = firstNode;
	firstNode->next = firstNode;
	return firstNode;
}

void
AppendNode(LinkData *firstNode, int size, char *mem)
{
	LinkData   *node = NewNode(size, mem);
	LinkData   *last = firstNode->prev;

	node->prev = last;
	node->next = firstNode;
	last->next = node;
	firstNode->prev = node;
}

LinkData *
FindNode(LinkData *firstNode, int size, char *mem)
{
	LinkData   *p = firstNode;
	uint64_t	head8,
				d8;

	if (size < 8)
	{
		do
		{
			if (p->dataSize == size && memcmp(p->data, mem, size) == 0)
				return p;
			p = p->next;
		} while (p != firstNode);
		return NULL;
	}

	memcpy(&head8, mem, 8);
	do
	{
		/*
		 * 同一条链表的节点在内存池中地址递增，不必等 next 读回来，直接
		 * 按地址预取后面的节点。预取越界的地址不会出错。
		 */
		__builtin_prefetch((char *) p + PREFETCH_DIST);
		if (__builtin_expect(p->dataSize == size, 0))
		{
			memcpy(&d8, p->data, 8);
			if (d8 == head8 && memcmp(p->data + 8, mem + 8, size - 8) == 0)
				return p;
		}
		p = p->next;
	} while (p != firstNode);

	return NULL;
}

/* ---------------- 普通写法（对照） ---------------- */

static LinkData *
NaiveNewNode(int size, char *mem)
{
	LinkData   *node = malloc(sizeof(LinkData));

	node->type = 0;
	node->dataSize = size;
	node->data = malloc(size);
	memcpy(node->data, mem, size);
	return node;
}

static LinkData *
NaiveInitLink(int size, char *mem)
{
	LinkData   *firstNode = NaiveNewNode(size, mem);

	firstNode->prev = firstNode;
	firstNode->next = firstNode;
	return firstNode;
}

static void
NaiveAppendNode(LinkData *firstNode, int size, char *mem)
{
	LinkData   *node = NaiveNewNode(size, mem);
	LinkData   *last = firstNode->prev;

	node->prev = last;
	node->next = firstNode;
	last->next = node;
	firstNode->prev = node;
}

static LinkData *
NaiveFindNode(LinkData *firstNode, int size, char *mem)
{
	LinkData   *p = firstNode;

	do
	{
		if (p->dataSize == size && memcmp(p->data, mem, size) == 0)
			return p;
		p = p->next;
	} while (p != firstNode);

	return NULL;
}

/* ---------------- perf_event_open ---------------- */

#define HW_CACHE(cache, op, result) \
	((cache) | ((op) << 8) | ((result) << 16))

enum
{
	EV_INSTRUCTIONS,
	EV_CYCLES,
	EV_L1D_RD_ACCESS,
	EV_L1D_RD_MISS,
	EV_L1D_WR_ACCESS,
	NEV
};

static const struct
{
	const char *name;
	uint32_t	type;
	uint64_t	config;
}			events[NEV] =
{
	{"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
	{"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
	{"L1D read access", PERF_TYPE_HW_CACHE,
		HW_CACHE(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
	{"L1D read miss", PERF_TYPE_HW_CACHE,
		HW_CACHE(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS)},
	{"L1D write access", PERF_TYPE_HW_CACHE,
		HW_CACHE(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_WRITE, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
};

static int	fds[NEV];

struct read_val
{
	uint64_t	value;
	uint64_t	time_enabled;
	uint64_t	time_running;
};

static FILE *logf;

/* 同时输出到屏幕和日志文件 */
static void
out(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (logf)
	{
		va_start(ap, fmt);
		vfprintf(logf, fmt, ap);
		va_end(ap);
	}
}

static void
counters_open(void)
{
	int			k;

	for (k = 0; k < NEV; k++)
	{
		struct perf_event_attr pe;

		memset(&pe, 0, sizeof(pe));
		pe.size = sizeof(pe);
		pe.type = events[k].type;
		pe.config = events[k].config;
		pe.disabled = 1;
		pe.exclude_kernel = 1;
		pe.exclude_hv = 1;
		/* 计数器不够时内核会分时复用，用这两个时间把结果折算回来 */
		pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

		fds[k] = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
		if (fds[k] < 0)
			out("warning: cannot open %s: %m\n", events[k].name);
	}
}

static void
counters_ctl(unsigned long req)
{
	int			k;

	for (k = 0; k < NEV; k++)
		if (fds[k] >= 0)
			ioctl(fds[k], req, 0);
}

/* 返回折算后的计数值，计数器不可用时返回 -1 */
static double
counter_value(int k)
{
	struct read_val rv;

	if (fds[k] < 0 || read(fds[k], &rv, sizeof(rv)) != sizeof(rv) || rv.time_running == 0)
		return -1;
	return (double) rv.value * rv.time_enabled / rv.time_running;
}

/* ---------------- 测试 ---------------- */

typedef LinkData *(*find_fn) (LinkData *, int, char *);

typedef struct Query
{
	int			list;
	int			size;
	char	   *mem;
} Query;

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void
run(const char *label, find_fn find, LinkData **heads, Query *qs, long nq)
{
	double		v[NEV],
				t0,
				t1;
	long		i,
				found = 0;
	int			k;

	counters_ctl(PERF_EVENT_IOC_RESET);
	counters_ctl(PERF_EVENT_IOC_ENABLE);
	t0 = now_ms();

	for (i = 0; i < nq; i++)
		if (find(heads[qs[i].list], qs[i].size, qs[i].mem) != NULL)
			found++;

	t1 = now_ms();
	counters_ctl(PERF_EVENT_IOC_DISABLE);

	for (k = 0; k < NEV; k++)
		v[k] = counter_value(k);

	out("\n[%s]  found %ld / %ld\n", label, found, nq);
	out("  %-16s: %.2f ms (%.2f us/search)\n", "time", t1 - t0, (t1 - t0) * 1000 / nq);
#define PRINT_COUNT(name, x) \
	do { \
		if ((x) < 0) out("  %-16s: n/a\n", name); \
		else out("  %-16s: %.0f\n", name, x); \
	} while (0)
	PRINT_COUNT("instructions", v[EV_INSTRUCTIONS]);
	PRINT_COUNT("cycles", v[EV_CYCLES]);
	if (v[EV_INSTRUCTIONS] > 0 && v[EV_CYCLES] > 0)
		out("  %-16s: %.3f\n", "IPC", v[EV_INSTRUCTIONS] / v[EV_CYCLES]);

	/* 访存次数 = L1D 读访问 + L1D 写访问（写访问不支持时只计读） */
	if (v[EV_L1D_RD_ACCESS] >= 0 && v[EV_L1D_WR_ACCESS] >= 0)
		out("  %-16s: %.0f (loads %.0f + stores %.0f)\n", "memory accesses",
			v[EV_L1D_RD_ACCESS] + v[EV_L1D_WR_ACCESS],
			v[EV_L1D_RD_ACCESS], v[EV_L1D_WR_ACCESS]);
	else if (v[EV_L1D_RD_ACCESS] >= 0)
		out("  %-16s: %.0f (loads only, stores n/a)\n", "memory accesses",
			v[EV_L1D_RD_ACCESS]);
	else
		out("  %-16s: n/a\n", "memory accesses");

	PRINT_COUNT("L1D read miss", v[EV_L1D_RD_MISS]);
	if (v[EV_L1D_RD_ACCESS] > 0 && v[EV_L1D_RD_MISS] >= 0)
		out("  %-16s: %.2f%%\n", "L1D hit rate",
			100.0 * (1.0 - v[EV_L1D_RD_MISS] / v[EV_L1D_RD_ACCESS]));
	else
		out("  %-16s: n/a\n", "L1D hit rate");
}

/* 随机数据：长度 32~512 字节 */
static int
rand_size(void)
{
	return 32 + random() % (512 - 32 + 1);
}

static void
rand_fill(char *buf, int size)
{
	int			i;

	for (i = 0; i < size; i++)
		buf[i] = (char) random();
}

int
main(int argc, char **argv)
{
	int			nlists = argc > 1 ? atoi(argv[1]) : 100;
	int			nnodes = argc > 2 ? atoi(argv[2]) : 10000;
	long		nq = argc > 3 ? atol(argv[3]) : 2000;
	LinkData  **fast,
			  **naive;
	Query	   *qs;
	char		buf[512];
	char		logname[32];
	time_t		tt = time(NULL);
	long		i;
	int			l,
				n;

	if (nlists <= 0 || nnodes <= 0 || nq <= 0)
	{
		fprintf(stderr, "usage: %s [lists] [nodes per list] [searches]\n", argv[0]);
		return 1;
	}

	strftime(logname, sizeof(logname), "%m%d_1.log", localtime(&tt));
	logf = fopen(logname, "a");

	out("==== list_search  lists=%d nodes/list=%d searches=%ld  sizeof(LinkData)=%zu\n",
		nlists, nnodes, nq, sizeof(LinkData));

	/* 用同一份随机数据建两套链表 */
	srandom(20260924);
	fast = malloc(sizeof(LinkData *) * nlists);
	naive = malloc(sizeof(LinkData *) * nlists);
	for (l = 0; l < nlists; l++)
	{
		for (n = 0; n < nnodes; n++)
		{
			int			size = rand_size();

			rand_fill(buf, size);
			if (n == 0)
			{
				fast[l] = InitLink(size, buf);
				naive[l] = NaiveInitLink(size, buf);
			}
			else
			{
				AppendNode(fast[l], size, buf);
				NaiveAppendNode(naive[l], size, buf);
			}
		}
	}

	/* 查询：90% 命中（随机位置的已有节点），10% 不存在（遍历整条链表） */
	qs = malloc(sizeof(Query) * nq);
	for (i = 0; i < nq; i++)
	{
		qs[i].list = random() % nlists;
		if (random() % 10 != 0)
		{
			LinkData   *p = fast[qs[i].list];
			int			pos = random() % nnodes;

			while (pos-- > 0)
				p = p->next;
			qs[i].size = p->dataSize;
			qs[i].mem = malloc(p->dataSize);
			memcpy(qs[i].mem, p->data, p->dataSize);
		}
		else
		{
			qs[i].size = rand_size();
			qs[i].mem = malloc(qs[i].size);
			rand_fill(qs[i].mem, qs[i].size);
		}
	}

	counters_open();

	/* 先各跑一遍预热，再正式统计 */
	for (i = 0; i < nq && i < 100; i++)
	{
		NaiveFindNode(naive[qs[i].list], qs[i].size, qs[i].mem);
		FindNode(fast[qs[i].list], qs[i].size, qs[i].mem);
	}

	run("naive", NaiveFindNode, naive, qs, nq);
	run("optimized", FindNode, fast, qs, nq);

	if (logf)
	{
		fclose(logf);
		printf("\nresult appended to %s\n", logname);
	}
	return 0;
}
