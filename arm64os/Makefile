# arm64os/Makefile
#
# Phase 1 构建系统
#
# 用法：
#   make            — 编译内核 ELF
#   make run        — 在 QEMU 中运行
#   make disasm     — 反汇编（查看向量表地址等）
#   make clean      — 清理构建产物
#
# 依赖：
#   - aarch64-linux-gnu-gcc（交叉编译工具链）
#   - qemu-system-aarch64（运行验证）

# ---- 交叉编译工具链 ----
CROSS_COMPILE   ?= aarch64-linux-gnu-
CC              := $(CROSS_COMPILE)gcc
LD              := $(CROSS_COMPILE)ld
OBJCOPY         := $(CROSS_COMPILE)objcopy
OBJDUMP         := $(CROSS_COMPILE)objdump
READELF         := $(CROSS_COMPILE)readelf

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
    -Wno-unused-parameter

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

# Phase 1 目标文件（顺序重要：head.o 必须第一个，确保 Image 头在 0x40080000）
OBJS := \
    arch/arm64/kernel/head.o \
    arch/arm64/kernel/entry.o \
    kernel/printk.o \
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
# -nographic        : 无图形界面，串口输出到 stdio
# -serial stdio     : PL011 UART 连接到标准输入输出
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
	    -nographic \
	    -serial stdio

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
	@echo "Clean done."

# ---- 帮助 ----
.PHONY: help
help:
	@echo "arm64os Phase 1 Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       — Build arm64os.elf (default)"
	@echo "  run       — Run in QEMU (Ctrl-A X to exit)"
	@echo "  debug     — Run in QEMU with GDB stub (port 1234)"
	@echo "  disasm    — Disassemble the kernel ELF"
	@echo "  sections  — Show ELF section headers"
	@echo "  syms      — Show symbol table (sorted by address)"
	@echo "  clean     — Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS_COMPILE — Toolchain prefix (default: aarch64-linux-gnu-)"
	@echo ""
	@echo "Example:"
	@echo "  make CROSS_COMPILE=aarch64-none-elf-"
