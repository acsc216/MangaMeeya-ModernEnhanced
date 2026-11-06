BITS 32
org 0x4E4700

%define IAT_REGCLOSE        0x4E5000
%define IAT_REGSETVALUE     0x4E5004
%define IAT_REGCREATE       0x4E5008
%define IAT_REGQUERYVALUE   0x4E5010
%define IAT_REGOPEN         0x4E5014
%define IAT_LSTRCAT         0x4E52A4
%define IAT_LSTRCPY         0x4E52C4
%define IAT_LSTRLEN         0x4E52D8
%define IAT_SHDELETEKEY     0x4E53A0
%define LPSUBKEY_ARRAY      0x4FEF1C
%define VERB_STRING         0x4E9B38
%define HKCU                0x80000001
%define KEY_READ            0x20019
%define KEY_ALL_ACCESS      0xF003F

; ------------------------------------------------------------------
; build_verb: edi = destination buffer, eax = index (0..11)
; builds "Software\Classes\..." up to the verb name
; ------------------------------------------------------------------
build_verb:
    push esi
    push ebx
    mov esi, eax
    push str_classes
    push edi
    call dword [IAT_LSTRCPY]
    cmp esi, 0
    je .folder
    cmp esi, 4
    je .all
    push str_sfa
    push edi
    call dword [IAT_LSTRCAT]
    mov ebx, LPSUBKEY_ARRAY
    mov eax, [ebx + esi*4]
    push eax
    push edi
    call dword [IAT_LSTRCAT]
    jmp .tail
.folder:
    push str_folder
    push edi
    call dword [IAT_LSTRCAT]
    jmp .tail
.all:
    push str_star
    push edi
    call dword [IAT_LSTRCAT]
.tail:
    push str_shell
    push edi
    call dword [IAT_LSTRCAT]
    push VERB_STRING
    push edi
    call dword [IAT_LSTRCAT]
    pop ebx
    pop esi
    ret

; ------------------------------------------------------------------
; check_entry(a1, buf, cch)  -> 1 if command exists (buf filled)
; ------------------------------------------------------------------
check_entry:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    sub esp, 0x420
    mov eax, [ebp+8]
    cmp eax, 4
    je .fail
    lea edi, [ebp-0x420]
    call build_verb
    push str_command
    push edi
    call dword [IAT_LSTRCAT]

    lea eax, [ebp-0x18]
    push eax
    push KEY_READ
    push 0
    lea eax, [ebp-0x420]
    push eax
    push HKCU
    call dword [IAT_REGOPEN]
    test eax, eax
    jnz .fail

    mov eax, [ebp+0x10]
    mov [ebp-0x14], eax
    lea eax, [ebp-0x14]
    push eax
    push dword [ebp+0xC]
    push 0
    push 0
    push 0
    push dword [ebp-0x18]
    call dword [IAT_REGQUERYVALUE]
    mov esi, eax
    push dword [ebp-0x18]
    call dword [IAT_REGCLOSE]
    test esi, esi
    jnz .fail
    mov eax, 1
    jmp .done
.fail:
    xor eax, eax
.done:
    add esp, 0x420
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ------------------------------------------------------------------
; set_entry(a1, cmd) -> 1 on success
; ------------------------------------------------------------------
set_entry:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    sub esp, 0x420
    mov eax, [ebp+8]
    cmp eax, 4
    je .fail
    lea edi, [ebp-0x420]
    call build_verb
    push str_command
    push edi
    call dword [IAT_LSTRCAT]

    push 0
    lea eax, [ebp-0x18]
    push eax
    push 0
    push KEY_ALL_ACCESS
    push 0
    push 0
    push 0
    lea eax, [ebp-0x420]
    push eax
    push HKCU
    call dword [IAT_REGCREATE]
    test eax, eax
    jnz .fail

    push dword [ebp+0xC]
    call dword [IAT_LSTRLEN]
    inc eax
    push eax
    push dword [ebp+0xC]
    push 1
    push 0
    push 0
    push dword [ebp-0x18]
    call dword [IAT_REGSETVALUE]
    mov esi, eax
    push dword [ebp-0x18]
    call dword [IAT_REGCLOSE]
    test esi, esi
    jnz .fail
    mov eax, 1
    jmp .done
.fail:
    xor eax, eax
.done:
    add esp, 0x420
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ------------------------------------------------------------------
; del_entry(a1) -> 1 if deleted
; ------------------------------------------------------------------
del_entry:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    sub esp, 0x420
    mov eax, [ebp+8]
    cmp eax, 4
    je .fail
    lea edi, [ebp-0x420]
    call build_verb
    lea eax, [ebp-0x420]
    push eax
    push HKCU
    call dword [IAT_SHDELETEKEY]
    test eax, eax
    jnz .fail
    mov eax, 1
    jmp .done
.fail:
    xor eax, eax
.done:
    add esp, 0x420
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ------------------------------------------------------------------
; strings
; ------------------------------------------------------------------
align 4
str_classes: db 'Software\Classes\', 0
str_folder:  db 'Folder', 0
str_sfa:     db 'SystemFileAssociations\', 0
str_shell:   db '\shell\', 0
str_command: db '\command', 0
str_star:    db '*', 0
