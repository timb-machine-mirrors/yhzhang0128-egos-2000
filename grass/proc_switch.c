/*
 * (C) 2022, Cornell University
 * All rights reserved.
 */

/* Author: Yunhao Zhang
 * Description: process scheduling and communication
 * system calls are basically inter-process communication
 */


#include "egos.h"
#include "grass.h"
#include "syscall.h"
#include <string.h>

#define INTR_ID_TMR        7
#define INTR_ID_SOFT       3

static void proc_yield();
static void proc_syscall();
static void (*kernel_entry)();

int proc_curr_idx;
struct  process     proc_set[MAX_NPROCESS];
#define curr_pid    proc_set[proc_curr_idx].pid
#define curr_status proc_set[proc_curr_idx].status

void intr_entry(int id) {
    if (curr_pid < GPID_SHELL && id == INTR_ID_TMR) {
        /* IO may be busy in kernel processes */
        timer_reset();
        return;
    }

    if (curr_pid >= GPID_USER_START && earth->tty_intr()) {
        /* User process killed by ctrl+c interrupt */
        __asm__ volatile("csrw mepc, %0" ::"r"(0x8005006));
        return;
    }

    /* switch to the grass kernel stack */
    switch(id) {
    case INTR_ID_TMR:
        kernel_entry = proc_yield;
        ctx_start(&proc_set[proc_curr_idx].sp, (void*)GRASS_STACK_TOP);
        break;
    case INTR_ID_SOFT:
        kernel_entry = proc_syscall;
        ctx_start(&proc_set[proc_curr_idx].sp, (void*)GRASS_STACK_TOP);
        break;
    default:
        FATAL("Got unknown interrupt #%d", id);
    }
}

void ctx_entry() {
    int tmp, mepc;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    proc_set[proc_curr_idx].mepc = (void*) mepc;

    /* kernel_entry is either proc_yield() or proc_syscall() */
    kernel_entry();

    /* switch back to the user application (intr_entry) */
    mepc = (int)proc_set[proc_curr_idx].mepc;
    __asm__ volatile("csrw mepc, %0" ::"r"(mepc));
    ctx_switch((void**)&tmp, proc_set[proc_curr_idx].sp);
}

static void proc_yield() {
    int next_idx = -1;
    for (int i = 1; i <= MAX_NPROCESS; i++) {
        int s = proc_set[(proc_curr_idx + i) % MAX_NPROCESS].status;
        if (s == PROC_READY || s == PROC_RUNNING || s == PROC_RUNNABLE) {
            next_idx = (proc_curr_idx + i) % MAX_NPROCESS;
            break;
        }
    }

    if (next_idx == -1) FATAL("proc_yield: no more runnable process");
    if (curr_status == PROC_RUNNING) proc_set_runnable(curr_pid);

    proc_curr_idx = next_idx;
    earth->mmu_switch(curr_pid);
    timer_reset();

    if (curr_status == PROC_READY) {
        proc_set_running(curr_pid);
        /* Prepare argc and argv */
        int argc = *((int*)APPS_MAIN_ARG);
        __asm__ volatile("mv a0, %0" ::"r"(argc));
        __asm__ volatile("mv a1, %0" ::"r"(APPS_MAIN_ARG + 4));
        /* Enter application code */
        __asm__ volatile("csrw mepc, %0" ::"r"(APPS_ENTRY));
        __asm__ volatile("mret");
    }

    proc_set_running(curr_pid);
}


static void proc_send(struct syscall *sc);
static void proc_recv(struct syscall *sc);

static void proc_syscall() {
    struct syscall *sc = (struct syscall*)GRASS_SYSCALL_ARG;

    int type = sc->type;
    sc->retval = 0;
    sc->type = SYS_UNUSED;
    *((int*)RISCV_CLINT0_MSIP_BASE) = 0;

    switch (type) {
    case SYS_RECV:
        proc_recv(sc);
        break;
    case SYS_SEND:
        proc_send(sc);
        break;
    default:
        FATAL("proc_sched: got unknown syscall type=%d", type);
    }
}

static void proc_send(struct syscall *sc) {
    sc->payload.msg.sender = curr_pid;
    int receiver = sc->payload.msg.receiver;

    int receiver_idx = -1;
    for (int i = 0; i < MAX_NPROCESS; i++)
        if (proc_set[i].pid == receiver) {
            receiver_idx = i;
            break;
        }

    if (receiver_idx == -1) {
        sc->retval = -1;
        return;
    }

    if (proc_set[receiver_idx].status != PROC_WAIT_TO_RECV) {
        proc_set[proc_curr_idx].status = PROC_WAIT_TO_SEND;
        proc_set[proc_curr_idx].receiver_pid = receiver;
    } else {
        struct sys_msg tmp;
        memcpy(&tmp, &sc->payload.msg, SYSCALL_MSG_LEN);
        earth->mmu_switch(receiver);

        sc->payload.msg.sender = curr_pid;
        memcpy(&sc->payload.msg, &tmp, SYSCALL_MSG_LEN);

        earth->mmu_switch(curr_pid);

        proc_set_runnable(receiver);
    }

    proc_yield();
}

static void proc_recv(struct syscall *sc) {
    sc->payload.msg.sender = 0;
    sc->payload.msg.receiver = curr_pid;

    int sender = -1;
    for (int i = 0; i < MAX_NPROCESS; i++)
        if (proc_set[i].status == PROC_WAIT_TO_SEND &&
            proc_set[i].receiver_pid == curr_pid) {
            sender = proc_set[i].pid;
            break;
        }

    if (sender == -1) {
        curr_status = PROC_WAIT_TO_RECV;
    } else {
        sc->payload.msg.sender = sender;

        struct sys_msg tmp;
        earth->mmu_switch(sender);
        memcpy(&tmp, &sc->payload.msg, SYSCALL_MSG_LEN);
        sc->payload.msg.receiver = curr_pid;

        earth->mmu_switch(curr_pid);
        memcpy(&sc->payload.msg, &tmp, SYSCALL_MSG_LEN);

        proc_set_runnable(sender);
    }

    proc_yield();
}