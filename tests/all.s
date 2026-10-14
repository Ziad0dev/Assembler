# Exercises every instruction form rvasm supports.
.text
start:
    add  a0, a1, a2
    sub  t0, t1, t2
    sll  s0, s1, s2
    slt  x5, x6, x7
    sltu a3, a4, a5
    xor  t3, t4, t5
    srl  t6, s3, s4
    sra  s5, s6, s7
    or   s8, s9, s10
    and  s11, ra, sp
    addi a0, a0, -1
    slti a1, a2, 2047
    sltiu a1, a2, -2048
    xori a3, a4, 0xff
    ori  a5, a6, 1
    andi a7, t0, 0x7f
    slli t1, t2, 31
    srli t1, t2, 1
    srai t1, t2, 7
    lb   a0, -4(sp)
    lh   a0, 2(sp)
    lw   a0, 0(sp)
    lbu  a0, 2047(gp)
    lhu  a0, (tp)
    sb   a1, 0(sp)
    sh   a1, -2048(sp)
    sw   ra, 12(sp)
    lui  a0, 0x12345
    auipc t0, 0xfffff
loop:
    beq  a0, a1, loop
    bne  a0, a1, done
    blt  a0, a1, start
    bge  a0, a1, done
    bltu a0, a1, loop
    bgeu a0, a1, done
    jal  ra, done
    jal  start
    jalr ra, 4(t0)
    jalr t0
    ecall
    ebreak
    nop
    li   a0, 42
    li   a0, -1
    li   a0, 0x12345678
    li   a0, 0x80000000
    li   a1, 4096
    li   a2, 0x7ff
    # li a3, 0x800 is left out: LLVM emits addi+slli, rvasm emits lui+addi (both valid)
    mv   s0, a0
    not  t0, t1
    neg  t0, t1
    seqz a0, a1
    snez a0, a1
    j    loop
    jr   ra
    ret
    beqz a0, loop
    bnez a0, done
    bltz a0, loop
    bgez a0, done
    blez a0, loop
    bgtz a0, done
    bgt  a0, a1, loop
    ble  a0, a1, done
    bgtu a0, a1, loop
    bleu a0, a1, done
done:
    addi x0, x0, 0
