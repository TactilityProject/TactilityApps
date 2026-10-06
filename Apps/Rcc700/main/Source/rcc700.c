// rcc700.c: A self-hosting mini C compiler for RISC-V / RV32
// Tested on ESP32-P4 with ESP-IDF 5.5 elf_loader.
// Ported from my earlier xcc700 Xtensa variant.

// --- Libc Shims ---
int printf(char *fmt, ...); void exit(int status);
void *malloc(int size); void free(void *ptr);
void *memcpy(void *dest, void *src, int n); void *memset(void *s, int c, int n);
int strcmp(char *s1, char *s2); char *strcpy(char *dest, char *src);
int strlen(char *s); int strtol(char *nptr, char **endptr, int base);
int open(char *pathname, int flags, int mode); int close(int fd);
int read(int fd, void *buf, int count); int write(int fd, void *buf, int count);
int lseek(int fd, int offset, int whence);

// --- Constants & Globals ---
enum {
    // Capacities sized so rcc700 can compile its own source (every global
    // access, call and string is one pool patch; every declaration one name).
    MAX_FUNCS=128, MAX_VARS=256, MAX_LITS=512, MAX_PATCHES=1200, NAMES=8192,
    MAXFRAME=2000, MAX_EPI=64, // frame cap (sp adjust + offsets must fit 12 bits); max returns/func
    EHDR_SZ=52, PHDR_SZ=32, N_PHDRS=1, N_SECS=7, SHDR_SZ=280, // N_SECS * 40
    R_RISCV_RELATIVE=3, R_RISCV_JUMP_SLOT=5,
    ZERO=0, RA=1, SP=2, T0=5, A0=10, A1=11, A2=12, A7=17,
    L_FUNC=0, L_STR=1, L_BSS=2,
    // open() flags are newlib's (the libc on both the device and in elfrun)
    O_RDONLY=0, O_WRONLY=1, O_CREAT=512, O_TRUNC=1024, SEEK_SET=0, SEEK_END=2
};

enum {
    T_EOF=0, T_INT=256, T_CHAR, T_VOID, T_IDENT, T_NUM, T_STR, T_RETURN,
    T_EQ, T_NE, T_LE, T_GE, T_IF, T_ELSE, T_WHILE, T_ENUM, T_ELLIPSIS,
    T_SHL, T_SHR, T_LAND, T_LOR, T_INC, T_DEC
};

// Variable/expression types: TY_* values, TF_* the bits they are made of.
enum {
    TF_BYTE=1, TF_PTR=2, TF_ARR=4, TF_GLOBAL=8, TF_CONST=16,
    TY_INT=0, TY_BYTE=1, TY_INTPTR=2, TY_BYTEPTR=3, TY_INTARR=4, TY_BYTEARR=5
};

char *src;
int token; int num_val; int line; int token_cnt;
char str_val[256]; int str_len;
char *code_data; int code_size; int code_cap;
char *rodata; int rodata_sz; int rodata_cap;
char name_buf[NAMES]; int name_sz;

int func_name_off[MAX_FUNCS]; int func_addrs[MAX_FUNCS]; int n_funcs;
int var_name_off[MAX_VARS]; int var_offsets[MAX_VARS]; int var_types[MAX_VARS];
int n_vars; // globals + locals of the current function
int n_globals; int bss_size; // globals (incl. enum constants) / .bss bytes
int locals; int esp; // bytes of locals so far / expression stack depth, both sp-relative
int expr_type; // TY_* of the last parsed factor/expression
int lit_vals[MAX_LITS]; int lit_types[MAX_LITS]; int n_lits;
int patch_offs[MAX_PATCHES]; int patch_lits[MAX_PATCHES]; int n_patches;
// Per-function frame sizing: high-water mark of (locals+spills), the prologue's
// sp-adjust offset, and every epilogue's sp-adjust offset (all back-patched once
// the body is parsed and the real frame size is known).
int frame_max; int prologue_off; int n_epi; int epi_offs[MAX_EPI];

// Must put main() first. Forward-declare the functions it uses.
void next(); void parse_func(); void write_elf(char *out);
void print_stats(char *outfile);

int main(int argc, char **argv) {
    line = 1;
    char *input_fname = argc > 1 ? argv[1] : "input.c";
    char *output_fname = "a.out";
    if (argc > 3) if (!strcmp(argv[2], "-o")) output_fname = argv[3];  // double if: no short-circuit && in self-hosted
    int f = open(input_fname, O_RDONLY, 0);
    if (f < 0) { printf("rcc700: cannot open input file: %s\n", input_fname); return 1; }
    int f_size = lseek(f, 0, SEEK_END); lseek(f, 0, SEEK_SET);
    char *src_buf = malloc(f_size+1); src = src_buf;
    code_cap = 262144; code_data = malloc(code_cap);
    rodata_cap = 8192; rodata = malloc(rodata_cap);
    if (!src_buf || !code_data || !rodata) { printf("rcc700: out of memory\n"); return 1; }
    read(f, src, f_size); src[f_size] = 0; close(f);
    next(); while (token != T_EOF) parse_func();
    free(src_buf); write_elf(output_fname);
    print_stats(output_fname);
    free(code_data); free(rodata);
    return 0;
}

// Replace <ctype.h> macros to avoid ABI issues
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int isalnum(int c) { return isdigit(c) || isalpha(c); }

// Helpers
void put32(char *b, int v) { b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
void put16(char *b, int v) { b[0]=v; b[1]=v>>8; }
int align4(int x) { return (x + 3) & ~3; }

// Not-yet-ported features bail out with exit code 42 -> run_tests.sh reports SKIP.
void unsupported(char *what) {
    printf("rcc700: line %d: not implemented: %s (token %d)\n", line, what, token);
    exit(42);
}

// --- Lexer ---
void next() {
    while (isspace(*src) || (src[0]=='/' && src[1]=='/')) {
        if (*src == '\n') ++line;
        if (*src == '/') while (*src && *src != '\n') ++src; else ++src;
    }
    if (!*src) { token = T_EOF; return; }
    if (src[0]=='+' && src[1]=='+') { token=T_INC; src=src+2; return; }
    if (src[0]=='-' && src[1]=='-') { token=T_DEC; src=src+2; return; }
    if (src[0]=='=' && src[1]=='=') { token=T_EQ; src=src+2; return; }
    if (src[0]=='!' && src[1]=='=') { token=T_NE; src=src+2; return; }
    if (src[0]=='<' && src[1]=='=') { token=T_LE; src=src+2; return; }
    if (src[0]=='>' && src[1]=='=') { token=T_GE; src=src+2; return; }
    if (src[0]=='<' && src[1]=='<') { token=T_SHL; src=src+2; return; }
    if (src[0]=='>' && src[1]=='>') { token=T_SHR; src=src+2; return; }
    if (src[0]=='&' && src[1]=='&') { token=T_LAND; src=src+2; return; }
    if (src[0]=='|' && src[1]=='|') { token=T_LOR; src=src+2; return; }
    if (src[0]=='.' && src[1]=='.' && src[2]=='.') { token=T_ELLIPSIS; src=src+3; return; }
    if (isalpha(*src) || *src == '_') {
        char *p = str_val;
        while (isalnum(*src) || *src == '_') { *p = *src; ++p; ++src; }
        *p = 0;
        token = !strcmp(str_val,"int") ? T_INT : !strcmp(str_val,"char") ? T_CHAR :
            !strcmp(str_val,"void") ? T_VOID : !strcmp(str_val,"if") ? T_IF :
            !strcmp(str_val,"else") ? T_ELSE : !strcmp(str_val,"while") ? T_WHILE :
            !strcmp(str_val,"enum") ? T_ENUM :
            !strcmp(str_val,"return") ? T_RETURN : T_IDENT;
    } else if (isdigit(*src)) {
        num_val = strtol(src, &src, 0); token = T_NUM;
    } else if (*src == '\'') {
        ++src;
        if (*src == '\\') {
            ++src;
            if (*src == 'n') num_val = '\n'; else if (*src == 't') num_val = '\t';
            else if (*src == 'r') num_val = '\r'; else if (*src == '0') num_val = 0; else num_val = *src;
        } else num_val = *src;
        ++src; if (*src == '\'') ++src;
        token = T_NUM;
    } else if (*src == '"') {
        char *p = str_val; ++src;
        while (*src && *src != '"') {
            if (*src == '\\') {
                ++src;
                if (*src == 'n') *p='\n'; else if (*src == 't') *p='\t';
                else if (*src == 'r') *p='\r'; else if (*src == '0') *p='\0'; else *p=*src;
            } else *p=*src;
            ++src; ++p;
        }
        *p = 0; if (*src) ++src;
        str_len = p - str_val;
        token = T_STR;
    } else { token = *src; ++src; }
    ++token_cnt;
}

void expect(int tok) {
    if (token != tok) {
        printf("rcc700: line %d: expected token %d, got %d (%s)\n", line, tok, token, str_val);
        exit(1);
    }
    next();
}

// --- Code Emitter (RV32I) ---
void emit32(int v) {
    if (code_size + 4 > code_cap) { printf("rcc700: out of memory (code)\n"); exit(1); }
    put32(code_data + code_size, v); code_size = code_size + 4;
}
void emit16(int v) {
    if (code_size + 2 > code_cap) { printf("rcc700: out of memory (code)\n"); exit(1); }
    put16(code_data + code_size, v); code_size = code_size + 2;
}
void emit_r(int f7, int rs2, int rs1, int f3, int rd, int op) {
    emit32((f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op);
}
void emit_i(int imm, int rs1, int f3, int rd, int op) {
    emit32(((imm&0xfff)<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op);
}
int enc_j(int off, int rd) { // J-type: imm[20|10:1|11|19:12]
    return (((off>>20)&1)<<31)|(((off>>1)&0x3ff)<<21)|(((off>>11)&1)<<20)|(((off>>12)&0xff)<<12)|(rd<<7)|0x6f;
}
int enc_b(int off, int rs2, int rs1, int f3) { // B-type: imm[12|10:5] ... imm[4:1|11]
    return (((off>>12)&1)<<31)|(((off>>5)&0x3f)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(((off>>1)&0xf)<<8)|(((off>>11)&1)<<7)|0x63;
}
void emit_addi(int rd, int rs1, int imm) { emit_i(imm, rs1, 0, rd, 0x13); }
// A word access fits c.lwsp/c.swsp when it is sp-relative with off 0..252, /4
// (rd is always a real reg here, so c.lwsp's rd != x0 holds for free).
int sp_word(int base, int off) { return base == SP && (off & 3) == 0 && off >= 0 && off < 256; }
void emit_lw(int rd, int base, int off) {
    if (sp_word(base, off)) emit16(0x4002 | (rd<<7) | (((off>>5)&1)<<12) | (((off>>2)&7)<<4) | (((off>>6)&3)<<2)); // c.lwsp
    else emit_i(off, base, 2, rd, 3);
}
void emit_lbu(int rd, int base, int off) { emit_i(off, base, 4, rd, 3); }
void emit_s(int rs2, int base, int off, int f3) {
    emit32((((off>>5)&0x7f)<<25)|(rs2<<20)|(base<<15)|(f3<<12)|((off&0x1f)<<7)|0x23);
}
void emit_sw(int rs2, int base, int off) {
    if (sp_word(base, off)) emit16(0xC002 | (rs2<<2) | (((off>>2)&0xf)<<9) | (((off>>6)&3)<<7)); // c.swsp
    else emit_s(rs2, base, off, 2);
}
void emit_sb(int rs2, int base, int off) { emit_s(rs2, base, off, 0); }
void emit_lui(int rd, int hi20) { emit32(((hi20&0xfffff)<<12)|(rd<<7)|0x37); }
void emit_li(int rd, int v) {
    if (v >= -2048 && v < 2048) emit_addi(rd, ZERO, v);
    else { int hi = (v + 0x800) >> 12; emit_lui(rd, hi); emit_addi(rd, rd, v - (hi<<12)); }
}
void emit_ecall() { emit32(0x73); }
void emit_ret() { emit_i(0, RA, 0, ZERO, 0x67); } // jalr zero, ra, 0
void emit_beqz(int rs, int off) { emit32(enc_b(off, ZERO, rs, 0)); }
void emit_j(int off) { emit32(enc_j(off, ZERO)); }

// Resolve a forward branch/jump emitted with offset 0: OR the now-known
// offset (current position - branch position) into the instruction.
void patch(int addr, int is_j) {
    int off = code_size - addr; int w = 0;
    memcpy(&w, code_data + addr, 4);
    put32(code_data + addr, w | (is_j ? enc_j(off, ZERO) : enc_b(off, 0, 0, 0)));
}

void patch_frame(int addr, int imm) {
    int w = 0; memcpy(&w, code_data + addr, 4);
    put32(code_data + addr, w | ((imm & 0xfff) << 20));
}

// Spill a0 into the frame above the locals; sp itself never moves inside a
// function body, so local var offsets stay valid.
void push() {
    if (locals + esp >= MAXFRAME) { printf("rcc700: line %d: expression too deep\n", line); exit(1); }
    emit_sw(A0, SP, locals + esp); esp = esp + 4;
    if (locals + esp > frame_max) frame_max = locals + esp;
}
void pop(int r) { esp = esp - 4; emit_lw(r, SP, locals + esp); }

// --- Symbols & patches ---
int add_name(char *s) {
    int len = strlen(s) + 1;
    if (name_sz + len > NAMES) { printf("rcc700: out of memory (names)\n"); exit(1); }
    int off = name_sz; strcpy(name_buf + off, s);
    name_sz = name_sz + len; return off;
}
int get_func(char *name) {
    int i = 0;
    while (i < n_funcs) { if (!strcmp(name_buf + func_name_off[i], name)) return i; ++i; }
    if (n_funcs >= MAX_FUNCS) { printf("rcc700: too many functions\n"); exit(1); }
    func_name_off[n_funcs] = add_name(name);
    func_addrs[n_funcs] = -1;
    ++n_funcs;
    return n_funcs - 1;
}
int get_lit(int val, int type) {
    int i = 0;
    while (i < n_lits && (lit_vals[i] != val || lit_types[i] != type)) ++i;
    if (i == n_lits) {
        if (n_lits >= MAX_LITS) { printf("rcc700: too many literals\n"); exit(1); }
        lit_vals[i] = val; lit_types[i] = type; ++n_lits;
    }
    return i;
}

// rd = pool[lit], loaded pc-relative: auipc rd, hi; lw rd, lo(rd).
// Immediates stay 0 here; write_elf patches them once the pool size (and so
// the slot's distance from the auipc) is known.
void emit_pool_lw(int rd, int lit) {
    if (n_patches >= MAX_PATCHES) { printf("rcc700: too many patches\n"); exit(1); }
    patch_offs[n_patches] = code_size; patch_lits[n_patches] = lit; ++n_patches;
    emit32((rd << 7) | 0x17); // auipc rd, 0
    emit_lw(rd, rd, 0);       // lw rd, 0(rd)
}

void emit_call(char *name) {
    emit_pool_lw(T0, get_lit(get_func(name), L_FUNC));
    emit_i(0, T0, 0, RA, 0x67); // jalr ra, t0
}

void emit_load_str() { // a0 = &.rodata[str]
    if (rodata_sz + str_len + 1 > rodata_cap) { printf("rcc700: out of memory (rodata)\n"); exit(1); }
    emit_pool_lw(A0, get_lit(rodata_sz, L_STR));
    memcpy(rodata + rodata_sz, str_val, str_len + 1);
    rodata_sz = rodata_sz + str_len + 1;
}

// --- Parser ---
void parse_expr(int limit);

int find_var(char *name) {
    int i = n_vars;
    while (i > 0) { --i; if (!strcmp(name_buf + var_name_off[i], name)) return i; }
    return -1;
}

int get_prec(int t) {
    if (t == '?') return 1;
    if (t == T_LOR) return 2;
    if (t == T_LAND) return 3;
    if (t == '|') return 4;
    if (t == '^') return 5;
    if (t == '&') return 6;
    if (t == T_EQ || t == T_NE) return 7;
    if (t == '<' || t == '>' || t == T_LE || t == T_GE) return 8;
    if (t == T_SHL || t == T_SHR) return 9;
    if (t == '+' || t == '-') return 10;
    if (t == '*' || t == '/' || t == '%') return 11;
    return 0;
}

void emit_binop(int op) { // a0 = t0 op a0  (t0 = lhs, a0 = rhs)
    if (op == '+') emit_r(0, A0, T0, 0, A0, 0x33);
    else if (op == '-') emit_r(0x20, A0, T0, 0, A0, 0x33);
    else if (op == '*') emit_r(1, A0, T0, 0, A0, 0x33);
    else if (op == '/') emit_r(1, A0, T0, 4, A0, 0x33);
    else if (op == '%') emit_r(1, A0, T0, 6, A0, 0x33);
    else if (op == '&') emit_r(0, A0, T0, 7, A0, 0x33);
    else if (op == '|') emit_r(0, A0, T0, 6, A0, 0x33);
    else if (op == '^') emit_r(0, A0, T0, 4, A0, 0x33);
    else if (op == T_SHL) emit_r(0, A0, T0, 1, A0, 0x33);    // sll
    else if (op == T_SHR) emit_r(0x20, A0, T0, 5, A0, 0x33); // sra
    else if (op == '<') emit_r(0, A0, T0, 2, A0, 0x33);  // slt a0, t0, a0
    else if (op == '>') emit_r(0, T0, A0, 2, A0, 0x33);  // slt a0, a0, t0
    else if (op == T_GE) { emit_r(0, A0, T0, 2, A0, 0x33); emit_i(1, A0, 4, A0, 0x13); } // !(t0 < a0)
    else if (op == T_LE) { emit_r(0, T0, A0, 2, A0, 0x33); emit_i(1, A0, 4, A0, 0x13); } // !(a0 < t0)
    else if (op == T_EQ) { emit_r(0, A0, T0, 4, A0, 0x33); emit_i(1, A0, 3, A0, 0x13); } // xor; sltiu 1
    else if (op == T_NE) { emit_r(0, A0, T0, 4, A0, 0x33); emit_r(0, A0, ZERO, 3, A0, 0x33); } // xor; sltu 0<
    else if (op == T_LAND || op == T_LOR) { // normalize to 0/1; like xcc700, not short-circuit
        emit_r(0, T0, ZERO, 3, T0, 0x33);                  // sltu t0, zero, t0
        emit_r(0, A0, ZERO, 3, A0, 0x33);                  // sltu a0, zero, a0
        emit_r(0, A0, T0, op == T_LAND ? 7 : 6, A0, 0x33); // and/or
    }
    else unsupported("operator");
    expr_type = TY_INT;
}

void load_var_address(int i) { // a0 = &var
    if (var_types[i] & TF_GLOBAL) emit_pool_lw(A0, get_lit(var_offsets[i], L_BSS));
    else emit_addi(A0, SP, var_offsets[i]);
}
void load_var(int i) { // a0 = var (arrays decay to a pointer)
    int ty = var_types[i]; int is_byte = ((ty & ~TF_GLOBAL) == TY_BYTE);
    if (ty & TF_CONST) { emit_li(A0, var_offsets[i]); expr_type = TY_INT; }
    else if (ty & TF_ARR) { load_var_address(i); expr_type = (ty & TF_BYTE) ? TY_BYTEPTR : TY_INTPTR; }
    else if (ty & TF_GLOBAL) {
        load_var_address(i);
        if (is_byte) emit_lbu(A0, A0, 0); else emit_lw(A0, A0, 0);
        expr_type = ty & ~TF_GLOBAL;
    } else {
        if (is_byte) emit_lbu(A0, SP, var_offsets[i]); else emit_lw(A0, SP, var_offsets[i]);
        expr_type = ty;
    }
}

void parse_index(int base_type) { // a0 = a0 + index (scaled); '[' already current
    next(); push();
    parse_expr(1); expect(']');
    if (!(base_type & TF_BYTE)) emit_i(2, A0, 1, A0, 0x13); // slli a0, a0, 2
    pop(T0); emit_r(0, A0, T0, 0, A0, 0x33); // add a0, t0, a0
}

void parse_call(char *name) {
    int arg_cnt = 0; next();
    if (token != ')') {
        parse_expr(1); push(); ++arg_cnt;
        while (token == ',') { next(); parse_expr(1); push(); ++arg_cnt; }
        if (arg_cnt > 6) { printf("rcc700: line %d: more than 6 args\n", line); exit(1); }
        while (arg_cnt > 0) { --arg_cnt; pop(A0 + arg_cnt); }
    }
    expect(')'); emit_call(name);
}

void parse_factor() { // Result in a0
    if (token == T_INC || token == T_DEC) { // prefix ++/--: returns the new value
        int diff = (token == T_INC) ? 1 : -1; next();
        char name[64]; strcpy(name, str_val); expect(T_IDENT);
        int i = find_var(name);
        if (i < 0) unsupported("identifier");
        load_var(i); emit_addi(A0, A0, diff);
        int is_byte = ((var_types[i] & ~TF_GLOBAL) == TY_BYTE);
        if (var_types[i] & TF_GLOBAL) {
            emit_pool_lw(T0, get_lit(var_offsets[i], L_BSS));
            if (is_byte) emit_sb(A0, T0, 0); else emit_sw(A0, T0, 0);
        } else if (is_byte) emit_sb(A0, SP, var_offsets[i]);
        else emit_sw(A0, SP, var_offsets[i]);
        expr_type = TY_INT;
    }
    else if (token == '!' || token == '~' || token == '-') {
        int op = token; next(); parse_factor();
        if (op == '-') emit_r(0x20, A0, ZERO, 0, A0, 0x33); // neg: sub a0, zero, a0
        else if (op == '~') emit_i(-1, A0, 4, A0, 0x13);    // xori a0, a0, -1
        else emit_i(1, A0, 3, A0, 0x13);                    // seqz: sltiu a0, a0, 1
        expr_type = TY_INT;
    }
    else if (token == T_NUM) { emit_li(A0, num_val); expr_type = TY_INT; next(); }
    else if (token == T_STR) { emit_load_str(); expr_type = TY_BYTEPTR; next(); }
    else if (token == '*') {
        next(); parse_factor(); int pt = expr_type;
        if (pt & TF_BYTE) { emit_lbu(A0, A0, 0); expr_type = TY_BYTE; }
        else { emit_lw(A0, A0, 0); expr_type = TY_INT; }
    }
    else if (token == '&') {
        next(); char name[64]; strcpy(name, str_val); expect(T_IDENT);
        int i = find_var(name);
        if (i < 0) unsupported("identifier");
        load_var_address(i);
        expr_type = (var_types[i] & TF_BYTE) ? TY_BYTEPTR : TY_INTPTR;
    }
    else if (token == T_IDENT) {
        char name[64]; strcpy(name, str_val); next();
        if (token == '(') { parse_call(name); expr_type = TY_INT; }
        else {
            int i = find_var(name);
            if (i < 0) unsupported("identifier");
            load_var(i);
            if (token == '[') {
                int bt = expr_type;
                parse_index(bt);
                if (bt & TF_BYTE) { emit_lbu(A0, A0, 0); expr_type = TY_BYTE; }
                else { emit_lw(A0, A0, 0); expr_type = TY_INT; }
            }
        }
    }
    else if (token == '(') { next(); parse_expr(1); expect(')'); }
    else unsupported("expression");
}

void parse_expr(int limit) { // Precedence climbing; result in a0
    parse_factor();
    while (get_prec(token) >= limit) {
        int op = token; next();
        if (op == '?') { // ternary; the false branch recurses, so right-assoc
            int p1 = code_size; emit_beqz(A0, 0);
            parse_expr(2);
            int p2 = code_size; emit_j(0);
            expect(':'); patch(p1, 0);
            parse_expr(1);
            patch(p2, 1);
        } else {
            push(); parse_expr(get_prec(op) + 1);
            pop(T0); emit_binop(op);
        }
    }
}

// ra lives at sp+0 (fixed); the frame size isn't known until the body is parsed,
// so the sp-restore offset is recorded here and patched in parse_func.
void emit_epilogue() {
    emit_lw(RA, SP, 0);
    if (n_epi >= MAX_EPI) { printf("rcc700: line %d: too many returns\n", line); exit(1); }
    epi_offs[n_epi] = code_size; ++n_epi;
    emit_addi(SP, SP, 0); emit_ret();
}

void parse_stmt() {
    esp = 0;
    if (token == T_WHILE) {
        next(); int loop_start = code_size;
        expect('('); parse_expr(1); expect(')');
        int exit_patch = code_size; emit_beqz(A0, 0);
        parse_stmt();
        emit_j(loop_start - code_size);
        patch(exit_patch, 0);
    } else if (token == T_IF) {
        next(); expect('('); parse_expr(1); expect(')');
        int p1 = code_size; emit_beqz(A0, 0);
        parse_stmt();
        if (token == T_ELSE) {
            int p2 = code_size; emit_j(0);
            patch(p1, 0); next(); parse_stmt(); patch(p2, 1);
        } else patch(p1, 0);
    } else if (token == T_INT || token == T_CHAR) { // local: int x = expr; / int x[N];
        int is_byte = (token == T_CHAR); next();
        int is_ptr = 0; while (token == '*') { is_ptr = 1; next(); }
        if (n_vars >= MAX_VARS) { printf("rcc700: line %d: too many locals\n", line); exit(1); }
        var_name_off[n_vars] = add_name(str_val);
        var_offsets[n_vars] = locals; ++n_vars;
        expect(T_IDENT);
        if (token == '[') {
            next(); int sz = num_val; expect(T_NUM); expect(']');
            var_types[n_vars-1] = is_byte ? TY_BYTEARR : TY_INTARR;
            locals = locals + (is_byte ? align4(sz) : sz * 4);
        } else {
            var_types[n_vars-1] = is_ptr ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT);
            locals = locals + 4;
            expect('='); parse_expr(1);
            if (is_byte && !is_ptr) emit_sb(A0, SP, var_offsets[n_vars-1]);
            else emit_sw(A0, SP, var_offsets[n_vars-1]);
        }
        if (locals >= MAXFRAME) { printf("rcc700: line %d: stack frame overflow\n", line); exit(1); }
        if (locals > frame_max) frame_max = locals;
        expect(';');
    } else if (token == T_RETURN) {
        next();
        if (token != ';') parse_expr(1); else emit_li(A0, 0);
        emit_epilogue(); expect(';');
    } else if (token == '{') {
        next(); while (token != '}' && token != T_EOF) parse_stmt(); expect('}');
    } else if (token == T_IDENT) {
        char name[64]; strcpy(name, str_val); next();
        if (token == '(') { parse_call(name); expect(';'); }
        else {
            int i = find_var(name);
            if (i < 0) unsupported("identifier");
            if (token == '[') { // arr[idx] = expr;
                load_var(i);
                int bt = expr_type;
                parse_index(bt); push();
                expect('='); parse_expr(1);
                pop(T0);
                if (bt & TF_BYTE) emit_sb(A0, T0, 0); else emit_sw(A0, T0, 0);
            } else { // var = expr;
                expect('='); parse_expr(1);
                if (var_types[i] & TF_GLOBAL) {
                    emit_pool_lw(T0, get_lit(var_offsets[i], L_BSS));
                    emit_sw(A0, T0, 0);
                } else emit_sw(A0, SP, var_offsets[i]);
            }
            expect(';');
        }
    } else if (token == '*') { // *ptr = expr;
        next(); parse_factor();
        int pt = expr_type;
        push(); expect('='); parse_expr(1);
        pop(T0);
        if (pt & TF_BYTE) emit_sb(A0, T0, 0); else emit_sw(A0, T0, 0);
        expect(';');
    } else { parse_expr(1); expect(';'); }
}

void parse_func() {
    if (token == T_ENUM) { // enum [Name] { A, B = 2, ... }; members become int constants
        next(); if (token == T_IDENT) next();
        expect('{'); int val = 0;
        while (token == T_IDENT) {
            if (n_globals >= MAX_VARS) { printf("rcc700: too many globals\n"); exit(1); }
            var_name_off[n_globals] = add_name(str_val);
            var_offsets[n_globals] = val;
            var_types[n_globals] = TF_CONST;
            ++n_globals; n_vars = n_globals; next();
            if (token == '=') { next(); val = num_val; var_offsets[n_globals-1] = val; next(); }
            ++val; if (token == ',') next();
        }
        expect('}'); expect(';'); return;
    }

    int is_byte = (token == T_CHAR);
    if (token == T_INT || token == T_CHAR || token == T_VOID) next();
    else unsupported("declaration");
    int is_ptr = 0; while (token == '*') { is_ptr = 1; next(); }
    char name[64]; strcpy(name, str_val); expect(T_IDENT);

    if (token == ';' || token == '[') { // global variable: zero-initialized bss
        if (n_globals >= MAX_VARS) { printf("rcc700: too many globals\n"); exit(1); }
        int ty = TF_GLOBAL | (is_ptr ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT));
        var_offsets[n_globals] = bss_size;
        if (token == '[') {
            next(); int sz = 0;
            if (token == T_NUM) { sz = num_val; next(); }
            else if (token == T_IDENT) { // size given as an enum constant
                int i = find_var(str_val);
                if (i < 0 || !(var_types[i] & TF_CONST)) {
                    printf("rcc700: line %d: undefined constant: %s\n", line, str_val); exit(1);
                }
                sz = var_offsets[i]; next();
            } else { printf("rcc700: line %d: array size expected\n", line); exit(1); }
            expect(']');
            ty = TF_GLOBAL | (is_byte ? TY_BYTEARR : TY_INTARR);
            bss_size = bss_size + (is_byte ? align4(sz) : sz * 4);
        } else bss_size = bss_size + 4;
        var_name_off[n_globals] = add_name(name);
        var_types[n_globals] = ty;
        ++n_globals; n_vars = n_globals;
        expect(';'); return;
    }

    expect('('); n_vars = n_globals; locals = 4; int n_args = 0; // locals start above ra (sp+0)
    frame_max = 4; n_epi = 0;
    while (token != ')' && token != T_EOF) { // parameters
        is_byte = 0; int ptr_count = 0; char pname[64]; pname[0] = 0;
        // Skip type specifiers/qualifiers (const, int, ...); last ident is the name.
        while (token != ',' && token != ')' && token != T_EOF) {
            if (token == T_CHAR) is_byte = 1;
            else if (token == '*') ++ptr_count;
            else if (token == T_IDENT) strcpy(pname, str_val);
            else if (token != T_INT && token != T_VOID && token != T_ELLIPSIS) unsupported("parameter");
            next();
        }
        if (pname[0]) {
            if (n_vars >= MAX_VARS) { printf("rcc700: too many locals\n"); exit(1); }
            var_name_off[n_vars] = add_name(pname);
            var_offsets[n_vars] = locals;
            var_types[n_vars] = ptr_count >= 2 ? TY_INTPTR : ptr_count ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT);
            locals = locals + 4; ++n_vars; ++n_args;
        }
        if (token == ',') next();
    }
    expect(')');
    if (token == ';') { next(); return; } // prototype
    if (n_args > 6) { printf("rcc700: line %d: more than 6 args\n", line); exit(1); }
    if (locals > frame_max) frame_max = locals; // params live in the frame too
    func_addrs[get_func(name)] = code_size;
    prologue_off = code_size; emit_addi(SP, SP, 0); emit_sw(RA, SP, 0); // prologue; sp adjust patched at end
    int j = 0; while (j < n_args) { emit_sw(A0 + j, SP, var_offsets[n_globals + j]); ++j; } // spill args
    expect('{');
    while (token != '}' && token != T_EOF) parse_stmt();
    expect('}');
    emit_li(A0, 0); emit_epilogue(); // implicit return 0
    // Back-patch the frame to what the body actually used
    int fsz = (frame_max + 15) & ~15;
    patch_frame(prologue_off, -fsz);
    int k = 0; while (k < n_epi) { patch_frame(epi_offs[k], fsz); ++k; }
}

// --- ELF Writer ---
// Layout: ehdr | phdrs | .text (pool + code) | .rodata | .rela.dyn | .symtab |
// .strtab | .shstrtab | section headers. Alloc vaddrs equal file offsets (like
// a gcc -shared image), so one PT_LOAD at vaddr 0 covers them; the output loads
// on both Xtensa (via sections) and RISC-V (via PT_LOAD).
void write_elf(char *out) {
    int pool_sz = n_lits * 4;
    int phoff = EHDR_SZ; // phdr table right after ehdr; .text follows
    int text_addr = EHDR_SZ + N_PHDRS*PHDR_SZ; int text_size = pool_sz + code_size;
    int rodata_addr = align4(text_addr + text_size);
    // bss is a vaddr range only (p_memsz zero-fill), so no file bytes.
    // The trailing non-alloc sections sit right after .rodata
    int bss_addr = align4(rodata_addr + rodata_sz);
    int rela_off = bss_addr;
    int n_syms = n_funcs + 1;
    int symtab_off = rela_off + n_lits * 12;
    int strtab_off = symtab_off + n_syms * 16;

    // Symbols: one per function; defined ones carry their .text address,
    // undefined ones are imports for the loader to resolve by name.
    char *syms = malloc(n_syms * 16); memset(syms, 0, n_syms * 16);
    char *strtab = malloc(NAMES + 1); strtab[0] = 0; int str_off = 1;
    int main_addr = -1;
    int i = 0;
    while (i < n_funcs) {
        char *s = syms + (i + 1) * 16;
        char *name = name_buf + func_name_off[i];
        put32(s, str_off);
        strcpy(strtab + str_off, name); str_off = str_off + strlen(name) + 1;
        if (func_addrs[i] < 0) {
            s[12] = 16;                                   // GLOBAL | NOTYPE, UNDEF
        } else {
            put32(s + 4, text_addr + pool_sz + func_addrs[i]);
            s[12] = 18; put16(s + 14, 1);                 // GLOBAL | FUNC, .text
            if (!strcmp(name, "main")) main_addr = text_addr + pool_sz + func_addrs[i];
        }
        ++i;
    }
    if (main_addr < 0) { printf("rcc700: main() is not defined\n"); exit(1); }

    // Literal pool + one relocation per slot.
    char *pool = malloc(pool_sz + 4);
    char *rels = malloc(n_lits * 12 + 4);
    i = 0;
    while (i < n_lits) {
        int val = 0; int info = 0;
        if (lit_types[i] == L_STR) {
            val = rodata_addr + lit_vals[i]; info = R_RISCV_RELATIVE;
        } else if (lit_types[i] == L_BSS) {
            val = bss_addr + lit_vals[i]; info = R_RISCV_RELATIVE;
        } else if (func_addrs[lit_vals[i]] >= 0) {
            val = text_addr + pool_sz + func_addrs[lit_vals[i]]; info = R_RISCV_RELATIVE;
        } else {
            info = ((lit_vals[i] + 1) << 8) | R_RISCV_JUMP_SLOT; // import, by symbol
        }
        put32(pool + i*4, val);
        put32(rels + i*12, text_addr + i*4); put32(rels + i*12 + 4, info);
        put32(rels + i*12 + 8, val);                      // addend (RELATIVE: base + A)
        ++i;
    }

    // Point each auipc+lw pair at its pool slot, now that distances are known.
    i = 0;
    while (i < n_patches) {
        int off = patch_offs[i];
        int delta = patch_lits[i] * 4 - (pool_sz + off);
        int hi = (delta + 0x800) >> 12; int w = 0;
        memcpy(&w, code_data + off, 4);
        put32(code_data + off, w | ((hi & 0xfffff) << 12));
        memcpy(&w, code_data + off + 4, 4);
        put32(code_data + off + 4, w | (((delta - (hi << 12)) & 0xfff) << 20));
        ++i;
    }

    // Section headers: 0:null 1:.text 2:.rodata 3:.rela.dyn 4:.symtab
    // 5:.strtab 6:.shstrtab.  shstrtab name offsets are fixed.
    char *shstr = "\0.text\0.rodata\0.rela.dyn\0.symtab\0.strtab\0.shstrtab\0";
    int shstr_sz = 51; int shstr_off = strtab_off + str_off;
    int shoff = align4(shstr_off + shstr_sz);
    char *shdr = malloc(SHDR_SZ); memset(shdr, 0, SHDR_SZ);
    // 1: .text  (PROGBITS, ALLOC|EXEC)
    put32(shdr+40, 1); put32(shdr+44, 1); put32(shdr+48, 6); put32(shdr+52, text_addr);
    put32(shdr+56, text_addr); put32(shdr+60, text_size); put32(shdr+72, 4);
    // 2: .rodata  (PROGBITS, ALLOC: literals only; globals live in the bss tail)
    put32(shdr+80, 7); put32(shdr+84, 1); put32(shdr+88, 2); put32(shdr+92, rodata_addr);
    put32(shdr+96, rodata_addr); put32(shdr+100, rodata_sz); put32(shdr+112, 4);
    // 3: .rela.dyn  (RELA, link=.symtab)
    put32(shdr+120, 15); put32(shdr+124, 4); put32(shdr+136, rela_off);
    put32(shdr+140, n_lits*12); put32(shdr+144, 4); put32(shdr+148, 1);
    put32(shdr+152, 4); put32(shdr+156, 12);
    // 4: .symtab  (link=.strtab, info=first global)
    put32(shdr+160, 25); put32(shdr+164, 2); put32(shdr+176, symtab_off);
    put32(shdr+180, n_syms*16); put32(shdr+184, 5); put32(shdr+188, 1);
    put32(shdr+192, 4); put32(shdr+196, 16);
    // 5: .strtab
    put32(shdr+200, 33); put32(shdr+204, 3); put32(shdr+216, strtab_off);
    put32(shdr+220, str_off); put32(shdr+232, 1);
    // 6: .shstrtab
    put32(shdr+240, 41); put32(shdr+244, 3); put32(shdr+256, shstr_off);
    put32(shdr+260, shstr_sz); put32(shdr+272, 1);

    // Single PT_LOAD: text+rodata file-backed, .bss tail is memsz-filesz zero fill.
    // The RISC-V loader pins svaddr to 0, so this segment must be vaddr 0.
    char phdr[32]; memset(phdr, 0, PHDR_SZ); // PHDR_SZ; literal size for self-host
    put32(phdr+0, 1);                                // p_type = PT_LOAD
    put32(phdr+4, 0);                                // p_offset = 0
    put32(phdr+8, 0);                                // p_vaddr = 0 (== p_offset)
    put32(phdr+16, rodata_addr + rodata_sz);         // p_filesz: through .rodata
    put32(phdr+20, bss_addr + bss_size);             // p_memsz: + zero-fill bss
    put32(phdr+24, 7); put32(phdr+28, 4);            // p_flags = R|W|X, p_align

    char ehdr[52]; memset(ehdr, 0, 52);
    ehdr[0]=0x7f; ehdr[1]='E'; ehdr[2]='L'; ehdr[3]='F'; ehdr[4]=1; ehdr[5]=1; ehdr[6]=1;
    put16(ehdr+16, 3); put16(ehdr+18, 243);          // e_type=ET_DYN, e_machine=EM_RISCV
    put32(ehdr+20, 1); put32(ehdr+24, main_addr);    // e_version, e_entry
    put32(ehdr+28, phoff); put32(ehdr+32, shoff);    // e_phoff, e_shoff
    put16(ehdr+40, 52); put16(ehdr+46, 40);          // ehsize, shentsize
    put16(ehdr+42, PHDR_SZ); put16(ehdr+44, N_PHDRS); // phentsize, phnum
    put16(ehdr+48, N_SECS); put16(ehdr+50, 6);       // shnum, shstrndx

    int f = open(out, O_WRONLY | O_CREAT | O_TRUNC, 420); // 0644
    if (f < 0) { printf("rcc700: cannot open output file: %s\n", out); exit(1); }
    write(f, ehdr, 52);
    write(f, phdr, PHDR_SZ);
    write(f, pool, pool_sz); write(f, code_data, code_size);
    lseek(f, rodata_addr, SEEK_SET); write(f, rodata, rodata_sz);
    lseek(f, rela_off, SEEK_SET); write(f, rels, n_lits * 12);
    write(f, syms, n_syms * 16);
    write(f, strtab, str_off);
    write(f, shstr, shstr_sz);
    lseek(f, shoff, SEEK_SET); write(f, shdr, SHDR_SZ);
    close(f);
    free(pool); free(rels); free(syms); free(strtab); free(shdr);
}

void print_stats(char *outfile) {
    int fsz = 0; int f = open(outfile, O_RDONLY, 0);
    if (f >= 0) { fsz = lseek(f, 0, SEEK_END); close(f); }
    printf("\n[ rcc700 ] BUILD COMPLETED > OK\n");
    printf("> IN  : %d Lines / %d Tokens\n", line, token_cnt);
    printf("> SYM : %d Funcs / %d Globals / %d Literals / %d Patches\n", n_funcs, n_globals, n_lits, n_patches);
    printf("> OUT : %d B .text / %d B .rodata / %d B bss / %d B ELF\n", code_size, rodata_sz, bss_size, fsz);
}
