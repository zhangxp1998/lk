/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arch/arch_interrupts.h>
#include <arch/mmu.h>
#include <arch/ops.h>
#include <lib/bio.h>
#include <libfdt.h>
#include <lk/console_cmd.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "defer.h"
#include "memory_protocols.h"
#include "uefi_platform.h"

namespace {

constexpr uint32_t kArm64ImageMagic = 0x644d5241;
constexpr size_t kFdtExtraSpace = 4096;
constexpr size_t kMaxBootargs = 1024;
constexpr size_t kLinuxStackSize = 1 * 1024ul * 1024;
constexpr const char kDefaultBootargs[] =
    "console=ttyAMA0 earlycon=pl011,mmio32,0x9000000";
constexpr const char kDefaultStdoutPath[] = "/pl011@9000000";

struct Arm64ImageHeader {
  uint32_t code0;
  uint32_t code1;
  uint64_t text_offset;
  uint64_t image_size;
  uint64_t flags;
  uint64_t reserved[3];
  uint32_t magic;
  uint32_t reserved2;
};

static_assert(offsetof(Arm64ImageHeader, magic) == 0x38);

extern "C" const void *get_fdt(void);
extern "C" void linux_boot_jump(paddr_t kernel_entry, paddr_t fdt,
                                paddr_t stack_top) __NO_RETURN;

void *alloc_identity(size_t size, size_t align_log2) {
  void *allocated = alloc_page(size, align_log2);
  if (allocated == nullptr) {
    return nullptr;
  }
  printf("Allocated identity-mapped memory at %p, size %#zx\n", allocated, size);
  return allocated;
}

status_t map_linux_boot_trampoline() {
  auto aspace = set_boot_aspace();
  if (aspace == nullptr) {
    return ERR_NO_MEMORY;
  }

  const paddr_t trampoline_pa =
      vaddr_to_paddr(reinterpret_cast<void *>(linux_boot_jump));
  if (trampoline_pa == 0) {
    printf("Failed to translate linux_boot_jump to physical address\n");
    return ERR_BAD_STATE;
  }

  const paddr_t trampoline_page = ROUNDDOWN(trampoline_pa, PAGE_SIZE);
  const int ret =
      arch_mmu_map(&aspace->arch_aspace, trampoline_page, trampoline_page, 1, 0);
  if (ret != 0) {
    printf("Failed to identity-map Linux boot trampoline %#lx: %d\n",
           trampoline_page, ret);
    return ret;
  }

  printf("Linux boot trampoline identity-mapped at %#lx\n", trampoline_page);
  return NO_ERROR;
}

status_t prepare_fdt(const char *bootargs, paddr_t initrd_start,
                     paddr_t initrd_end, void **out_fdt, paddr_t *out_paddr) {
  const void *src_fdt = get_fdt();
  if (src_fdt == nullptr) {
    printf("No FDT available for Linux boot\n");
    return ERR_NOT_FOUND;
  }

  const int fdt_size = fdt_totalsize(src_fdt);
  if (fdt_size <= 0) {
    printf("Invalid source FDT size %d\n", fdt_size);
    return ERR_BAD_STATE;
  }

  const size_t new_fdt_size = ROUNDUP(fdt_size + kFdtExtraSpace, PAGE_SIZE);
  void *fdt = alloc_identity(new_fdt_size, PAGE_SIZE_SHIFT);
  if (fdt == nullptr) {
    printf("Failed to allocate %zu bytes for Linux FDT\n", new_fdt_size);
    return ERR_NO_MEMORY;
  }

  int ret = fdt_open_into(src_fdt, fdt, static_cast<int>(new_fdt_size));
  if (ret < 0) {
    printf("fdt_open_into failed: %d\n", ret);
    return ERR_BAD_STATE;
  }

  int chosen = fdt_path_offset(fdt, "/chosen");
  if (chosen < 0) {
    chosen = fdt_add_subnode(fdt, 0, "chosen");
  }
  if (chosen < 0) {
    printf("Failed to find/create /chosen: %d\n", chosen);
    return ERR_BAD_STATE;
  }

  ret = fdt_setprop_string(fdt, chosen, "bootargs", bootargs);
  if (ret < 0) {
    printf("Failed to set /chosen/bootargs: %d\n", ret);
    return ERR_BAD_STATE;
  }

  ret = fdt_setprop_string(fdt, chosen, "stdout-path", kDefaultStdoutPath);
  if (ret < 0) {
    printf("Failed to set /chosen/stdout-path: %d\n", ret);
    return ERR_BAD_STATE;
  }

  if (initrd_start != 0 && initrd_end > initrd_start) {
    ret = fdt_setprop_u64(fdt, chosen, "linux,initrd-start", initrd_start);
    if (ret < 0) {
      printf("Failed to set /chosen/linux,initrd-start: %d\n", ret);
      return ERR_BAD_STATE;
    }
    ret = fdt_setprop_u64(fdt, chosen, "linux,initrd-end", initrd_end);
    if (ret < 0) {
      printf("Failed to set /chosen/linux,initrd-end: %d\n", ret);
      return ERR_BAD_STATE;
    }
    printf("Linux initrd at [%#lx, %#lx)\n", initrd_start, initrd_end);
  }

  ret = fdt_pack(fdt);
  if (ret < 0) {
    printf("fdt_pack failed: %d\n", ret);
    return ERR_BAD_STATE;
  }

  *out_fdt = fdt;
  *out_paddr = reinterpret_cast<paddr_t>(fdt);
  printf("Linux FDT at %p, bootargs: %s\n", fdt, bootargs);
  return NO_ERROR;
}

status_t load_initrd(const char *blkdev, void **out_initrd, size_t *out_size) {
  bdev_t *dev = bio_open(blkdev);
  if (dev == nullptr) {
    printf("error opening initrd block device %s\n", blkdev);
    return ERR_NOT_FOUND;
  }
  DEFER { bio_close(dev); };

  if (dev->total_size <= 0) {
    printf("Initrd block device %s has invalid size %lld\n", blkdev,
           static_cast<long long>(dev->total_size));
    return ERR_BAD_LEN;
  }

  size_t loaded_size = static_cast<size_t>(dev->total_size);
  size_t initrd_size = ROUNDUP(loaded_size, PAGE_SIZE);
  void *initrd = alloc_identity(initrd_size, PAGE_SIZE_SHIFT);
  if (initrd == nullptr) {
    printf("Failed to allocate %zu bytes for Linux initrd\n", initrd_size);
    return ERR_NO_MEMORY;
  }

  ssize_t bytes = bio_read(dev, initrd, 0, dev->total_size);
  if (bytes != dev->total_size) {
    printf("Failed to read Linux initrd: %zd/%lld\n", bytes,
           static_cast<long long>(dev->total_size));
    return ERR_IO;
  }
  if (initrd_size > loaded_size) {
    memset(static_cast<uint8_t *>(initrd) + loaded_size, 0,
           initrd_size - loaded_size);
  }

  printf("Loaded Linux initrd from %s to %p, size %#zx\n", blkdev, initrd,
         loaded_size);
  *out_initrd = initrd;
  *out_size = loaded_size;
  return NO_ERROR;
}

status_t load_linux_image(const char *blkdev, void **out_image, size_t *out_size) {
  bdev_t *dev = bio_open(blkdev);
  if (dev == nullptr) {
    printf("error opening block device %s\n", blkdev);
    return ERR_NOT_FOUND;
  }
  DEFER { bio_close(dev); };

  if (dev->total_size < static_cast<off_t>(sizeof(Arm64ImageHeader))) {
    printf("Block device %s too small for ARM64 Image header: %lld\n", blkdev,
           static_cast<long long>(dev->total_size));
    return ERR_BAD_LEN;
  }

  Arm64ImageHeader header{};
  ssize_t bytes = bio_read(dev, &header, 0, sizeof(header));
  if (bytes != static_cast<ssize_t>(sizeof(header))) {
    printf("Failed to read ARM64 Image header: %zd\n", bytes);
    return ERR_IO;
  }

  if (header.magic != kArm64ImageMagic) {
    printf("ARM64 Image magic check failed %#x\n", header.magic);
    return ERR_BAD_STATE;
  }

  size_t loaded_size = static_cast<size_t>(dev->total_size);
  size_t image_size = static_cast<size_t>(header.image_size);
  if (image_size == 0) {
    image_size = loaded_size;
  }
  if (image_size < loaded_size) {
    printf("ARM64 Image header size %#zx is smaller than block device size %#zx\n",
           image_size, loaded_size);
    return ERR_BAD_LEN;
  }
  image_size = ROUNDUP(image_size, PAGE_SIZE);

  void *image = alloc_identity(image_size, 21 /* Linux prefers 2 MiB alignment. */);
  if (image == nullptr) {
    printf("Failed to allocate %zu bytes for Linux Image\n", image_size);
    return ERR_NO_MEMORY;
  }

  bytes = bio_read(dev, image, 0, dev->total_size);
  if (bytes != dev->total_size) {
    printf("Failed to read Linux Image: %zd/%lld\n", bytes,
           static_cast<long long>(dev->total_size));
    return ERR_IO;
  }
  if (image_size > loaded_size) {
    memset(static_cast<uint8_t *>(image) + loaded_size, 0, image_size - loaded_size);
  }

  printf("Loaded ARM64 Linux Image from %s to %p, size %#zx, text_offset %#llx\n",
         blkdev, image, image_size,
         static_cast<unsigned long long>(header.text_offset));

  *out_image = image;
  *out_size = image_size;
  return NO_ERROR;
}

int cmd_linux_load(int argc, const console_cmd_args *argv) {
  if (argc < 2) {
    printf("Usage: %s <kernel block device> [--initrd <initrd block device>] [bootargs]\n",
           argv[0].str);
    return ERR_INVALID_ARGS;
  }

  const char *initrd_blkdev = nullptr;
  int bootargs_start = 2;
  if (argc >= 4 && strcmp(argv[2].str, "--initrd") == 0) {
    initrd_blkdev = argv[3].str;
    bootargs_start = 4;
  }

  char bootargs[kMaxBootargs];
  if (bootargs_start >= argc) {
    strlcpy(bootargs, kDefaultBootargs, sizeof(bootargs));
  } else {
    bootargs[0] = '\0';
    for (int i = bootargs_start; i < argc; i++) {
      if (i > bootargs_start) {
        strlcat(bootargs, " ", sizeof(bootargs));
      }
      strlcat(bootargs, argv[i].str, sizeof(bootargs));
    }
  }

  if (set_boot_aspace() == nullptr) {
    return ERR_NO_MEMORY;
  }
  status_t ret = map_linux_boot_trampoline();
  if (ret != NO_ERROR) {
    return ret;
  }

  void *image = nullptr;
  size_t image_size = 0;
  ret = load_linux_image(argv[1].str, &image, &image_size);
  if (ret != NO_ERROR) {
    return ret;
  }

  void *initrd = nullptr;
  size_t initrd_size = 0;
  if (initrd_blkdev != nullptr) {
    ret = load_initrd(initrd_blkdev, &initrd, &initrd_size);
    if (ret != NO_ERROR) {
      return ret;
    }
  }
  paddr_t initrd_start = reinterpret_cast<paddr_t>(initrd);
  paddr_t initrd_end = initrd_start + initrd_size;

  void *fdt = nullptr;
  paddr_t fdt_paddr = 0;
  ret = prepare_fdt(bootargs, initrd_start, initrd_end, &fdt, &fdt_paddr);
  if (ret != NO_ERROR) {
    return ret;
  }

  void *stack = alloc_identity(kLinuxStackSize, 23);
  if (stack == nullptr) {
    printf("Failed to allocate %zu bytes for Linux boot stack\n", kLinuxStackSize);
    return ERR_NO_MEMORY;
  }
  memset(stack, 0, kLinuxStackSize);
  paddr_t stack_top = reinterpret_cast<paddr_t>(stack) + kLinuxStackSize;
  stack_top &= ~static_cast<paddr_t>(0xf);
  printf("Linux boot stack at [%p, %#lx)\n", stack, stack_top);

  arch_disable_ints();
  arch_clean_cache_range(reinterpret_cast<addr_t>(image), image_size);
  if (initrd != nullptr) {
    arch_clean_cache_range(reinterpret_cast<addr_t>(initrd), initrd_size);
  }
  arch_clean_cache_range(reinterpret_cast<addr_t>(fdt), fdt_totalsize(fdt));
  arch_clean_cache_range(reinterpret_cast<addr_t>(stack), kLinuxStackSize);
  arm64_local_clean_invalidate_cache_all();

  printf("Jumping to Linux Image entry %p with FDT %#lx and stack %#lx\n", image,
         fdt_paddr, stack_top);
  linux_boot_jump(reinterpret_cast<paddr_t>(image), fdt_paddr, stack_top);
}

STATIC_COMMAND_START
STATIC_COMMAND("linux_load", "load raw ARM64 Linux Image and run it", &cmd_linux_load)
STATIC_COMMAND_END(linux_load);

} // namespace
