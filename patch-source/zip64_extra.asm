; ZIP64 extra-field parser patch for arc.dll
; Replaces the broken block at VA 0x100093C5.
; Entry state: esi = stream object, edi = output info struct,
;   E[+0x14]=extra_len, E[+0x1c]=comment_len,
;   E[+0x3c]=uncompressed lo, E[+0x40]=hi,
;   E[+0x44]=compressed lo,  E[+0x48]=hi,
;   ebp = local header offset lo, ebx = hi   (E = esp at 0x100093C5)
; We allocate 0x20 bytes of scratch and keep the original frame untouched.
; On exit restores esp and jumps to 0x1000944C.
BITS 32
org 0x10029170

%macro READ8 2
    mov eax, [esi]
    mov eax, [eax+0x18]          ; read method
    mov [esp+0x0c], eax
    push 8
    lea ecx, [esp+0x14]          ; E2+0x10 buffer
    push ecx
    mov ecx, esi
    call dword [esp+0x14]        ; E2+0x0c + 8
    mov eax, [esp+0x10]
    mov edx, [esp+0x14]
    mov %1, eax
    mov %2, edx
%endmacro

%macro SKIP_EAX 0
    test eax, eax
    jle %%done
    mov edx, [esi]
    mov edx, [edx+0x20]          ; seek method
    mov [esp+0x0c], edx
    cdq
    push 1
    push edx
    push eax
    mov ecx, esi
    call dword [esp+0x18]        ; E2+0x0c + 12
%%done:
%endmacro

start:
    sub esp, 0x20
    movzx eax, word [esp+0x34]   ; extra_len (E+0x14)
    test eax, eax
    jz .done
    mov [esp+0x18], eax          ; remaining
.next_sub:
    cmp dword [esp+0x18], 4
    jb .skip_rest
    ; read subfield id (2) into [esp+0x00]
    mov eax, [esi]
    push 2
    lea ecx, [esp+0x04]
    push ecx
    mov ecx, esi
    call dword [eax+0x18]
    ; read subfield size (2) into [esp+0x04]
    mov eax, [esi]
    push 2
    lea ecx, [esp+0x08]
    push ecx
    mov ecx, esi
    call dword [eax+0x18]
    sub dword [esp+0x18], 4
    movzx eax, word [esp+0x04]
    sub [esp+0x18], eax
    cmp word [esp+0x00], 1
    jne .skip_sub

    ; ---- ZIP64 extended information subfield ----
    mov dword [esp+0x08], 0      ; consumed
    cmp dword [esp+0x5c], -1     ; uncompressed size missing? (E+0x3c)
    jne .no_uncomp
    READ8 [esp+0x5c], [esp+0x60]
    add dword [esp+0x08], 8
.no_uncomp:
    cmp dword [esp+0x64], -1     ; compressed size missing? (E+0x44)
    jne .no_comp
    READ8 [esp+0x64], [esp+0x68]
    add dword [esp+0x08], 8
.no_comp:
    cmp ebp, -1                  ; local header offset missing?
    jne .no_off
    READ8 ebp, ebx
    add dword [esp+0x08], 8
.no_off:
    movzx eax, word [esp+0x04]
    sub eax, [esp+0x08]          ; skip anything else (disk number etc.)
    SKIP_EAX
    jmp .skip_rest

.skip_sub:
    movzx eax, word [esp+0x04]
    SKIP_EAX
    cmp dword [esp+0x18], 4
    jae .next_sub

.skip_rest:
    mov eax, [esp+0x18]
    SKIP_EAX

.done:
    add esp, 0x20
    jmp 0x1000944C
