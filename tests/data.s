# PC-relative pseudo instructions and data directives.
.equ STDOUT, 1
.equ SYS_WRITE, 64
_start:
    la    a1, msg
    li    a0, STDOUT
    li    a2, msg_end - msg
    call  print
    tail  exit
print:
    li    a7, SYS_WRITE
    ecall
    ret
exit:
    li    a7, 93
    li    a0, 0
    ecall
msg:
    .ascii "Hello, RISC-V!\n"
msg_end:
    .align 2
table:
    .word 0xdeadbeef, -1, msg_end - msg  # absolute symbols differ: clang leaves a relocation
    .half 0x1234, 'A'
    .byte 1, 2, 3
    .asciz "quote \" and # inside"
    .zero 3
