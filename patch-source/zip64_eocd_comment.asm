; ZIP64 EOCD handling for the "scan" branch (archives with an archive comment)
; Replaces the 5-byte instruction at VA 0x10009DAE (movzx eax,word[esp+0x56]).
; At entry: edx = EOCD central-directory offset low (32-bit field),
;           the 18 bytes after the EOCD signature are at E+0x50,
;           stream position = just after those 18 bytes (start of comment).
; Normal path: replicate instruction and jump to 0x10009DB3.
; ZIP64 path: follow locator, read ZIP64 EOCD, fill v29/v32/comment length,
;             restore stream to the comment start, jump to 0x10009E59.
BITS 32
org 0x10029390

start:
    cmp edx, -1
    je .zip64
    cmp word [esp+0x56], 0xFFFF
    je .zip64
    movzx eax, word [esp+0x56]
    jmp 0x10009DB3

.zip64:
    ; save stream position (= start of archive comment) in E+0x38/0x3C
    mov ecx, esi
    mov eax, [esi]
    call dword [eax+0x24]          ; tell -> edx:eax
    mov [esp+0x38], eax
    mov [esp+0x3C], edx
    ; seek to ZIP64 EOCD locator: pos - 42
    mov eax, [esp+0x38]
    mov edx, [esp+0x3C]
    sub eax, 42
    sbb edx, 0
    push 0
    push edx
    push eax
    mov ecx, esi
    mov eax, [esi]
    call dword [eax+0x20]
    ; read 20-byte locator into E+0x64
    mov eax, [esi]
    push 20
    lea ecx, [esp+0x68]
    push ecx
    mov ecx, esi
    call dword [eax+0x18]
    cmp dword [esp+0x64], 0x07064B50   ; "PK\x06\x07"
    jne .fallback
    ; seek to ZIP64 EOCD offset (locator + 8 = E+0x6C)
    mov eax, [esp+0x6C]
    mov edx, [esp+0x70]
    push 0
    push edx
    push eax
    mov ecx, esi
    mov eax, [esi]
    call dword [eax+0x20]
    ; read 56-byte ZIP64 EOCD into E+0x64
    mov eax, [esi]
    push 0x38
    lea ecx, [esp+0x68]
    push ecx
    mov ecx, esi
    call dword [eax+0x18]
    cmp dword [esp+0x64], 0x06064B50   ; "PK\x06\x06"
    jne .fallback
    ; central directory offset (64-bit at +48 = E+0x94)
    mov eax, [esp+0x94]
    mov edx, [esp+0x98]
    mov [esp+0x20], eax
    mov [esp+0x24], edx
    ; total entries (64-bit at +32 = E+0x84; store low 32)
    mov eax, [esp+0x84]
    mov edx, [esp+0x88]
    mov [esp+0x30], eax
    mov [esp+0x34], edx
    ; comment length from normal EOCD (+20 = E+0x60)
    movzx eax, word [esp+0x60]
    mov [esp+0x10], eax
    mov ecx, [esp+0x1c]
    mov byte [ecx+8], 1
    ; restore stream to comment start
    mov eax, [esp+0x38]
    mov edx, [esp+0x3C]
    push 0
    push edx
    push eax
    mov ecx, esi
    mov eax, [esi]
    call dword [eax+0x20]
    jmp 0x10009E59

.fallback:
    mov edx, [esp+0x5c]            ; reload EOCD cd offset low
    movzx eax, word [esp+0x56]
    jmp 0x10009DB3
