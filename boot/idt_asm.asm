bits 32
section .text

extern isr_handler
extern irq_handler

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1       ; interrupt number
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

%macro DEFINE_IRQ 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

DEFINE_IRQ 0, 32
DEFINE_IRQ 1, 33
DEFINE_IRQ 2, 34
DEFINE_IRQ 3, 35
DEFINE_IRQ 4, 36
DEFINE_IRQ 5, 37
DEFINE_IRQ 6, 38
DEFINE_IRQ 7, 39
DEFINE_IRQ 8, 40
DEFINE_IRQ 9, 41
DEFINE_IRQ 10, 42
DEFINE_IRQ 11, 43
DEFINE_IRQ 12, 44
DEFINE_IRQ 13, 45
DEFINE_IRQ 14, 46
DEFINE_IRQ 15, 47

isr_common_stub:
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call isr_handler
    add esp, 4

    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd

irq_common_stub:
    push ds
    push es
    push fs
    push gs
    pushad

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
