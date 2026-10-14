/*
 * rvasm: a two-pass assembler for RISC-V RV32I.
 *
 * Pass 1 walks the source to assign an address to every label.
 * Pass 2 encodes each instruction/directive into machine code.
 *
 * Usage: rvasm [-f bin|hex|list] [-o out] input.s
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 512
#define MAX_TOKENS 8
#define MAX_SYMBOLS 4096

/* ---- small helpers ---------------------------------------------------- */

static const char *g_file = "<input>";
static int g_line = 0;
static int g_errors = 0;

static void error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s:%d: error: ", g_file, g_line);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  g_errors++;
}

typedef struct {
  uint8_t *data;
  size_t len, cap;
} Buffer;

static void buf_push(Buffer *b, uint8_t byte)
{
  if (b->len == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 1024;
    b->data = realloc(b->data, b->cap);
    if (!b->data) {
      perror("realloc");
      exit(1);
    }
  }
  b->data[b->len++] = byte;
}

static void buf_word(Buffer *b, uint32_t w, int bytes)
{
  for (int i = 0; i < bytes; i++)
    buf_push(b, (uint8_t)(w >> (8 * i))); /* little endian */
}

/* ---- symbol table ----------------------------------------------------- */

typedef struct {
  char name[64];
  int64_t value;
} Symbol;

static Symbol g_symbols[MAX_SYMBOLS];
static int g_nsymbols = 0;

static Symbol *sym_find(const char *name)
{
  for (int i = 0; i < g_nsymbols; i++)
    if (strcmp(g_symbols[i].name, name) == 0)
      return &g_symbols[i];
  return NULL;
}

static void sym_define(const char *name, int64_t value)
{
  if (sym_find(name)) {
    error("symbol '%s' defined twice", name);
    return;
  }
  if (g_nsymbols == MAX_SYMBOLS || strlen(name) >= sizeof g_symbols[0].name) {
    error("too many symbols or name too long: '%s'", name);
    return;
  }
  snprintf(g_symbols[g_nsymbols].name, sizeof g_symbols[0].name, "%s", name);
  g_symbols[g_nsymbols++].value = value;
}

/* ---- registers -------------------------------------------------------- */

static const char *abi_names[32] = {
  "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
  "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
  "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static int parse_reg(const char *s)
{
  if (s[0] == 'x' && isdigit((unsigned char)s[1])) {
    char *end;
    long n = strtol(s + 1, &end, 10);
    if (*end == '\0' && n >= 0 && n < 32)
      return (int)n;
  }
  if (strcmp(s, "fp") == 0)
    return 8;
  for (int i = 0; i < 32; i++)
    if (strcmp(s, abi_names[i]) == 0)
      return i;
  error("unknown register '%s'", s);
  return 0;
}

/* ---- immediates ------------------------------------------------------- */

/* Parses one term: a number, a 'c' char literal or a symbol.
 * When `pass` is 1 unknown symbols are fine (value 0); in pass 2 they error. */
static int64_t parse_term(const char *s, int pass)
{
  if (s[0] == '\'' && s[1] && s[2] == '\'' && s[3] == '\0')
    return (unsigned char)s[1];

  char *end;
  errno = 0;
  int64_t v = strtoll(s, &end, 0);
  if (end != s && *end == '\0' && errno == 0)
    return v;

  if (!(isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == '.')) {
    error("bad immediate '%s'", s);
    return 0;
  }
  Symbol *sym = sym_find(s);
  if (!sym) {
    if (pass == 2)
      error("undefined symbol '%s'", s);
    return 0;
  }
  return sym->value;
}

/* Parses an expression of terms joined by + and -, e.g. "msg_end - msg + 1". */
static int64_t parse_imm(const char *s, int pass)
{
  char term[128];
  int64_t total = 0;
  int sign = 1;
  for (;;) {
    while (isspace((unsigned char)*s))
      s++;
    if (*s == '-' || *s == '+') { /* unary sign */
      if (*s == '-')
        sign = -sign;
      s++;
      continue;
    }
    size_t n = 0;
    if (*s == '\'') { /* char literals may contain + or - */
      n = s[1] && s[2] == '\'' ? 3 : 0;
    } else {
      while (s[n] && s[n] != '+' && s[n] != '-' && !isspace((unsigned char)s[n]))
        n++;
    }
    if (n == 0 || n >= sizeof term) {
      error("bad immediate '%s'", s);
      return 0;
    }
    memcpy(term, s, n);
    term[n] = '\0';
    total += sign * parse_term(term, pass);
    s += n;
    while (isspace((unsigned char)*s))
      s++;
    if (!*s)
      return total;
    if (*s != '+' && *s != '-') {
      error("bad immediate near '%s'", s);
      return 0;
    }
    sign = *s++ == '-' ? -1 : 1;
  }
}

static int fits_signed(int64_t v, int bits)
{
  return v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1));
}

static void check_range(int64_t v, int bits, const char *what)
{
  if (!fits_signed(v, bits))
    error("%s %lld does not fit in %d bits", what, (long long)v, bits);
}

/* ---- encoders --------------------------------------------------------- */

static uint32_t enc_r(uint32_t op, int rd, uint32_t f3, int rs1, int rs2, uint32_t f7)
{
  return f7 << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}

static uint32_t enc_i(uint32_t op, int rd, uint32_t f3, int rs1, int64_t imm)
{
  check_range(imm, 12, "immediate");
  return ((uint32_t)imm & 0xfff) << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}

static uint32_t enc_s(uint32_t op, uint32_t f3, int rs1, int rs2, int64_t imm)
{
  check_range(imm, 12, "offset");
  uint32_t u = (uint32_t)imm;
  return (u >> 5 & 0x7f) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 |
         (u & 0x1f) << 7 | op;
}

static uint32_t enc_b(uint32_t f3, int rs1, int rs2, int64_t off)
{
  check_range(off, 13, "branch offset");
  if (off & 1)
    error("branch offset %lld is not 2-byte aligned", (long long)off);
  uint32_t u = (uint32_t)off;
  return (u >> 12 & 1) << 31 | (u >> 5 & 0x3f) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 |
         f3 << 12 | (u >> 1 & 0xf) << 8 | (u >> 11 & 1) << 7 | 0x63;
}

static uint32_t enc_u(uint32_t op, int rd, int64_t imm20)
{
  if (imm20 < -(1 << 19) || imm20 >= (1 << 20))
    error("upper immediate %lld out of range", (long long)imm20);
  return ((uint32_t)imm20 & 0xfffff) << 12 | (uint32_t)rd << 7 | op;
}

static uint32_t enc_j(int rd, int64_t off)
{
  check_range(off, 21, "jump offset");
  if (off & 1)
    error("jump offset %lld is not 2-byte aligned", (long long)off);
  uint32_t u = (uint32_t)off;
  return (u >> 20 & 1) << 31 | (u >> 1 & 0x3ff) << 21 | (u >> 11 & 1) << 20 | (u >> 12 & 0xff) << 12 |
         (uint32_t)rd << 7 | 0x6f;
}

/* Split a 32-bit value into the hi20/lo12 pair used by lui/auipc + addi. */
static void split_hi_lo(int64_t v, int64_t *hi, int64_t *lo)
{
  int32_t value = (int32_t)(uint32_t)v;
  *hi = ((int64_t)value + 0x800) >> 12;
  *lo = value - (*hi << 12);
  *hi &= 0xfffff;
}

/* ---- instruction tables ----------------------------------------------- */

typedef struct {
  const char *name;
  uint32_t f3, f7;
} Op;

static const Op r_ops[] = {
  {"add", 0, 0x00}, {"sub", 0, 0x20}, {"sll", 1, 0},  {"slt", 2, 0},  {"sltu", 3, 0},
  {"xor", 4, 0},    {"srl", 5, 0},    {"sra", 5, 0x20}, {"or", 6, 0}, {"and", 7, 0},
};
static const Op i_ops[] = {
  {"addi", 0, 0}, {"slti", 2, 0}, {"sltiu", 3, 0}, {"xori", 4, 0}, {"ori", 6, 0}, {"andi", 7, 0},
};
static const Op shift_ops[] = {{"slli", 1, 0x00}, {"srli", 5, 0x00}, {"srai", 5, 0x20}};
static const Op load_ops[] = {{"lb", 0, 0}, {"lh", 1, 0}, {"lw", 2, 0}, {"lbu", 4, 0}, {"lhu", 5, 0}};
static const Op store_ops[] = {{"sb", 0, 0}, {"sh", 1, 0}, {"sw", 2, 0}};
static const Op branch_ops[] = {{"beq", 0, 0}, {"bne", 1, 0},  {"blt", 4, 0},
                                {"bge", 5, 0}, {"bltu", 6, 0}, {"bgeu", 7, 0}};

#define FIND(table, name) find_op(table, sizeof table / sizeof table[0], name)

static const Op *find_op(const Op *table, size_t n, const char *name)
{
  for (size_t i = 0; i < n; i++)
    if (strcmp(table[i].name, name) == 0)
      return &table[i];
  return NULL;
}

/* ---- tokenizer -------------------------------------------------------- */

typedef struct {
  char *label;            /* label defined on this line, or NULL */
  char *mnemonic;         /* instruction or directive, or NULL */
  char *args[MAX_TOKENS]; /* operands */
  int nargs;
  char *string;           /* for .ascii/.asciz: the raw quoted string */
} Line;

static char *trim(char *s)
{
  while (isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
  return s;
}

/* Strip comments (# or ;) that are not inside a string literal. */
static void strip_comment(char *s)
{
  int in_str = 0;
  char prev = '\0';
  for (; *s; prev = *s++) {
    if (*s == '"' && prev != '\\')
      in_str = !in_str;
    else if (!in_str && (*s == '#' || *s == ';')) {
      *s = '\0';
      return;
    }
  }
}

static int parse_line(char *text, Line *out)
{
  memset(out, 0, sizeof *out);
  strip_comment(text);
  char *s = trim(text);

  char *colon = strchr(s, ':');
  char *quote = strchr(s, '"');
  if (colon && (!quote || colon < quote)) {
    *colon = '\0';
    out->label = trim(s);
    s = trim(colon + 1);
  }
  if (!*s)
    return 0;

  out->mnemonic = s;
  while (*s && !isspace((unsigned char)*s))
    s++;
  if (*s)
    *s++ = '\0';
  s = trim(s);
  for (char *p = out->mnemonic; *p; p++)
    *p = (char)tolower((unsigned char)*p);

  if (*s == '"') {
    out->string = s;
    return 0;
  }
  /* Operands are separated by commas; "8(sp)" becomes two operands "8", "sp". */
  while (*s && out->nargs < MAX_TOKENS) {
    size_t n = strcspn(s, ",()");
    char sep = s[n];
    s[n] = '\0';
    char *arg = trim(s);
    if (*arg)
      out->args[out->nargs++] = arg;
    else if (sep == '(')
      out->args[out->nargs++] = "0"; /* "(sp)" means "0(sp)" */
    if (!sep)
      break;
    s += n + 1;
  }
  return 0;
}

/* ---- the assembler ---------------------------------------------------- */

typedef struct {
  Buffer out;
  int pass;
  uint32_t pc;
  FILE *listing; /* pass 2 listing output, or NULL */
  const char *source_line;
} Asm;

static void emit(Asm *a, uint32_t word)
{
  if (a->pass == 2) {
    buf_word(&a->out, word, 4);
    if (a->listing)
      fprintf(a->listing, "%08x:  %08x    %s\n", a->pc, word, a->source_line);
  }
  a->pc += 4;
}

static void emit_data(Asm *a, uint32_t value, int bytes)
{
  if (a->pass == 2)
    buf_word(&a->out, value, bytes);
  a->pc += (uint32_t)bytes;
}

static int expect(const Line *l, int n)
{
  if (l->nargs != n) {
    error("'%s' expects %d operand(s), got %d", l->mnemonic, n, l->nargs);
    return 0;
  }
  return 1;
}

#define REG(i) parse_reg(l->args[i])
#define IMM(i) parse_imm(l->args[i], a->pass)
/* PC-relative offset to a label/number operand */
#define REL(i) (IMM(i) - (int64_t)a->pc)

static void emit_string(Asm *a, const Line *l, int zero_terminate)
{
  if (!l->string) {
    error("'%s' expects a string literal", l->mnemonic);
    return;
  }
  const char *s = l->string + 1;
  for (; *s && *s != '"'; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '\\' && s[1]) {
      s++;
      switch (*s) {
      case 'n': c = '\n'; break;
      case 't': c = '\t'; break;
      case 'r': c = '\r'; break;
      case '0': c = '\0'; break;
      default: c = (unsigned char)*s; break;
      }
    }
    emit_data(a, c, 1);
  }
  if (*s != '"')
    error("unterminated string");
  if (zero_terminate)
    emit_data(a, 0, 1);
}

static void assemble_directive(Asm *a, const Line *l)
{
  const char *m = l->mnemonic;
  if (strcmp(m, ".text") == 0 || strcmp(m, ".data") == 0 || strcmp(m, ".globl") == 0 ||
      strcmp(m, ".global") == 0 || strcmp(m, ".section") == 0 || strcmp(m, ".option") == 0)
    return; /* flat binary: sections and visibility are not meaningful */
  if (strcmp(m, ".word") == 0 || strcmp(m, ".half") == 0 || strcmp(m, ".byte") == 0) {
    int size = m[1] == 'w' ? 4 : m[1] == 'h' ? 2 : 1;
    for (int i = 0; i < l->nargs; i++)
      emit_data(a, (uint32_t)IMM(i), size);
  } else if (strcmp(m, ".ascii") == 0) {
    emit_string(a, l, 0);
  } else if (strcmp(m, ".asciz") == 0 || strcmp(m, ".string") == 0) {
    emit_string(a, l, 1);
  } else if (strcmp(m, ".align") == 0 || strcmp(m, ".p2align") == 0) {
    if (!expect(l, 1))
      return;
    uint32_t align = 1u << IMM(0);
    while (a->pc % align)
      emit_data(a, 0, 1);
  } else if (strcmp(m, ".zero") == 0 || strcmp(m, ".space") == 0) {
    if (!expect(l, 1))
      return;
    for (int64_t i = IMM(0); i > 0; i--)
      emit_data(a, 0, 1);
  } else if (strcmp(m, ".equ") == 0 || strcmp(m, ".set") == 0) {
    if (expect(l, 2) && a->pass == 1)
      sym_define(l->args[0], parse_imm(l->args[1], 1));
  } else {
    error("unknown directive '%s'", m);
  }
}

static void assemble_line(Asm *a, const Line *l)
{
  const char *m = l->mnemonic;
  const Op *op;

  if (m[0] == '.') {
    assemble_directive(a, l);
    return;
  }

  /* -- base instructions -- */
  if ((op = FIND(r_ops, m))) {
    if (expect(l, 3))
      emit(a, enc_r(0x33, REG(0), op->f3, REG(1), REG(2), op->f7));
  } else if ((op = FIND(i_ops, m))) {
    if (expect(l, 3))
      emit(a, enc_i(0x13, REG(0), op->f3, REG(1), IMM(2)));
  } else if ((op = FIND(shift_ops, m))) {
    if (!expect(l, 3))
      return;
    int64_t sh = IMM(2);
    if (sh < 0 || sh > 31)
      error("shift amount %lld out of range", (long long)sh);
    emit(a, enc_r(0x13, REG(0), op->f3, REG(1), (int)(sh & 31), op->f7));
  } else if ((op = FIND(load_ops, m))) {
    if (expect(l, 3)) /* lw rd, off(rs1) */
      emit(a, enc_i(0x03, REG(0), op->f3, REG(2), IMM(1)));
  } else if ((op = FIND(store_ops, m))) {
    if (expect(l, 3)) /* sw rs2, off(rs1) */
      emit(a, enc_s(0x23, op->f3, REG(2), REG(0), IMM(1)));
  } else if ((op = FIND(branch_ops, m))) {
    if (expect(l, 3))
      emit(a, enc_b(op->f3, REG(0), REG(1), REL(2)));
  } else if (strcmp(m, "lui") == 0 || strcmp(m, "auipc") == 0) {
    if (expect(l, 2))
      emit(a, enc_u(m[0] == 'l' ? 0x37 : 0x17, REG(0), IMM(1)));
  } else if (strcmp(m, "jal") == 0) {
    if (l->nargs == 1)
      emit(a, enc_j(1, REL(0)));
    else if (expect(l, 2))
      emit(a, enc_j(REG(0), REL(1)));
  } else if (strcmp(m, "jalr") == 0) {
    if (l->nargs == 1) /* jalr rs */
      emit(a, enc_i(0x67, 1, 0, REG(0), 0));
    else if (expect(l, 3)) /* jalr rd, off(rs1) */
      emit(a, enc_i(0x67, REG(0), 0, REG(2), IMM(1)));
  } else if (strcmp(m, "ecall") == 0) {
    emit(a, 0x00000073);
  } else if (strcmp(m, "ebreak") == 0) {
    emit(a, 0x00100073);
  } else if (strcmp(m, "fence") == 0) {
    emit(a, 0x0ff0000f);

  /* -- pseudo instructions -- */
  } else if (strcmp(m, "nop") == 0) {
    emit(a, enc_i(0x13, 0, 0, 0, 0));
  } else if (strcmp(m, "li") == 0) {
    if (!expect(l, 2))
      return;
    int64_t v = IMM(1);
    if (v < INT32_MIN || v > UINT32_MAX)
      error("li immediate %lld does not fit in 32 bits", (long long)v);
    int rd = REG(0);
    if (fits_signed(v, 12)) {
      emit(a, enc_i(0x13, rd, 0, 0, v));
    } else {
      int64_t hi, lo;
      split_hi_lo(v, &hi, &lo);
      emit(a, enc_u(0x37, rd, hi));
      if (lo)
        emit(a, enc_i(0x13, rd, 0, rd, lo));
    }
  } else if (strcmp(m, "la") == 0) {
    if (!expect(l, 2))
      return;
    int64_t hi, lo;
    split_hi_lo(REL(1), &hi, &lo);
    int rd = REG(0);
    emit(a, enc_u(0x17, rd, hi));
    emit(a, enc_i(0x13, rd, 0, rd, lo));
  } else if (strcmp(m, "call") == 0 || strcmp(m, "tail") == 0) {
    if (!expect(l, 1))
      return;
    int link = m[0] == 'c' ? 1 : 0;
    int tmp = m[0] == 'c' ? 1 : 6; /* call uses ra, tail uses t1 */
    int64_t hi, lo;
    split_hi_lo(REL(0), &hi, &lo);
    emit(a, enc_u(0x17, tmp, hi));
    emit(a, enc_i(0x67, link, 0, tmp, lo));
  } else if (strcmp(m, "mv") == 0) {
    if (expect(l, 2))
      emit(a, enc_i(0x13, REG(0), 0, REG(1), 0));
  } else if (strcmp(m, "not") == 0) {
    if (expect(l, 2))
      emit(a, enc_i(0x13, REG(0), 4, REG(1), -1));
  } else if (strcmp(m, "neg") == 0) {
    if (expect(l, 2))
      emit(a, enc_r(0x33, REG(0), 0, 0, REG(1), 0x20));
  } else if (strcmp(m, "seqz") == 0) {
    if (expect(l, 2))
      emit(a, enc_i(0x13, REG(0), 3, REG(1), 1));
  } else if (strcmp(m, "snez") == 0) {
    if (expect(l, 2))
      emit(a, enc_r(0x33, REG(0), 3, 0, REG(1), 0));
  } else if (strcmp(m, "j") == 0) {
    if (expect(l, 1))
      emit(a, enc_j(0, REL(0)));
  } else if (strcmp(m, "jr") == 0) {
    if (expect(l, 1))
      emit(a, enc_i(0x67, 0, 0, REG(0), 0));
  } else if (strcmp(m, "ret") == 0) {
    emit(a, enc_i(0x67, 0, 0, 1, 0));
  } else if (strcmp(m, "beqz") == 0 || strcmp(m, "bnez") == 0 || strcmp(m, "bltz") == 0 ||
             strcmp(m, "bgez") == 0) {
    static const uint32_t f3[] = {['e'] = 0, ['n'] = 1, ['l'] = 4, ['g'] = 5};
    if (expect(l, 2))
      emit(a, enc_b(f3[(unsigned char)m[1]], REG(0), 0, REL(1)));
  } else if (strcmp(m, "blez") == 0) { /* bge zero, rs */
    if (expect(l, 2))
      emit(a, enc_b(5, 0, REG(0), REL(1)));
  } else if (strcmp(m, "bgtz") == 0) { /* blt zero, rs */
    if (expect(l, 2))
      emit(a, enc_b(4, 0, REG(0), REL(1)));
  } else if (strcmp(m, "bgt") == 0 || strcmp(m, "ble") == 0 || strcmp(m, "bgtu") == 0 ||
             strcmp(m, "bleu") == 0) {
    /* swapped operands: bgt a, b == blt b, a */
    uint32_t f3 = (m[1] == 'g' ? 4 : 5) + (m[3] == 'u' ? 2 : 0);
    if (expect(l, 3))
      emit(a, enc_b(f3, REG(1), REG(0), REL(2)));
  } else {
    error("unknown instruction '%s'", m);
  }
}

static int assemble(FILE *in, Asm *a)
{
  char text[MAX_LINE], copy[MAX_LINE];
  for (a->pass = 1; a->pass <= 2; a->pass++) {
    rewind(in);
    a->pc = 0;
    g_line = 0;
    while (fgets(text, sizeof text, in)) {
      g_line++;
      text[strcspn(text, "\r\n")] = '\0';
      snprintf(copy, sizeof copy, "%s", text);
      a->source_line = trim(copy);
      Line l;
      parse_line(text, &l);
      if (l.label && a->pass == 1)
        sym_define(l.label, a->pc);
      if (l.mnemonic)
        assemble_line(a, &l);
    }
    if (g_errors)
      return 1;
  }
  return 0;
}

static void usage(void)
{
  fprintf(stderr, "usage: rvasm [-f bin|hex|list] [-o output] input.s\n"
                  "  bin   raw little-endian machine code (default with -o)\n"
                  "  hex   one 32-bit word per line, for $readmemh\n"
                  "  list  address, encoding and source (default to stdout)\n");
  exit(2);
}

int main(int argc, char **argv)
{
  const char *format = NULL, *output = NULL, *input = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
      format = argv[++i];
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      output = argv[++i];
    else if (argv[i][0] == '-' && argv[i][1])
      usage();
    else
      input = argv[i];
  }
  if (!input)
    usage();
  if (!format)
    format = output ? "bin" : "list";
  if (strcmp(format, "bin") && strcmp(format, "hex") && strcmp(format, "list"))
    usage();

  FILE *in = strcmp(input, "-") == 0 ? stdin : fopen(input, "r");
  if (!in) {
    perror(input);
    return 1;
  }
  g_file = input;

  /* stdin can't be rewound for pass 2, so copy it to a temp file first */
  if (in == stdin) {
    FILE *tmp = tmpfile();
    int c;
    while ((c = fgetc(stdin)) != EOF)
      fputc(c, tmp);
    in = tmp;
  }

  FILE *out = output ? fopen(output, strcmp(format, "bin") == 0 ? "wb" : "w") : stdout;
  if (!out) {
    perror(output);
    return 1;
  }

  Asm a = {0};
  if (strcmp(format, "list") == 0)
    a.listing = out;
  if (assemble(in, &a)) {
    fprintf(stderr, "%d error(s)\n", g_errors);
    if (output) {
      fclose(out);
      remove(output);
    }
    return 1;
  }

  if (strcmp(format, "bin") == 0) {
    fwrite(a.out.data, 1, a.out.len, out);
  } else if (strcmp(format, "hex") == 0) {
    for (size_t i = 0; i < a.out.len; i += 4) {
      uint32_t w = 0;
      for (size_t b = 0; b < 4 && i + b < a.out.len; b++)
        w |= (uint32_t)a.out.data[i + b] << (8 * b);
      fprintf(out, "%08x\n", w);
    }
  }
  if (out != stdout)
    fclose(out);
  free(a.out.data);
  return 0;
}
