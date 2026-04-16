/* Minimal pico-sdk stub for host-side capture_pipe tests. */
#ifndef _HARDWARE_SYNC_H
#define _HARDWARE_SYNC_H

/*
 * On host (x86), the compiler already won't reorder volatile-free
 * stores past a compiler barrier, and x86 is TSO so the hardware
 * won't either.  The empty asm is a compiler barrier — sufficient
 * for the host test, and compiles to nothing.
 */
#define __mem_fence_release() __asm volatile("" ::: "memory")
#define __mem_fence_acquire() __asm volatile("" ::: "memory")

#endif
