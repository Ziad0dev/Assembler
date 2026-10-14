# Computes the first 10 Fibonacci numbers into the `fibs` table.
_start:
    la   t0, fibs
    li   t1, 0          # a = 0
    li   t2, 1          # b = 1
    li   t3, 10         # count
loop:
    sw   t1, 0(t0)
    add  t4, t1, t2     # next = a + b
    mv   t1, t2
    mv   t2, t4
    addi t0, t0, 4
    addi t3, t3, -1
    bnez t3, loop
    ebreak

    .align 2
fibs:
    .zero 40
