#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#define likely(x) (__builtin_expect(!!(x), 1))
#define unlikely(x) (__builtin_expect(!!(x), 0))
#define set_bit(var, mask, cond) ((cond) ? ((var) |= (mask)) : ((var) &= ~(mask)))

constexpr uint64_t TIMEBASE_FREQ = 10'000'000;   // 10MHz
constexpr uint32_t RAM_BASE  = 0x80000000;
constexpr uint32_t RAM_SIZE  = 32 * 1024 * 1024; // RAM: 32MB
constexpr uint32_t PLIC_BASE = 0x0c000000;
constexpr uint32_t UART_BASE = 0x10000000;
constexpr uint32_t UART_SIZE = 0x00000100;
constexpr uint32_t TLB_SIZE  = 64;
constexpr uint32_t TLB_MASK  = 0b0011'1111;

enum : uint32_t {
    PRIV_U = 0, // Privilege of User
    PRIV_S = 1, // Privilege of Supervisor

    ACCESS_FETCH = 0,
    ACCESS_LOAD  = 1,
    ACCESS_STORE = 2,

    CSR_SSTATUS  = 0x100, // Supervisor Status
    CSR_SIE      = 0x104, // Supervisor Interrupt Enable
    CSR_STVEC    = 0x105, // Supervisor Trap Vector
    CSR_SSCRATCH = 0x140, // Supervisor Scratch
    CSR_SEPC     = 0x141, // Supervisor Exception PC
    CSR_SCAUSE   = 0x142, // Supervisor Trap Cause
    CSR_STVAL    = 0x143, // Supervisor Trap Value
    CSR_SIP      = 0x144, // Supervisor Interrupt Pending
    CSR_SATP     = 0x180, // Supervisor Address Translation and Protection

    CAUSE_FETCH_ACCESS     = 1,
    CAUSE_ILLEGAL_INSN     = 2,
    CAUSE_BREAKPOINT       = 3,
    CAUSE_ECALL_U          = 8,
    CAUSE_FETCH_PAGE_FAULT = 12,
    CAUSE_LOAD_PAGE_FAULT  = 13,
    CAUSE_STORE_PAGE_FAULT = 15,

    CAUSE_IRQ_S_TIMER = 0x80000005,
    CAUSE_IRQ_S_EXT   = 0x80000009,

    INT_STIP = 1 << 5, // Interrupt of Timer
    INT_SEIP = 1 << 9, // Interrupt of External Interrupt Pending

    UART_REG_TXDATA = 0,
    UART_REG_RXDATA = 1,
    UART_REG_IRQ_ENABLE = 4,
    UART_REG_IRQ_STATUS = 5,
};


typedef struct {
    uint32_t :1, interrupt_enable: 1;
    uint32_t :3, previous_interrupt_enable: 1;
    uint32_t :2, previous_priv: 1;
} csr_sstatus_t;

typedef struct {
    uint32_t valid: 1, readable: 1, writable: 1, executable: 1;
    uint32_t user: 1, global: 1, accessed: 1, dirty: 1;
    uint32_t rsw: 2, ppn: 22;
} page_table_entry_t;


static struct {
    uint32_t regs[32];
    uint32_t pc;
    uint32_t priv; // Current Privilege Level

    uint64_t mtime;
    uint64_t timer_interrupt_mtime;
    uint32_t lr_addr;

    uint32_t csr[4096]; // Control and Status Register
    alignas(4096)
    uint8_t ram[RAM_SIZE];
} CPU;


static uint64_t get_current_mtime() {
    struct timespec ts = {};
    timespec_get(&ts, TIME_MONOTONIC);
    return (uint64_t)ts.tv_sec * TIMEBASE_FREQ + (uint64_t)ts.tv_nsec * TIMEBASE_FREQ / 1'000'000'000ull;
}

static struct termios origin_termios;

static void disable_raw_mode() {
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origin_termios);
}

static void enable_raw_mode() {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &origin_termios);
    atexit(disable_raw_mode);

    struct termios raw = origin_termios;
    set_bit(raw.c_iflag, BRKINT | ICRNL | INPCK | ISTRIP | IXON, false);
    set_bit(raw.c_lflag, ECHO | ICANON | IEXTEN | ISIG, false);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


// SiFive UART
static struct {
    uint32_t regs[7];
    uint8_t buf_head, buf_tail;
    uint8_t receive_buf[256];
} UART;

static bool uart_buf_empty() {
    return UART.buf_head == UART.buf_tail;
}

static void uart_push(uint8_t ch) {
    UART.receive_buf[UART.buf_tail++] = ch;
}

static uint8_t uart_pop() {
    return UART.receive_buf[UART.buf_head++];
}

static void poll_uart_input(int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0) return;
    
    uint8_t ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == (']' & 0b1'1111)) exit(0); // Ctrl + ]
        uart_push(ch);
    }
}

static uint32_t uart_irq_status() {
    return !uart_buf_empty() << 1 | 0b01;
}

static bool uart_irq_raised() {
    return (uart_irq_status() & UART.regs[UART_REG_IRQ_ENABLE]) != 0;
}

static uint32_t uart_read(uint32_t offset) {
    uint32_t reg = offset >> 2;
    if (reg == UART_REG_TXDATA) return 0;
    if (reg == UART_REG_RXDATA) return !uart_buf_empty() ? uart_pop() : 1u << 31;
    if (reg == UART_REG_IRQ_STATUS) return uart_irq_status();
    if (reg < 7) return UART.regs[reg];
    return 0;
}

static void uart_write(uint32_t offset, uint32_t val) {
    uint32_t reg = offset >> 2;
    if (reg == UART_REG_TXDATA) {
        putchar(val & 0xff);
        fflush(stdout);
    } else if (reg < 7) {
        UART.regs[reg] = val;
    }
}


static jmp_buf trap_jmp_buf;

static void take_trap(uint32_t cause, uint32_t value) {
    CPU.csr[CSR_SEPC] = CPU.pc;
    CPU.csr[CSR_SCAUSE] = cause;
    CPU.csr[CSR_STVAL] = value;

    auto sstatus = (csr_sstatus_t*)&CPU.csr[CSR_SSTATUS];
    sstatus->previous_interrupt_enable = sstatus->interrupt_enable;
    sstatus->interrupt_enable = false;
    sstatus->previous_priv = CPU.priv;

    CPU.priv = PRIV_S;
    CPU.lr_addr = -1u;
    CPU.pc = CPU.csr[CSR_STVEC] & ~0b11;
}

[[noreturn]] static void trap_throw(uint32_t cause, uint32_t value) {
    take_trap(cause, value);
    longjmp(trap_jmp_buf, 1);
}


static struct {
    uint32_t virtual_page;
    uint32_t physical_page;
} TLB[2][3][TLB_SIZE]; // [priv: 0=User, 1=Super][access_type: 0=Fetch, 1=Load, 2=Store][cache_line]

static void tlb_flush() {
    memset(TLB, 0xff, sizeof(TLB));
}


static bool check_pte_permission(page_table_entry_t pte, uint32_t access_type) {
    if (!pte.valid) return false;
    if (CPU.priv == PRIV_U && !pte.user) return false;

    if (access_type == ACCESS_FETCH && !pte.executable) return false;
    if (access_type == ACCESS_STORE && !pte.writable) return false;
    if (access_type == ACCESS_LOAD && !pte.readable) return false;

    return true;
}

static uint32_t translate(uint32_t virtual_addr, uint32_t access_type) {
    struct satp_t {
        uint32_t ppn: 22, asid: 9, is_sv32_mode: 1;
    } satp;

    satp = *(struct satp_t*)&CPU.csr[CSR_SATP];
    if (!satp.is_sv32_mode)
        return virtual_addr;

    uint32_t vpage = virtual_addr >> 12;    // vaddr[31:12](20)
    uint32_t offset = virtual_addr & 0xfff; // vaddr[11:0](12)

    auto tlb_entry = &TLB[CPU.priv][access_type][vpage & TLB_MASK];
    if (tlb_entry->virtual_page == vpage)
        return (tlb_entry->physical_page << 12) | offset;

    uint32_t vpage1 = vpage >> 10;   // vaddr[31:22](10)
    uint32_t vpage2 = vpage & 0x3ff; // vaddr[21:12](10)

    uint32_t vtable_addr = satp.ppn << 12;
    auto pte = *(page_table_entry_t*)&CPU.ram[vtable_addr + vpage1 * 4 - RAM_BASE];

    uint32_t physical_page;

    if (pte.valid && !pte.readable && !pte.writable && !pte.executable) { // 4KB Page (2 level)
        uint32_t next_vtable_addr = pte.ppn << 12;
        pte = *(page_table_entry_t*)&CPU.ram[next_vtable_addr + vpage2 * 4 - RAM_BASE];
        physical_page = pte.ppn;
    }
    else physical_page = pte.ppn | vpage2; // 4MB Page (1 level)

    if (!check_pte_permission(pte, access_type))
        switch (access_type) {
            case ACCESS_FETCH: trap_throw(CAUSE_FETCH_PAGE_FAULT, virtual_addr);
            case ACCESS_LOAD: trap_throw(CAUSE_LOAD_PAGE_FAULT, virtual_addr);
            case ACCESS_STORE: default: trap_throw(CAUSE_STORE_PAGE_FAULT, virtual_addr);
        }

    tlb_entry->virtual_page = vpage;
    tlb_entry->physical_page = physical_page;
    return (physical_page << 12) | offset;
}


static uint32_t vmem_read(uint32_t vaddr, uint32_t size, uint32_t access_type) {
    uint32_t paddr = translate(vaddr, access_type);

    if likely(paddr - RAM_BASE <= RAM_SIZE - size) {
        const uint8_t* p = &CPU.ram[paddr - RAM_BASE];
        switch (size) {
            case 4: return *(uint32_t*)p;
            case 2: return *(uint16_t*)p;
            case 1: return *p;
        }
    }

    else if (paddr >= UART_BASE && paddr < UART_BASE + UART_SIZE)
        return uart_read(paddr - UART_BASE);

    if (paddr == PLIC_BASE + 0x200004 || paddr == PLIC_BASE + 0x201004) // Hart 0 S-mode or Hart 0 M-mode
        return uart_irq_raised();

    return 0;
}


static void vmem_write(uint32_t vaddr, uint32_t size, uint32_t val) {
    uint32_t paddr = translate(vaddr, ACCESS_STORE);

    if likely(paddr - RAM_BASE <= RAM_SIZE - size) {
        uint8_t* p = &CPU.ram[paddr - RAM_BASE];
        switch (size) {
            case 4: *(uint32_t*)p = val; break;
            case 2: *(uint16_t*)p = val; break;
            case 1: *p = val; break;
        }
    }

    else if (paddr >= UART_BASE && paddr < UART_BASE + UART_SIZE)
        uart_write(paddr - UART_BASE, val);
}


static void check_interrupts() {
    set_bit(CPU.csr[CSR_SIP], INT_STIP, CPU.mtime >= CPU.timer_interrupt_mtime);
    set_bit(CPU.csr[CSR_SIP], INT_SEIP, uart_irq_raised());

    auto sstatus = (csr_sstatus_t*)&CPU.csr[CSR_SSTATUS];
    if (CPU.priv == PRIV_S && !sstatus->interrupt_enable) return;

    uint32_t irq_flags = CPU.csr[CSR_SIP] & CPU.csr[CSR_SIE];
    if (irq_flags & INT_SEIP) take_trap(CAUSE_IRQ_S_EXT, 0);
    else if (irq_flags & INT_STIP) take_trap(CAUSE_IRQ_S_TIMER, 0);
}


static void handle_sbi() {
    uint32_t eid = CPU.regs[17], fid = CPU.regs[16];
    uint32_t a0 = CPU.regs[10], a1 = CPU.regs[11];
    int32_t err = 0;
    uint32_t ret = 0;

    switch (eid) {
        case 0x54494D45: // TIME
            CPU.timer_interrupt_mtime = ((uint64_t)a1 << 32) | a0;
            break;

        case 0x8: // Legacy Shutdown
        case 0x53525354: // SRST
            exit(0);

        case 0x10: // Base
            if (fid == 0) ret = 3; // SBI version: 0.3
            else if (fid < 3) ret = 1;
            else if (fid == 3)
                ret = a0 == 0x8 || a0 == 0x10 || a0 == 0x54494D45 || a0 == 0x53525354;
            break;

        default:
            err = -2;
            break;
    }

    CPU.regs[10] = err;
    CPU.regs[11] = ret;
}


static uint32_t csr_read(uint32_t csr) {
    if (csr >= 0xc00 && csr <= 0xc02) return (uint32_t)CPU.mtime;
    if (csr >= 0xc80 && csr <= 0xc82) return CPU.mtime >> 32;
    return CPU.csr[csr];
}

static void csr_write(uint32_t csr, uint32_t val) {
    if (csr == CSR_SATP) tlb_flush();
    CPU.csr[csr] = val;
}

static void handle_system_csr_ins(uint32_t ins, uint32_t rd, uint32_t funct3, uint32_t rs1) {
    if (funct3 == 0) { // SYSTEM
        uint32_t funct12 = ins >> 20;
        switch (funct12) {
            case 0: // ECALL
                if (CPU.priv == PRIV_U) trap_throw(CAUSE_ECALL_U, 0);
                if (CPU.priv == PRIV_S) handle_sbi();
                break;

            case 1: // EBREAK
                trap_throw(CAUSE_BREAKPOINT, CPU.pc);

            case 0x102: { // SRET
                auto sstatus = (csr_sstatus_t*)&CPU.csr[CSR_SSTATUS];
                CPU.priv = sstatus->previous_priv;
                sstatus->interrupt_enable = sstatus->previous_interrupt_enable;
                sstatus->previous_priv = PRIV_U;
                sstatus->previous_interrupt_enable = true;

                CPU.pc = CPU.csr[CSR_SEPC];
                return;
            }

            case 0x105: // WFI (Waiting For Interrupt)
                int timeout_ms;
                if (CPU.timer_interrupt_mtime == UINT64_MAX) timeout_ms = -1;
                else if (CPU.timer_interrupt_mtime <= CPU.mtime) timeout_ms = 0;
                else timeout_ms = (CPU.timer_interrupt_mtime - CPU.mtime) * 1000 / TIMEBASE_FREQ + 1;

                poll_uart_input(uart_buf_empty() ? timeout_ms : 0);
                CPU.mtime = get_current_mtime();
                break;

            default:
                if (funct12 >> 5 == 0x09) // SFENCE.VMA
                    tlb_flush();
                break;
        }
    }

    else { // CSR
        uint32_t csr = ins >> 20;
        uint32_t old_val = csr_read(csr);
        uint32_t src = funct3 & 0b100 ? rs1 : CPU.regs[rs1];

        switch (funct3 & 0b011) {
            case 1: csr_write(csr, src); break; // CSRRW / CSRRWI
            case 2: csr_write(csr, old_val | src); break;  // CSRRS / CSRRSI
            case 3: csr_write(csr, old_val & ~src); break; // CSRRC / CSRRCI
        }

        CPU.regs[rd] = old_val;
    }

    CPU.pc += 4;
    check_interrupts();
}


static void cpu_step() {
    CPU.regs[0] = 0;
    uint32_t ins = vmem_read(CPU.pc, 4, ACCESS_FETCH);
    
    uint32_t opcode = ins & 0b111'1111;         // bits [6:0]   (7 bits)
    uint32_t rd     = (ins >> 7) & 0b1'1111;    // bits [11:7]  (5 bits)
    uint32_t funct3 = (ins >> 12) & 0b111;      // bits [14:12] (3 bits)
    uint32_t rs1    = (ins >> 15) & 0b1'1111;   // bits [19:15] (5 bits)
    uint32_t rs2    = (ins >> 20) & 0b1'1111;   // bits [24:20] (5 bits)

    switch (opcode) {
        case 0x37: { // LUI
            uint32_t imm_u = ins & 0xfffff000;
            CPU.regs[rd] = imm_u;
            CPU.pc += 4;
            break;
        }

        case 0x17: { // AUIPC
            uint32_t imm_u = ins & 0xfffff000;
            CPU.regs[rd] = CPU.pc + imm_u;
            CPU.pc += 4;
            break;
        }

        case 0x6f: { // JAL
            // imm_j = ins[31](1) | ins[19:12](8) | ins[20](1) | ins[30:21](10) | 0(1)
            int32_t imm_j = ((int32_t)(ins & 0x80000000) >> 11) | (ins & 0xff000) | ((ins >> 9) & 0x800) | ((ins >> 20) & 0x7fe);
            CPU.regs[rd] = CPU.pc + 4;
            CPU.pc += imm_j;
            break;
        }

        case 0x67: { // JALR
            int32_t imm_i = (int32_t)ins >> 20;
            uint32_t target = (CPU.regs[rs1] + imm_i) & ~1u;
            CPU.regs[rd] = CPU.pc + 4;
            CPU.pc = target;
            break;
        }

        case 0x63: { // BRANCH
            // imm_b = ins[31](1) | ins[7](1) | ins[30:25](6) | ins[11:8](4) | 0(1)
            int32_t imm_b = (int32_t)((ins & 0x80000000) | ((ins & 0x7e000000) >> 1) | ((ins & 0x80) << 23) | ((ins & 0xf00) << 12)) >> 19;
            uint32_t r1 = CPU.regs[rs1];
            uint32_t r2 = CPU.regs[rs2];

            bool cond = false;
            switch (funct3) {
                case 0: cond = r1 == r2; break;
                case 1: cond = r1 != r2; break;
                case 4: cond = (int32_t)r1 < (int32_t)r2; break;
                case 5: cond = (int32_t)r1 >= (int32_t)r2; break;
                case 6: cond = r1 < r2; break;
                case 7: cond = r1 >= r2; break;
            }

            CPU.pc += cond ? imm_b : 4;
            break;
        }

        case 0x03: { // LOAD
            int32_t imm_i = (int32_t)ins >> 20;
            int size = 1 << (funct3 & 0b11);

            uint32_t val = vmem_read(CPU.regs[rs1] + imm_i, size, ACCESS_LOAD);
            if (funct3 == 0) val = (int32_t)(int8_t)val;
            else if (funct3 == 1) val = (int32_t)(int16_t)val;

            CPU.regs[rd] = val;
            CPU.pc += 4;
            break;
        }

        case 0x23: { // STORE
            // imm_s = ins[31:25](7) | ins[11:7](5)
            int32_t imm_s = ((int32_t)(ins & 0xfe000000) >> 20) | (ins >> 7 & 0x1f);
            int size = 1 << (funct3 & 0b11);
            vmem_write(CPU.regs[rs1] + imm_s, size, CPU.regs[rs2]);
            CPU.pc += 4;
            break;
        }

        case 0x13: { // OP-IMM
            uint32_t r1 = CPU.regs[rs1];
            uint32_t imm = (int32_t)ins >> 20;
            uint32_t shamt = imm & 0b1'1111;
            uint32_t alt = ins & 0x40000000;

            uint32_t res = 0;
            switch (funct3) {
                case 0: res = r1 + imm; break;
                case 1: res = r1 << shamt; break;
                case 2: res = (int32_t)r1 < (int32_t)imm; break;
                case 3: res = r1 < imm; break;
                case 4: res = r1 ^ imm; break;
                case 5: res = alt ? (int32_t)r1 >> shamt : r1 >> shamt; break;
                case 6: res = r1 | imm; break;
                case 7: res = r1 & imm; break;
            }

            CPU.regs[rd] = res;
            CPU.pc += 4;
            break;
        }

        case 0x33: { // OP
            uint32_t r1 = CPU.regs[rs1];
            uint32_t r2 = CPU.regs[rs2];
            uint32_t shamt = r2 & 0b1'1111;
            uint32_t alt = ins & 0x40000000;

            uint32_t res = 0;
            if (ins & 0x2000000) // RV32M
                switch (funct3) {
                    case 0: res = r1 * r2; break;
                    case 1: res = (int64_t)(int32_t)r1 * (int32_t)r2 >> 32; break;
                    case 2: res = (int64_t)(int32_t)r1 * r2 >> 32; break;
                    case 3: res = (uint64_t)r1 * r2 >> 32; break;
                    case 4: res = r2 == 0 ? ~0 : ((int32_t)r1 == INT32_MIN && r2 == -1u) ? r1 : (int32_t)r1 / (int32_t)r2; break;
                    case 5: res = r2 == 0 ? ~0 : r1 / r2; break;
                    case 6: res = r2 == 0 ? r1 : ((int32_t)r1 == INT32_MIN && r2 == -1u) ? 0 : (int32_t)r1 % (int32_t)r2; break;
                    case 7: res = r2 == 0 ? r1 : r1 % r2; break;
                }
            else // ALU
                switch (funct3) {
                    case 0: res = alt ? r1 - r2 : r1 + r2; break;
                    case 1: res = r1 << shamt; break;
                    case 2: res = (int32_t)r1 < (int32_t)r2; break;
                    case 3: res = r1 < r2; break;
                    case 4: res = r1 ^ r2; break;
                    case 5: res = alt ? (int32_t)r1 >> shamt : r1 >> shamt; break;
                    case 6: res = r1 | r2; break;
                    case 7: res = r1 & r2; break;
                }

            CPU.regs[rd] = res;
            CPU.pc += 4;
            break;
        }

        case 0x0f: // FENCE / FENCE.I
            CPU.pc += 4;
            break;

        case 0x2f: { // RV32A
            uint32_t r1 = CPU.regs[rs1];
            uint32_t r2 = CPU.regs[rs2];
            uint32_t funct5 = (ins >> 27) & 0b11111;

            if (funct5 == 0x02) { // LR.W
                CPU.regs[rd] = vmem_read(r1, 4, ACCESS_LOAD);
                CPU.lr_addr = r1;
            }
            
            else if (funct5 == 0x03) { // SC.W
                bool ok = CPU.lr_addr == r1;
                if (ok) vmem_write(r1, 4, r2);
                CPU.regs[rd] = !ok;
                CPU.lr_addr = -1u;
            }

            else { // AMO*.W
                uint32_t old_val = vmem_read(r1, 4, ACCESS_STORE);
                uint32_t new_val = old_val;
                switch (funct5) {
                    case 0x00: new_val = old_val + r2; break;
                    case 0x01: new_val = r2; break;
                    case 0x04: new_val = old_val ^ r2; break;
                    case 0x08: new_val = old_val | r2; break;
                    case 0x0c: new_val = old_val & r2; break;
                    case 0x10: new_val = (int32_t)old_val < (int32_t)r2 ? old_val : r2; break;
                    case 0x14: new_val = (int32_t)old_val > (int32_t)r2 ? old_val : r2; break;
                    case 0x18: new_val = old_val < r2 ? old_val : r2; break;
                    case 0x1c: new_val = old_val > r2 ? old_val : r2; break;
                }
                vmem_write(r1, 4, new_val);
                CPU.regs[rd] = old_val;
            }

            CPU.pc += 4;
            break;
        }

        case 0x73: // SYSTEM / CSR
            handle_system_csr_ins(ins, rd, funct3, rs1);
            break;
    }
}


static bool load_file(const char *path, uint32_t offset) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(CPU.ram + offset, 1, RAM_SIZE - offset, f);
    fclose(f);
    return n > 0;
}


int main(int argc, char* argv[]) {
    if (!load_file("linux.image", 0) || !load_file("device_tree.dtb", RAM_SIZE - 0x100000))
        return fprintf(stderr, "Unable to load the kernel image or device tree file(.dtb).\n");

    enable_raw_mode();
    CPU.priv = PRIV_S;
    CPU.pc = RAM_BASE;
    CPU.mtime = 0;
    CPU.timer_interrupt_mtime = UINT64_MAX;
    CPU.lr_addr = -1u;
    CPU.regs[10] = 0;
    CPU.regs[11] = RAM_BASE + RAM_SIZE - 0x100000;
    tlb_flush();

    while (true) {
        volatile int steps = 8192;
        setjmp(trap_jmp_buf);
        
        while (steps --> 0)
            cpu_step();

        CPU.mtime = get_current_mtime();
        poll_uart_input(0);
        check_interrupts();
    }
}
