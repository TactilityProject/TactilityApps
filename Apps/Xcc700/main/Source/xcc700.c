// xcc700.c - A mini C compiler for esp32 / Xtensa

// --- Libc Shims ---
int printf(char *fmt, ...); void exit(int status); int clock();
void *malloc(int size); void *calloc(int nmemb, int size); void free(void *ptr);
void *memcpy(void *dest, void *src, int n); void *memset(void *s, int c, int n);
int strcmp(char *s1, char *s2); char *strcpy(char *dest, char *src);
int strlen(char *s); int strtol(char *nptr, char **endptr, int base);
int open(char *pathname, int flags, int mode); int close(int fd);
int read(int fd, void *buf, int count); int write(int fd, void *buf, int count);
int lseek(int fd, int offset, int whence);

// --- Constants & Globals ---
enum {
    MAX_VARS=256, MAX_LOCAL_VARS=128, MAX_LITS=256, MAX_PATCHES=1200, STRTAB=2048,
    R_XTENSA_RELATIVE=5, R_XTENSA_JMP_SLOT=4,
    SP_REG=1, RES_REG=8, TMP_REG=9, ARG1_REG=10, ARG2_REG=11,
    O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=64, O_TRUNC=512,
    SEEK_SET=0, SEEK_END=2
};

enum { 
    T_EOF=0, T_INT=256, T_CHAR, T_VOID, T_IDENT, T_NUM, T_STR, T_EQ, T_NE, T_LE, T_GE,
    T_SHL, T_SHR, T_LAND, T_LOR, T_RETURN, T_ELLIPSIS, T_IF, T_ELSE, T_WHILE, T_ENUM, T_INC, T_DEC 
};

enum { L_INT, L_STR, L_FUNC, L_BSS };
enum { TF_BYTE=1, TF_PTR=2, TF_ARR=4, TF_GLOBAL=8, TF_CONST=16 };
enum { TY_INT=0, TY_BYTE=1, TY_INTPTR=2, TY_BYTEPTR=3, TY_INTARR=4, TY_BYTEARR=5 };

// Global Context
char *src;
char *rodata; int rodata_sz; int rodata_cap;
int token; int num_val; int line; int token_cnt;
char str_val[256]; int str_len;
char *code_data; int code_size; int code_cap;
char *name_buf; int name_sz; int name_cap;

// Vars
int var_name_off[MAX_VARS];
int var_offsets[MAX_VARS];
int var_types[MAX_VARS];
int n_vars; int locals; int esp; int expr_type; int n_globals; int bss_size;

// Funcs
int func_name_off[MAX_VARS];
int func_addrs[MAX_VARS];
int n_funcs;

// Literals
int lit_vals[MAX_LITS];
int lit_types[MAX_LITS];
int n_lits;

// Patches
int patch_offs[MAX_PATCHES];
int patch_lits[MAX_PATCHES];
int n_patches;

// Must put main() first. Forward-declare the functions it uses.
void next(); void parse_func(); void write_elf(char *out); void print_stats(char *outfile, int t_ms);

int main(int argc, char **argv) {
    line = 1; name_buf = 0;
    char *input_fname = argc > 1 ? argv[1] : "input.c";
    char *output_fname = (argc > 3 && !strcmp(argv[2], "-o")) ? argv[3] : "output.elf";
    int f = open(input_fname, O_RDONLY, 0);
    if (f < 0) { printf("Cannot open input file: %s\n", input_fname); return 1; }
    int f_size = lseek(f, 0, SEEK_END); lseek(f, 0, SEEK_SET);
    char *src_buf = malloc(f_size+1); src = src_buf;
    code_cap = 32768; code_data = malloc(code_cap); 
    rodata_cap = 2048; rodata = malloc(rodata_cap);
    name_cap = 4096; name_buf = malloc(name_cap); name_sz = 0;
    if (!src_buf || !code_data || !rodata || !name_buf) { printf("Error: Not enough memory!\n"); return 1; }
    
    read(f, src, f_size); src[f_size]=0; close(f);
    int start_clk = clock();
    next(); while(token!=T_EOF) parse_func();
    int duration_ms = clock() - start_clk; // In espressif newlib, CLOCKS_PER_SEC=1000
    free(src_buf); write_elf(output_fname);
    print_stats(output_fname, duration_ms);
    free(rodata); free(name_buf);
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
            !strcmp(str_val,"void") ? T_VOID : !strcmp(str_val, "enum") ? T_ENUM :
            !strcmp(str_val,"if") ? T_IF : !strcmp(str_val,"else") ? T_ELSE :
            !strcmp(str_val,"while") ? T_WHILE : !strcmp(str_val,"return") ? T_RETURN : T_IDENT;
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
                else if (*src == 'r') *p='\r'; else if (*src == '0') *p = '\0'; else *p=*src;
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
        if (tok < 256) printf("Error: Line %d: expected '%c'", line, tok);
        else printf("Error: Line %d: expected token %d", line, tok); 
        if (token < 256) printf(", got '%c'\n", token);
        else printf(", got token %d (%s)\n", token, str_val); 
        exit(1); 
    } 
    next(); 
}

// --- Code Emitter ---
void ensure_code_capacity(int n) {
    if (code_size + n > code_cap) { printf("Error: Out of memory (code)!\n"); exit(1); }
}
void emit3(int b0, int b1, int b2) {
    ensure_code_capacity(3);
    code_data[code_size] = b0; code_data[code_size+1] = b1; code_data[code_size+2] = b2;
    code_size = code_size + 3;
}
void emit2(int b0, int b1) {
    ensure_code_capacity(2);
    code_data[code_size] = b0; ++code_size;
    code_data[code_size] = b1; ++code_size;
}

void emit_rrr(int op, int op0, int r, int s, int t) { emit3((t<<4)|op0, (r<<4)|s, op); }
void emit_l32i(int d, int b, int off) { emit3((d<<4)|2, (2<<4)|b, off/4); }
void emit_s32i(int s, int b, int off) { emit3((s<<4)|2, (6<<4)|b, off/4); }
void emit_l8ui(int d, int b, int off) { emit3((d<<4)|2, (0<<4)|b, off); }
void emit_s8i(int s, int b, int off) { emit3((s<<4)|2, (4<<4)|b, off); }
void emit_l32r(int r) { emit3((r<<4)|1, 0, 0); }
void emit_add_n(int d, int s1, int s2) { emit2((s2<<4)|0xa, (d<<4)|s1); }
void emit_mov_n(int d, int s) { emit2((d<<4)|0xd, s); }
void emit_movi_n(int d, int imm) { 
    int i=(((imm&0xf)<<12)|(d<<8)|(((imm&0x70)>>4)<<4)|0xc); 
    emit2(i&0xff, i>>8); 
}
void emit_load_lit(int val, int type);
void emit_movi(int d, int imm) {
    if (imm >= -2048 && imm < 2048) emit3((d<<4)|2, 0xa0|((imm>>8)&0xf), imm&0xff);
    else { emit_load_lit(imm, L_INT); if (d != RES_REG) emit_mov_n(d, RES_REG); }
}
void emit_addi(int d, int s, int imm) {
    if (imm >= -128 && imm < 128) emit3((d<<4)|2, (0xc<<4)|s, imm&0xff);
    else { emit_movi(TMP_REG, imm); emit_add_n(d, s, TMP_REG); }
}
void emit_op(int op, int d, int s1, int s2) { emit_rrr(op, 0, d, s1, s2); }
void emit_neg(int d, int s1) { emit_op(0x60, d, 0, s1); }
void emit_xor(int d, int s1, int s2) { emit_op(0x30, d, s1, s2); }
void emit_br(int op, int s, int t) { emit3((t<<4)|7, (op<<4)|s, 1); }
void emit_j(int off) { int i=0x06|((off&0x3ffff)<<6); emit3(i, i>>8, i>>16); }
void emit_beqz(int s, int off) { int i=((off&0xfff)<<12)|(s<<8)|0x16; emit3(i, i>>8, i>>16); }
void emit_callx8() { emit3(0xe0, 0x08, 0x00); }
void emit_retw_n() { emit2(0x1d, 0xf0); }
void emit_entry(int sz) { int imm12 = sz/8; emit3(0x36, (imm12<<4)|1, imm12>>4); }

void patch(int addr, int is_j) {
    int off = code_size - addr - 4;
    int i = 0; memcpy(&i, code_data + addr, 3);
    if (is_j) i = i | ((off & 0x3ffff) << 6);
    else      i = i | ((off & 0xfff)   << 12);
    memcpy(code_data + addr, &i, 3);
}

// --- Literals/symbols handling ---
int add_name(char *s) {
    int len = strlen(s) + 1;
    if (name_sz + len > name_cap) { printf("Out of memory (names)\n"); exit(1); }
    int off = name_sz; strcpy(name_buf + off, s);
    name_sz = name_sz + len; return off;
}
int get_func(char *name) {
    int i=0;
    while (i<n_funcs) { if(!strcmp(name_buf + func_name_off[i], name)) return i; ++i; }
    func_name_off[n_funcs] = add_name(name);
    func_addrs[n_funcs] = -1;
    ++n_funcs;
    return n_funcs - 1;
}

void emit_load_lit(int val, int type) {
    int i=0;
    while(i<n_lits && (lit_vals[i] != val || lit_types[i] != type)) ++i;
    if (i == n_lits) {
        if (n_lits >= MAX_LITS) { printf("Error: Too many literals\n"); exit(1); }
        lit_vals[n_lits] = val; lit_types[n_lits] = type; ++n_lits;
    }
    if (n_patches >= MAX_PATCHES) { printf("Error: Too many patches\n"); exit(1); }
    patch_offs[n_patches] = code_size; patch_lits[n_patches] = i; ++n_patches;
    emit_l32r(RES_REG);
}

void push(int r) { emit_s32i(r, SP_REG, locals + esp); esp = esp + 4; }
void pop(int r) { esp = esp - 4; emit_l32i(r, SP_REG, locals + esp); }

// --- Parser ---
int get_prec(int t) {
    if(t=='?') return 1; if(t==T_LOR) return 2; if(t==T_LAND) return 3;
    if(t=='|') return 4; if(t=='^') return 5; if(t=='&') return 6;
    if(t==T_EQ||t==T_NE) return 7;
    if(t=='<'||t=='>'||t==T_LE||t==T_GE) return 8;
    if(t==T_SHL||t==T_SHR) return 9;
    if(t=='+'||t=='-') return 10; if(t=='*'||t=='/'||t=='%') return 11;
    return 0;
}

void emit_binop(int op) {
    if (op == T_LAND || op == T_LOR) {
        emit_movi_n(ARG1_REG, 0); emit_beqz(TMP_REG, 1); emit_movi_n(ARG1_REG, 1);
        emit_movi_n(ARG2_REG, 0); emit_beqz(RES_REG, 1); emit_movi_n(ARG2_REG, 1);
        emit_op(op==T_LAND ? 0x10 : 0x20, RES_REG, ARG2_REG, ARG1_REG);
    } else if (get_prec(op) >= 7 && get_prec(op) <= 8) {
        emit_mov_n(ARG1_REG, RES_REG); emit_movi_n(RES_REG, 0);
        if(op=='<') emit_br(0xa, TMP_REG, ARG1_REG); else if(op==T_LE) emit_br(0x2, ARG1_REG, TMP_REG);
        else if(op=='>') emit_br(0xa, ARG1_REG, TMP_REG); else if(op==T_GE) emit_br(0x2, TMP_REG, ARG1_REG);
        else if(op==T_EQ) emit_br(0x9, TMP_REG, ARG1_REG); else if(op==T_NE) emit_br(0x1, TMP_REG, ARG1_REG);
        emit_movi_n(RES_REG, 1);
    } else if(op=='+') emit_add_n(RES_REG, TMP_REG, RES_REG);
    else if(op=='-') emit_op(0xc0, RES_REG, TMP_REG, RES_REG);
    else if(op=='*') emit_op(0x82, RES_REG, TMP_REG, RES_REG);
    else if(op=='/') emit_op(0xd2, RES_REG, TMP_REG, RES_REG);
    else if(op=='%') emit_op(0xf2, RES_REG, TMP_REG, RES_REG);
    else if(op=='&') emit_op(0x10, RES_REG, TMP_REG, RES_REG);
    else if(op=='|') emit_op(0x20, RES_REG, TMP_REG, RES_REG);
    else if(op=='^') emit_op(0x30, RES_REG, TMP_REG, RES_REG);
    else if(op==T_SHL) { emit3(0,0x10|RES_REG,0x40); emit3(0,(RES_REG<<4)|TMP_REG,0xA1); }
    else if(op==T_SHR) { emit3(0,RES_REG,0x40); emit3(TMP_REG<<4,RES_REG<<4,0xB1); }
    expr_type = TY_INT;
}

void parse_expr(int limit);

void parse_call(char *name) {
    int arg_cnt = 0; next();
    if (token != ')') {
        parse_expr(1); push(RES_REG); ++arg_cnt;
        if (arg_cnt > 5) { printf("Error: L%d: Arg count exceeds the supported maximum of 5\n", line); exit(1); }
        while (token == ',') { next(); parse_expr(1); push(RES_REG); ++arg_cnt; }
        while (arg_cnt > 0) { --arg_cnt; pop(arg_cnt <= 4 ? ARG1_REG + arg_cnt : TMP_REG); }
    }
    expect(')'); emit_load_lit(get_func(name), L_FUNC); emit_callx8();
}

int find_var(char *name) {
    int i = n_vars;
    while (i > 0) { --i; if (!strcmp(name_buf + var_name_off[i], name)) return i; }
    return -1;
}
void load_var_address(int i) {
    if (var_types[i] & TF_GLOBAL) emit_load_lit(var_offsets[i], L_BSS);
    else emit_addi(RES_REG, SP_REG, var_offsets[i]);
}
void load_var(int i) {
    int ty = var_types[i]; int is_byte = ((ty & ~TF_GLOBAL) == TY_BYTE);
    if (ty & TF_CONST) {
        emit_movi(RES_REG, var_offsets[i]); expr_type = TY_INT;
    } else if (ty & TF_ARR) {
        load_var_address(i); expr_type = (ty & TF_BYTE) ? TY_BYTEPTR : TY_INTPTR;
    } else if (ty & TF_GLOBAL) {
        load_var_address(i);
        if (is_byte) emit_l8ui(RES_REG, RES_REG, 0); else emit_l32i(RES_REG, RES_REG, 0);
        expr_type = ty & ~TF_GLOBAL;
    } else {
        if (is_byte) emit_l8ui(RES_REG, SP_REG, var_offsets[i]);
        else emit_l32i(RES_REG, SP_REG, var_offsets[i]);
        expr_type = ty & ~TF_GLOBAL;
    }
}

void parse_index(int base_type) {
    next(); push(RES_REG);
    parse_expr(1); expect(']');
    if (!(base_type & TF_BYTE)) { emit_add_n(RES_REG, RES_REG, RES_REG); emit_add_n(RES_REG, RES_REG, RES_REG); }
    pop(TMP_REG); emit_add_n(RES_REG, TMP_REG, RES_REG);
}

void parse_factor() {
    if (token == T_INC || token == T_DEC) {
        int diff = (token == T_INC) ? 1 : -1; next();
        char name[64]; strcpy(name, str_val); expect(T_IDENT);
        int i = find_var(name);
        if (i < 0) { printf("Undef: %s\n", name); exit(1); }
        load_var(i); emit_addi(RES_REG, RES_REG, diff);
        int is_byte = ((var_types[i] & ~TF_GLOBAL) == TY_BYTE);
        if (var_types[i] & TF_GLOBAL) {
            emit_mov_n(ARG1_REG, RES_REG); load_var_address(i);
            if (is_byte) emit_s8i(ARG1_REG, RES_REG, 0); else emit_s32i(ARG1_REG, RES_REG, 0);
            emit_mov_n(RES_REG, ARG1_REG);
        } else {
            if (is_byte) emit_s8i(RES_REG, SP_REG, var_offsets[i]);
            else emit_s32i(RES_REG, SP_REG, var_offsets[i]);
        }
        expr_type = TY_INT;
    } else if (token == '!' || token == '~' || token == '-') {
        int op = token; next(); parse_factor();
        if (op == '-') emit_neg(RES_REG, RES_REG);
        else if (op == '~') { emit_movi(TMP_REG, -1); emit_xor(RES_REG, RES_REG, TMP_REG); }
        else { emit_movi_n(TMP_REG, 1); emit_beqz(RES_REG, 1); emit_movi_n(TMP_REG, 0); emit_mov_n(RES_REG, TMP_REG); }
        expr_type = TY_INT;
    } else if (token == '*') {
        next(); parse_factor(); int pt = expr_type;
        if (pt & TF_BYTE) { emit_l8ui(RES_REG, RES_REG, 0); expr_type = TY_BYTE; }
        else { emit_l32i(RES_REG, RES_REG, 0); expr_type = TY_INT; }
    } else if (token == '&') {
        next(); char name[64]; strcpy(name, str_val); expect(T_IDENT);
        int i = find_var(name);
        if (i < 0) { printf("Undef: %s\n", name); exit(1); }
        load_var_address(i);
        expr_type = (var_types[i] & TF_BYTE) ? TY_BYTEPTR : TY_INTPTR;
    } else if (token == T_NUM) { 
        emit_movi(RES_REG, num_val); expr_type = TY_INT; next();
    } else if (token == T_STR) {
        if (rodata_sz + str_len + 1 > rodata_cap) { printf("Out of memory (rodata)\n"); exit(1); }
        emit_load_lit(rodata_sz, L_STR);
        memcpy(rodata + rodata_sz, str_val, str_len + 1);
        rodata_sz = rodata_sz + str_len + 1;
        expr_type = TY_BYTEPTR; next();
    } else if (token == T_IDENT) {
        char name[64]; strcpy(name, str_val); next();
        if (token == '(') {
            parse_call(name); emit_mov_n(RES_REG, ARG1_REG); expr_type = TY_INT;
        } else {
            int i = find_var(name);
            if (i < 0) { printf("Undef: %s\n", name); exit(1); }
            load_var(i);
            if (token == '[') {
                int bt = expr_type;
                parse_index(bt);
                if (bt & TF_BYTE) { emit_l8ui(RES_REG, RES_REG, 0); expr_type = TY_BYTE; }
                else { emit_l32i(RES_REG, RES_REG, 0); expr_type = TY_INT; }
            }
        }
    } else if (token == '(') { next(); parse_expr(1); expect(')'); }
    else { printf("Error: Line %d: unexpected token %d\n", line, token); exit(1); }
}

void parse_expr(int limit) {
    parse_factor();
    while (get_prec(token) >= limit) {
        int op = token; next();
        if (op == '?') {
            int patch_to_false = code_size; emit_beqz(RES_REG, 0);
            parse_expr(2);
            int patch_to_end = code_size; emit_j(0);
            expect(':'); patch(patch_to_false, 0);
            parse_expr(1);
            patch(patch_to_end, 1);
        } else {
            push(RES_REG); parse_expr(get_prec(op) + 1);
            pop(TMP_REG); emit_binop(op);
        }
    }
}

int align4(int x) { return (x + 3) & ~3; }

void parse_stmt() {
    esp = 0;
    if (token == T_WHILE) {
        next(); int loop_start = code_size;
        expect('('); parse_expr(1); expect(')');
        int exit_patch = code_size; emit_beqz(RES_REG, 0);
        parse_stmt();
        emit_j(loop_start - code_size - 4);
        patch(exit_patch, 0);
    } else if (token == T_IF) {
        next(); expect('('); parse_expr(1); expect(')');
        int p1 = code_size; emit_beqz(RES_REG, 0);
        parse_stmt();
        if (token == T_ELSE) {
            int p2 = code_size; emit_j(0);
            patch(p1, 0); next(); parse_stmt(); patch(p2, 1);
        } else patch(p1, 0);
    } else if (token == '{') {
        next(); while (token != '}' && token != T_EOF) parse_stmt(); expect('}');
    } else if (token == T_INT || token == T_CHAR) {
        int is_byte = (token == T_CHAR); next();
        int is_ptr = 0; while (token == '*') { is_ptr = 1; next(); }
        var_name_off[n_vars] = add_name(str_val);
        var_offsets[n_vars] = locals;
        if (++n_vars >= MAX_VARS) { printf("MAX_VARS exceeded\n"); exit(1); }
        expect(T_IDENT);
        if (token == '[') {
            next(); int sz = num_val; expect(T_NUM); expect(']');
            var_types[n_vars-1] = is_byte ? TY_BYTEARR : TY_INTARR;
            locals = locals + (is_byte ? align4(sz) : sz * 4);
        } else {
            var_types[n_vars-1] = is_ptr ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT);
            locals = locals + 4;
            expect('='); parse_expr(1);
            if (is_byte && !is_ptr) emit_s8i(RES_REG, SP_REG, var_offsets[n_vars-1]);
            else emit_s32i(RES_REG, SP_REG, var_offsets[n_vars-1]);
        }
        if (locals >= MAX_LOCAL_VARS * 4) {
            printf("Error: Line %d: Function stack frame exceeded MAX_LOCAL_VARS\n", line); exit(1);
        }
        expect(';');
    } else if (token == T_RETURN) {
        next(); if (token != ';') parse_expr(1); else emit_movi_n(RES_REG, 0);
        emit_mov_n(2, RES_REG); emit_retw_n(); expect(';');
    } else if (token == T_IDENT) {
        char name[64]; strcpy(name, str_val); next();
        if (token == '(') {
            parse_call(name); expect(';'); 
        } else {
            int i = find_var(name);
            if (i < 0) { printf("Undef: %s\n", name); exit(1); }
            if (token == '[') {
                load_var(i);
                int bt = expr_type;
                parse_index(bt); push(RES_REG);
                expect('='); parse_expr(1);
                pop(ARG1_REG);
                if (bt & TF_BYTE) emit_s8i(RES_REG, ARG1_REG, 0);
                else emit_s32i(RES_REG, ARG1_REG, 0);
            } else {
                expect('='); parse_expr(1);
                if (var_types[i] & TF_GLOBAL) {
                    emit_mov_n(ARG1_REG, RES_REG); 
                    emit_load_lit(var_offsets[i], L_BSS);
                    emit_s32i(ARG1_REG, RES_REG, 0);
                    emit_mov_n(RES_REG, ARG1_REG);
                } else emit_s32i(RES_REG, SP_REG, var_offsets[i]);
            }
            expect(';');
        }
    } else if (token == '*') {
        next(); parse_factor();
        int pt = expr_type;
        push(RES_REG); expect('='); parse_expr(1);
        pop(ARG1_REG);
        if (pt & TF_BYTE) emit_s8i(RES_REG, ARG1_REG, 0);
        else emit_s32i(RES_REG, ARG1_REG, 0);
        expect(';');
    } else { parse_expr(1); expect(';'); }
}

void parse_func() {
    if (token == T_ENUM) {
        next(); if (token == T_IDENT) next();
        expect('{'); int val = 0;
        while (token == T_IDENT) {
            var_name_off[n_globals] = add_name(str_val);
            var_offsets[n_globals] = val;
            var_types[n_globals] = TF_CONST | TY_INT;
            ++n_globals; n_vars = n_globals; next();
            if (token == '=') { next(); val = num_val; var_offsets[n_globals - 1] = val; next(); }
            ++val; if (token == ',') next();
        }
        expect('}'); expect(';'); return;
    }

    int is_byte = (token == T_CHAR);
    if (token == T_INT || token == T_CHAR || token == T_VOID) next();
    int is_ptr = 0; while(token=='*') { is_ptr = 1; next(); }
    char name[64]; strcpy(name, str_val); expect(T_IDENT);
    
    if (token == ';' || token == '[') {
        int ty = TF_GLOBAL | (is_ptr ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT));
        if (token == '[') {
            next(); int sz=0;
            if (token == T_NUM) { sz = num_val; next(); }
            else if (token == T_IDENT) {
                int i = find_var(str_val);
                if (i < 0 || !(var_types[i] & TF_CONST)) {
                    printf("Error: Line %d: Undefined constant: %s\n", line, str_val); exit(1);
                }
                sz = var_offsets[i]; next();
            } else { printf("Error: Line %d: Array size expected\n", line); exit(1); }
            expect(']');
            ty = TF_GLOBAL | (is_byte ? TY_BYTEARR : TY_INTARR);
            var_offsets[n_globals] = bss_size;
            bss_size = bss_size + ((ty & TF_BYTE) ? align4(sz) : sz * 4);
        } else {
            var_offsets[n_globals] = bss_size; bss_size = bss_size + 4;
        }
        var_name_off[n_globals] = add_name(name);
        var_types[n_globals] = ty;
        ++n_globals; n_vars = n_globals;
        expect(';'); return;
    }

    expect('('); n_vars = n_globals; locals = 32; int n_args = 0; // Reserve 32 bytes for base save area
    while (token != ')') {
        is_byte = 0; int ptr_count = 0;
        if (token == T_CHAR) { is_byte = 1; next(); }
        else if (token == T_INT || token == T_VOID || token == T_IDENT || token == T_ELLIPSIS) next();
        while (token == '*') { ++ptr_count; next(); }
        if (token == T_IDENT) {
            var_name_off[n_vars] = add_name(str_val);
            var_offsets[n_vars] = locals;
            var_types[n_vars] = ptr_count >= 2 ? TY_INTPTR : ptr_count ? (is_byte ? TY_BYTEPTR : TY_INTPTR) : (is_byte ? TY_BYTE : TY_INT);
            locals = locals + 4; ++n_vars; ++n_args;
            if (n_vars >= MAX_VARS) { printf("MAX_VARS exceeded\n"); exit(1); }
            next();
        }
        if (token == ',') next();
    }
    expect(')');
    if (token == ';') { next(); return; } // Prototype

    int i = get_func(name); int entry_addr = code_size; func_addrs[i] = entry_addr;
    expect('{');
    int stack_sz = (MAX_LOCAL_VARS*4 + 32 + 15) & ~15;
    emit_entry(stack_sz);
    int j=0; while(j<n_args) { emit_s32i(2+j, SP_REG, var_offsets[n_globals + j]); ++j; }
    while(token!='}' && token!=T_EOF) parse_stmt();
    int actual_stack_sz_imm12 = ((locals + 32 + 15) & ~15) / 8;
    code_data[entry_addr + 1] = (actual_stack_sz_imm12 << 4) | 1;
    code_data[entry_addr + 2] = actual_stack_sz_imm12 >> 4;
    emit_retw_n(); expect('}');
}

void write_elf(char *out) {
    int code_start = n_lits * 4;
    int off = 52; // Ehdr
    int text_off = off; int text_addr = off;
    off = off + align4(code_start + code_size);
    int rodata_off = off; int rodata_addr = off;
    off = off + align4(rodata_sz);
    int bss_off = off; int bss_addr = off;

    int n_syms = n_funcs + 1;
    char *syms = calloc(n_funcs+1, 16);
    char *strtab = calloc(1, STRTAB); int str_off = 1;
    char *lits = calloc(1, code_start); 
    char *rels = calloc(n_lits, 12);
    char *shdr=calloc(1, 320); // 8 sections * 40 bytes

    int fname_len = 0;
    int i = 0; while (i < n_funcs) {
        put32(syms + (i+1)*16, str_off);
        fname_len = strlen(name_buf + func_name_off[i]);
        if (str_off + fname_len >= STRTAB) { printf("strtab overflow on func %d len=%d name=%s\n", i, fname_len, name_buf + func_name_off[i]); exit(1); }
        strcpy(strtab + str_off, name_buf + func_name_off[i]); str_off = str_off + fname_len+1;
        int is_ext = (func_addrs[i] == -1);
        syms[(i+1)*16 + 12] = (1<<4)|(is_ext ? 0 : 2); // ST_INFO
        put16(syms + (i+1)*16 + 14, is_ext ? 0 : 1);   // shndx
        if(!is_ext) put32(syms + (i+1)*16 + 4, code_start + func_addrs[i]);
        ++i;
    }

    int n_rels = 0;
    i=0; while(i<n_lits) {
        int val = 0; int r_offset = text_addr + i*4;
        if (lit_types[i] == L_INT) val = lit_vals[i];
        else if (lit_types[i] == L_STR) {
            val = rodata_addr + lit_vals[i];
            put32(rels + n_rels*12, r_offset); 
            put32(rels + n_rels*12 + 4, R_XTENSA_RELATIVE); 
            ++n_rels;
        } else if (lit_types[i] == L_FUNC) {
            int fidx = lit_vals[i];
            if (func_addrs[fidx] == -1) { 
                put32(rels + n_rels*12, r_offset); 
                put32(rels + n_rels*12 + 4, ((fidx+1)<<8)|R_XTENSA_JMP_SLOT); 
                ++n_rels;
            } else { 
                val = text_addr + code_start + func_addrs[fidx];
                put32(rels + n_rels*12, r_offset); 
                put32(rels + n_rels*12 + 4, R_XTENSA_RELATIVE); 
                ++n_rels;
            }
        } else if (lit_types[i] == L_BSS) {
            val = bss_addr + lit_vals[i];
            put32(rels + n_rels*12, r_offset);
            put32(rels + n_rels*12 + 4, R_XTENSA_RELATIVE);
            ++n_rels;
        }
        memcpy(lits + i*4, &val, 4); ++i;
    }

    i = 0; while (i < n_patches) {
        int target = text_addr + patch_lits[i] * 4;
        int pc = text_addr + code_start + patch_offs[i];
        int imm = (target - ((pc+3)&~3)) >> 2;
        code_data[patch_offs[i] + 1] = imm;
        code_data[patch_offs[i] + 2] = imm>>8;
        ++i;
    }

    // Section Headers
    int rela_off=off; off = off + align4(n_rels * 12);
    int symtab_off=off; off = off + n_syms * 16;
    int strtab_off=off; off = off + str_off;
    char *shstrtab = "\0.text\0.rodata\0.bss\0.rela\0.symtab\0.strtab\0.shstrtab\0";
    int shstrtab_off = off; off = off + 53; // sizeof(shstrtab) 
    
    // 1: .text
    put32(shdr+40, 1); put32(shdr+44, 1); put32(shdr+48, 6); put32(shdr+52, text_addr); 
    put32(shdr+56, text_off); put32(shdr+60, code_start+code_size); put32(shdr+72, 4);
    // 2: .rodata
    put32(shdr+80, 7); put32(shdr+84, 1); put32(shdr+88, 2); put32(shdr+92, rodata_addr);
    put32(shdr+96, rodata_off); put32(shdr+100, align4(rodata_sz)); put32(shdr+112, 4);
    // 3: .bss
    put32(shdr+120, 15); put32(shdr+124, 8); put32(shdr+128, 3); put32(shdr+132, bss_addr);
    put32(shdr+136, bss_off); put32(shdr+140, bss_size); put32(shdr+152, 4);
    // 4: .rela
    put32(shdr+160, 20); put32(shdr+164, 4); put32(shdr+168, 2); put32(shdr+176, rela_off);
    put32(shdr+180, n_rels*12); put32(shdr+184, 5); put32(shdr+188, 1); put32(shdr+192, 4); put32(shdr+196, 12);
    // 5: .symtab
    put32(shdr+200, 26); put32(shdr+204, 2); put32(shdr+216, symtab_off);
    put32(shdr+220, n_syms*16); put32(shdr+224, 6); put32(shdr+228, 1); put32(shdr+232, 4); put32(shdr+236, 16);
    // 6: .strtab
    put32(shdr+240, 34); put32(shdr+244, 3); put32(shdr+256, strtab_off);
    put32(shdr+260, str_off); put32(shdr+272, 1);
    // 7: .shstrtab
    put32(shdr+280, 42); put32(shdr+284, 3); put32(shdr+296, shstrtab_off);
    put32(shdr+300, 53); put32(shdr+312, 1); // 53 == sizeof(shstrtab)

    int entry_func = get_func("main");
    int entry_vaddr = text_addr + code_start + func_addrs[entry_func];
    
    char ehdr[52]; memset(ehdr, 0, 52);
    ehdr[0]=0x7f; ehdr[1]='E'; ehdr[2]='L'; ehdr[3]='F'; ehdr[4]=1; ehdr[5]=1; ehdr[6]=1;
    put16(ehdr+16, 1); put16(ehdr+18, 94); // e_type=ET_REL, e_machine=XTENSA
    put32(ehdr+20, 1); put32(ehdr+24, entry_vaddr); 
    put32(ehdr+32, align4(off)); put32(ehdr+36, 0x300); put16(ehdr+40, 52);
    put16(ehdr+42, 0); put16(ehdr+44, 0); put16(ehdr+46, 40); put16(ehdr+48, 8); put16(ehdr+50, 7);

    int f = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644); 
    if (f < 0) { printf("Cannot open output file: %s\n", out); exit(1); }
    
    write(f, ehdr, 52); write(f, lits, code_start);
    write(f, code_data, code_size); free(code_data);
    
    lseek(f, rodata_off, SEEK_SET); write(f, rodata, rodata_sz);
    lseek(f, rela_off, SEEK_SET); write(f, rels, n_rels * 12);
    lseek(f, symtab_off, SEEK_SET); write(f, syms, n_syms * 16);
    lseek(f, strtab_off, SEEK_SET); write(f, strtab, str_off);
    lseek(f, shstrtab_off, SEEK_SET); write(f, shstrtab, 53); // 53 == sizeof(shstrtab)
    lseek(f, align4(off), SEEK_SET); write(f, shdr, 320);
    
    close(f); free(lits); free(rels); free(syms); free(strtab); free(shdr);
}

void print_stats(char *outfile, int t_ms) {
    int fsz = 0; int f = open(outfile, O_RDONLY, 0);
    if (f >= 0) { fsz = lseek(f, 0, SEEK_END); close(f); }
    else printf("open() failed for stats: %s\n", outfile);
    int speed = t_ms > 0 ? (line * 1000) / t_ms : 0;
    printf("\n[ xcc700 ] BUILD COMPLETED > OK\n");
    printf("> IN  : %d Lines / %d Tokens\n", line, token_cnt);
    printf("> SYM : %d Funcs / %d Globals\n", n_funcs, n_globals);
    printf("> REL : %d Literals / %d Patches\n", n_lits, n_patches);
    printf("> MEM : %d B .rodata / %d B .bss\n", rodata_sz, bss_size);
    printf("> OUT : %d B .text / %d B ELF\n", code_size, fsz);
    printf("[ %d ms ] >> %d Lines/sec <<\n", t_ms, speed);
}