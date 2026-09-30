bits 32
global vbe_set_mode
global vbe_get_mode_info
global bios_set_mode
global bios_shutdown

section .text

vbe_set_mode:
    push ebp
    mov ebp, esp
    pushad
    
    mov bx, [ebp+8]
    mov [0x7FF0], bx
    mov dword [0x7FF4], 0x4F02
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

vbe_get_mode_info:
    push ebp
    mov ebp, esp
    pushad
    
    mov cx, [ebp+8]
    mov [0x7FF0], cx
    mov dword [0x7FF4], 0x4F01
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

bios_set_mode:
    push ebp
    mov ebp, esp
    pushad
    
    mov ax, [ebp+8]
    mov [0x7FF0], ax
    mov dword [0x7FF4], 0x0000
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

bios_shutdown:
    push ebp
    mov ebp, esp
    pushad
    
    mov dword [0x7FF4], 0x5307
    mov [0x7FFC], esp
    
    mov esi, payload_start
    mov edi, 0x8000
    mov ecx, payload_end - payload_start
    rep movsb
    
    jmp 0x18:0x8000

align 4
payload_start:
bits 16
    mov ax, 0x20 
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    
    jmp 0x00:real_mode - payload_start + 0x8000

real_mode:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov sp, 0x7E00
    
    lidt [ivt_ptr - payload_start + 0x8000]
    
    mov ax, [0x7FF4]
    cmp ax, 0x0000
    je .do_bios
    cmp ax, 0x4F02
    je .do_set
    cmp ax, 0x4F01
    je .do_get
    cmp ax, 0x5307
    je .do_shutdown
    jmp .done

.do_bios:
    mov ax, [0x7FF0]
    int 0x10
    jmp .done

.do_set:
    mov bx, [0x7FF0]
    int 0x10
    jmp .done

.do_get:
    mov cx, [0x7FF0]
    mov di, 0x9000
    int 0x10
    jmp .done

.do_shutdown:
    mov ax, 0x5301 ; APM Connect
    xor bx, bx
    int 0x15
    mov ax, 0x530E ; APM Enable
    xor bx, bx
    mov cx, 0x0102
    int 0x15
    mov ax, 0x5307 ; Set Power State
    mov bx, 0x0001 ; All devices
    mov cx, 0x0003 ; Off
    int 0x15
    ; If APM fails, try Bochs/QEMU ports
    mov dx, 0x604
    mov ax, 0x2000
    out dx, ax
    mov dx, 0xB004
    out dx, ax
    jmp .done

.done:
    mov [0x7FF8], ax

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    
    jmp 0x08:pm32 - payload_start + 0x8000
    
bits 32
pm32:
    mov ax, 0x10 
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    mov esp, [0x7FFC]
    
    popad
    movzx eax, word [0x7FF8]
    pop ebp
    ret

align 4
ivt_ptr:
    dw 0x03FF
    dd 0x00000000

payload_end:
