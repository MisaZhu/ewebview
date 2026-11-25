/*
 * js_glsl.c - implementation of the GLSL ES 1.00 subset interpreter declared
 * in js_glsl.h. Pure C, no mario / DOM / GL dependency: it only parses a
 * shader into an AST and executes main() through caller-supplied get/set
 * callbacks. See js_glsl.h for the supported subset and the design rationale.
 */

#include "js_glsl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Small allocation helpers (zeroing). js_glsl.c is deliberately free of */
/* mario so it can be unit-tested with a plain host gcc build.           */
/* ------------------------------------------------------------------ */
static void* xz(size_t n) { void* p = malloc(n ? n : 1); if(p) memset(p, 0, n); return p; }
static char* xdup(const char* s) {
    if(s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if(p) memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */
enum { EXK_NUM, EXK_BOOL, EXK_VAR, EXK_CTOR, EXK_SWIZ, EXK_CALL,
       EXK_BIN, EXK_UN, EXK_TERN, EXK_ASSIGN };

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV,
       OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
       OP_LAND, OP_LOR,
       OP_ASSIGN, OP_ADD_A, OP_SUB_A, OP_MUL_A, OP_DIV_A };

enum { UN_NEG, UN_POS, UN_NOT };

typedef struct glsl_ex {
    int   k;
    double num;               /* EXK_NUM */
    int    bval;              /* EXK_BOOL */
    char*  name;              /* EXK_VAR / EXK_CALL */
    int    op;                /* EXK_BIN / EXK_UN / EXK_ASSIGN */
    int    ncomp;             /* EXK_CTOR target size, EXK_SWIZ comp count */
    int    mat;               /* EXK_CTOR: matN target dimension (0 = vector) */
    int    swiz[4];           /* EXK_SWIZ component indices */
    struct glsl_ex* a;        /* unary operand / binary lhs / ternary cond / assign target / swiz base */
    struct glsl_ex* b;        /* binary rhs / ternary true */
    struct glsl_ex* c;        /* ternary false */
    struct glsl_ex** args;    /* EXK_CALL / EXK_CTOR arguments */
    int    nargs;
} glsl_ex;

enum { STK_BLOCK, STK_DECL, STK_EXPR, STK_IF, STK_FOR, STK_WHILE, STK_RETURN, STK_DISCARD };

typedef struct glsl_st {
    int   k;
    int   nopop;              /* a declaration wrapper block: do NOT pop its
                               * locals at block end (they belong to the
                               * enclosing function scope, GLSL/C-like). */
    char* name;               /* STK_DECL variable name */
    int   type;               /* STK_DECL base component count */
    int   mat;                /* STK_DECL matN dimension (0 = not a matrix) */
    int   samp;               /* STK_DECL sampler2D flag */
    glsl_ex* init;            /* STK_DECL initializer / STK_RETURN value */
    glsl_ex* cond;            /* STK_IF / STK_WHILE / STK_FOR condition */
    glsl_ex* upd;             /* STK_FOR update expression */
    struct glsl_st* pre;      /* STK_FOR init statement */
    struct glsl_st** body;    /* STK_BLOCK children */
    int   nbody;
    struct glsl_st* s1;       /* STK_IF then / STK_FOR / STK_WHILE body */
    struct glsl_st* s2;       /* STK_IF else */
} glsl_st;

typedef struct { char* name; int kind; int base; int mat; int samp; } glsl_iface;

struct glsl_shader {
    int   stage;
    glsl_iface* iface; int niface, icap;
    glsl_st* main_body;       /* the block of void main() */
};

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */
enum { TK_EOF, TK_IDENT, TK_NUM, TK_PUNCT };
typedef struct { int k; char text[64]; double num; } tk;

static int is_ident_start(int c) { return (c=='_'||isalpha((unsigned char)c)); }
static int is_ident_char(int c)  { return (c=='_'||isalnum((unsigned char)c)); }

/* Tokenize the whole source into a NULL-terminated growable array. Comments
 * (// and block) and #-preprocessor lines are skipped; a malformed number is
 * still consumed so the lexer cannot stall. */
static tk* lex(const char* s, int* out_n) {
    int cap = 64, n = 0;
    tk* a = (tk*)xz((size_t)cap * sizeof(tk));
    if(a == NULL) { *out_n = 0; return NULL; }
    while(*s) {
        unsigned char c = (unsigned char)*s;
        if(isspace(c)) { s++; continue; }
        if(c=='/' && s[1]=='/') { while(*s && *s!='\n') s++; continue; }
        if(c=='/' && s[1]=='*') { s+=2; while(*s && !(*s=='*'&&s[1]=='/')) s++; if(*s) s+=2; continue; }
        if(c=='#') { while(*s && *s!='\n') s++; continue; }   /* skip preprocessor line */
        if(n+1 >= cap) {
            cap *= 2;
            tk* na = (tk*)realloc(a, (size_t)cap * sizeof(tk));
            if(na == NULL) break;
            a = na;
            memset(a + n, 0, (size_t)(cap - n) * sizeof(tk));
        }
        tk* t = &a[n];
        if(is_ident_start(c)) {
            int j = 0;
            while(*s && is_ident_char((unsigned char)*s) && j < 62) t->text[j++] = *s++;
            t->text[j] = 0;
            if(*s && is_ident_char((unsigned char)*s)) while(*s && is_ident_char((unsigned char)*s)) s++;
            t->k = TK_IDENT; n++; continue;
        }
        if(isdigit(c) || (c=='.' && isdigit((unsigned char)s[1]))) {
            char* end = NULL;
            if(c=='0' && (s[1]=='x' || s[1]=='X')) {
                long v = strtol(s, &end, 16); t->num = (double)v;
            } else {
                t->num = strtod(s, &end);
            }
            if(end == s) end = (char*)s + 1;    /* never stall */
            s = end;
            if(*s=='f' || *s=='F') s++;          /* trailing float suffix */
            t->k = TK_NUM; snprintf(t->text, sizeof(t->text), "%g", t->num); n++; continue;
        }
        /* punctuators: longest match first */
        {
            static const char* p2[] = { "<<=", ">>=", "<=", ">=", "==", "!=", "&&", "||",
                                        "++", "--", "+=", "-=", "*=", "/=", NULL };
            int matched = 0;
            for(int i = 0; p2[i] != NULL; ++i) {
                size_t L = strlen(p2[i]);
                if(strncmp(s, p2[i], L) == 0) {
                    snprintf(t->text, sizeof(t->text), "%s", p2[i]);
                    s += L; matched = 1; break;
                }
            }
            if(!matched) { t->text[0] = (char)c; t->text[1] = 0; s++; }
            t->k = TK_PUNCT; n++;
        }
    }
    a[n].k = TK_EOF; a[n].text[0] = 0;
    *out_n = n;
    return a;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    tk* a; int n, pos;
    int   stage;
    char* err;
} parser;

static tk* pk(parser* p)          { return &p->a[p->pos < p->n ? p->pos : p->n]; }
static tk* pk2(parser* p)         { int i = p->pos+1; return &p->a[i < p->n ? i : p->n]; }
static void adv(parser* p)        { if(p->pos < p->n) p->pos++; }
static int  is_punct(parser* p, const char* s) { tk* t = pk(p); return t->k==TK_PUNCT && strcmp(t->text,s)==0; }
static int  eat_punct(parser* p, const char* s){ if(is_punct(p,s)){ adv(p); return 1;} return 0; }
static int  is_ident(parser* p, const char* s) { tk* t = pk(p); return t->k==TK_IDENT && strcmp(t->text,s)==0; }
static int  eat_ident(parser* p, const char* s){ if(is_ident(p,s)){ adv(p); return 1;} return 0; }
static void perr(parser* p, const char* m) { if(p->err==NULL) p->err = xdup(m); }

static glsl_ex* new_ex(int k) { glsl_ex* e = (glsl_ex*)xz(sizeof(glsl_ex)); if(e) e->k = k; return e; }
static glsl_st* new_st(int k) { glsl_st* s = (glsl_st*)xz(sizeof(glsl_st)); if(s) s->k = k; return s; }

static int type_comps(const char* id) {
    if(!strcmp(id,"float")||!strcmp(id,"int")||!strcmp(id,"bool")) return 1;
    if(!strcmp(id,"vec2")||!strcmp(id,"ivec2")||!strcmp(id,"bvec2")) return 2;
    if(!strcmp(id,"vec3")||!strcmp(id,"ivec3")||!strcmp(id,"bvec3")) return 3;
    if(!strcmp(id,"vec4")||!strcmp(id,"ivec4")||!strcmp(id,"bvec4")) return 4;
    return 0;   /* not a scalar/vector type we support */
}
/* matN dimension (2/3/4), 0 for anything else. */
static int type_mat(const char* id) {
    if(!strcmp(id,"mat2")) return 2;
    if(!strcmp(id,"mat3")) return 3;
    if(!strcmp(id,"mat4")) return 4;
    return 0;
}
static int type_sampler(const char* id) {
    return !strcmp(id,"sampler2D");
}
static int is_type_ident(const char* id) {
    return type_comps(id) != 0 || type_mat(id) != 0 || type_sampler(id);
}

static glsl_ex* parse_expr(parser* p);
static glsl_st* parse_stmt(parser* p);
static glsl_st* parse_block(parser* p);

/* Parse a comma-separated argument list after '(' has been consumed; eats ')'. */
static int parse_args(parser* p, glsl_ex*** out) {
    glsl_ex** args = NULL; int n = 0, cap = 0;
    if(!is_punct(p, ")")) {
        for(;;) {
            glsl_ex* e = parse_expr(p);
            if(e == NULL) { free(args); return -1; }
            if(n == cap) { cap = cap ? cap*2 : 4; args = (glsl_ex**)realloc(args, (size_t)cap*sizeof(glsl_ex*)); }
            args[n++] = e;
            if(eat_punct(p, ",")) continue;
            break;
        }
    }
    if(!eat_punct(p, ")")) { perr(p, "expected ')'"); }
    *out = args; return n;
}

static int swiz_index(char ch) {
    switch(ch) {
        case 'x': case 'r': case 's': return 0;
        case 'y': case 'g': case 't': return 1;
        case 'z': case 'b': case 'p': return 2;
        case 'w': case 'a': case 'q': return 3;
        default: return -1;
    }
}

static glsl_ex* parse_postfix(parser* p, glsl_ex* base) {
    for(;;) {
        if(is_punct(p, ".")) {
            adv(p);
            tk* t = pk(p);
            if(t->k != TK_IDENT) { perr(p, "bad swizzle"); return base; }
            int idx[4], cnt = 0, ok = 1;
            for(int i = 0; t->text[i] && cnt < 4; ++i) {
                int si = swiz_index(t->text[i]);
                if(si < 0) { ok = 0; break; }
                idx[cnt++] = si;
            }
            adv(p);
            if(!ok || cnt == 0) { perr(p, "bad swizzle"); return base; }
            glsl_ex* sw = new_ex(EXK_SWIZ);
            sw->a = base; sw->ncomp = cnt;
            for(int i = 0; i < cnt; ++i) sw->swiz[i] = idx[i];
            base = sw;
            continue;
        }
        if(is_punct(p, "(") && base->k == EXK_VAR) {
            /* function call */
            adv(p);
            glsl_ex** args = NULL;
            int n = parse_args(p, &args);
            if(n < 0) return base;
            base->k = EXK_CALL;
            base->args = args; base->nargs = n;
            continue;
        }
        break;
    }
    return base;
}

static glsl_ex* parse_primary(parser* p) {
    tk* t = pk(p);
    if(t->k == TK_NUM) {
        adv(p);
        glsl_ex* e = new_ex(EXK_NUM); e->num = t->num; return e;
    }
    if(is_punct(p, "(")) {
        adv(p);
        glsl_ex* e = parse_expr(p);
        eat_punct(p, ")");
        return e;
    }
    if(is_punct(p, "-") || is_punct(p, "+") || is_punct(p, "!")) {
        int op = is_punct(p,"-") ? UN_NEG : (is_punct(p,"+") ? UN_POS : UN_NOT);
        adv(p);
        glsl_ex* operand = parse_primary(p);   /* unary binds tighter than postfix chain start */
        operand = parse_postfix(p, operand);
        glsl_ex* e = new_ex(EXK_UN); e->op = op; e->a = operand; return e;
    }
    if(t->k == TK_IDENT) {
        if(!strcmp(t->text,"true")||!strcmp(t->text,"false")) {
            adv(p); glsl_ex* e = new_ex(EXK_BOOL); e->bval = (t->text[0]=='t'); return e;
        }
        if(is_type_ident(t->text) && pk2(p)->k==TK_PUNCT && !strcmp(pk2(p)->text,"(")) {
            int nc = type_comps(t->text);
            int mt = type_mat(t->text);
            adv(p);   /* type name */
            adv(p);   /* '(' */
            glsl_ex** args = NULL;
            int n = parse_args(p, &args);
            glsl_ex* e = new_ex(EXK_CTOR);
            e->ncomp = mt ? mt : nc; e->mat = mt;
            e->args = args; e->nargs = (n<0?0:n);
            return e;
        }
        adv(p);
        glsl_ex* e = new_ex(EXK_VAR); e->name = xdup(t->text);
        return parse_postfix(p, e);
    }
    perr(p, "unexpected token in expression");
    adv(p);   /* consume to avoid stall */
    return new_ex(EXK_NUM);
}

/* precedence levels (loosest first) */
static glsl_ex* parse_mul(parser* p) {
    glsl_ex* a = parse_primary(p);
    for(;;) {
        int op = -1;
        if(is_punct(p,"*")) op = OP_MUL; else if(is_punct(p,"/")) op = OP_DIV;
        if(op < 0) break;
        adv(p);
        glsl_ex* b = parse_primary(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = op; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_add(parser* p) {
    glsl_ex* a = parse_mul(p);
    for(;;) {
        int op = -1;
        if(is_punct(p,"+")) op = OP_ADD; else if(is_punct(p,"-")) op = OP_SUB;
        if(op < 0) break;
        adv(p);
        glsl_ex* b = parse_mul(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = op; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_rel(parser* p) {
    glsl_ex* a = parse_add(p);
    for(;;) {
        int op = -1;
        if(is_punct(p,"<=")) op = OP_LE; else if(is_punct(p,">=")) op = OP_GE;
        else if(is_punct(p,"<")) op = OP_LT; else if(is_punct(p,">")) op = OP_GT;
        if(op < 0) break;
        adv(p);
        glsl_ex* b = parse_add(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = op; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_eq(parser* p) {
    glsl_ex* a = parse_rel(p);
    for(;;) {
        int op = -1;
        if(is_punct(p,"==")) op = OP_EQ; else if(is_punct(p,"!=")) op = OP_NE;
        if(op < 0) break;
        adv(p);
        glsl_ex* b = parse_rel(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = op; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_land(parser* p) {
    glsl_ex* a = parse_eq(p);
    while(is_punct(p,"&&")) {
        adv(p); glsl_ex* b = parse_eq(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = OP_LAND; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_lor(parser* p) {
    glsl_ex* a = parse_land(p);
    while(is_punct(p,"||")) {
        adv(p); glsl_ex* b = parse_land(p);
        glsl_ex* e = new_ex(EXK_BIN); e->op = OP_LOR; e->a = a; e->b = b; a = e;
    }
    return a;
}
static glsl_ex* parse_ternary(parser* p) {
    glsl_ex* cond = parse_lor(p);
    if(eat_punct(p, "?")) {
        glsl_ex* t1 = parse_expr(p);
        eat_punct(p, ":");
        glsl_ex* f1 = parse_ternary(p);
        glsl_ex* e = new_ex(EXK_TERN); e->a = cond; e->b = t1; e->c = f1;
        return e;
    }
    return cond;
}
static int assign_op(parser* p) {
    if(is_punct(p,"="))  return OP_ASSIGN;
    if(is_punct(p,"+=")) return OP_ADD_A;
    if(is_punct(p,"-=")) return OP_SUB_A;
    if(is_punct(p,"*=")) return OP_MUL_A;
    if(is_punct(p,"/=")) return OP_DIV_A;
    return -1;
}
static glsl_ex* parse_expr(parser* p) {
    glsl_ex* lhs = parse_ternary(p);
    int op = assign_op(p);
    if(op >= 0) {
        adv(p);
        glsl_ex* rhs = parse_expr(p);   /* right associative */
        glsl_ex* e = new_ex(EXK_ASSIGN); e->op = op; e->a = lhs; e->b = rhs;
        return e;
    }
    return lhs;
}

/* A declaration statement: `typespec name [= expr] (, name [= expr])* ;`.
 * Returns a STK_BLOCK holding one STK_DECL per declarator. The leading type
 * token has NOT been consumed yet. */
static glsl_st* parse_decl_stmt(parser* p) {
    int base = type_comps(pk(p)->text);
    int mt = type_mat(pk(p)->text);
    int sp = type_sampler(pk(p)->text);
    adv(p);   /* type */
    glsl_st* blk = new_st(STK_BLOCK);
    glsl_st** items = NULL; int n = 0, cap = 0;
    for(;;) {
        tk* idt = pk(p);
        if(idt->k != TK_IDENT) { perr(p, "expected identifier in declaration"); break; }
        adv(p);
        glsl_st* d = new_st(STK_DECL);
        d->name = xdup(idt->text); d->type = base; d->mat = mt; d->samp = sp;
        if(eat_punct(p, "=")) d->init = parse_expr(p);
        if(n == cap) { cap = cap?cap*2:4; items = (glsl_st**)realloc(items, (size_t)cap*sizeof(glsl_st*)); }
        items[n++] = d;
        if(eat_punct(p, ",")) continue;
        break;
    }
    eat_punct(p, ";");
    blk->body = items; blk->nbody = n;
    blk->nopop = 1;   /* declared locals live in the enclosing scope, not this
                       * synthetic wrapper block - do not pop them here. */
    return blk;
}

static glsl_st* parse_block(parser* p) {
    glsl_st* blk = new_st(STK_BLOCK);
    if(!eat_punct(p, "{")) { perr(p, "expected '{'"); return blk; }
    glsl_st** items = NULL; int n = 0, cap = 0;
    while(!is_punct(p, "}") && pk(p)->k != TK_EOF) {
        glsl_st* s = parse_stmt(p);
        if(s == NULL) break;
        if(n == cap) { cap = cap?cap*2:8; items = (glsl_st**)realloc(items, (size_t)cap*sizeof(glsl_st*)); }
        items[n++] = s;
    }
    eat_punct(p, "}");
    blk->body = items; blk->nbody = n;
    return blk;
}

static glsl_st* parse_stmt(parser* p) {
    tk* t = pk(p);
    if(t->k == TK_EOF) return NULL;
    if(is_punct(p, ";")) { adv(p); return new_st(STK_BLOCK); }
    if(is_punct(p, "{")) return parse_block(p);
    if(t->k == TK_IDENT) {
        if(is_type_ident(t->text)) return parse_decl_stmt(p);
        if(!strcmp(t->text,"if")) {
            adv(p); eat_punct(p,"(");
            glsl_st* s = new_st(STK_IF);
            s->cond = parse_expr(p); eat_punct(p,")");
            s->s1 = parse_stmt(p);
            if(eat_ident(p,"else")) s->s2 = parse_stmt(p);
            return s;
        }
        if(!strcmp(t->text,"while")) {
            adv(p); eat_punct(p,"(");
            glsl_st* s = new_st(STK_WHILE);
            s->cond = parse_expr(p); eat_punct(p,")");
            s->s1 = parse_stmt(p);
            return s;
        }
        if(!strcmp(t->text,"for")) {
            adv(p); eat_punct(p,"(");
            glsl_st* s = new_st(STK_FOR);
            if(!is_punct(p,";")) {
                if(is_type_ident(pk(p)->text)) s->pre = parse_decl_stmt(p);
                else { glsl_ex* e = parse_expr(p); eat_punct(p,";"); glsl_st* es = new_st(STK_EXPR); es->init = e; s->pre = es; }
            } else eat_punct(p,";");
            if(!is_punct(p,";")) s->cond = parse_expr(p);
            eat_punct(p,";");
            if(!is_punct(p,")")) s->upd = parse_expr(p);
            eat_punct(p,")");
            s->s1 = parse_stmt(p);
            return s;
        }
        if(!strcmp(t->text,"return")) {
            adv(p);
            glsl_st* s = new_st(STK_RETURN);
            if(!is_punct(p,";")) s->init = parse_expr(p);
            eat_punct(p,";");
            return s;
        }
        if(!strcmp(t->text,"discard")) {
            adv(p); eat_punct(p,";");
            return new_st(STK_DISCARD);
        }
    }
    /* expression statement */
    {
        glsl_ex* e = parse_expr(p);
        eat_punct(p, ";");
        glsl_st* s = new_st(STK_EXPR); s->init = e; return s;
    }
}

/* Record an interface variable (attribute/varying/uniform) for reflection. */
static void iface_add(glsl_shader* sh, const char* name, int kind, int base, int mat, int samp) {
    for(int i = 0; i < sh->niface; ++i)
        if(!strcmp(sh->iface[i].name, name)) return;   /* dedupe across stages */
    if(sh->niface == sh->icap) {
        sh->icap = sh->icap ? sh->icap*2 : 8;
        sh->iface = (glsl_iface*)realloc(sh->iface, (size_t)sh->icap*sizeof(glsl_iface));
    }
    sh->iface[sh->niface].name = xdup(name);
    sh->iface[sh->niface].kind = kind;
    sh->iface[sh->niface].base = base;
    sh->iface[sh->niface].mat = mat;
    sh->iface[sh->niface].samp = samp;
    sh->niface++;
}

/* Parse one top-level declaration (qualifier* typespec name [, name]* ;).
 * `qual` is -1 for none, else GLSL_VAR_*. */
static void parse_top_decl(parser* p, int qual, glsl_shader* sh) {
    int base = type_comps(pk(p)->text);
    int mt = type_mat(pk(p)->text);
    int sp = type_sampler(pk(p)->text);
    if(base == 0 && mt == 0 && sp == 0) { perr(p, "unsupported top-level type"); while(pk(p)->k!=TK_EOF && !is_punct(p,";")) adv(p); eat_punct(p,";"); return; }
    adv(p);   /* type */
    for(;;) {
        tk* idt = pk(p);
        if(idt->k != TK_IDENT) { perr(p, "expected identifier"); break; }
        adv(p);
        if(qual >= 0) iface_add(sh, idt->text, qual, base, mt, sp);
        if(eat_punct(p,"=")) { glsl_ex* e = parse_expr(p); (void)e; }   /* const init: parsed, unused */
        if(eat_punct(p,",")) continue;
        break;
    }
    eat_punct(p,";");
}

glsl_shader* glsl_compile(const char* src, int stage, char** err) {
    if(err) *err = NULL;
    if(src == NULL) { if(err) *err = xdup("null source"); return NULL; }
    int ntok = 0;
    tk* toks = lex(src, &ntok);
    if(toks == NULL) { if(err) *err = xdup("lex failed"); return NULL; }
    parser P; memset(&P, 0, sizeof(P));
    P.a = toks; P.n = ntok; P.pos = 0; P.stage = stage;

    glsl_shader* sh = (glsl_shader*)xz(sizeof(glsl_shader));
    sh->stage = stage;

    while(pk(&P)->k != TK_EOF) {
        tk* t = pk(&P);
        if(t->k == TK_IDENT && !strcmp(t->text,"precision")) {
            adv(&P);
            /* precisionqual type ; */
            while(pk(&P)->k==TK_IDENT && !is_type_ident(pk(&P)->text)) adv(&P);   /* lowp/mediump/highp */
            if(is_type_ident(pk(&P)->text)) adv(&P);
            eat_punct(&P,";");
            continue;
        }
        if(t->k == TK_IDENT && (!strcmp(t->text,"attribute")||!strcmp(t->text,"varying")||!strcmp(t->text,"uniform"))) {
            int qual = !strcmp(t->text,"attribute") ? GLSL_VAR_ATTRIB
                     : (!strcmp(t->text,"varying")  ? GLSL_VAR_VARYING : GLSL_VAR_UNIFORM);
            adv(&P);
            /* skip extra qualifiers (invariant, const, precision) */
            while(pk(&P)->k==TK_IDENT && !is_type_ident(pk(&P)->text) &&
                  strcmp(pk(&P)->text,"attribute")&&strcmp(pk(&P)->text,"varying")&&strcmp(pk(&P)->text,"uniform"))
                adv(&P);
            parse_top_decl(&P, qual, sh);
            continue;
        }
        if(t->k == TK_IDENT && (!strcmp(t->text,"const")||!strcmp(t->text,"invariant"))) {
            adv(&P);
            while(pk(&P)->k==TK_IDENT && !is_type_ident(pk(&P)->text)) adv(&P);
            parse_top_decl(&P, -1, sh);
            continue;
        }
        /* function definition: [qual] rettype name '(' params ')' block */
        if(t->k == TK_IDENT) {
            /* return type: 'void' or a typespec */
            if(is_type_ident(t->text) || !strcmp(t->text,"void")) {
                adv(&P);   /* return type */
                tk* nt = pk(&P);
                if(nt->k != TK_IDENT) { perr(&P, "expected function name"); break; }
                char* fname = xdup(nt->text);
                adv(&P);
                if(!eat_punct(&P,"(")) { perr(&P,"expected '('"); free(fname); break; }
                while(!is_punct(&P,")") && pk(&P)->k!=TK_EOF) adv(&P);   /* skip params */
                eat_punct(&P,")");
                glsl_st* body = is_punct(&P,"{") ? parse_block(&P) : parse_stmt(&P);
                if(!strcmp(fname,"main")) sh->main_body = body;
                free(fname);
                continue;
            }
        }
        perr(&P, "unexpected top-level token");
        adv(&P);
    }

    free(toks);
    if(P.err != NULL) {
        if(err) *err = P.err; else free(P.err);
        /* A parse error means the shader does not compile (honest GLSL behaviour). */
        glsl_free(sh);
        return NULL;
    }
    if(sh->main_body == NULL) {
        if(err) *err = xdup("no main()");
        glsl_free(sh);
        return NULL;
    }
    return sh;
}

void glsl_free(glsl_shader* s) {
    if(s == NULL) return;
    /* AST is leaked-by-design only if we tracked it; we do a shallow free of the
     * interface table and main body via recursive helpers. */
    for(int i = 0; i < s->niface; ++i) free(s->iface[i].name);
    free(s->iface);
    /* Free the statement/expression tree. */
    extern void glsl__free_st(glsl_st* st);
    glsl__free_st(s->main_body);
    free(s);
}

/* Recursive AST free (declared here, referenced by glsl_free above). */
static void free_ex(glsl_ex* e);
static void free_st(glsl_st* st);
static void free_ex(glsl_ex* e) {
    if(e == NULL) return;
    free(e->name);
    free_ex(e->a); free_ex(e->b); free_ex(e->c);
    for(int i = 0; i < e->nargs; ++i) free_ex(e->args[i]);
    free(e->args);
    free(e);
}
static void free_st(glsl_st* st) {
    if(st == NULL) return;
    free(st->name);
    free_ex(st->init); free_ex(st->cond); free_ex(st->upd);
    free_st(st->pre); free_st(st->s1); free_st(st->s2);
    for(int i = 0; i < st->nbody; ++i) free_st(st->body[i]);
    free(st->body);
    free(st);
}
void glsl__free_st(glsl_st* st) { free_st(st); }

int glsl_var_count(glsl_shader* s, int kind) {
    if(s == NULL) return 0;
    int c = 0;
    for(int i = 0; i < s->niface; ++i) if(s->iface[i].kind == kind) c++;
    return c;
}
const char* glsl_var_name(glsl_shader* s, int kind, int i) {
    if(s == NULL) return NULL;
    int c = 0;
    for(int j = 0; j < s->niface; ++j)
        if(s->iface[j].kind == kind) { if(c == i) return s->iface[j].name; c++; }
    return NULL;
}
int glsl_var_mat(glsl_shader* s, int kind, int i) {
    if(s == NULL) return 0;
    int c = 0;
    for(int j = 0; j < s->niface; ++j)
        if(s->iface[j].kind == kind) { if(c == i) return s->iface[j].mat; c++; }
    return 0;
}
int glsl_var_sampler(glsl_shader* s, int kind, int i) {
    if(s == NULL) return 0;
    int c = 0;
    for(int j = 0; j < s->niface; ++j)
        if(s->iface[j].kind == kind) { if(c == i) return s->iface[j].samp; c++; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */
typedef struct { char name[64]; glsl_val v; } local_t;

typedef struct {
    glsl_shader* sh;
    void* ud; glsl_getvar_fn getv; glsl_setvar_fn setv;
    glsl_sample_fn sample;
    local_t locals[96]; int nlocals;
    int discarded, returned;
} run_t;

static glsl_val vzero(int n) { glsl_val v; memset(&v, 0, sizeof(v)); v.n = n; return v; }
/* Identity matN (column-major). */
static glsl_val videntity(int dim) {
    glsl_val v = vzero(dim);
    v.mat = dim;
    for(int i = 0; i < dim; ++i) v.m[i * dim + i] = 1.0f;
    return v;
}
static int vtruthy(const glsl_val* v) { return (v->n >= 1) ? (v->c[0] != 0.0f) : 0; }

static local_t* find_local(run_t* r, const char* name) {
    for(int i = r->nlocals-1; i >= 0; --i)
        if(!strcmp(r->locals[i].name, name)) return &r->locals[i];
    return NULL;
}
static void set_local(run_t* r, const char* name, const glsl_val* v) {
    local_t* L = find_local(r, name);
    if(L) { L->v = *v; return; }
    if(r->nlocals < (int)(sizeof(r->locals)/sizeof(r->locals[0]))) {
        snprintf(r->locals[r->nlocals].name, sizeof(r->locals[0].name), "%s", name);
        r->locals[r->nlocals].v = *v;
        r->nlocals++;
    }
}

/* Read an interface/built-in input by name (locals take precedence). */
static glsl_val read_var(run_t* r, const char* name) {
    local_t* L = find_local(r, name);
    if(L) return L->v;
    glsl_val out = vzero(1);
    if(r->getv && r->getv(r->ud, name, &out)) return out;
    return vzero(4);   /* unknown: zero vec4 (harmless, keeps component reads in range) */
}

/* Component accessor with clamping (swizzle indices may exceed a scalar's n). */
static float comp(const glsl_val* v, int i) {
    if(v->n == 1) return v->c[0];      /* scalar broadcasts */
    if(i < v->n) return v->c[i];
    return 0.0f;
}

static glsl_val eval_ex(run_t* r, glsl_ex* e);
static void   exec_st(run_t* r, glsl_st* s);

static glsl_val do_ctor(glsl_ex* e, run_t* r) {
    int target = e->ncomp;
    /* matN constructors: identity (no args), scalar diagonal, matrix copy
     * (same or larger source -> upper-left submatrix), or column-major fill
     * from flattened scalar/vector arguments (GLSL ES 1.00 rules). */
    if(e->mat >= 2 && e->mat <= 4) {
        int dim = e->mat;
        glsl_val out = vzero(dim);
        out.mat = dim;
        if(e->nargs == 0) {
            for(int i = 0; i < dim; ++i) out.m[i * dim + i] = 1.0f;
            return out;
        }
        if(e->nargs == 1) {
            glsl_val a = eval_ex(r, e->args[0]);
            if(a.mat >= 2) {
                int src = a.mat;
                int cp = dim < src ? dim : src;
                for(int col = 0; col < cp; ++col)
                    for(int row = 0; row < cp; ++row)
                        out.m[col * dim + row] = a.m[col * src + row];
                return out;
            }
            if(a.n == 1) {
                for(int i = 0; i < dim; ++i) out.m[i * dim + i] = a.c[0];
                return out;
            }
            /* single vector shorter than dim*dim: fill leading elements */
            for(int i = 0; i < dim * dim && i < a.n; ++i) out.m[i] = a.c[i];
            return out;
        }
        int fill = 0;
        for(int i = 0; i < e->nargs && fill < dim * dim; ++i) {
            glsl_val a = eval_ex(r, e->args[i]);
            if(a.mat >= 2) {
                for(int j = 0; j < a.mat * a.mat && fill < dim * dim; ++j)
                    out.m[fill++] = a.m[j];
            } else {
                for(int j = 0; j < a.n && fill < dim * dim; ++j)
                    out.m[fill++] = a.c[j];
            }
        }
        return out;
    }
    glsl_val out = vzero(target);
    /* Single scalar argument broadcasts to all components (GLSL rule). */
    if(e->nargs == 1) {
        glsl_val a = eval_ex(r, e->args[0]);
        if(a.n == 1) { for(int i=0;i<target;++i) out.c[i]=a.c[0]; return out; }
        for(int i=0;i<target && i<a.n;++i) out.c[i]=a.c[i];
        return out;
    }
    int fill = 0;
    for(int i = 0; i < e->nargs && fill < target; ++i) {
        glsl_val a = eval_ex(r, e->args[i]);
        for(int j = 0; j < a.n && fill < target; ++j) out.c[fill++] = a.c[j];
    }
    return out;
}

static float fmin2(float a, float b){ return a<b?a:b; }
static float fmax2(float a, float b){ return a>b?a:b; }

/* Apply a scalar unary math builtin to every component. */
static glsl_val map1(const glsl_val* a, int fn) {
    glsl_val r = vzero(a->n);
    for(int i=0;i<a->n;++i) {
        float x = a->c[i], y = 0;
        switch(fn) {
            case 0: y = x*(float)M_PI/180.0f; break;      /* radians */
            case 1: y = x*180.0f/(float)M_PI; break;      /* degrees */
            case 2: y = sinf(x); break;
            case 3: y = cosf(x); break;
            case 4: y = tanf(x); break;
            case 5: y = asinf(x); break;
            case 6: y = acosf(x); break;
            case 7: y = fabsf(x); break;
            case 8: y = (x>0)?1.0f:((x<0)?-1.0f:0.0f); break;  /* sign */
            case 9: y = floorf(x); break;
            case 10: y = ceilf(x); break;
            case 11: y = x - floorf(x); break;            /* fract */
            case 12: y = sqrtf(x); break;
            case 13: y = expf(x); break;
            case 14: y = logf(x); break;
            case 15: y = exp2f(x); break;
            case 16: y = log2f(x); break;
            case 17: y = 1.0f/sqrtf(x); break;            /* inversesqrt */
            case 18: y = -x; break;                        /* reserved */
            default: y = x; break;
        }
        r.c[i] = y;
    }
    return r;
}

/* Component-wise binary with scalar broadcast (min/max/mod/pow/mix/step/...). */
static glsl_val map2(const glsl_val* a, const glsl_val* b, int fn) {
    glsl_val r = vzero((a->n > b->n) ? a->n : b->n);
    for(int i=0;i<r.n;++i) {
        float x = comp(a,i), y = comp(b,i), z = 0;
        switch(fn) {
            case 0: z = fmin2(x,y); break;                 /* min */
            case 1: z = fmax2(x,y); break;                 /* max */
            case 2: z = x - y*floorf(x/(y!=0?y:1e-20f)); break;  /* mod */
            case 3: z = powf(x,y); break;                  /* pow */
            case 4: z = (x<y)?0.0f:1.0f; break;            /* step(edge=x,x=y) */
            case 5: z = atan2f(x,y); break;                /* atan(y=x, x=y) */
            default: z = x; break;
        }
        r.c[i] = z;
    }
    return r;
}

static float vdot(const glsl_val* a, const glsl_val* b) {
    int n = (a->n<b->n)?a->n:b->n;
    float s = 0; for(int i=0;i<n;++i) s += comp(a,i)*comp(b,i);
    return s;
}
static float vlen(const glsl_val* a) { return sqrtf(vdot(a,a)); }

static glsl_val eval_call(run_t* r, glsl_ex* e) {
    const char* fn = e->name ? e->name : "";
    glsl_val A[4]; int na = e->nargs < 4 ? e->nargs : 4;
    for(int i=0;i<na;++i) A[i] = eval_ex(r, e->args[i]);

    if(!strcmp(fn,"texture2D")||!strcmp(fn,"texture")) {
        /* Real sampling when the GL layer supplied a callback: A[0] is the
         * sampler (c[0] = texture unit), A[1] the (s,t) coordinate. Without a
         * callback keep the historical opaque-black stub. */
        if(r->sample != NULL && na >= 2) {
            glsl_val out = vzero(4);
            if(r->sample(r->ud, (int)comp(&A[0],0), comp(&A[1],0), comp(&A[1],1), &out))
                return out;
        }
        glsl_val t = vzero(4); t.c[3]=1.0f; return t;
    }
    if(!strcmp(fn,"matrixCompMult") && na >= 2 && A[0].mat >= 2 && A[0].mat == A[1].mat) {
        glsl_val r = vzero(A[0].mat); r.mat = A[0].mat;
        for(int i = 0; i < r.mat * r.mat; ++i) r.m[i] = A[0].m[i] * A[1].m[i];
        return r;
    }
    if(!strcmp(fn,"float")) { glsl_val v = vzero(1); v.c[0] = na?comp(&A[0],0):0; return v; }
    if(!strcmp(fn,"int")||!strcmp(fn,"bool")) { glsl_val v = vzero(1); v.c[0] = na?(float)(int)comp(&A[0],0):0; return v; }
    if(na == 0) return vzero(1);

    /* 1-arg component-wise */
    struct { const char* n; int id; } m1[] = {
        {"radians",0},{"degrees",1},{"sin",2},{"cos",3},{"tan",4},{"asin",5},{"acos",6},
        {"abs",7},{"sign",8},{"floor",9},{"ceil",10},{"fract",11},{"sqrt",12},
        {"exp",13},{"log",14},{"exp2",15},{"log2",16},{"inversesqrt",17},{NULL,0}
    };
    for(int i=0;m1[i].n;++i) if(!strcmp(fn,m1[i].n)) return map1(&A[0], m1[i].id);

    if(!strcmp(fn,"atan")) return (na>=2) ? map2(&A[0],&A[1],5) : map1(&A[0],4);
    if(!strcmp(fn,"min"))  return map2(&A[0],&A[1],0);
    if(!strcmp(fn,"max"))  return map2(&A[0],&A[1],1);
    if(!strcmp(fn,"mod"))  return map2(&A[0],&A[1],2);
    if(!strcmp(fn,"pow"))  return map2(&A[0],&A[1],3);
    if(!strcmp(fn,"step")) return map2(&A[0],&A[1],4);

    if(!strcmp(fn,"length"))   { glsl_val v=vzero(1); v.c[0]=vlen(&A[0]); return v; }
    if(!strcmp(fn,"distance")) { glsl_val d = vzero(A[0].n); for(int i=0;i<d.n;++i) d.c[i]=comp(&A[0],i)-comp(&A[1],i); glsl_val v=vzero(1); v.c[0]=vlen(&d); return v; }
    if(!strcmp(fn,"dot"))      { glsl_val v=vzero(1); v.c[0]=vdot(&A[0],&A[1]); return v; }
    if(!strcmp(fn,"normalize")){ float L=vlen(&A[0]); glsl_val r=A[0]; if(L!=0) for(int i=0;i<r.n;++i) r.c[i]/=L; return r; }
    if(!strcmp(fn,"cross")) {
        glsl_val r=vzero(3);
        float x0=comp(&A[0],0),y0=comp(&A[0],1),z0=comp(&A[0],2);
        float x1=comp(&A[1],0),y1=comp(&A[1],1),z1=comp(&A[1],2);
        r.c[0]=y0*z1-z0*y1; r.c[1]=z0*x1-x0*z1; r.c[2]=x0*y1-y0*x1; return r;
    }
    if(!strcmp(fn,"reflect")) {
        float d=vdot(&A[1],&A[0]); glsl_val r=A[0];
        for(int i=0;i<r.n;++i) r.c[i]=comp(&A[0],i)-2.0f*d*comp(&A[1],i);
        return r;
    }
    if(!strcmp(fn,"mix")) {
        glsl_val r = vzero((A[0].n>A[1].n)?A[0].n:A[1].n);
        for(int i=0;i<r.n;++i){ float x=comp(&A[0],i),y=comp(&A[1],i),a=comp(&A[2],i); r.c[i]=x*(1.0f-a)+y*a; }
        return r;
    }
    if(!strcmp(fn,"clamp")) {
        glsl_val r=A[0];
        for(int i=0;i<r.n;++i){ float x=comp(&A[0],i); r.c[i]=fmin2(fmax2(x,comp(&A[1],i)),comp(&A[2],i)); }
        return r;
    }
    if(!strcmp(fn,"smoothstep")) {
        glsl_val r = vzero(A[2].n);
        for(int i=0;i<r.n;++i){
            float e0=comp(&A[0],i),e1=comp(&A[1],i),x=comp(&A[2],i);
            float denom=(e1-e0); float t=(denom!=0)?(x-e0)/denom:0.0f;
            t=fmin2(fmax2(t,0.0f),1.0f); r.c[i]=t*t*(3.0f-2.0f*t);
        }
        return r;
    }
    /* Unknown builtin: zero of the first arg's shape (never crash). */
    return vzero(A[0].n);
}

/* Matrix algebra helpers (column-major: element(row,col) = m[col*dim+row]). */
static glsl_val mat_mul_vec(const glsl_val* m, const glsl_val* v) {
    int dim = m->mat;
    glsl_val r = vzero(dim);
    for(int row = 0; row < dim; ++row) {
        float s = 0;
        for(int k = 0; k < dim; ++k) s += m->m[k * dim + row] * comp(v, k);
        r.c[row] = s;
    }
    return r;
}
static glsl_val vec_mul_mat(const glsl_val* v, const glsl_val* m) {
    int dim = m->mat;
    glsl_val r = vzero(dim);
    for(int col = 0; col < dim; ++col) {
        float s = 0;
        for(int k = 0; k < dim; ++k) s += comp(v, k) * m->m[col * dim + k];
        r.c[col] = s;
    }
    return r;
}
static glsl_val mat_mul_mat(const glsl_val* a, const glsl_val* b) {
    int dim = a->mat;   /* caller guarantees matching dimensions */
    glsl_val r = vzero(dim);
    r.mat = dim;
    for(int col = 0; col < dim; ++col)
        for(int row = 0; row < dim; ++row) {
            float s = 0;
            for(int k = 0; k < dim; ++k)
                s += a->m[k * dim + row] * b->m[col * dim + k];
            r.m[col * dim + row] = s;
        }
    return r;
}
/* Component-wise op of a matrix with a scalar or another same-dim matrix. */
static glsl_val mat_compwise(int op, const glsl_val* a, const glsl_val* b) {
    int dim = a->mat, ne = dim * dim;
    glsl_val r = vzero(dim);
    r.mat = dim;
    int bmat = (b->mat == dim);
    for(int i = 0; i < ne; ++i) {
        float x = a->m[i], y = bmat ? b->m[i] : comp(b, 0), z = 0;
        if(op==OP_ADD) z=x+y;
        else if(op==OP_SUB) z=x-y;
        else if(op==OP_MUL) z=x*y;
        else z = (y!=0)? x/y : (x!=0? (x>0?INFINITY:-INFINITY):0.0f);
        r.m[i] = z;
    }
    return r;
}

static glsl_val do_binop(int op, glsl_val a, glsl_val b) {
    /* GLSL matrix rules: mat*vec / vec*mat / mat*mat are linear algebra;
     * mat+mat, mat+-scalar, mat*scalar, mat/scalar are component-wise. */
    if(a.mat >= 2 || b.mat >= 2) {
        if(op == OP_MUL) {
            if(a.mat >= 2 && b.mat == 0 && b.n == a.mat) return mat_mul_vec(&a, &b);
            if(a.mat == 0 && b.mat >= 2 && a.n == b.mat) return vec_mul_mat(&a, &b);
            if(a.mat >= 2 && a.mat == b.mat) return mat_mul_mat(&a, &b);
            if(a.mat >= 2 && b.mat == 0 && b.n == 1) return mat_compwise(OP_MUL, &a, &b);
            if(b.mat >= 2 && a.mat == 0 && a.n == 1) {
                /* scalar * mat: component-wise, commute */
                return mat_compwise(OP_MUL, &b, &a);
            }
        }
        if(a.mat >= 2 && (op==OP_ADD || op==OP_SUB || op==OP_DIV) &&
           (b.mat == a.mat || (b.mat == 0 && b.n == 1)))
            return mat_compwise(op, &a, &b);
        if(b.mat >= 2 && (op==OP_ADD || op==OP_SUB) && a.mat == 0 && a.n == 1) {
            /* scalar + mat is component-wise; scalar - mat too */
            glsl_val am = vzero(b.mat); am.mat = b.mat;
            for(int i = 0; i < b.mat * b.mat; ++i) am.m[i] = a.c[0];
            return mat_compwise(op, &am, &b);
        }
        if(op == OP_EQ || op == OP_NE) {
            int eq = (a.mat == b.mat);
            if(eq) for(int i = 0; i < a.mat * a.mat; ++i)
                if(a.m[i] != b.m[i]) { eq = 0; break; }
            glsl_val r = vzero(1);
            r.c[0] = (op==OP_EQ) ? (eq?1.0f:0.0f) : (eq?0.0f:1.0f);
            return r;
        }
        /* Unsupported matrix combination: zero scalar (never crash). */
        return vzero(1);
    }
    switch(op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
            glsl_val r = vzero((a.n>b.n)?a.n:b.n);
            for(int i=0;i<r.n;++i){
                float x=comp(&a,i), y=comp(&b,i), z=0;
                if(op==OP_ADD) z=x+y;
                else if(op==OP_SUB) z=x-y;
                else if(op==OP_MUL) z=x*y;
                else z = (y!=0)? x/y : (x!=0? (x>0?INFINITY:-INFINITY):0.0f);
                r.c[i]=z;
            }
            return r;
        }
        case OP_EQ: case OP_NE: {
            int n=(a.n>b.n)?a.n:b.n; int eq=1;
            for(int i=0;i<n;++i) if(comp(&a,i)!=comp(&b,i)) { eq=0; break; }
            glsl_val r=vzero(1); r.c[0] = (op==OP_EQ)? (eq?1.0f:0.0f) : (eq?0.0f:1.0f); return r;
        }
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            float x=comp(&a,0), y=comp(&b,0); int t=0;
            if(op==OP_LT) t=x<y; else if(op==OP_GT) t=x>y; else if(op==OP_LE) t=x<=y; else t=x>=y;
            glsl_val r=vzero(1); r.c[0]=t?1.0f:0.0f; return r;
        }
        case OP_LAND: { glsl_val r=vzero(1); r.c[0]=(vtruthy(&a)&&vtruthy(&b))?1.0f:0.0f; return r; }
        case OP_LOR:  { glsl_val r=vzero(1); r.c[0]=(vtruthy(&a)||vtruthy(&b))?1.0f:0.0f; return r; }
        default: return vzero(1);
    }
}

/* Write `v` into an assignment target expression (EXK_VAR or EXK_SWIZ-of-var). */
static void write_target(run_t* r, glsl_ex* tgt, const glsl_val* v) {
    if(tgt == NULL) return;
    if(tgt->k == EXK_VAR) {
        const char* nm = tgt->name ? tgt->name : "";
        if(find_local(r, nm)) set_local(r, nm, v);
        else if(r->setv) r->setv(r->ud, nm, v);
        return;
    }
    if(tgt->k == EXK_SWIZ && tgt->a && tgt->a->k == EXK_VAR) {
        const char* nm = tgt->a->name ? tgt->a->name : "";
        glsl_val cur;
        int is_local = (find_local(r, nm) != NULL);
        if(is_local) cur = find_local(r, nm)->v;
        else { cur = vzero(4); if(r->getv) r->getv(r->ud, nm, &cur); }
        for(int i=0;i<tgt->ncomp;++i) {
            int ci = tgt->swiz[i];
            float src = comp(v, i);
            if(ci < 4) cur.c[ci] = src;
        }
        if(cur.n < tgt->ncomp) cur.n = tgt->ncomp;
        if(is_local) set_local(r, nm, &cur);
        else if(r->setv) r->setv(r->ud, nm, &cur);
        return;
    }
    /* Unsupported target: evaluate and drop. */
}

static glsl_val eval_ex(run_t* r, glsl_ex* e) {
    if(e == NULL) return vzero(1);
    switch(e->k) {
        case EXK_NUM:  { glsl_val v=vzero(1); v.c[0]=(float)e->num; return v; }
        case EXK_BOOL: { glsl_val v=vzero(1); v.c[0]=e->bval?1.0f:0.0f; return v; }
        case EXK_VAR:  return read_var(r, e->name ? e->name : "");
        case EXK_CTOR: return do_ctor(e, r);
        case EXK_CALL: return eval_call(r, e);
        case EXK_SWIZ: {
            glsl_val base = eval_ex(r, e->a);
            glsl_val out = vzero(e->ncomp);
            for(int i=0;i<e->ncomp;++i) out.c[i] = comp(&base, e->swiz[i]);
            return out;
        }
        case EXK_UN: {
            glsl_val a = eval_ex(r, e->a);
            if(e->op==UN_NOT){ glsl_val r2=vzero(1); r2.c[0]=vtruthy(&a)?0.0f:1.0f; return r2; }
            if(e->op==UN_POS) return a;
            glsl_val r2=a;
            if(a.mat >= 2) { for(int i=0;i<a.mat*a.mat;++i) r2.m[i]=-a.m[i]; return r2; }
            for(int i=0;i<a.n;++i) r2.c[i]=-a.c[i]; return r2;
        }
        case EXK_BIN: {
            glsl_val a = eval_ex(r, e->a);
            /* short-circuit for logical ops */
            if(e->op==OP_LAND && !vtruthy(&a)) return vzero(1);
            if(e->op==OP_LOR && vtruthy(&a)) { glsl_val t=vzero(1); t.c[0]=1.0f; return t; }
            glsl_val b = eval_ex(r, e->b);
            return do_binop(e->op, a, b);
        }
        case EXK_TERN: {
            glsl_val c = eval_ex(r, e->a);
            return vtruthy(&c) ? eval_ex(r, e->b) : eval_ex(r, e->c);
        }
        case EXK_ASSIGN: {
            glsl_val rhs = eval_ex(r, e->b);
            if(e->op != OP_ASSIGN) {
                glsl_val cur = eval_ex(r, e->a);
                int ao = (e->op==OP_ADD_A)?OP_ADD:(e->op==OP_SUB_A)?OP_SUB:(e->op==OP_MUL_A)?OP_MUL:OP_DIV;
                rhs = do_binop(ao, cur, rhs);
            }
            write_target(r, e->a, &rhs);
            return rhs;
        }
        default: return vzero(1);
    }
}

static void exec_block(run_t* r, glsl_st* blk) {
    if(blk == NULL) return;
    int save = r->nlocals;
    for(int i=0;i<blk->nbody && !r->returned && !r->discarded;++i) exec_st(r, blk->body[i]);
    if(!blk->nopop) r->nlocals = save;   /* pop block-scoped locals (unless this
                                          * is a declaration wrapper block) */
}

static void exec_st(run_t* r, glsl_st* s) {
    if(s == NULL || r->returned || r->discarded) return;
    switch(s->k) {
        case STK_BLOCK: exec_block(r, s); return;
        case STK_DECL: {
            glsl_val v;
            if(s->init) v = eval_ex(r, s->init);
            else if(s->mat >= 2) v = videntity(s->mat);
            else v = vzero(s->type ? s->type : 1);
            if(!s->init && s->mat == 0) v.n = s->type ? s->type : 1;
            set_local(r, s->name ? s->name : "", &v);
            return;
        }
        case STK_EXPR: eval_ex(r, s->init); return;
        case STK_IF: {
            glsl_val c = eval_ex(r, s->cond);
            if(vtruthy(&c)) exec_st(r, s->s1);
            else if(s->s2) exec_st(r, s->s2);
            return;
        }
        case STK_WHILE: {
            int guard = 0;
            while(!r->returned && !r->discarded && guard++ < 100000) {
                glsl_val c = eval_ex(r, s->cond);
                if(!vtruthy(&c)) break;
                exec_st(r, s->s1);
            }
            return;
        }
        case STK_FOR: {
            int save = r->nlocals;
            /* The init statement may be a declaration block (`for(int i=0;..)`).
             * Execute its children DIRECTLY (not via exec_block, which would pop
             * the loop variable) so `i` lives in the for-statement's own scope,
             * popped only when the whole loop finishes. */
            if(s->pre != NULL) {
                if(s->pre->k == STK_BLOCK) {
                    for(int i=0;i<s->pre->nbody && !r->returned && !r->discarded;++i)
                        exec_st(r, s->pre->body[i]);
                } else exec_st(r, s->pre);
            }
            int guard = 0;
            while(!r->returned && !r->discarded && guard++ < 100000) {
                if(s->cond) { glsl_val c = eval_ex(r, s->cond); if(!vtruthy(&c)) break; }
                exec_st(r, s->s1);
                if(s->upd) eval_ex(r, s->upd);
            }
            r->nlocals = save;
            return;
        }
        case STK_RETURN: if(s->init) eval_ex(r, s->init); r->returned = 1; return;
        case STK_DISCARD: r->discarded = 1; return;
        default: return;
    }
}

int glsl_run(glsl_shader* s, void* ud, glsl_getvar_fn getv, glsl_setvar_fn setv) {
    return glsl_run_ex(s, ud, getv, setv, NULL);
}

int glsl_run_ex(glsl_shader* s, void* ud, glsl_getvar_fn getv, glsl_setvar_fn setv,
                glsl_sample_fn sample) {
    if(s == NULL || s->main_body == NULL) return 1;
    run_t r; memset(&r, 0, sizeof(r));
    r.sh = s; r.ud = ud; r.getv = getv; r.setv = setv; r.sample = sample;
    exec_block(&r, s->main_body);
    return r.discarded ? 0 : 1;
}
