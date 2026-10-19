/*
 * SvmRun.S - enter a guest and come back.
 *
 *   void rhv_svm_vmrun(GuestGprs *regs, uint64_t guestVmcbPa, uint64_t hostVmcbPa);
 *                      rdi              rsi                    rdx
 *
 * Does, with GIF cleared for the whole window:
 *   VMSAVE host  -> VMLOAD guest -> load guest GPRs -> VMRUN
 *   -> store guest GPRs -> VMSAVE guest -> VMLOAD host
 *
 * VMRUN itself only swaps a part of the state (segments, control registers,
 * RIP/RSP/RAX, EFER...). FS/GS/TR/LDTR, KernelGSBase, STAR/LSTAR/CSTAR/SFMASK
 * and the SYSENTER MSRs travel through VMSAVE/VMLOAD, which is why the host
 * needs a VMCB page of its own. macOS keeps its per-CPU pointer in GS, so
 * getting that wrong would crash the very next kernel instruction.
 *
 * Guest RAX and RSP live in the VMCB, not in `regs`.
 * The caller must run this with host interrupts disabled and SVM enabled on
 * this CPU. GuestGprs offsets are checked by static_asserts in Vm.cpp.
 */

#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

#define G_RBX 0
#define G_RCX 8
#define G_RDX 16
#define G_RSI 24
#define G_RDI 32
#define G_RBP 40
#define G_R8  48
#define G_R9  56
#define G_R10 64
#define G_R11 72
#define G_R12 80
#define G_R13 88
#define G_R14 96
#define G_R15 104

    .text
    .globl SYM(rhv_svm_vmrun)
    .p2align 4
SYM(rhv_svm_vmrun):
    /* System V: rbx, rbp, r12-r15 belong to the caller. */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    pushq   %rdi                /* 16(%rsp) = regs           */
    pushq   %rsi                /*  8(%rsp) = guest VMCB pa  */
    pushq   %rdx                /*  0(%rsp) = host VMCB pa   */

    clgi

    movq    0(%rsp), %rax
    vmsave  %rax                /* host FS/GS/TR/LDTR/KernelGSBase/SYSCALL MSRs -> host VMCB */
    movq    8(%rsp), %rax
    vmload  %rax                /* guest ones <- guest VMCB */

    movq    16(%rsp), %rdi
    movq    G_RBX(%rdi), %rbx
    movq    G_RCX(%rdi), %rcx
    movq    G_RDX(%rdi), %rdx
    movq    G_RSI(%rdi), %rsi
    movq    G_RBP(%rdi), %rbp
    movq    G_R8(%rdi),  %r8
    movq    G_R9(%rdi),  %r9
    movq    G_R10(%rdi), %r10
    movq    G_R11(%rdi), %r11
    movq    G_R12(%rdi), %r12
    movq    G_R13(%rdi), %r13
    movq    G_R14(%rdi), %r14
    movq    G_R15(%rdi), %r15
    movq    G_RDI(%rdi), %rdi   /* last: rdi was our pointer */

    movq    8(%rsp), %rax
    vmrun   %rax

    /*
     * Back from the guest. RSP is the host's again (restored by the #VMEXIT).
     * Do not trust RAX or anything but the stack slots for the addresses.
     */
    pushq   %rdi                /* park guest RDI, frees it as a pointer */
    movq    24(%rsp), %rdi      /* regs (slot moved up by the push) */
    movq    %rbx, G_RBX(%rdi)
    movq    %rcx, G_RCX(%rdi)
    movq    %rdx, G_RDX(%rdi)
    movq    %rsi, G_RSI(%rdi)
    movq    %rbp, G_RBP(%rdi)
    movq    %r8,  G_R8(%rdi)
    movq    %r9,  G_R9(%rdi)
    movq    %r10, G_R10(%rdi)
    movq    %r11, G_R11(%rdi)
    movq    %r12, G_R12(%rdi)
    movq    %r13, G_R13(%rdi)
    movq    %r14, G_R14(%rdi)
    movq    %r15, G_R15(%rdi)
    popq    %rax                /* guest RDI */
    movq    %rax, G_RDI(%rdi)

    movq    8(%rsp), %rax
    vmsave  %rax                /* guest FS/GS/... -> guest VMCB */
    movq    0(%rsp), %rax
    vmload  %rax                /* host FS/GS/... restored */

    stgi

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
