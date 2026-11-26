# 课1-2 作业

## 1. 选择题

| 题号 | 答案 | 说明 |
|---|---|---|
| 1.1 | C | `--enable-debug` 让编译器加 `-g`，目标文件中带调试信息；优化级别由 CFLAGS 决定 |
| 1.2 | A | `-O0` 不优化，`-O3` 优化级别最高 |
| 1.3 | C | 发行版一般用 `-O2` |
| 1.4 | ABC | configure → make → make install |
| 1.5 | BC | 编译选项、安装位置不变，不必重新 configure；make 重新编译，make install 把新的可执行文件装到目标位置 |
| 1.6 | B | `make clean` 后找出问题再 make |
| 1.7 | A | 先找 PID(5) → `gdb -p`(1) → 设断点(4) → 在该 Session 执行 SQL 触发断点(2) → s/n 单步(3) |
| 1.8 | AB | C 在启动 gdb 前设断点，顺序错误；第四个选项在程序运行前对内存地址设 watch，此时地址还没有意义 |
| 1.9 | ABC | C 语言层面入口是 main；链接时加入的 `_start`（crt1.o）先运行，再经 `__libc_start_main` 调用 main；`PostmasterMain` 只是 main 分派出的一个分支 |
| 1.10 | A | 代码中判断的选项为 `--check`、`--boot`、`--fork`、`--describe-config`、`--single`。其中 `--fork` 只在定义了 `EXEC_BACKEND`（Windows）时存在；不带这些选项走 `PostmasterMain`，但“不加选项”不算一个命令行选项 |

## 2. 链表搜索与 perf_event_open

任务说明：课程提供的 link1.md。程序：[list_search.c](list_search.c)

- `InitLink` / `AppendNode` / `FindNode` 按 link1.md 要求实现：双向循环链表，`firstNode->prev` 指向最后一个节点，`LinkData` 结构不变
- 同一程序中保留普通写法（`Naive*`，每个节点、数据各 malloc 一次）作对照，两者使用同一份数据、同一组查询
- 用 perf_event_open 只统计搜索阶段：耗时、指令数、周期数（附 IPC）、访存次数（L1D 读访问 + 写访问）、L1D 命中率
- 输出同时追加到当前目录的 `MMDD_1.log`

```bash
gcc -O2 -march=native -o list_search list_search.c      # 按要求不用 -O3
sudo sysctl kernel.perf_event_paranoid=1                 # 若提示 Permission denied
./list_search [链表条数=100] [每条节点数=10000] [查询次数=2000]
```

### 优化手段（不使用 HASH）

1. **冷热分离**：搜索时每个节点只需读 `next`、`dataSize`。`LinkData`（32 字节）集中放在一块连续内存，一条 cache line 放 2 个；数据放另一块。1 万个节点的链表只占 320KB，而普通写法每个节点连同数据 300 多字节
2. **比较顺序**：先比 `dataSize`，相等再比前 8 字节，最后才 `memcmp`。数据长度 32~512 字节随机时，绝大多数节点只做一次整数比较，不碰数据
3. **顺序分配 + 大页**：从 mmap 的大块内存顺序分配，`MADV_HUGEPAGE` 减少 TLB miss
4. **软件预取**：同一条链表的节点地址递增，不必等 `next` 读回来，直接按地址预取后面第 256 个节点。预取距离可用 `-DPREFETCH_DIST=` 调整，`-DPREFETCH_DIST=0` 相当于不预取。在 M1 虚拟机上效果不明显（见下），需在目标机器上实测

### 结果

Apple M1 上的 Linux 虚拟机，100 条链表 × 1 万节点，2000 次查询（90% 命中，10% 不存在）。虚拟机里计时波动较大，以下为多次运行的范围：

| | 每次搜索耗时 |
|---|---|
| 普通写法 | 240~420 µs |
| 优化版，不预取（`PREFETCH_DIST=0`） | 25~52 µs |
| 优化版，预取 8KB（默认） | 24~45 µs |

- 优化版比普通写法快约 **6~10 倍**，主要来自冷热分离和顺序分配：遍历时只扫 32 字节一个的节点结构，内存连续，硬件预取器就能跟上
- 软件预取在这台机器上**没有明显收益**：节点已经连续存放，硬件预取已经足够。是否有用取决于 CPU 的硬件预取能力，需在物理机上对比 `PREFETCH_DIST=0` 与其他距离

QEMU 模拟 PMU 下的指令数（20 条 × 2000 节点，200 次查询）：

| | instructions |
|---|---|
| 普通写法 | 1,355,100 |
| 优化版 | 1,568,166 |

**优化版指令反而多 16%（每个节点多一条预取指令），却快数倍**：链表搜索的瓶颈是等内存，不是执行的指令多少，所以要看 cycles、IPC 和 cache 命中率，而不是指令数。

### 验证情况

- 两个版本对同一组查询的命中数一致（1796/2000、181/200）
- 开发机为 Apple M1，Linux 虚拟机内没有硬件 PMU；指令数在 QEMU 模拟的 PMU 中测得，QEMU 不模拟 cache
- **cycles、访存次数、L1 命中率需在物理 Linux 机器上运行得到**，到时预取距离也建议在该机器上重新扫一遍
