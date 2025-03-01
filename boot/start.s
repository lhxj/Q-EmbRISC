    .section .text                  //定义数据段名为.text
    .globl _start                   //定义全局符号_star
    .type _start,@function          //_star为函数

_start:                             //函数入口
    csrr    a0, mhartid             //csr是riscv专有的内核私有寄存器，独立编址在12位地址
                                    //mhartid寄存器是定义了内核的hard_id,这里将hard_id读取到a0

    li      t0, 0X0                 //li是伪指令，将立即数0X0加载到t0
    beq     a0, t0, _core0          //比较a0和t0是否相等，若相等则跳转到_core0，否则向下执行
_loop:                              //跳转到_loop，此处形成循环，用意为如果当前cpu core不为
        j   _loop                   //hart 0则循环等待，为hart 0则继续向下执行

_core0:                             //定义一个core0才能执行到此处
    li      t0, 0X100               //t0 = 0x100
    slli    t0, t0, 20              //t0左移20位 t0 = 0x10000000
    li      t1, 'H'                 //t1 = 'H' 字符的ASCII码值写入t1
    sb      t1, 0(t0)               //s是store写入的意思，b是byte，这里指的是写入t1
                                    //的值到t0指向的地址，即为写入0x10000000这个寄存器
                                    //这个寄存器正是uart0的发送data寄存器，此时串口会输出"H"
    li      t1, 'e'
    sb      t1, 0(t0)
    li      t1, 'l'
    sb      t1, 0(t0)
    li      t1, 'l'
    sb      t1, 0(t0)
    li      t1, 'o'
    sb      t1, 0(t0)
    li      t1, ','
    sb      t1, 0(t0)
    li      t1, ' '
    sb      t1, 0(t0)
    li      t1, 'M'
    sb      t1, 0(t0)
    li      t1, 'y'
    sb      t1, 0(t0)
    li      t1, ' '
    sb      t1, 0(t0)
    li      t1, 'L'
    sb      t1, 0(t0)
    li      t1, 'o'
    sb      t1, 0(t0)
    li      t1, 'v'
    sb      t1, 0(t0)
    li      t1, 'e'
    sb      t1, 0(t0)
    li      t1, 'r'
    sb      t1, 0(t0)
    j       _loop

.end