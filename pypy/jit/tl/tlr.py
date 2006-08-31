from pypy.rpython.objectmodel import hint


MOV_A_R    = 1
MOV_R_A    = 2
JUMP_IF_A  = 3
SET_A      = 4
ADD_R_TO_A = 5
RETURN_A   = 6
ALLOCATE   = 7
NEG_A      = 8


def interpret(bytecode, a):
    """Another Toy Language interpreter, this one register-based."""
    regs = []
    pc = 0
    while True:
        opcode = hint(ord(bytecode[pc]), concrete=True)
        pc += 1
        if opcode == MOV_A_R:
            n = ord(bytecode[pc]); hint(n, concrete=True)
            pc += 1
            regs[n] = a
        elif opcode == MOV_R_A:
            n = ord(bytecode[pc]); hint(n, concrete=True)
            pc += 1
            a = regs[n]
        elif opcode == JUMP_IF_A:
            target = ord(bytecode[pc])
            pc += 1
            if a:
                pc = target
        elif opcode == SET_A:
            a = ord(bytecode[pc]); hint(a, concrete=True)
            pc += 1
        elif opcode == ADD_R_TO_A:
            n = ord(bytecode[pc]); hint(n, concrete=True)
            pc += 1
            a += regs[n]
        elif opcode == RETURN_A:
            return a
        elif opcode == ALLOCATE:
            n = ord(bytecode[pc]); hint(n, concrete=True)
            pc += 1
            regs = [0] * n
        elif opcode == NEG_A:
            a = -a

# ____________________________________________________________
# example bytecode: compute the square of 'a' >= 1

SQUARE_LIST = [
    ALLOCATE,    3,
    MOV_A_R,     0,   # counter
    MOV_A_R,     1,   # copy of 'a'
    SET_A,       0,
    MOV_A_R,     2,   # accumulator for the result
    # 10:
    SET_A,       1,
    NEG_A,
    ADD_R_TO_A,  0,
    MOV_A_R,     0,
    MOV_R_A,     2,
    ADD_R_TO_A,  1,
    MOV_A_R,     2,
    MOV_R_A,     0,
    JUMP_IF_A,  10,

    MOV_R_A,     2,
    RETURN_A ]

SQUARE = ''.join([chr(n) for n in SQUARE_LIST])

if __name__ == '__main__':
    print ','.join([str(n) for n in SQUARE_LIST])
