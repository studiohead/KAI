# =============================================================================
# Makefile — KAI OS
#
# Requirements:
#   aarch64-elf-gcc   (cross-compiler, e.g. from Homebrew or AUR)
#   aarch64-elf-ld    (linker, part of the same toolchain)
#   aarch64-elf-objcopy
#   qemu-system-aarch64
#
# Targets:
#   all       — build ELF + raw binary image (default)
#   run       — launch QEMU in the current terminal (portable)
#   clean     — remove all build artefacts
#   size      — print section sizes
# =============================================================================

# ---- Toolchain -------------------------------------------------------------
CC      := aarch64-elf-gcc
LD      := aarch64-elf-ld
OBJCOPY := aarch64-elf-objcopy
SIZE    := aarch64-elf-size

# ---- Directories -----------------------------------------------------------
SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build

# ---- Flags -----------------------------------------------------------------
CFLAGS := \
    -std=c11                \
    -Wall                   \
    -Wextra                 \
    -Wpedantic              \
    -ffreestanding          \
    -nostdlib               \
    -nostartfiles           \
    -fno-stack-protector    \
    -O2                     \
    -I$(INC_DIR)

LDFLAGS := \
    -T scripts/linker.ld    \
    --no-warn-rwx-segments

# ---- Sources ----------------------------------------------------------------
ASM_SRCS := $(SRC_DIR)/arch/aarch64/boot.S

C_SRCS   := \
    $(SRC_DIR)/kernel.c                  \
    $(SRC_DIR)/uart.c                    \
    $(SRC_DIR)/syscall.c                 \
    $(SRC_DIR)/memory.c                  \
    $(SRC_DIR)/sandbox/sandbox.c         \
    $(SRC_DIR)/sandbox/interpreter.c     \
    $(SRC_DIR)/sandbox/verifier.c        \
    $(SRC_DIR)/lib/string.c

# ---- Objects (mirrors source tree under build/) ----------------------------
ASM_OBJS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(ASM_SRCS))
C_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
ALL_OBJS := $(ASM_OBJS) $(C_OBJS)

# ---- Outputs ---------------------------------------------------------------
ELF := $(BUILD_DIR)/kernel.elf
IMG := $(BUILD_DIR)/kernel.img   # flat binary for real hardware / inspection

# ---- QEMU ------------------------------------------------------------------
QEMU       := qemu-system-aarch64
QEMU_FLAGS := \
    -M virt                 \
    -cpu max                \
    -nographic              \
    -kernel $(ELF)

# ============================================================================
.PHONY: all run clean size

all: $(ELF) $(IMG)

# ---- Link ------------------------------------------------------------------
$(ELF): $(ALL_OBJS)
	$(LD) $(LDFLAGS) $^ -o $@

# ---- Flat binary -----------------------------------------------------------
$(IMG): $(ELF)
	$(OBJCOPY) -O binary $< $@

# ---- Compile C sources -----------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Assemble ASM sources --------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Run in current terminal (works on macOS, Linux, WSL) -----------------
run: all
	$(QEMU) $(QEMU_FLAGS)

# ---- Section size report ---------------------------------------------------
size: $(ELF)
	$(SIZE) $<

# ---- Clean -----------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)