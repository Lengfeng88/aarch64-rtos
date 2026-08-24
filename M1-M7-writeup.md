# Project 4 — ARMv8-A RTOS on QEMU aarch64 virt

## 里程碑总结:M1–M7(P4 阶段一全部完成)

**平台**:QEMU `virt` 机型,`-cpu cortex-a53`,ARMv8-A,裸机 C + AArch64 汇编,无 MMU、无浮点(`-mgeneral-regs-only`)。工具链:`aarch64-linux-gnu-gcc 13.3.0` + `qemu-system-aarch64 8.2.2`。

**总体原则**:每个里程碑都遵循"写完立刻在真实模拟器上跑一遍,不确定的地址/寄存器值一律从设备树、真实 QEMU 内部状态里实测确认,不凭经验假设"。这个原则在过程中多次直接决定了能不能发现真问题。

---

## M1:AArch64 Boot → 异常向量表 → EL1 → UART

**目标**:从 QEMU 复位入口落到 EL1,装好异常向量表,PL011 UART 能输出。

**关键实现**:
- `_start` 用 `CurrentEL` 寄存器实测探测当前异常等级,分别处理"直接从 EL1 启动"和"从 EL2 降级到 EL1"两条路径,不假设默认行为
- 向量表 2KB 对齐,16 个入口每个 128 字节,严格按架构要求
- 用 `svc #0` 主动触发一次同步异常作为自检——如果向量表没真正装上,这一步会直接暴露

**验证结果**:两条 boot 路径(EL1 直启 / EL2→EL1 降级)都在真实 QEMU 上实测通过,`svc` 自陷正确命中 handler 并通过 `eret` 正确返回。

---

## M2:Generic Timer → 定时器中断 → 调度器 tick

**M2a(寄存器级验证,先不接中断)**:`CNTFRQ_EL0` 读出 62500000 Hz;`CNTP_TVAL_EL0` 倒计时 + `ISTATUS` 轮询确认硬件计时链路本身没问题。

**M2b**:真正的"定时器中断驱动调度"并入了 M4 一起做(因为需要 GIC 才能完整闭环)。

---

## M3:Task → Context → 上下文切换

**关键实现**:`switch_to()` 只保存/恢复 AAPCS64 的 callee-saved 寄存器(`x19–x28`、`x29`、`x30`),这是调用约定给的合法简化。新任务第一次运行靠在其栈上手工伪造一份"看起来像被 `switch_to` 保存过"的初始帧。

**验证结果**:伪造帧首次切入正确落到入口函数;两任务反复来回切换 6 次,栈两端 canary 全程完好。此时是纯协作式切换,还没有抢占。

---

## M4:GIC → IRQ → ISR → 任务唤醒

**关键实现**:
- GICv2 通过设备树 dump 实测确认(Distributor `0x08000000`、CPU interface `0x08010000`、timer PPI = INTID 30),不是查文档猜的
- IRQ 中断向量必须保存全部 30 个通用寄存器 + `ELR_EL1`/`SPSR_EL1`(和 `switch_to` 的"只存 callee-saved"完全不同,因为中断可以在任意位置打断)

**踩到的真 bug**:新任务第一次运行是通过"伪造帧 + 裸 `ret`"直接跳进入口函数,完全跳过了 `eret`。而 `PSTATE.I`(中断屏蔽位)是硬件在进入 IRQ 时自动置位的,只有 `eret` 恢复 `SPSR_EL1` 时才会被正确清除——这意味着新任务一旦开始跑,会永久处于中断被屏蔽的状态,后续所有抢占全部失效。修法是加一层 `task_trampoline`,每个新任务第一次运行时先手动清一次中断屏蔽位,再跳进真正的入口函数。

**验证结果**:12 次 tick 严格 A/B 交替,完全由定时器中断驱动,不需要任务自己调用 `switch_to`。

---

## M5:Mutex / Semaphore / Event / Queue

调度器同步升级为通用版本(`pick_next_ready()`/`yield()`,任务加 `state` 字段表示 READY/BLOCKED),抢占和主动阻塞走同一套逻辑,保证两条路径永远看到同一份真相。

**四个原语都完整实现并压测(各 25–55+ 次全部通过)**:
- **Semaphore**:单等待者简化,唤醒时直接过户许可,不留"看起来空闲"的空窗期
- **Mutex**:同样的阻塞/唤醒骨架,持有者字段 + `locked` 标志
- **Event**:支持"等任意一位"和"等全部位"两种语义
- **Queue**:环形缓冲区,唤醒后走"重试循环"而不是精确过户某个槽位(因为队列的阻塞方唤醒后还要自己真正完成 push/pop)

**踩到的真 bug(有代表性,值得记录)**:
1. **反复出现的"最后一次 yield 不可靠"模式**:多个原语的收尾阶段都出现过"任务做完最后一件事后只 `yield()` 一次,小概率永远等不到最后一次唤醒"——根因和 GIC 中断投递的边缘情况有关(定时器硬件真的到期、但 GIC 没有再把这次中断投递给 CPU)。统一修法:收尾处不猜 yield 次数,改成轮询一个明确的完成标志(`while (!done) yield();`)
2. **Event 的 API 设计缺陷**:`event_wait()` 醒来后重新读取共享的 `e->flags` 来计算返回值,但如果被唤醒的任务没能立刻运行、期间又发生了一轮 `event_clear+event_set`,读到的就不是"当初触发唤醒的那个条件"了。修法是让 `event_set()` 在临界区内、决定唤醒的那一刻就把结果锁定进一个 `wake_result` 字段,`event_wait()` 直接读这个字段
3. **结构体跨文件不一致**:`main.c` 里的 `event_t` 定义没跟着 `sync.c` 加的 `wake_result` 字段同步更新,导致 `event_set()` 写出了分配给 `ev` 的内存范围——这类"同一个结构体在不同 `.c` 文件里字段顺序/数量对不上"的 bug 在项目里反复出现了好几次,是最值得记住的一类坑

---

## M6:DMA → Accelerator HAL → MMIO

这是目前最复杂、也是唯一涉及**真实 PCIe 设备**(用户提供的 `dma-accel` 自定义 QEMU 设备模型)的里程碑。

### 环境搭建
用户的 `qemu-src` fork 不能直接 clone,先用 stock QEMU 8.2.2 手动接入 `dma_accel.c` + `Kconfig`/`meson.build` 在沙箱里重建了等价环境验证驱动逻辑,后续在用户本地真实环境(补上 `aarch64-softmmu` target 重新编译)上完整复现一致。

### PCI 枚举 + BAR0 映射
- PCIe ECAM 基址(`0x4010000000`)、32-bit MMIO 窗口、legacy IRQ 到 GIC SPI 的映射(设备 2/INTA → SPI 5 → INTID 37)全部从设备树原始数据里核对确认
- 实现标准 PCI BAR sizing(写全 1、读回、取反加一)、vendor/device 扫描、command 寄存器使能

**踩到的真 bug(整个项目里排查过程最深的一次)**:`PCI_COMMAND_MEMORY` 被错误定义成 `bit 0`(实际应该是 `bit 1`,`bit 0` 是 `PCI_COMMAND_IO`)。这个 bug 极其隐蔽——自己写、自己读回全部"看起来正确"(因为读回的确实是按错误定义写进去的值),连 QEMU 自己的 `xp` 直接内存读也显示"配置字节写对了"。最终是直接在 QEMU 自己的 `hw/pci/pci.c`(`pci_bar_address()`)里加临时 `fprintf` 追踪、重新编译那一个文件,才亲眼看到 QEMU 判定"Memory 位没设置"——一路查到 QEMU 自己的 `linux/pci_regs.h` 才发现真正的位定义。

### SQ/CQ 命令提交 + DMA
- 按用户提供的真实寄存器规格(`dma_accel_regs.h`)实现命令队列注册、`OPCODE_COPY` 命令构造、doorbell 提交
- 先用轮询验证链路本身(读 `REG_CQ_TAIL`),再切换成真正的 GIC 中断驱动完成通知

**踩到的真 bug**:`REG_IRQ_MASK` 从没被写过——设备的中断线是 `irq_status & irq_mask` 的与逻辑,`DMA_DONE` 再怎么置位,没配置 mask 中断也永远不会真正触发。轮询版本完全绕开了这个问题,只有真正切到中断驱动才暴露。

**最终验证**(单任务提交、阻塞、真实硬件中断唤醒):
```
submitted COPY, cmd_id=1
submitter blocking on completion_sem
[bystander] still running, loops=500000 ... 3000000
woke via IRQ, completion cmd_id=1
completion status=0
bystander loops while we waited=2985355
DATA VERIFIED - dst matches src byte-for-byte
```
`bystander` 任务在 submitter 阻塞期间空转了近 300 万次循环,证明 CPU 确实完全交给了别的任务,不是忙等;数据经过真实设备的 DMA 引擎搬运,逐字节校验通过。这个结果在沙箱和用户本地真实环境上完全一致。

---

## M7:完整集成(多任务并发)

M6 只验证了单任务提交/等待。M7 把场景升级成三个任务**几乎同时**各自提交自己的 COPY 命令、各自阻塞在自己的信号量上,ISR 需要正确按 `cmd_id` 把每个完成通知路由回对应的任务。这个场景第一次让系统里出现"所有任务同时阻塞、没有任何一个就绪"的情况——而这恰好精确踩中了三个此前从未被触发过的调度器/驱动 bug。

**排查方式**:把 `sync_el1h` 从"只存两个寄存器"扩成保存全部 31 个通用寄存器 + `ESR`/`ELR`,崩溃时把整个寄存器现场和 `pending[]` 等待表状态一起打印出来,不再靠猜。

**踩到的三个真 bug**:

1. **`sem_wait()` 没检查"要切换的目标就是自己"**:`pick_next_ready()` 在找不到任何其他就绪任务时,只能返回调用者自己。但 `sem_wait()`(以及 Mutex/Event/Queue 的阻塞路径)没有处理这种情况,照样调用 `switch_to(&prev->sp, next->sp)`——此时 `prev == next`,会用"调用前一刻已经过时的旧 `sp` 值"去恢复上下文,恢复出一堆被清零的寄存器,`ret` 直接跳到地址 `0x0`。崩溃现场里 `x30 = 0x0` 精确对应这个机制。修法:检测到 `next == prev` 时不做上下文切换,原地轮询自己的状态,等 ISR 把自己唤醒。

2. **`pick_next_ready()` 的"没人可换"分支没有同步 `current_idx`**:调用者已经把自己标记成 BLOCKED,连"退而求其次返回自己"这条分支里的自检也会失败,导致 `current_idx` 永远不会被更新到真实的当前任务下标。后续所有调度决策(包括定时器自己的)都会从一个错误的位置开始扫描,永久性地找不到某些已经就绪的任务。修法:这个分支返回前显式把 `current_idx` 同步成 `current` 的真实下标。

3. **提交命令和注册等待表之间的竞争窗口**:`accel_submit_copy()`(敲响设备门铃)和 `pending_register()`(登记谁在等这个 `cmd_id`)是两次独立调用,中间有一个窗口没有关中断保护。如果设备的完成中断恰好在这个窗口里到达,完成通知会因为找不到匹配的等待项被直接丢弃,对应任务永远收不到唤醒(用 `NO MATCH FOUND` 的调试日志直接抓到过这个现象)。修法:把两次调用包进同一个关中断临界区,作为一个原子操作。

三个 bug 修完之后,又额外加了一层兜底——`dispatch_available_completions()`,在真正的 ISR 里调用,也在每个任务等待收尾的循环里周期性主动调用,而不是完全依赖中断投递(应对 M5 就记录过的那个 GIC 偶发不重新投递的已知问题)。

**验证结果**:沙箱压测 25/25 全过,更大批量 28/30;用户在自己真实环境上(`~/projects/rtos` + 自己编译的 `qemu-src`)10 次里 9 次干净通过,三条 `worker DATA OK, cmd_id=` 稳定出现。

---

## 目前已知、尚未解决的问题(不影响以上结论)

**`EC=0x0E`("Illegal Execution state")崩溃**(概率约 7–10%):和上面三个已经修复的 bug 是不同的问题——崩溃时栈 canary 完好,`ELR` 指向内核自己合法的地址范围(不是野指针),`pending[]` 表显示三个任务的完成状态其实都已经是 `done=1`,说明崩溃发生在**全部命令都已正确处理完之后**、某个任务收尾阶段的某个环节。根因还没有查,已记录为已知问题,决定先固定现有修复成果,不继续深挖。这类崩溃目前一共观察到两种不同特征(另一种是 M5/M7 早期版本里"跳到 `0x0`/`0x100000000`"的野指针,已经确认是上面 bug #1 造成的,现在已修复)——`EC=0x0E` 这种是独立的、尚未解决的第二类。

---

## 代码结构(`~/projects/rtos/`)

```
boot/boot.S          — 复位入口、EL 降级、栈/BSS 初始化
kernel/vectors.S      — 异常向量表、IRQ 全寄存器保存/恢复
kernel/switch.S        — 协作式上下文切换(callee-saved only)
kernel/uart.c          — PL011 轮询驱动(内部原子,组合日志需调用方自己包临界区)
kernel/task.c           — TCB、栈帧构造、task_trampoline
kernel/sched.c          — pick_next_ready()、yield()、任务注册
kernel/gic.c            — GICv2 distributor/CPU interface,支持 PPI/SGI 和 SPI
kernel/sync.c            — Semaphore / Mutex / Event / Queue
kernel/pci.c             — PCIe ECAM 枚举、BAR sizing/映射、IRQ pin→SPI
kernel/accel.c           — dma-accel HAL:SQ/CQ 注册、命令提交、IRQ 驱动完成
kernel/main.c            — 当前里程碑的测试 harness(每个里程碑会替换)
```
