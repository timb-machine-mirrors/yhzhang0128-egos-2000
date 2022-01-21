#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <metal/cpu.h>
#include <metal/uart.h>
#include <metal/interrupt.h>

#include "log.h"
#include "elf.h"
#include "mmu.h"

int tty_init();
int tty_read(char* buf, int len);
int tty_write(const char *format, ...);

int disk_init();
int disk_read(int block_no, int nblocks, char* dst);
int disk_write(int block_no, int nblocks, char* src);

typedef void (*handler_t)(int, void*);
int intr_init();
int intr_enable();
int intr_disable();
int intr_register(int id, handler_t handler);

int mmu_init();
int mmu_alloc(int* frame_no, int* addr);
int mmu_map(int pid, int page_no, int frame_no, int flag);
int mmu_switch(int pid);