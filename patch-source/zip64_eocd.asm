; ZIP64 end-of-central-directory patch for arc.dll
; Replaces the 7-byte instruction at VA 0x10009BB5 (mov eax,[esp+0x96]).
; Entry state: esp = E (frame of sub_10009A00), the last 56 bytes of the
; archive are in the buffer at E+0x64:
;   +0x0E: ZIP64 EOCD locator (PK\x06\x07), +0x16: 64-bit ZIP64 EOCD offset
;   +0x22: normal EOCD (PK\x05\x06),      +0x32: 32-bit cd offset
; Normal EOCD cd offset == 0xFFFFFFFF triggers ZIP64 handling.
; On normal path jumps to 0x10009BBC with eax = cd offset low.
; On ZIP64 path fills v29 (E+0x20/0x24) and count (E+0x30/0x34) and jumps to 0x10009EC3.
BITS 32
org 0x100292E0

start:
    mov eax, [esp+0x96]          ; EOCD cd offset low
    cmp eax, -1
    je .zip64
    cmp word [esp+0x90], 0xFFFF  ; entry count saturated -> ZIP64 EOCD
    je .zip64
    jmp 0x10009BBC               ; original instruction already applied to eax

.zip64:
    cmp dword [esp+0x72], 0x07064B50   ; "PK\x06\x07" locator signature?
    jne .fallback
    ; seek to ZIP64 EOCD offset from locator (buffer+0x16 / +0x1A)
    mov eax, [esp+0x7A]
    mov edx, [esp+0x7E]
    push 0                        ; mode = FILE_BEGIN
    push edx                      ; high
    push eax                      ; low
    mov ecx, esi
    mov eax, [esi]
    call dword [eax+0x20]         ; seek
    ; read 56-byte ZIP64 EOCD into buffer E+0x64
    mov eax, [esi]
    push 0x38
    lea ecx, [esp+0x68]           ; E+0x64
    push ecx
    mov ecx, esi
    call dword [eax+0x18]         ; read
    cmp dword [esp+0x64], 0x06064B50   ; "PK\x06\x06"
    jne .fallback
    ; v29 = central directory offset (8 bytes at E+0x94)
    mov eax, [esp+0x94]
    mov edx, [esp+0x98]
    mov [esp+0x20], eax
    mov [esp+0x24], edx
    ; entry count (8 bytes at E+0x84; low 32 stored)
    mov eax, [esp+0x84]
    mov edx, [esp+0x88]
    mov [esp+0x30], eax
    mov [esp+0x34], edx
    mov ecx, [esp+0x1c]           ; this
    mov byte [ecx+8], 1           ; ZIP64 flag
    jmp 0x10009EC3

.fallback:
    mov eax, [esp+0x96]
    jmp 0x10009BBC
