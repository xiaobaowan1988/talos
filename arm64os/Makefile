# arm64os/Makefile
#
# Phase 3 构建系统
#
# 用法：
#   make            — 编译内核 ELF（自动选择工具链）
#   make run        — 在 QEMU 中运行（交互，Ctrl-A X 退出）
#   make test       — 编译并在 QEMU 中运行验证（带超时，适合 CI）
#   make disasm     — 反汇编（查看向量表地址等）
#   make clean      — 清理构建产物
#
# 工具链选择（自动检测，也可手动指定）：
#   make                          — 自动检测（优先 GCC，回退 LLVM）
#   make LLVM=1                   — 强制使用 LLVM/Clang
#   make CROSS_COMPILE=aarch64-none-elf-  — 指定 GCC 前缀

# ---- 交叉编译工具链 ----
#
# 优先级：
#   1. LLVM=1 显式指定 → 使用 clang -target aarch64-linux-gnu + ld.lld
#   2. CROSS_COMPILE 显式指定 → 使用指定的 GCC 工具链
#   3. 自动检测：若 aarch64-linux-gnu-gcc 存在则用 GCC，否则回退到 LLVM
#
CROSS_COMPILE   ?= aarch64-linux-gnu-

# 自动检测：若未设置 LLVM 且 GCC 交叉编译器不存在，自动切换到 LLVM
LLVM            ?= $(shell command -v $(CROSS_COMPILE)gcc >/dev/null 2>&1 && echo 0 || echo 1)

ifeq ($(LLVM),1)
CC              := clang -target aarch64-linux-gnu
LD              := ld.lld
OBJCOPY         := llvm-objcopy
OBJDUMP         := llvm-objdump
READELF         := llvm-readelf
else
CC              := $(CROSS_COMPILE)gcc
LD              := $(CROSS_COMPILE)ld
OBJCOPY         := $(CROSS_COMPILE)objcopy
OBJDUMP         := $(CROSS_COMPILE)objdump
READELF         := $(CROSS_COMPILE)readelf
endif

# ---- 编译选项 ----
#
# -ffreestanding   : 不依赖标准库，不假设 main() 存在
# -nostdlib        : 不链接标准库（libc, crt0 等）
# -nostdinc        : 不使用系统头文件搜索路径
# -march=armv8-a   : 目标架构 ARMv8-A（Cortex-A72 兼容）
# -mstrict-align   : 禁止非对齐内存访问（ARM64 默认行为）
# -O1              : 基础优化（保留调试信息可读性）
# -g               : 生成 DWARF 调试信息
# -pipe            : 使用管道加速编译
#
CFLAGS := \
    -ffreestanding \
    -nostdlib \
    -nostdinc \
    -I include \
    -I arch/arm64/include \
    -march=armv8-a \
    -mstrict-align \
    -O1 \
    -g \
    -pipe \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -Wno-unused-function

# 汇编选项（.S 文件经过 C 预处理器，可使用 #include / #define）
ASFLAGS := \
    -ffreestanding \
    -nostdlib \
    -nostdinc \
    -I include \
    -I arch/arm64/include \
    -march=armv8-a \
    -g

# 链接选项
LDFLAGS := \
    -nostdlib \
    --no-undefined

# ---- 文件列表 ----
LDSCRIPT := scripts/linker.ld

# Phase 1+2+3 目标文件（顺序重要：head.o 必须第一个，确保 Image 头在 0x40080000）
OBJS := \
    arch/arm64/kernel/head.o \
    arch/arm64/kernel/entry.o \
    arch/arm64/mm/proc.o \
    arch/arm64/mm/tlb.o \
    arch/arm64/mm/mmu.o \
    mm/memblock.o \
    mm/page_alloc.o \
    kernel/printk.o \
    kernel/irq/irqdesc.o \
    kernel/irq/handle.o \
    drivers/irqchip/gic-v3.o \
    drivers/timer/arm_arch_timer.o \
    kernel/main.o

TARGET  := arm64os.elf
BINARY  := arm64os.bin          # 纯二进制格式（可选）

# ---- 默认目标 ----
.PHONY: all
all: $(TARGET)

# ---- 链接 ----
$(TARGET): $(OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)
	@echo ""
	@echo "=== Build successful: $@ ==="
	@$(READELF) -h $@ | grep -E "Entry|Load|Size" || true
	@echo ""

# ---- 编译汇编文件（.S）----
#
# 使用 gcc 而非 as，以便通过 C 预处理器处理 #include / #define。
# -x assembler-with-cpp 显式指定文件类型。
arch/arm64/kernel/%.o: arch/arm64/kernel/%.S
	$(CC) $(ASFLAGS) -x assembler-with-cpp -c -o $@ $<

# Phase 2: arch/arm64/mm 汇编文件
arch/arm64/mm/%.o: arch/arm64/mm/%.S
	$(CC) $(ASFLAGS) -x assembler-with-cpp -c -o $@ $<

# Phase 2: arch/arm64/mm C 文件
arch/arm64/mm/%.o: arch/arm64/mm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 2: mm/ C 文件
mm/%.o: mm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 3: kernel/irq/ C 文件
kernel/irq/%.o: kernel/irq/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 3: drivers/irqchip/ C 文件
drivers/irqchip/%.o: drivers/irqchip/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 3: drivers/timer/ C 文件
drivers/timer/%.o: drivers/timer/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- 编译 C 文件 ----
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- 可选：生成纯二进制（用于 -kernel 加载时若需要 Image 格式）----
$(BINARY): $(TARGET)
	$(OBJCOPY) -O binary $< $@

# ---- 运行（QEMU）----
#
# -M virt           : ARM virt machine（通用 ARM 开发板模型）
# -cpu cortex-a72   : Cortex-A72（ARMv8-A，支持 EL2）
# -m 1G             : 1GB 内存
# -kernel           : 加载 ELF 内核镜像
# -nographic        : 无图形界面，PL011 串口自动重定向到 stdio
#                     （不要额外加 -serial stdio，会冲突报错）
#
# 退出 QEMU：Ctrl-A X
#
.PHONY: run
run: $(TARGET)
	qemu-system-aarch64 \
	    -M virt \
	    -cpu cortex-a72 \
	    -m 1G \
	    -kernel $(TARGET) \
	    -nographic

# ---- 自动化测试（带超时，适合 CI / 快速验证）----
#
# 运行 QEMU 最多 QEMU_TIMEOUT 秒后强制终止。
# 检查输出中是否包含 Phase 2 的预期结尾行：
#   "Phase 2 complete"         — 全部阶段通过
#   "memory fully reclaimed"   — Buddy 分配/释放/合并正确
#
# 用法：
#   make test          — 编译 + 运行验证（超时 15 秒）
#   make test QEMU_TIMEOUT=30  — 调整超时时间
#
QEMU_TIMEOUT    ?= 15

.PHONY: test
test: $(TARGET)
	@echo "=== Running Phase 3 in QEMU (timeout $(QEMU_TIMEOUT)s) ==="
	@timeout $(QEMU_TIMEOUT) qemu-system-aarch64 \
	    -M virt \
	    -cpu cortex-a72 \
	    -m 1G \
	    -kernel $(TARGET) \
	    -nographic \
	    -no-reboot 2>&1 | tee /tmp/arm64os_test.log; true
	@echo ""
	@echo "--- Test results ---"
	@grep -q "MMU enabled"             /tmp/arm64os_test.log && echo "PASS: MMU enabled"             || echo "FAIL: 'MMU enabled' not found"
	@grep -q "memory fully reclaimed"  /tmp/arm64os_test.log && echo "PASS: Buddy alloc/free/merge"  || echo "FAIL: 'memory fully reclaimed' not found"
	@grep -q "GIC v3 initialized"      /tmp/arm64os_test.log && echo "PASS: GIC v3 initialized"      || echo "FAIL: 'GIC v3 initialized' not found"
	@grep -q "arch timer started"      /tmp/arm64os_test.log && echo "PASS: arch timer started"      || echo "FAIL: 'arch timer started' not found"
	@grep -q "Timer ticks: OK"         /tmp/arm64os_test.log && echo "PASS: Timer ticks received"    || echo "FAIL: 'Timer ticks: OK' not found"
	@grep -q "Phase 3 complete"        /tmp/arm64os_test.log && echo "PASS: Phase 3 complete"        || echo "FAIL: 'Phase 3 complete' not found"
	@echo "--------------------"

# ---- 带 GDB 调试的运行 ----
#
# 另开终端：aarch64-linux-gnu-gdb arm64os.elf
#   (gdb) target remote :1234
#   (gdb) b start_kernel
#   (gdb) c
#
.PHONY: debug
debug: $(TARGET)
	qemu-system-aarch64 \
	    -M virt \
	    -cpu cortex-a72 \
	    -m 1G \
	    -kernel $(TARGET) \
	    -nographic \
	    -serial stdio \
	    -s -S

# ---- 反汇编 ----
.PHONY: disasm
disasm: $(TARGET)
	$(OBJDUMP) -d -S $(TARGET)

# ---- 查看 ELF 节信息 ----
.PHONY: sections
sections: $(TARGET)
	$(READELF) -S $(TARGET)

# ---- 查看符号表（包含向量表地址）----
.PHONY: syms
syms: $(TARGET)
	$(OBJDUMP) -t $(TARGET) | sort

# ---- 清理 ----
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET) $(BINARY)
	find . -name '*.o' -delete 2>/dev/null || true
	@echo "Clean done."

# ---- 帮助 ----
.PHONY: help
help:
	@echo "arm64os Phase 3 Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       — Build arm64os.elf (default)"
	@echo "  run       — Run in QEMU interactively (Ctrl-A X to exit)"
	@echo "  test      — Build + run in QEMU with timeout, check output (CI)"
	@echo "  debug     — Run in QEMU with GDB stub (port 1234)"
	@echo "  disasm    — Disassemble the kernel ELF"
	@echo "  sections  — Show ELF section headers"
	@echo "  syms      — Show symbol table (sorted by address)"
	@echo "  clean     — Remove build artifacts"
	@echo ""
	@echo "Toolchain (auto-detected; override with variables):"
	@echo "  LLVM=1              — Force LLVM/Clang toolchain"
	@echo "  CROSS_COMPILE=...   — Override GCC prefix (default: aarch64-linux-gnu-)"
	@echo "  QEMU_TIMEOUT=N      — Timeout in seconds for 'make test' (default: 15)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          — auto-detect toolchain, build"
	@echo "  make test                     — build + QEMU verification"
	@echo "  make LLVM=1 test              — force clang, verify"
	@echo "  make CROSS_COMPILE=aarch64-none-elf- run"
