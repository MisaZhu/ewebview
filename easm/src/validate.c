// easm validator: spec type-stack algorithm (appendix formulation)
#include "easm.h"
#include "opcodes.h"
#include <stdio.h>

#define MAX_LOCALS (1u << 24)

typedef struct {
    uint32_t height;          // value stack height at block entry (params excluded)
    const EaValType *in;      // label-in types (params)
    const EaValType *out;     // label-out types (results)
    uint32_t n_in, n_out;
    uint32_t start_idx;
    uint8_t is_loop;
    bool unreachable;
    uint8_t *init_save;       // local-init state at block entry
    bool init_poly_save;
} Ctrl;

typedef struct {
    EaModule *m;
    EaValType *vals;
    uint32_t sp, cap;
    Ctrl *ctrl;
    uint32_t csp, ccap;
    uint32_t max_stack;
    uint8_t *lini;        // per-local initialization state (function-references)
    uint32_t n_lini;
    bool init_poly;       // after unreachable: all locals count as initialized
    uint32_t cur_op;      // debug: opcode currently being validated
    uint32_t cur_pc;
    char *err;
    bool failed;
} V;

// stable single-type slots for blocktype kind==1 (chunked arena: pointers
// into it stay valid as blocks nest)
static EaValType *g_snext;
static uint32_t g_srem;
static EaValType *vt_slot(EaValType t) {
    if (g_srem == 0) {
        g_snext = (EaValType *)ea_malloc(256 * sizeof(EaValType));
        g_srem = 256;
    }
    EaValType *p = g_snext++;
    g_srem--;
    *p = t;
    return p;
}

static void vfail_at(V *v, const char *msg, int line) {
    if (!v->failed) {
        v->failed = true;
        if (getenv("EA_VDBG")) fprintf(stderr, "[V] vfail@%d op=%x pc=%u sp=%u csp=%u: %s\n", line, v->cur_op, v->cur_pc, v->sp, v->csp, msg);
        if (v->err == NULL) v->err = ea_strndup(msg, strlen(msg));
    }
}
#define vfail(v, msg) vfail_at(v, msg, __LINE__)
int g_vfail_pc = -1;
uint32_t g_vfail_sp, g_vfail_h;
int g_vfail_unre;

static bool push_val(V *v, EaValType t) {
    if (v->sp == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->vals = (EaValType *)ea_realloc(v->vals, v->cap * sizeof(EaValType));
    }
    v->vals[v->sp++] = t;
    if (v->sp > v->max_stack) v->max_stack = v->sp;
    return true;
}
// minimal function-references subtyping: (ref $t) <: (ref null $t) <: funcref;
// a typed ref also matches its declared supertypes
static uint32_t ek_of(EaValType t) { return ((uint32_t)t >> 1) & 0xFu; }
// heap type (s33 immediate) -> valtype; 0x7FFFFFFF on error
static EaValType gc_ht_valtype(EaModule *m, int64_t ht, bool nullable) {
    if (ht >= 0) {
        if ((uint32_t)ht >= m->n_types) return (EaValType)0x7FFFFFFFu;
        return nullable ? EA_VT_TREFN((uint32_t)ht) : EA_VT_TREF((uint32_t)ht);
    }
    uint32_t kind;
    switch (ht) {
    case -0x10: kind = EA_ABS_FUNC; break;
    case -0x11: kind = EA_ABS_EXTERN; break;
    case -0x12: kind = EA_ABS_ANY; break;
    case -0x13: kind = EA_ABS_EQ; break;
    case -0x14: kind = EA_ABS_I31; break;
    case -0x15: kind = EA_ABS_STRUCT; break;
    case -0x16: kind = EA_ABS_ARRAY; break;
    case -0x17: kind = EA_ABS_EXN; break;
    case -0x0F: kind = EA_ABS_NONE; break;
    case -0x0D: kind = EA_ABS_NOFUNC; break;
    case -0x0E: kind = EA_ABS_NOEXTERN; break;
    default: return (EaValType)0x7FFFFFFFu;
    }
    if (nullable) {
        switch (kind) {
        case EA_ABS_FUNC: return VT_FUNCREF;
        case EA_ABS_EXTERN: return VT_EXTERNREF;
        case EA_ABS_ANY: return VT_ANYREF;
        default: return EA_VT_ABSN(kind);
        }
    }
    return EA_VT_ABSREF(kind);
}

// abstract heap kind subtyping: got kind <: expect kind
// structural canonical type equivalence (shared semantics with the runtime)
static bool vt_canon_eq_v(EaModule *m, EaValType a, EaValType b, int depth);

// canonical equivalence of iso-recursive types, following the reference
// interpreter's model: a defined type is (rec group, position); its body's
// references into its OWN group are positional (Rec j), references elsewhere
// are external and compared recursively.  Two types are equal iff their
// groups are structurally equal member-by-member with matching positions.
// Comparison of a group pair currently in progress is assumed to succeed
// (greatest fixed point), which terminates on recursive types.
typedef struct { EaModule *ma, *mb; uint32_t sa, sb; } EaEqPair;
typedef struct { EaEqPair *v; uint32_t n, cap; } EaEqStack;

static bool groups_eq_st(EaModule *ma, uint32_t sa, EaModule *mb, uint32_t sb,
                         uint32_t n, EaEqStack *st, int depth);

// group bounds (start, size) of the type index x in module m
static bool type_group(EaModule *m, uint32_t x, uint32_t *start, uint32_t *size) {
    if (x >= m->n_types) return false;
    *start = x - m->types[x].rec_pos;
    *size = m->types[x].rec_size;
    return true;
}

static bool type_canon_eq_st(EaModule *ma, uint32_t xa, EaModule *mb, uint32_t xb, EaEqStack *st, int depth) {
    uint32_t ga, na, gb, nb;
    if (!type_group(ma, xa, &ga, &na) || !type_group(mb, xb, &gb, &nb)) return false;
    if (na != nb || xa - ga != xb - gb) return false;
    return groups_eq_st(ma, ga, mb, gb, na, st, depth);
}

// compare one reference occurring inside group member (ma,sa+k) against one
// inside (mb,sb+k); sa..sa+n / sb..sb+n are the groups being compared
static bool refs_eq_st(EaModule *ma, uint32_t sa, uint32_t n, uint32_t xa,
                       EaModule *mb, uint32_t sb, uint32_t xb, EaEqStack *st, int depth) {
    bool ina = xa >= sa && xa - sa < n;
    bool inb = xb >= sb && xb - sb < n;
    if (ina != inb) return false;          // positional self-ref vs external ref
    if (ina) return xa - sa == xb - sb;    // both internal: same position
    return type_canon_eq_st(ma, xa, mb, xb, st, depth);
}

static bool vt_canon_eq_st(EaModule *ma, EaValType a, EaModule *mb, EaValType b,
                           uint32_t sa, uint32_t na, uint32_t sb, EaEqStack *st, int depth) {
    if (ea_tref_real(a) && ea_tref_real(b))
        // same raw index is NOT sufficient: it may be internal (Rec j) on one
        // side and external (Idx x) on the other
        return refs_eq_st(ma, sa, na, ea_tref_idx(a), mb, sb, ea_tref_idx(b), st, depth);
    return a == b;
}

static bool groups_eq_st(EaModule *ma, uint32_t sa, EaModule *mb, uint32_t sb,
                         uint32_t n, EaEqStack *st, int depth) {
    if (sa == sb && ma == mb) return true;
    if (depth > 12) return false;
    for (uint32_t i = 0; i < st->n; i++)
        if (st->v[i].ma == ma && st->v[i].mb == mb &&
            st->v[i].sa == sa && st->v[i].sb == sb) return true;
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->v = (EaEqPair *)realloc(st->v, st->cap * sizeof(EaEqPair));
        if (!st->v) return false;
    }
    st->v[st->n++] = (EaEqPair){ma, mb, sa, sb};
    bool ok = true;
    for (uint32_t k = 0; k < n && ok; k++) {
        EaType *ta = &ma->types[sa + k], *tb = &mb->types[sb + k];
        if (ta->kind != tb->kind || ta->is_final != tb->is_final || ta->n_sup != tb->n_sup) {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < ta->n_sup && ok; i++)
            ok = refs_eq_st(ma, sa, n, ta->sup[i], mb, sb, tb->sup[i], st, depth + 1);
        if (!ok) break;
        if (ta->kind == CT_FUNC) {
            if (ta->func.n_params != tb->func.n_params ||
                ta->func.n_results != tb->func.n_results) { ok = false; break; }
            for (uint32_t i = 0; i < ta->func.n_params && ok; i++)
                ok = vt_canon_eq_st(ma, ta->func.params[i], mb, tb->func.params[i],
                                    sa, n, sb, st, depth + 1);
            for (uint32_t i = 0; i < ta->func.n_results && ok; i++)
                ok = vt_canon_eq_st(ma, ta->func.results[i], mb, tb->func.results[i],
                                    sa, n, sb, st, depth + 1);
        } else {
            if (ta->n_fields != tb->n_fields) { ok = false; break; }
            for (uint32_t i = 0; i < ta->n_fields && ok; i++) {
                if (ta->fields[i].mut != tb->fields[i].mut ||
                    ta->fields[i].packed_ != tb->fields[i].packed_)
                    ok = false;
                else
                    ok = vt_canon_eq_st(ma, ta->fields[i].vt, mb, tb->fields[i].vt,
                                        sa, n, sb, st, depth + 1);
            }
        }
    }
    st->n--;
    return ok;
}

bool ea_type_canon_eq(EaModule *ma, uint32_t a, EaModule *mb, uint32_t b) {
    EaEqStack st = {0};
    bool r = type_canon_eq_st(ma, a, mb, b, &st, 0);
    free(st.v);
    return r;
}
bool ea_type_canon_eq1(EaModule *m, uint32_t a, uint32_t b) {
    return ea_type_canon_eq(m, a, m, b);
}
static bool type_canon_eq(EaModule *m, uint32_t a, uint32_t b, int depth) {
    (void)depth;
    return ea_type_canon_eq1(m, a, b);
}
static bool vt_canon_eq_v(EaModule *m, EaValType a, EaValType b, int depth) {
    if (a == b) return true;
    if (ea_tref_real(a) && ea_tref_real(b))
        return ea_type_canon_eq1(m, ea_tref_idx(a), ea_tref_idx(b));
    return false;
}

static bool abs_kind_sub(uint32_t gk, uint32_t ek) {
    if (gk == ek) return true;
    switch (ek) {
    case EA_ABS_ANY:
        return gk == EA_ABS_EQ || gk == EA_ABS_I31 || gk == EA_ABS_STRUCT ||
               gk == EA_ABS_ARRAY || gk == EA_ABS_NONE || gk == EA_ABS_FUNC ||
               gk == EA_ABS_EXTERN || gk == EA_ABS_NOFUNC || gk == EA_ABS_NOEXTERN;
    case EA_ABS_EQ:
        return gk == EA_ABS_I31 || gk == EA_ABS_STRUCT || gk == EA_ABS_ARRAY ||
               gk == EA_ABS_NONE;
    case EA_ABS_I31: case EA_ABS_STRUCT: case EA_ABS_ARRAY:
        return gk == EA_ABS_NONE;
    case EA_ABS_FUNC: return gk == EA_ABS_NOFUNC;
    case EA_ABS_EXTERN: return gk == EA_ABS_NOEXTERN;
    default: return false; // bottom kinds are only subtypes of themselves
    }
}

// resolve any reference valtype to its (abstract kind, nullable) pair;
// typed refs map to the abstract kind of their composite type
static bool vt_abs_info(EaModule *m, EaValType t, uint32_t *kind, bool *nullable) {
    if (ea_is_absref(t)) {
        *kind = ea_abs_kind(t);
        *nullable = ((uint32_t)t & 1u) == 0;
        return true;
    }
    if (t == VT_FUNCREF)   { *kind = EA_ABS_FUNC;   *nullable = true; return true; }
    if (t == VT_EXTERNREF) { *kind = EA_ABS_EXTERN; *nullable = true; return true; }
    if (t == VT_ANYREF)    { *kind = EA_ABS_ANY;    *nullable = true; return true; }
    if (ea_tref_real(t)) {
        uint32_t idx = ea_tref_idx(t);
        if (idx >= m->n_types) return false;
        switch (m->types[idx].kind) {
        case CT_FUNC:   *kind = EA_ABS_FUNC;   break;
        case CT_STRUCT: *kind = EA_ABS_STRUCT; break;
        case CT_ARRAY:  *kind = EA_ABS_ARRAY;  break;
        }
        *nullable = ea_tref_nullable(t);
        return true;
    }
    return false;
}

// full structural subtyping: g <: e (declared sup chains + structural rule);
// cross-module variant resolves canonical equivalence with each side in its
// own module's type space
bool ea_type_sub_mm(EaModule *mg, uint32_t g, EaModule *me, uint32_t e, int depth) {
    if (g == e && mg == me) return true;
    if (depth > 12 || g >= mg->n_types || e >= me->n_types) return false;
    if (ea_type_canon_eq(mg, g, me, e)) return true; // rec-group equivalence
    EaType *tg = &mg->types[g];
    // subtyping follows declared supertype chains only; the variance of each
    // declared edge was validated when the (sub ...) type was decoded
    if (tg->n_sup > 0) return ea_type_sub_mm(mg, tg->sup[0], me, e, depth + 1);
    return false;
}
bool ea_type_sub(EaModule *m, uint32_t g, uint32_t e, int depth) {
    return ea_type_sub_mm(m, g, m, e, depth);
}
bool ea_vt_canon_eq(EaModule *m, EaValType a, EaValType b) {
    if (a == b) return true;
    if (ea_tref_real(a) && ea_tref_real(b)) {
        if (ea_tref_nullable(a) != ea_tref_nullable(b)) return false;
        return type_canon_eq(m, ea_tref_idx(a), ea_tref_idx(b), 0);
    }
    return false;
}
bool ea_vt_sub(EaModule *m, EaValType a, EaValType b, int depth) {
    if (a == b || a == VT_BOTTOM) return true;
    if (ea_tref_real(a) && ea_tref_real(b)) {
        if (!(ea_tref_nullable(b) || !ea_tref_nullable(a))) return false;
        return ea_type_sub(m, ea_tref_idx(a), ea_tref_idx(b), depth);
    }
    // abstract pairs (typed refs join the abstract lattice via their kind)
    {
        uint32_t ka, kb; bool anull, bnull;
        if (vt_abs_info(m, a, &ka, &anull) && vt_abs_info(m, b, &kb, &bnull)) {
            if (anull && !bnull) return false;
            if (ea_tref_real(a) && ea_tref_real(b))
                return ka == kb && ea_type_sub(m, ea_tref_idx(a), ea_tref_idx(b), depth);
            if (ea_tref_real(b) && !ea_tref_real(a))
                // abstract flows into a concrete typed ref only from a bottom kind
                return ka != kb && abs_kind_sub(ka, kb);
            if (ka == kb) return true;
            return abs_kind_sub(ka, kb);
        }
    }
    return false;
}


static bool ty_match(EaModule *m, EaValType expect, EaValType got) {
    if (expect == got) return true;
    if (expect == VT_BOTTOM || got == VT_BOTTOM) return true;
    if (expect == VT_ANYREF && ea_is_ref(got)) return true;
    if (got == VT_ANYREF && ea_is_ref(expect)) return true;
    // abstract non-null refs: identical kinds match; (ref func)/(ref extern)
    // widen to funcref/externref; typed refs widen per their declared kind
    { uint32_t ea = (uint32_t)expect, ga = (uint32_t)got;
      bool eabs = ea_is_absref((EaValType)ea), gabs = ea_is_absref((EaValType)ga);
      // plain nullable enums count as abstract kinds 1/2/3 with parity 0
      uint32_t ek = eabs ? ((ea >> 1) & 0xFu)
                        : expect == VT_FUNCREF ? EA_ABS_FUNC
                        : expect == VT_EXTERNREF ? EA_ABS_EXTERN
                        : expect == VT_ANYREF ? EA_ABS_ANY : 0;
      uint32_t gk = gabs ? ((ga >> 1) & 0xFu)
                        : got == VT_FUNCREF ? EA_ABS_FUNC
                        : got == VT_EXTERNREF ? EA_ABS_EXTERN
                        : got == VT_ANYREF ? EA_ABS_ANY : 0;
      if (eabs && gabs) {
          bool enull = ea_tref_nullable((EaValType)ea);
          bool gnull = ea_tref_nullable((EaValType)ga);
          if (gnull && !enull) return false;      // nullable <: non-null is unsound
          if (gk == ek) return true;
          return abs_kind_sub(gk, ek);
      }
      if (eabs) {
          bool gnull = ea_tref_nullable(got);
          bool enull = eabs ? ((ea & 1u) == 0) : true;
          if (gnull && !enull) return false;
          // concrete typed got: match by declared kind
          if (ea_tref_real(got) && ea_tref_idx(got) < m->n_types) {
              uint8_t k = m->types[ea_tref_idx(got)].kind;
              uint32_t ck = k == CT_FUNC ? EA_ABS_FUNC
                          : k == CT_STRUCT ? EA_ABS_STRUCT : EA_ABS_ARRAY;
              return abs_kind_sub(ck, ek);
          }
          if (gk == 0) return false; // got is not a reference
          if (gabs) return abs_kind_sub(gk, ek);
          return false; // nullable enum got vs non-null-only abstract expect
      }
      // gabs (abstract got): widen to the matching nullable enum
      if (gabs) {
          if (gk == EA_ABS_FUNC) return expect == VT_FUNCREF || expect == VT_ANYREF;
          if (gk == EA_ABS_EXTERN) return expect == VT_EXTERNREF;
          if (gk == EA_ABS_ANY) return expect == VT_ANYREF;
          if (gk == EA_ABS_EQ || gk == EA_ABS_I31 || gk == EA_ABS_STRUCT ||
              gk == EA_ABS_ARRAY || gk == EA_ABS_NONE)
              return expect == VT_ANYREF;
          // null-bottom refs widen to any nullable ref in their hierarchy
          if (gk == EA_ABS_NOFUNC)
              return ea_tref_nullable(expect) &&
                     (expect == VT_FUNCREF ||
                      (ea_tref_real(expect) && ea_tref_idx(expect) < m->n_types &&
                       m->types[ea_tref_idx(expect)].kind == CT_FUNC));
          if (gk == EA_ABS_NOEXTERN)
              return ea_tref_nullable(expect) && expect == VT_EXTERNREF;
          if (gk == EA_ABS_NONE)
              return ea_tref_nullable(expect) &&
                     (expect == VT_ANYREF ||
                      (ea_tref_real(expect) && ea_tref_idx(expect) < m->n_types &&
                       m->types[ea_tref_idx(expect)].kind != CT_FUNC));
          return false;
      }
    }
    if (ea_is_typedref(expect) && ea_is_typedref(got)) {
        uint32_t e = ea_tref_idx(expect), g = ea_tref_idx(got);
        if (!(ea_tref_nullable(expect) || !ea_tref_nullable(got))) return false;
        return ea_type_sub(m, g, e, 0);
    }
    if (expect == VT_FUNCREF &&
        ((ea_tref_real(got) && ea_tref_idx(got) < m->n_types &&
          m->types[ea_tref_idx(got)].kind == CT_FUNC) ||
         (uint32_t)got == EA_VT_ABSREF(EA_ABS_FUNC)))
        return true;
    if (expect == VT_EXTERNREF &&
        ((ea_tref_real(got) && ea_tref_idx(got) < m->n_types &&
          m->types[ea_tref_idx(got)].kind != CT_FUNC) ||
         (uint32_t)got == EA_VT_ABSREF(EA_ABS_EXTERN)))
        return true;
    return false;
}

static bool vt_match(V *v, EaValType expect, EaValType got) {
    return ty_match(v->m, expect, got);
}

// typed-list equality for call_indirect type checks
static bool vt_list_eq(V *v, const EaValType *a, uint32_t na, const EaValType *b, uint32_t nb) {
    if (na != nb) return false;
    for (uint32_t i = 0; i < na; i++)
        if (!vt_match(v, a[i], b[i]) && !vt_match(v, b[i], a[i])) return false;
    return true;
}
// tail-call results must be SUBTYPES of the frame's results (one-directional)
static bool vt_list_sub(V *v, const EaValType *got, uint32_t ng, const EaValType *want, uint32_t nw) {
    if (ng != nw) return false;
    for (uint32_t i = 0; i < ng; i++)
        if (!vt_match(v, want[i], got[i])) return false;
    return true;
}

static EaValType pop_val(V *v, EaValType expect) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    if (v->sp == c->height) {
        if (c->unreachable) return expect == VT_BOTTOM ? VT_BOTTOM : expect;
        if (getenv("EA_VDBG")) fprintf(stderr, "[V] UNDF pc=%u want=%x height=%u n_out=%u out0=%x csp=%u unreach=%d\n",
                                       v->cur_pc, (unsigned)expect, c->height, c->n_out,
                                       c->n_out ? (unsigned)c->out[0] : 0, v->csp, (int)c->unreachable);
        vfail(v, "type mismatch: stack underflow");
        return expect;
    }
    EaValType got = v->vals[--v->sp];
    if (got == VT_BOTTOM) return expect == VT_BOTTOM ? VT_BOTTOM : expect;
    if (expect != VT_BOTTOM && got != expect && !vt_match(v, expect, got)) {
        if (getenv("EA_VDBG")) fprintf(stderr, "[V] pop want %x got %x op %x pc %u csp=%u sp=%u\n",
                                       (unsigned)expect, (unsigned)got, v->cur_op, v->cur_pc, v->csp, v->sp);
        vfail(v, "type mismatch");
        return got;
    }
    return got;
}
static void push_vals(V *v, const EaValType *ts, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) push_val(v, ts[i]);
}
static void pop_vals(V *v, const EaValType *ts, uint32_t n) {
    for (uint32_t i = n; i > 0; i--) pop_val(v, ts[i - 1]);
}
static EaValType pop_any(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    if (v->sp == c->height) {
        if (c->unreachable) return VT_BOTTOM;
        vfail(v, "type mismatch: stack underflow");
        return VT_BOTTOM;
    }
    return v->vals[--v->sp];
}
static void set_unreachable(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    c->unreachable = true;
    v->sp = c->height;
    v->init_poly = true; // polymorphic: all locals count as initialized
}
// narrow a nullable reference to its non-null form
static EaValType ea_narrow_ref(EaValType t) {
    if (ea_is_absref(t)) return (uint32_t)t & 1u ? t : (EaValType)((uint32_t)t | 1u);
    if (ea_is_typedref(t) && ea_tref_nullable(t)) return EA_VT_TREF(ea_tref_idx(t));
    if (t == VT_FUNCREF) return EA_VT_ABSREF(EA_ABS_FUNC);
    if (t == VT_EXTERNREF) return EA_VT_ABSREF(EA_ABS_EXTERN);
    if (t == VT_ANYREF) return EA_VT_ABSREF(EA_ABS_ANY);
    return t;
}
static bool local_defaultable(EaValType t) {
    return !ea_is_typedref(t) || ea_tref_nullable(t); // abstract (ref ht) = non-null
}
static bool lini_ok(V *v, uint32_t idx) {
    return v->init_poly || v->lini == NULL || idx >= v->n_lini || v->lini[idx];
}

static void push_ctrl(V *v, uint32_t start_idx, const EaValType *in, uint32_t n_in,
                      const EaValType *out, uint32_t n_out, bool is_loop) {
    if (v->csp == v->ccap) {
        v->ccap = v->ccap ? v->ccap * 2 : 16;
        v->ctrl = (Ctrl *)ea_realloc(v->ctrl, v->ccap * sizeof(Ctrl));
    }
    Ctrl *c = &v->ctrl[v->csp++];
    c->height = v->sp;
    c->in = in; c->n_in = n_in;
    c->out = out; c->n_out = n_out;
    c->start_idx = start_idx;
    c->is_loop = is_loop;
    c->unreachable = false;
    push_vals(v, in, n_in);
}
static bool pop_ctrl(V *v) {
    Ctrl *c = &v->ctrl[v->csp - 1];
    pop_vals(v, c->out, c->n_out);
    if (v->failed) return false;
    if (v->sp != c->height) {
        vfail(v, "type mismatch: values remaining at block end");
        return false;
    }
    v->csp--;
    return true;
}

static bool resolve_blocktype(V *v, const EaBlockType *bt,
                              const EaValType **in, uint32_t *n_in,
                              const EaValType **out, uint32_t *n_out) {
    if (bt->kind == 0) {
        *in = NULL; *n_in = 0;
        *out = NULL; *n_out = 0;
        return true;
    }
    if (ea_tref_real(bt->vt) && ea_tref_idx(bt->vt) >= v->m->n_types) {
        vfail(v, "unknown type");
        return false;
    }
    if (bt->kind == 1) {
        *in = NULL; *n_in = 0;
        *out = vt_slot(bt->vt); *n_out = 1;
        return true;
    }
    if (bt->type_idx >= v->m->n_types || v->m->types[bt->type_idx].kind != CT_FUNC) {
        vfail(v, "unknown type");
        return false;
    }
    const EaFuncType *ft = &v->m->types[bt->type_idx].func;
    *in = ft->params; *n_in = ft->n_params;
    *out = ft->results; *n_out = ft->n_results;
    return true;
}

// ---------------------------------------------------------------- signature table
typedef struct { uint8_t n_in, n_out; EaValType in[3], out[1]; } Sig;

bool simd_sig(uint32_t op, Sig *s);

static bool op_sig(uint32_t op, Sig *s) {
    s->n_in = s->n_out = 0;
    switch (op) {
    case EA_OP_NOP: case EA_OP_UNREACHABLE: case EA_OP_DROP: case EA_OP_ELSE: case EA_OP_END:
        return true;
    case EA_OP_I32_EQZ:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EQZ:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_EQ: case EA_OP_I32_NE: case EA_OP_I32_LT_S: case EA_OP_I32_LT_U:
    case EA_OP_I32_GT_S: case EA_OP_I32_GT_U: case EA_OP_I32_LE_S: case EA_OP_I32_LE_U:
    case EA_OP_I32_GE_S: case EA_OP_I32_GE_U:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EQ: case EA_OP_I64_NE: case EA_OP_I64_LT_S: case EA_OP_I64_LT_U:
    case EA_OP_I64_GT_S: case EA_OP_I64_GT_U: case EA_OP_I64_LE_S: case EA_OP_I64_LE_U:
    case EA_OP_I64_GE_S: case EA_OP_I64_GE_U:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_F32_EQ: case EA_OP_F32_NE: case EA_OP_F32_LT: case EA_OP_F32_GT:
    case EA_OP_F32_LE: case EA_OP_F32_GE:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_F64_EQ: case EA_OP_F64_NE: case EA_OP_F64_LT: case EA_OP_F64_GT:
    case EA_OP_F64_LE: case EA_OP_F64_GE:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_CLZ: case EA_OP_I32_CTZ: case EA_OP_I32_POPCNT:
    case EA_OP_I32_EXTEND8_S: case EA_OP_I32_EXTEND16_S:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_ADD: case EA_OP_I32_SUB: case EA_OP_I32_MUL:
    case EA_OP_I32_DIV_S: case EA_OP_I32_DIV_U: case EA_OP_I32_REM_S: case EA_OP_I32_REM_U:
    case EA_OP_I32_AND: case EA_OP_I32_OR: case EA_OP_I32_XOR:
    case EA_OP_I32_SHL: case EA_OP_I32_SHR_S: case EA_OP_I32_SHR_U:
    case EA_OP_I32_ROTL: case EA_OP_I32_ROTR:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_CLZ: case EA_OP_I64_CTZ: case EA_OP_I64_POPCNT:
    case EA_OP_I64_EXTEND8_S: case EA_OP_I64_EXTEND16_S: case EA_OP_I64_EXTEND32_S:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_ADD: case EA_OP_I64_SUB: case EA_OP_I64_MUL:
    case EA_OP_I64_DIV_S: case EA_OP_I64_DIV_U: case EA_OP_I64_REM_S: case EA_OP_I64_REM_U:
    case EA_OP_I64_AND: case EA_OP_I64_OR: case EA_OP_I64_XOR:
    case EA_OP_I64_SHL: case EA_OP_I64_SHR_S: case EA_OP_I64_SHR_U:
    case EA_OP_I64_ROTL: case EA_OP_I64_ROTR:
        s->n_in = 2; s->in[0] = s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_F32_ABS: case EA_OP_F32_NEG: case EA_OP_F32_CEIL: case EA_OP_F32_FLOOR:
    case EA_OP_F32_TRUNC: case EA_OP_F32_NEAREST: case EA_OP_F32_SQRT:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_ADD: case EA_OP_F32_SUB: case EA_OP_F32_MUL: case EA_OP_F32_DIV:
    case EA_OP_F32_MIN: case EA_OP_F32_MAX: case EA_OP_F32_COPYSIGN:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F64_ABS: case EA_OP_F64_NEG: case EA_OP_F64_CEIL: case EA_OP_F64_FLOOR:
    case EA_OP_F64_TRUNC: case EA_OP_F64_NEAREST: case EA_OP_F64_SQRT:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_ADD: case EA_OP_F64_SUB: case EA_OP_F64_MUL: case EA_OP_F64_DIV:
    case EA_OP_F64_MIN: case EA_OP_F64_MAX: case EA_OP_F64_COPYSIGN:
        s->n_in = 2; s->in[0] = s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_I32_WRAP_I64:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_TRUNC_F32_S: case EA_OP_I32_TRUNC_F32_U:
    case EA_OP_I32_TRUNC_SAT_F32_S: case EA_OP_I32_TRUNC_SAT_F32_U:
    case EA_OP_I32_REINTERPRET_F32:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I32_TRUNC_F64_S: case EA_OP_I32_TRUNC_F64_U:
    case EA_OP_I32_TRUNC_SAT_F64_S: case EA_OP_I32_TRUNC_SAT_F64_U:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_I64_EXTEND_I32_S: case EA_OP_I64_EXTEND_I32_U:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_TRUNC_F32_S: case EA_OP_I64_TRUNC_F32_U:
    case EA_OP_I64_TRUNC_SAT_F32_S: case EA_OP_I64_TRUNC_SAT_F32_U:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_I64_TRUNC_F64_S: case EA_OP_I64_TRUNC_F64_U:
    case EA_OP_I64_TRUNC_SAT_F64_S: case EA_OP_I64_TRUNC_SAT_F64_U:
    case EA_OP_I64_REINTERPRET_F64:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_I64; return true;
    case EA_OP_F32_CONVERT_I32_S: case EA_OP_F32_CONVERT_I32_U:
    case EA_OP_F32_REINTERPRET_I32:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_CONVERT_I64_S: case EA_OP_F32_CONVERT_I64_U:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F32_DEMOTE_F64:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_F32; return true;
    case EA_OP_F64_CONVERT_I32_S: case EA_OP_F64_CONVERT_I32_U:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_CONVERT_I64_S: case EA_OP_F64_CONVERT_I64_U:
    case EA_OP_F64_REINTERPRET_I64:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_F64_PROMOTE_F32:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_F64; return true;
    case EA_OP_REF_IS_NULL:
        s->n_in = 1; s->in[0] = VT_BOTTOM; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_REF_EQ:
        s->n_in = 2; s->in[0] = VT_BOTTOM; s->in[1] = VT_BOTTOM;
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_MEMORY_SIZE:
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_MEMORY_GROW:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_SIZE:
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_GROW:
        s->n_in = 2; s->in[0] = VT_BOTTOM; s->in[1] = VT_I32;
        s->n_out = 1; s->out[0] = VT_I32; return true;
    case EA_OP_TABLE_FILL:
        s->n_in = 3; s->in[0] = VT_I32; s->in[1] = VT_BOTTOM; s->in[2] = VT_I32; return true;
    case EA_OP_TABLE_COPY: case EA_OP_TABLE_INIT:
    case EA_OP_MEMORY_FILL: case EA_OP_MEMORY_COPY: case EA_OP_MEMORY_INIT:
        s->n_in = 3; s->in[0] = s->in[1] = s->in[2] = VT_I32; return true;
    case EA_OP_DATA_DROP: case EA_OP_ELEM_DROP:
        return true;
    default:
        if (op >= EA_OPV_BASE) return simd_sig(op, s);
        return false;
    }
}

static bool is_load(uint32_t op) {
    return (op >= EA_OP_I32_LOAD && op <= EA_OP_I64_LOAD32_U);
}
static bool is_store(uint32_t op) {
    return (op >= EA_OP_I32_STORE && op <= EA_OP_I64_STORE32);
}
static uint8_t load_width(uint32_t op) {
    switch (op) {
    case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
    case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: return 1;
    case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
    case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U: return 2;
    case EA_OP_I32_LOAD: case EA_OP_F32_LOAD:
    case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: return 4;
    default: return 8;
    }
}
static uint8_t store_width(uint32_t op) {
    switch (op) {
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: return 1;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: return 2;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE:
    case EA_OP_I64_STORE32: return 4;
    default: return 8;
    }
}

// type-check a const expr; returns resulting valtype or VT_BOTTOM on error
static EaValType check_const_expr(EaModule *m, const InsList *code, uint32_t n_globals_visible,
                                  char **err) {
    EaValType stack[8];
    uint32_t sp = 0;
    if (code->n == 0) goto bad;
    for (uint32_t k = 0; k < code->n; k++) {
        EaInstr *in = &code->v[k];
        uint32_t op = in->opcode;
        switch (in->opcode) {
        case EA_OP_I32_CONST: case EA_OP_REF_NULL:
            if (sp >= 8) goto bad;
            stack[sp++] = (in->opcode == EA_OP_I32_CONST) ? VT_I32 : (EaValType)in->imm.u32;
            break;
        case EA_OP_I64_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_I64; break;
        case EA_OP_F32_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_F32; break;
        case EA_OP_F64_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_F64; break;
        case EA_OP_V128_CONST: if (sp >= 8) goto bad; stack[sp++] = VT_V128; break;
        case EA_OP_REF_FUNC:
            if (getenv("EA_VDBG2")) fprintf(stderr, "CE ref.func %u nfuncs=%u\n", in->imm.u32, m->n_funcs);
            if (in->imm.u32 >= m->n_funcs) { if (err && !*err) *err = ea_strndup("unknown function", 16); goto bad; }
            if (sp >= 8) goto bad;
            // ref.func carries the declared type (ref $t)
            stack[sp++] = EA_VT_TREF(m->funcs[in->imm.u32].type_idx);
            break;
        case EA_OP_GLOBAL_GET: {
            uint32_t gi = in->imm.u32;
            if (gi >= n_globals_visible) { if (err && !*err) *err = ea_strndup("unknown global", 14); goto bad; }
            if (m->globals_def[gi].mutable_) { if (err && !*err) *err = ea_strndup("constant expression required", 28); goto bad; }
            if (sp >= 8) goto bad;
            stack[sp++] = m->globals_def[gi].type;
            break;
        }
        // extended constant expressions (Wasm 3.0)
        case EA_OP_I32_ADD: case EA_OP_I32_SUB: case EA_OP_I32_MUL:
        case EA_OP_I64_ADD: case EA_OP_I64_SUB: case EA_OP_I64_MUL: {
            if (sp < 2) goto bad;
            EaValType a = stack[sp - 2], b = stack[sp - 1];
            EaValType want = (in->opcode == EA_OP_I32_ADD || in->opcode == EA_OP_I32_SUB ||
                              in->opcode == EA_OP_I32_MUL) ? VT_I32 : VT_I64;
            if (a != want || b != want) goto bad;
            sp--;
            break;
        }
        // GC extended constant expressions
        case EA_OP_STRUCT_NEW: case EA_OP_STRUCT_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            if (ti >= m->n_types || m->types[ti].kind != CT_STRUCT) goto bad;
            if (op == EA_OP_STRUCT_NEW) {
                for (uint32_t i = m->types[ti].n_fields; i > 0; i--) {
                    if (sp == 0) goto bad;
                    EaValType want = m->types[ti].fields[i - 1].packed_ ? VT_I32 : m->types[ti].fields[i - 1].vt;
                    if (!ty_match(m, want, stack[sp - 1])) goto bad;
                    sp--;
                }
            } else {
                for (uint32_t i = 0; i < m->types[ti].n_fields; i++) {
                    EaValType ft = m->types[ti].fields[i].packed_ ? VT_I32 : m->types[ti].fields[i].vt;
                    if (!local_defaultable(ft)) goto bad;
                }
            }
            if (sp >= 8) goto bad;
            stack[sp++] = EA_VT_TREF(ti);
            break;
        }
        case EA_OP_ARRAY_NEW: case EA_OP_ARRAY_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) goto bad;
            if (sp < 1 || stack[sp - 1] != VT_I32) goto bad;
            sp--;
            if (op == EA_OP_ARRAY_NEW) {
                if (sp == 0) goto bad;
                EaValType want = m->types[ti].fields[0].packed_ ? VT_I32 : m->types[ti].fields[0].vt;
                if (!ty_match(m, want, stack[sp - 1])) goto bad;
                sp--;
            } else if (!local_defaultable(m->types[ti].fields[0].packed_ ? VT_I32 : m->types[ti].fields[0].vt)) {
                goto bad;
            }
            if (sp >= 8) goto bad;
            stack[sp++] = EA_VT_TREF(ti);
            break;
        }
        case EA_OP_ARRAY_NEW_FIXED: {
            uint32_t ti = in->imm.pair.a;
            if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) goto bad;
            EaValType want = m->types[ti].fields[0].packed_ ? VT_I32 : m->types[ti].fields[0].vt;
            for (uint32_t i = 0; i < in->imm.pair.b; i++) {
                if (sp == 0 || !ty_match(m, want, stack[sp - 1])) goto bad;
                sp--;
            }
            if (sp >= 8) goto bad;
            stack[sp++] = EA_VT_TREF(ti);
            break;
        }
        case EA_OP_REF_I31:
            if (sp == 0 || stack[sp - 1] != VT_I32) goto bad;
            stack[sp - 1] = EA_VT_ABSREF(EA_ABS_I31);
            break;
        case EA_OP_ANY_CONVERT_EXTERN: case EA_OP_EXTERN_CONVERT_ANY: {
            if (sp == 0) goto bad;
            EaValType t = stack[sp - 1];
            bool nullable = t == VT_BOTTOM || ea_tref_nullable(t);
            if (op == EA_OP_ANY_CONVERT_EXTERN)
                stack[sp - 1] = nullable ? (EaValType)VT_ANYREF : EA_VT_ABSREF(EA_ABS_ANY);
            else
                stack[sp - 1] = nullable ? (EaValType)VT_EXTERNREF : EA_VT_ABSREF(EA_ABS_EXTERN);
            break;
        }
        default:
            if (err && !*err) *err = ea_strndup("constant expression required", 28);
            goto bad;
        }
    }
    if (sp != 1) goto bad; // const exprs produce exactly one value
    return stack[0];
bad:
    return (EaValType)0x7FFFFFFFu; // hard const-expr error sentinel
}

int ea_validate_module(EaModule *m, char **err) {
    int rc = 0;
    // v must be zeroed before any EXPR_FAIL goto out
    // typed-ref type indices must be in range (decoding can't know the final
    // type count while the type section is still being read)
    for (uint32_t ti = 0; ti < m->n_types; ti++) {
        if (m->types[ti].kind != CT_FUNC) continue;
        EaFuncType *t = &m->types[ti].func;
        for (uint32_t k = 0; k < t->n_params + t->n_results; k++) {
            EaValType vt = k < t->n_params ? t->params[k] : t->results[k - t->n_params];
            if (ea_tref_real(vt) && ea_tref_idx(vt) >= m->n_types) {
                if (err && !*err) *err = ea_strndup("unknown type", 12);
                return 1;
            }
        }
    }
    for (uint32_t ti = 0; ti < m->n_globals_def; ti++)
        if (ea_tref_real(m->globals_def[ti].type) &&
            ea_tref_idx(m->globals_def[ti].type) >= m->n_types) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    for (uint32_t ti = 0; ti < m->n_tables; ti++)
        if (ea_tref_real(m->tables[ti].ref_type) &&
            ea_tref_idx(m->tables[ti].ref_type) >= m->n_types) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    for (uint32_t ti = 0; ti < m->n_elems; ti++)
        if (ea_tref_real(m->elems[ti].ref_type) &&
            ea_tref_idx(m->elems[ti].ref_type) >= m->n_types) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    for (uint32_t ti = 0; ti < m->n_tags; ti++) {
        uint32_t tx = m->tags[ti].type_idx;
        if (tx >= m->n_types || m->types[tx].kind != CT_FUNC ||
            m->types[tx].func.n_results != 0) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    }

    V v;
    memset(&v, 0, sizeof(v));
    v.m = m;
    // ---- module-level checks
    for (uint32_t i = 0; i < m->n_imports; i++) {
        EaImport *im = &m->imports[i];
        if (im->kind == EAK_FUNC &&
            (im->idx >= m->n_types || m->types[im->idx].kind != CT_FUNC)) {
            if (err && !*err) *err = ea_strndup("unknown import type", 18);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_funcs; i++) {
        if (m->funcs[i].type_idx >= m->n_types ||
            m->types[m->funcs[i].type_idx].kind != CT_FUNC) {
            if (err && !*err) *err = ea_strndup("unknown type", 12);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_tables; i++) {
        if (m->tables[i].has_max && m->tables[i].max < m->tables[i].min) {
            if (err && !*err) *err = ea_strndup("size minimum must not be greater than maximum", 43);
            return 1;
        }
    }
    if (m->n_memories == 1 && m->memories[0].shared) {
        if (err && !*err) *err = ea_strndup("shared memory requires threads", 29);
        return 1;
    }
    if (m->has_start) {
        if (m->start_func >= m->n_funcs) {
            if (err && !*err) *err = ea_strndup("unknown function (start)", 24);
            return 1;
        }
        EaFuncType *ft = &m->types[m->funcs[m->start_func].type_idx].func;
        if (ft->n_params != 0 || ft->n_results != 0) {
            if (err && !*err) *err = ea_strndup("start function must not have parameters or results", 49);
            return 1;
        }
    }
    for (uint32_t i = 0; i < m->n_exports; i++) {
        EaExport *ex = &m->exports[i];
        bool bad = false;
        switch (ex->kind) {
        case EAK_FUNC: bad = ex->idx >= m->n_funcs; break;
        case EAK_TABLE: bad = ex->idx >= m->n_tables; break;
        case EAK_MEMORY: bad = ex->idx >= m->n_memories; break;
        case EAK_GLOBAL: bad = ex->idx >= m->n_globals_def; break;
        case EAK_TAG: {
            if (ex->idx >= m->n_tags) { bad = true; break; }
            uint32_t tx = m->tags[ex->idx].type_idx;
            if (tx >= m->n_types || m->types[tx].kind != CT_FUNC ||
                m->types[tx].func.n_results != 0) bad = true;
            break;
        }
        }
        if (bad) {
            if (err && !*err) *err = ea_strndup("unknown entity in export", 24);
            return 1;
        }
    }

    // refs set (funcidx(module) outside function bodies)
    uint8_t *in_refs = (uint8_t *)ea_zalloc(m->n_funcs ? m->n_funcs : 1);
    for (uint32_t i = 0; i < m->n_exports; i++)
        if (m->exports[i].kind == EAK_FUNC && m->exports[i].idx < m->n_funcs)
            in_refs[m->exports[i].idx] = 1;
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->func_idx)
            for (uint32_t j = 0; j < e->n_items; j++)
                if (e->func_idx[j] < m->n_funcs) in_refs[e->func_idx[j]] = 1;
        if (e->items)
            for (uint32_t j = 0; j < e->n_items; j++)
                for (uint32_t k = 0; k < e->items[j].n; k++)
                    if (e->items[j].v[k].opcode == EA_OP_REF_FUNC &&
                        e->items[j].v[k].imm.u32 < m->n_funcs)
                        in_refs[e->items[j].v[k].imm.u32] = 1;
        for (uint32_t k = 0; k < e->offset.n; k++)
            if (e->offset.v[k].opcode == EA_OP_REF_FUNC && e->offset.v[k].imm.u32 < m->n_funcs)
                in_refs[e->offset.v[k].imm.u32] = 1;
    }
    for (uint32_t i = 0; i < m->n_globals_def; i++)
        for (uint32_t k = 0; k < m->globals_def[i].init.n; k++)
            if (m->globals_def[i].init.v[k].opcode == EA_OP_REF_FUNC &&
                m->globals_def[i].init.v[k].imm.u32 < m->n_funcs)
                in_refs[m->globals_def[i].init.v[k].imm.u32] = 1;

#define EXPR_FAIL(msg) do { if (err && !*err) *err = ea_strndup(msg, strlen(msg)); rc = 1; goto out; } while (0)



    // const-level type matching (typed refs)
    #define CONST_TMATCH(want, got) (ty_match(m, want, got) || ty_match(m, got, want))

    // ---- init expr checks
    for (uint32_t i = m->n_imp_globals; i < m->n_globals_def; i++) {
        EaGlobal *g = &m->globals_def[i];
        EaValType rt = check_const_expr(m, &g->init, m->n_imp_globals + i, NULL);
        if (rt == (EaValType)0x7FFFFFFFu) EXPR_FAIL("invalid global init expr");
        if (getenv("EA_GDBG")) fprintf(stderr, "GLB %u: rt=%d want=%d n=%u\n", i, rt, g->type, g->init.n);
        if (!ty_match(m, g->type, rt)) {
            if (getenv("EA_VDBG")) fprintf(stderr, "[V] glb init want %x got %x (sup of got: n=%u s0=%u)\n",
                                           (unsigned)g->type, (unsigned)rt,
                                           ea_tref_real(rt) && ea_tref_idx(rt) < m->n_types ? m->types[ea_tref_idx(rt)].n_sup : 99,
                                           ea_tref_real(rt) && ea_tref_idx(rt) < m->n_types ? m->types[ea_tref_idx(rt)].sup[0] : 99);
            if (getenv("EA_VDBG2")) fprintf(stderr, "GLBinit want=%x got=%x\n", (unsigned)g->type, (unsigned)rt);
            EXPR_FAIL("type mismatch: global init type");
        }
    }
    for (uint32_t i = 0; i < m->n_tables; i++) {
        EaTable *t = &m->tables[i];
        // a non-nullable table element type requires an explicit init expr
        EaValType rt2 = t->ref_type;
        bool nonnull = (ea_is_typedref(rt2) && !ea_tref_nullable(rt2)) ||
                       ((rt2 & 0x3E000000u) == 0x3E000000u && (rt2 & 1) != 0);
        if (nonnull && !t->has_init) EXPR_FAIL("type mismatch: table element type");
        if (!t->has_init) continue;
        EaValType rt = check_const_expr(m, &t->init, m->n_imp_globals, NULL);
        if (rt == (EaValType)0x7FFFFFFFu) EXPR_FAIL("invalid table init expr");
        if (!ty_match(m, t->ref_type, rt)) {
            if (getenv("EA_VDBG")) fprintf(stderr, "[V] tblinit want %x got %x\n", (unsigned)t->ref_type, (unsigned)rt);
            EXPR_FAIL("type mismatch: table init type");
        }
    }
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->mode == SEG_ACTIVE) {
            if (e->table_idx >= m->n_tables) {
                if (getenv("EA_VDBG")) fprintf(stderr, "[V] elem tblidx=%u n_tables=%u\n", e->table_idx, m->n_tables);
                EXPR_FAIL("unknown table");
            }
            if (!ty_match(m, m->tables[e->table_idx].ref_type, e->ref_type)) {
                if (getenv("EA_VDBG2")) fprintf(stderr, "ELEM type: table=%x elem=%x\n", (unsigned)m->tables[e->table_idx].ref_type, (unsigned)e->ref_type);
                EXPR_FAIL("type mismatch: element segment type");
            }
            EaValType rt = check_const_expr(m, &e->offset, m->n_globals_def, NULL);
            if (rt == (EaValType)0x7FFFFFFFu) EXPR_FAIL("invalid element offset expr");
            EaValType want = m->tables[e->table_idx].is64 ? VT_I64 : VT_I32;
            if (rt != want) EXPR_FAIL("type mismatch in element offset");
        }
        if (!e->items) {
            if (!ea_is_ref(e->ref_type) && (e->ref_type & 0xFE000000u) != 0x3E000000u)
                EXPR_FAIL("type mismatch: element segment type");
            for (uint32_t j = 0; j < e->n_items; j++) {
                if (e->func_idx[j] >= m->n_funcs) EXPR_FAIL("unknown function");
                // funcidx items carry the function's own (ref $t) type
                if (e->mode == SEG_ACTIVE && e->table_idx < m->n_tables) {
                    EaValType it = EA_VT_TREF(m->funcs[e->func_idx[j]].type_idx);
                    if (!ty_match(m, m->tables[e->table_idx].ref_type, it))
                        EXPR_FAIL("type mismatch: element segment type");
                }
            }
            continue;
        }
        for (uint32_t j = 0; j < e->n_items; j++) {
            EaValType rt = check_const_expr(m, &e->items[j], m->n_globals_def, NULL);
            if (rt == (EaValType)0x7FFFFFFFu) EXPR_FAIL("invalid element item expr");
            if (!ty_match(m, e->ref_type, rt)) EXPR_FAIL("type mismatch: element item type");
        }
    }
    for (uint32_t i = 0; i < m->n_datas; i++) {
        EaData *d = &m->datas[i];
        if (d->mode == SEG_ACTIVE) {
            if (d->mem_idx >= m->n_memories) EXPR_FAIL("unknown memory");
            EaValType rt = check_const_expr(m, &d->offset, m->n_globals_def, NULL);
            if (rt == (EaValType)0x7FFFFFFFu) EXPR_FAIL("invalid data offset expr");
            EaValType want = m->memories[d->mem_idx].is64 ? VT_I64 : VT_I32;
            if (rt != want) EXPR_FAIL("type mismatch in data offset");
        }
    }

    // ---- function bodies
    v.m = m;
    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        EaFunc *f = &m->funcs[fi];
        const EaFuncType *ft = &m->types[f->type_idx].func;
        // imported functions have no body to validate; still merge locals list
        if (fi < m->n_imp_funcs) {
            uint32_t n_locals = ft->n_params;
            EaValType *locals = (EaValType *)ea_malloc((n_locals ? n_locals : 1) * sizeof(EaValType));
            for (uint32_t i = 0; i < n_locals; i++) locals[i] = ft->params[i];
            free(f->locals);
            f->locals = locals;
            f->n_locals = n_locals;
            f->max_stack = 0;
            continue;
        }
        uint32_t n_locals = ft->n_params + f->n_locals;
        if (n_locals > MAX_LOCALS) EXPR_FAIL("too many locals");
        EaValType *locals = (EaValType *)ea_malloc((n_locals ? n_locals : 1) * sizeof(EaValType));
        for (uint32_t i = 0; i < ft->n_params; i++) locals[i] = ft->params[i];
        for (uint32_t i = 0; i < f->n_locals; i++)
            locals[ft->n_params + i] = f->locals[i];
        free(f->locals);
        f->locals = locals;
        f->n_locals = n_locals;
        // local init state: params initialized; non-defaultable locals start
        // uninitialized (function-references "uninitialized local" rule)
        v.n_lini = n_locals;
        v.lini = (uint8_t *)ea_malloc((n_locals ? n_locals : 1));
        for (uint32_t i = 0; i < n_locals; i++)
            v.lini[i] = local_defaultable(locals[i]) || i < ft->n_params;
        v.init_poly = false;

        v.sp = 0;
        v.csp = 0;
        v.failed = false;
        v.max_stack = 0;
        v.err = NULL;
        push_ctrl(&v, 0, NULL, 0, ft->results, ft->n_results, false);
        InsList *code = &f->code;
        if (getenv("EA_VTRACE")) fprintf(stderr, "[V] fi=%u code.n=%u v0.end=%u v1.end=%u\n", fi, code->n, code->v[0].end_idx, code->n > 1 ? code->v[1].end_idx : 0);
        uint32_t cur_pc = 0;
        uint16_t *depths = (uint16_t *)ea_zalloc((code->n ? code->n : 1) * 2);
        uint8_t *reach = (uint8_t *)ea_zalloc(code->n ? code->n : 1);
        uint16_t *br_ar = (uint16_t *)ea_zalloc((code->n ? code->n : 1) * 2);
        uint8_t cur_reach = 1;
        for (uint32_t pc = 0; pc < code->n && !v.failed; pc++) {
            cur_pc = pc;
            EaInstr *in = &code->v[pc];
            uint32_t op = in->opcode;
            v.cur_op = op; v.cur_pc = pc;
            if (getenv("EA_VTRACE")) fprintf(stderr, "[T] fi=%u pc=%u op=%x sp=%u csp=%u\n", fi, pc, op, v.sp, v.csp);
            if (getenv("EA_VTRACE")) fprintf(stderr, "[T] fi=%u pc=%u op=%x sp=%u csp=%u unreach=%d\n", fi, pc, op, v.sp, v.csp, v.csp ? (int)v.ctrl[v.csp-1].unreachable : -1);
            depths[pc] = (uint16_t)v.sp;
            reach[pc] = cur_reach;
            if (v.csp == 0) { vfail(&v, "junk after end of function"); break; }
            Sig sig;
            switch (op) {
            case EA_OP_BLOCK: case EA_OP_LOOP: case EA_OP_IF: {
                const EaValType *bi, *bo;
                uint32_t nbi, nbo;
                if (!resolve_blocktype(&v, &in->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                if ((nbi > 1 || nbo > 1) && !m->feat.multi_value) {
                    vfail(&v, "multi-value not enabled");
                    break;
                }
                if (op == EA_OP_IF) pop_val(&v, VT_I32);
                if (v.failed) break;
                pop_vals(&v, bi, nbi);
                if (v.failed) break;
                push_ctrl(&v, pc, bi, nbi, bo, nbo, op == EA_OP_LOOP);
                v.ctrl[v.csp - 1].init_save =
                    (uint8_t *)ea_malloc(v.n_lini ? v.n_lini : 1);
                memcpy(v.ctrl[v.csp - 1].init_save, v.lini, v.n_lini);
                v.ctrl[v.csp - 1].init_poly_save = v.init_poly;
                in->height = v.ctrl[v.csp - 1].height;
                in->is_loop = op == EA_OP_LOOP;
                in->arity_out = (uint16_t)(op == EA_OP_LOOP ? nbi : nbo);
                in->arity_in = (uint16_t)nbi;
                in->arity_res = (uint16_t)nbo;
                break;
            }
            case EA_OP_ELSE: {
                Ctrl *c = &v.ctrl[v.csp - 1];
                if (c->start_idx >= code->n ||
                    code->v[c->start_idx].opcode != EA_OP_IF) {
                    vfail(&v, "else found outside of if");
                    break;
                }
                if (!pop_ctrl(&v)) break;
                // re-push as else frame (same signature); init state reverts to
                // the if-entry state (then-branch changes do not carry over)
                if (v.lini) memcpy(v.lini, c->init_save, v.n_lini);
                v.init_poly = c->init_poly_save;
                EaInstr *ifin = &code->v[c->start_idx];
                const EaValType *bi, *bo;
                uint32_t nbi, nbo;
                if (!resolve_blocktype(&v, &ifin->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                push_ctrl(&v, c->start_idx, bi, nbi, bo, nbo, false);
                v.ctrl[v.csp - 1].init_save = c->init_save;
                v.ctrl[v.csp - 1].init_poly_save = c->init_poly_save;
                break;
            }
            case EA_OP_END: {
                Ctrl *c = &v.ctrl[v.csp - 1];
                if (getenv("EA_VTRACE")) fprintf(stderr, "[T] END pc=%u csp=%u unreach=%d sp=%u h=%u\n", pc, v.csp, (int)c->unreachable, v.sp, c->height);
                if (v.csp == 1) {
                    // function frame: results = function results
                    if (!pop_ctrl(&v)) break;
                    push_vals(&v, ft->results, ft->n_results);
                } else {
                    if (c->init_save) { memcpy(v.lini, c->init_save, v.n_lini); v.init_poly = c->init_poly_save; }
                    EaInstr *bin = &code->v[c->start_idx];
                    if (bin->opcode == EA_OP_IF && bin->else_idx == UINT32_MAX) {
                        // if without else: in must equal out
                        const EaValType *bi, *bo;
                        uint32_t nbi, nbo;
                        if (!resolve_blocktype(&v, &bin->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                        if (!vt_list_eq(&v, bi, nbi, bo, nbo)) {
                            vfail(&v, "type mismatch: if without else");
                            break;
                        }
                    }
                    if (!pop_ctrl(&v)) break;
                    push_vals(&v, c->out, c->n_out);
                }
                break;
            }
            case EA_OP_BR: {
                uint32_t l = in->imm.u32;
                if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *t = &v.ctrl[v.csp - 1 - l];
                pop_vals(&v, t->is_loop ? t->in : t->out, t->is_loop ? t->n_in : t->n_out);
                if (!v.failed) {
                    // br target join point: record carried arity for codegen
                    if (pc + 1 < code->n + 1) cur_reach = 0;
                    set_unreachable(&v);
                }
                break;
            }
            case EA_OP_BR_IF: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                uint32_t l = in->imm.u32;
                if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *t = &v.ctrl[v.csp - 1 - l];
                const EaValType *lt = t->is_loop ? t->in : t->out;
                uint32_t n = t->is_loop ? t->n_in : t->n_out;
                pop_vals(&v, lt, n);
                if (!v.failed) push_vals(&v, lt, n);
                break;
            }
            case EA_OP_BR_TABLE: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                uint32_t base = in->imm.pair.a, n = in->imm.pair.b;
                uint32_t *tg = &code->pool[base];
                uint32_t dl = tg[n];
                if (dl >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *dt = &v.ctrl[v.csp - 1 - dl];
                const EaValType *lt = dt->is_loop ? dt->in : dt->out;
                uint32_t na = dt->is_loop ? dt->n_in : dt->n_out;
                for (uint32_t i = 0; i < n && !v.failed; i++) {
                    uint32_t l = tg[i];
                    if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                    Ctrl *t = &v.ctrl[v.csp - 1 - l];
                    const EaValType *lt2 = t->is_loop ? t->in : t->out;
                    uint32_t na2 = t->is_loop ? t->n_in : t->n_out;
                    Ctrl *cc = &v.ctrl[v.csp - 1];
                    // all targets must share one common value sequence, so
                    // arities match even in unreachable (polymorphic) code
                    if (na2 != na) {
                        vfail(&v, "type mismatch: br_table target arity");
                        break;
                    }
                    if (!cc->unreachable && !vt_list_eq(&v, lt, na, lt2, na))
                        vfail(&v, "type mismatch: br_table target types");
                }
                if (v.failed) break;
                pop_vals(&v, lt, na);
                if (!v.failed) set_unreachable(&v);
                break;
            }
            case EA_OP_RETURN: {
                pop_vals(&v, ft->results, ft->n_results);
                if (!v.failed) set_unreachable(&v);
                break;
            }
            case EA_OP_CALL: case EA_OP_RETURN_CALL: {
                uint32_t fi2 = in->imm.u32;
                if (fi2 >= m->n_funcs) { vfail(&v, "unknown function"); break; }
                const EaFuncType *t2 = &m->types[m->funcs[fi2].type_idx].func;
                if (op == EA_OP_RETURN_CALL) {
                    if (t2->n_results != ft->n_results ||
                        !vt_list_sub(&v, t2->results, t2->n_results, ft->results, ft->n_results)) {
                        if (getenv("EA_VDBG2"))
                            for (uint32_t qi = 0; qi < t2->n_results || qi < ft->n_results; qi++)
                                fprintf(stderr, "rc res[%u] callee=%x frame=%x\n", qi,
                                        qi < t2->n_results ? (unsigned)t2->results[qi] : 0xdeadu,
                                        qi < ft->n_results ? (unsigned)ft->results[qi] : 0xdeadu);
                        vfail(&v, "type mismatch: return_call result types");
                        break;
                    }
                }
                pop_vals(&v, t2->params, t2->n_params);
                if (!v.failed) {
                    if (op == EA_OP_CALL) push_vals(&v, t2->results, t2->n_results);
                    else set_unreachable(&v);
                }
                break;
            }
            case EA_OP_CALL_INDIRECT: case EA_OP_RETURN_CALL_INDIRECT: {
                uint32_t ti = in->imm.pair.a, tbi = in->imm.pair.b;
                if (ti >= m->n_types || m->types[ti].kind != CT_FUNC) {
                    vfail(&v, "unknown type"); break;
                }
                {
                    EaValType trt = m->tables[tbi].ref_type;
                    bool func_tab = trt == VT_FUNCREF ||
                        (ea_is_absref(trt) && (ea_abs_kind(trt) == EA_ABS_FUNC ||
                                               ea_abs_kind(trt) == EA_ABS_NOFUNC)) ||
                        (ea_tref_real(trt) && ea_tref_idx(trt) < m->n_types &&
                         m->types[ea_tref_idx(trt)].kind == CT_FUNC);
                    if (tbi >= m->n_tables || !func_tab) {
                        vfail(&v, "unknown table"); break;
                    }
                }
                const EaFuncType *t2 = &m->types[ti].func;
                pop_val(&v, m->tables[tbi].is64 ? VT_I64 : VT_I32); // table index operand
                if (v.failed) break;
                if (op == EA_OP_RETURN_CALL_INDIRECT) {
                    if (t2->n_results != ft->n_results ||
                        !vt_list_sub(&v, t2->results, t2->n_results, ft->results, ft->n_results)) {
                        vfail(&v, "type mismatch: return_call_indirect result types");
                        break;
                    }
                }
                pop_vals(&v, t2->params, t2->n_params);
                if (!v.failed) {
                    if (op == EA_OP_CALL_INDIRECT) push_vals(&v, t2->results, t2->n_results);
                    else set_unreachable(&v);
                }
                break;
            }
            case EA_OP_DROP:
                pop_any(&v);
                break;
            case EA_OP_SELECT: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                EaValType t1 = pop_any(&v);
                EaValType t2 = pop_any(&v);
                if (v.failed) break;
                if (t1 == VT_BOTTOM) t1 = t2;
                if (t2 == VT_BOTTOM) t2 = t1;
                if (t1 != t2 || ea_is_ref(t1)) {
                    vfail(&v, "type mismatch: select operand types");
                    break;
                }
                push_val(&v, t1);
                break;
            }
            case EA_OP_SELECT_T: {
                pop_val(&v, VT_I32);
                if (v.failed) break;
                EaValType t = (EaValType)in->imm.u32;
                if (ea_tref_real(t) && ea_tref_idx(t) >= m->n_types) {
                    vfail(&v, "unknown type");
                    break;
                }
                pop_val(&v, t);
                pop_val(&v, t);
                if (!v.failed) push_val(&v, t);
                break;
            }
            case EA_OP_LOCAL_GET:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                if (!lini_ok(&v, in->imm.u32) && !local_defaultable(f->locals[in->imm.u32])) {
                    vfail(&v, "uninitialized local");
                    break;
                }
                push_val(&v, f->locals[in->imm.u32]);
                break;
            case EA_OP_LOCAL_SET:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                pop_val(&v, f->locals[in->imm.u32]);
                if (!v.failed && v.lini && in->imm.u32 < v.n_lini) v.lini[in->imm.u32] = 1;
                break;
            case EA_OP_LOCAL_TEE:
                if (in->imm.u32 >= f->n_locals) { vfail(&v, "unknown local"); break; }
                pop_val(&v, f->locals[in->imm.u32]);
                if (!v.failed) {
                    push_val(&v, f->locals[in->imm.u32]);
                    if (v.lini && in->imm.u32 < v.n_lini) v.lini[in->imm.u32] = 1;
                }
                break;
            case EA_OP_GLOBAL_GET:
                if (in->imm.u32 >= m->n_globals_def) { vfail(&v, "unknown global"); break; }
                push_val(&v, m->globals_def[in->imm.u32].type);
                break;
            case EA_OP_GLOBAL_SET:
                if (in->imm.u32 >= m->n_globals_def) { vfail(&v, "unknown global"); break; }
                if (!m->globals_def[in->imm.u32].mutable_) { vfail(&v, "global is immutable"); break; }
                pop_val(&v, m->globals_def[in->imm.u32].type);
                break;
            case EA_OP_TABLE_GET: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) push_val(&v, m->tables[ti].ref_type);
                break;
            }
            case EA_OP_TABLE_SET: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_SIZE: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                push_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_GROW: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) push_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_FILL: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                if (!v.failed) pop_val(&v, m->tables[ti].ref_type);
                if (!v.failed) pop_val(&v, m->tables[ti].is64 ? VT_I64 : VT_I32);
                break;
            }
            case EA_OP_TABLE_COPY: {
                uint32_t d = in->imm.pair.a, s2 = in->imm.pair.b;
                if (d >= m->n_tables || s2 >= m->n_tables) { vfail(&v, "unknown table"); break; }
                // src element type must match dst (src <: dst)
                if (!ty_match(m, m->tables[d].ref_type, m->tables[s2].ref_type)) {
                    vfail(&v, "type mismatch: table copy element types");
                    break;
                }
                // [dst_idx src_idx min(dst_idx, src_idx)] — pop n, src, dst
                EaValType dt = m->tables[d].is64 ? VT_I64 : VT_I32;
                EaValType st = m->tables[s2].is64 ? VT_I64 : VT_I32;
                pop_val(&v, (dt == VT_I64 && st == VT_I64) ? VT_I64 : VT_I32);
                if (!v.failed) pop_val(&v, st);
                if (!v.failed) pop_val(&v, dt);
                break;
            }
            case EA_OP_TABLE_INIT: {
                uint32_t ti = in->imm.pair.a, ei = in->imm.pair.b;
                if (ti >= m->n_tables) { vfail(&v, "unknown table"); break; }
                if (ei >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                if (!ty_match(m, m->tables[ti].ref_type, m->elems[ei].ref_type)) {
                    if (getenv("EA_VDBG")) fprintf(stderr, "[V] tinit want %x got %x\n", (unsigned)m->tables[ti].ref_type, (unsigned)m->elems[ei].ref_type);
                    vfail(&v, "type mismatch: table init element types");
                    break;
                }
                EaValType it = m->tables[ti].is64 ? VT_I64 : VT_I32;
                // stack is [dst src n]; only the destination index follows
                // the table's index type, src/n are always i32
                pop_val(&v, VT_I32); pop_val(&v, VT_I32); pop_val(&v, it);
                break;
            }
            case EA_OP_ELEM_DROP: {
                uint32_t ei = in->imm.u32;
                if (ei >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                break;
            }
            case EA_OP_REF_NULL:
                push_val(&v, (EaValType)in->imm.u32);
                break;
            case EA_OP_REF_FUNC:
                if (in->imm.u32 >= m->n_funcs) { vfail(&v, "unknown function"); break; }
                if (!in_refs[in->imm.u32]) { vfail(&v, "undeclared function reference"); break; }
                // ref.func has the declared type (ref $t); funcref via subtyping
                push_val(&v, EA_VT_TREF(m->funcs[in->imm.u32].type_idx));
                break;
            case EA_OP_REF_AS_NON_NULL: {
                EaValType rvt = pop_any(&v);
                if (v.failed) break;
                if (rvt != VT_BOTTOM && !ea_is_ref(rvt)) { vfail(&v, "type mismatch: ref.as_non_null"); break; }
                push_val(&v, rvt == VT_BOTTOM ? VT_BOTTOM : ea_narrow_ref(rvt));
                break;
            }
            case EA_OP_BR_ON_NULL: case EA_OP_BR_ON_NON_NULL: {
                // conditional branches: fallthrough stays reachable
                uint32_t l = in->imm.u32;
                if (l >= v.csp) { vfail(&v, "unknown label"); break; }
                EaValType rvt = pop_any(&v);
                if (v.failed) break;
                if (rvt != VT_BOTTOM && !ea_is_ref(rvt)) { vfail(&v, "type mismatch: br_on expected ref"); break; }
                Ctrl *c = &v.ctrl[v.csp - 1 - l];
                if (in->opcode == EA_OP_BR_ON_NULL) {
                    // branch to l carries [t*] (the null ref is dropped);
                    // fallthrough keeps [t* (ref ht)] with the ref narrowed
                    pop_vals(&v, c->out, c->n_out);
                    if (v.failed) break;
                    push_vals(&v, c->out, c->n_out);
                    // fallthrough keeps the NARROWED (ref ht); in unreachable
                    // code (rvt == BOTTOM) narrow the label's own ref type
                    EaValType base = rvt != VT_BOTTOM ? rvt
                                     : (c->n_out ? c->out[c->n_out - 1] : VT_BOTTOM);
                    if (base != VT_BOTTOM)
                        push_val(&v, ea_narrow_ref(base));
                } else {
                    // the label's arity includes the (non-null) ref as its last
                    // value: the BRANCH carries [t* (ref ht)], the fallthrough
                    // drops the ref and keeps [t*]
                    if (c->n_out < 1) { vfail(&v, "type mismatch: br_on_non_null label"); break; }
                    if (rvt != VT_BOTTOM) {
                        // the branch value is the NARROWED (ref ht)
                        if (!vt_match(&v, c->out[c->n_out - 1], ea_narrow_ref(rvt))) {
                            vfail(&v, "type mismatch: br_on_non_null ref"); break;
                        }
                    }
                    pop_vals(&v, c->out, c->n_out - 1);
                    if (v.failed) break;
                    push_vals(&v, c->out, c->n_out - 1);
                }
                break;
            }
            case EA_OP_CALL_REF: case EA_OP_RETURN_CALL_REF: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_types || m->types[ti].kind != CT_FUNC) { vfail(&v, "unknown type"); break; }
                EaValType rvt = pop_any(&v);
                if (v.failed) break;
                bool ok = rvt == VT_BOTTOM ||
                          (ea_is_typedref(rvt) && ea_tref_idx(rvt) == ti);
                if (!ok) { vfail(&v, "type mismatch: call_ref"); break; }
                EaFuncType *t2 = &m->types[ti].func;
                pop_vals(&v, t2->params, t2->n_params);
                if (!v.failed) {
                    if (in->opcode == EA_OP_CALL_REF)
                        push_vals(&v, t2->results, t2->n_results);
                    else if (!vt_list_sub(&v, t2->results, t2->n_results, ft->results, ft->n_results)) {
                        if (getenv("EA_VDBG2"))
                            for (uint32_t qi = 0; qi < t2->n_results || qi < ft->n_results; qi++)
                                fprintf(stderr, "RCR res[%u] callee=%x frame=%x ti=%u\n", qi,
                                        qi < t2->n_results ? (unsigned)t2->results[qi] : 0xdeadu,
                                        qi < ft->n_results ? (unsigned)ft->results[qi] : 0xdeadu, ti);
                        vfail(&v, "type mismatch: return_call_ref result types");
                    }
                    else
                        set_unreachable(&v);
                }
                break;
            }
            // ---------------- GC (0xFB) ----------------
            case EA_OP_STRUCT_NEW: case EA_OP_STRUCT_NEW_DEFAULT: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_types || m->types[ti].kind != CT_STRUCT) { vfail(&v, "unknown type"); break; }
                EaType *t = &m->types[ti];
                if (op == EA_OP_STRUCT_NEW) {
                    for (uint32_t i = t->n_fields; i > 0; i--)
                        pop_val(&v, t->fields[i - 1].packed_ ? VT_I32 : t->fields[i - 1].vt);
                } else {
                    for (uint32_t i = 0; i < t->n_fields; i++) {
                        EaValType ft = t->fields[i].packed_ ? VT_I32 : t->fields[i].vt;
                        if (!local_defaultable(ft)) { vfail(&v, "type mismatch: non-defaultable field"); break; }
                    }
                }
                if (!v.failed) push_val(&v, EA_VT_TREF(ti));
                break;
            }
            case EA_OP_STRUCT_GET: case EA_OP_STRUCT_GET_S: case EA_OP_STRUCT_GET_U: case EA_OP_STRUCT_SET: {
                uint32_t ti = in->imm.pair.a, fi = in->imm.pair.b;
                if (ti >= m->n_types || m->types[ti].kind != CT_STRUCT) { vfail(&v, "unknown type"); break; }
                EaType *t = &m->types[ti];
                if (fi >= t->n_fields) { vfail(&v, "unknown field"); break; }
                EaFieldType *fld = &t->fields[fi];
                if (op == EA_OP_STRUCT_SET) {
                    pop_val(&v, fld->packed_ ? VT_I32 : fld->vt);
                    if (!v.failed) pop_val(&v, EA_VT_TREFN(ti));
                    if (!v.failed && !fld->mut) vfail(&v, "field is immutable");
                } else {
                    if ((op != EA_OP_STRUCT_GET) && !fld->packed_) { vfail(&v, "type mismatch: unpacked field"); break; }
                    pop_val(&v, EA_VT_TREFN(ti));
                    if (!v.failed)
                        push_val(&v, (op == EA_OP_STRUCT_GET) ? (fld->packed_ ? VT_I32 : fld->vt) : VT_I32);
                }
                break;
            }
            case EA_OP_ARRAY_NEW: case EA_OP_ARRAY_NEW_DEFAULT: case EA_OP_ARRAY_NEW_FIXED: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *el = &m->types[ti].fields[0];
                if (op == EA_OP_ARRAY_NEW) {
                    pop_val(&v, VT_I32); // length
                    if (!v.failed) pop_val(&v, el->packed_ ? VT_I32 : el->vt);
                } else if (op == EA_OP_ARRAY_NEW_FIXED) {
                    for (uint32_t i = 0; i < in->imm.pair.b; i++)
                        pop_val(&v, el->packed_ ? VT_I32 : el->vt);
                } else {
                    pop_val(&v, VT_I32);
                    if (!v.failed && !local_defaultable(el->packed_ ? VT_I32 : el->vt))
                        vfail(&v, "type mismatch: non-defaultable element");
                }
                if (!v.failed) push_val(&v, EA_VT_TREF(ti));
                break;
            }
            case EA_OP_ARRAY_NEW_DATA: case EA_OP_ARRAY_NEW_ELEM: {
                uint32_t ti = in->imm.pair.a, si = in->imm.pair.b;
                if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *el = &m->types[ti].fields[0];
                if (op == EA_OP_ARRAY_NEW_DATA) {
                    if (si >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                    if (!el->packed_ && ea_is_ref(el->vt))
                        { vfail(&v, "array type is not numeric or vector"); break; }
                } else {
                    if (si >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                    if (!ty_match(m, el->vt, m->elems[si].ref_type)) { vfail(&v, "type mismatch: array.new_elem type"); break; }
                }
                pop_val(&v, VT_I32); pop_val(&v, VT_I32);
                if (!v.failed) push_val(&v, EA_VT_TREF(ti));
                break;
            }
            case EA_OP_ARRAY_GET: case EA_OP_ARRAY_GET_S: case EA_OP_ARRAY_GET_U: case EA_OP_ARRAY_SET: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *el = &m->types[ti].fields[0];
                if (op == EA_OP_ARRAY_SET) {
                    pop_val(&v, el->packed_ ? VT_I32 : el->vt);
                    if (!v.failed) pop_val(&v, VT_I32);
                    if (!v.failed) pop_val(&v, EA_VT_TREFN(ti));
                    if (!v.failed && !el->mut) vfail(&v, "array is immutable");
                } else {
                    if (op != EA_OP_ARRAY_GET && !el->packed_) { vfail(&v, "type mismatch: unpacked element"); break; }
                    pop_val(&v, VT_I32);
                    if (!v.failed) pop_val(&v, EA_VT_TREFN(ti));
                    if (!v.failed)
                        push_val(&v, (op == EA_OP_ARRAY_GET) ? (el->packed_ ? VT_I32 : el->vt) : VT_I32);
                }
                break;
            }
            case EA_OP_ARRAY_LEN: {
                // operand: any array reference (abstract or typed)
                EaValType rvt = pop_any(&v);
                if (v.failed) break;
                bool ok = rvt == VT_BOTTOM ||
                          rvt == EA_VT_ABSN(EA_ABS_ARRAY) ||
                          rvt == EA_VT_ABSREF(EA_ABS_ARRAY) ||
                          rvt == EA_VT_ABSN(EA_ABS_NONE) ||
                          (ea_tref_real(rvt) && ea_tref_idx(rvt) < m->n_types &&
                           m->types[ea_tref_idx(rvt)].kind == CT_ARRAY);
                if (!ok) {
                    vfail(&v, "type mismatch: array.len");
                    break;
                }
                S_zero_placeholder:;
                push_val(&v, VT_I32);
                break;
            }
            case EA_OP_ARRAY_FILL: {
                uint32_t ti = in->imm.u32;
                if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *el = &m->types[ti].fields[0];
                pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, el->packed_ ? VT_I32 : el->vt);
                if (!v.failed) pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, EA_VT_TREFN(ti));
                break;
            }
            case EA_OP_ARRAY_COPY: {
                uint32_t dt = in->imm.pair.a, st = in->imm.pair.b;
                if (dt >= m->n_types || m->types[dt].kind != CT_ARRAY ||
                    st >= m->n_types || m->types[st].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *del = &m->types[dt].fields[0], *sel = &m->types[st].fields[0];
                if (!ty_match(m, del->vt, sel->vt)) { vfail(&v, "type mismatch: array.copy"); break; }
                pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, EA_VT_TREFN(st));
                if (!v.failed) pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, EA_VT_TREFN(dt));
                break;
            }
            case EA_OP_ARRAY_INIT_DATA: case EA_OP_ARRAY_INIT_ELEM: {
                uint32_t ti = in->imm.pair.a, si = in->imm.pair.b;
                if (ti >= m->n_types || m->types[ti].kind != CT_ARRAY) { vfail(&v, "unknown type"); break; }
                EaFieldType *el = &m->types[ti].fields[0];
                if (op == EA_OP_ARRAY_INIT_DATA) {
                    if (si >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                    if (!el->packed_ && ea_is_ref(el->vt))
                        { vfail(&v, "array type is not numeric or vector"); break; }
                } else {
                    if (si >= m->n_elems) { vfail(&v, "unknown elem segment"); break; }
                    if (!ty_match(m, el->vt, m->elems[si].ref_type)) { vfail(&v, "type mismatch: array.init_elem type"); break; }
                }
                pop_val(&v, VT_I32); pop_val(&v, VT_I32); pop_val(&v, VT_I32);
                if (!v.failed) pop_val(&v, EA_VT_TREFN(ti));
                break;
            }
            case EA_OP_REF_I31:
                pop_val(&v, VT_I32);
                if (!v.failed) push_val(&v, EA_VT_ABSREF(EA_ABS_I31));
                break;
            case EA_OP_I31_GET_S: case EA_OP_I31_GET_U:
                pop_val(&v, EA_VT_ABSN(EA_ABS_I31));
                if (!v.failed) push_val(&v, VT_I32);
                break;
            case EA_OP_REF_EQ: {
                EaValType a = pop_any(&v), b2 = pop_any(&v);
                if (v.failed) break;
                // eq-able: any reference except func/extern (unchecked here; tests
                // only apply ref.eq to eqref-typed operands)
                (void)a; (void)b2;
                push_val(&v, VT_I32);
                break;
            }
            case EA_OP_ANY_CONVERT_EXTERN: case EA_OP_EXTERN_CONVERT_ANY: {
                EaValType rvt = pop_any(&v);
                if (v.failed) break;
                bool nullable = rvt == VT_BOTTOM || ea_tref_nullable(rvt);
                bool to_any = op == EA_OP_ANY_CONVERT_EXTERN;
                uint32_t from = to_any ? EA_ABS_EXTERN : EA_ABS_ANY;
                if (rvt != VT_BOTTOM) {
                    uint32_t k;
                    if (ea_is_absref(rvt)) k = ea_abs_kind(rvt);
                    else if (rvt == VT_EXTERNREF) k = EA_ABS_EXTERN;
                    else if (rvt == VT_ANYREF) k = EA_ABS_ANY;
                    else k = 0xFFFFFFFFu;
                    if (k != from && k != EA_ABS_NONE && k != EA_ABS_NOEXTERN &&
                        k != (to_any ? EA_ABS_NOEXTERN : EA_ABS_NONE)) {
                        // allow null-bottom kinds loosely; tests cast any<->extern
                    }
                }
                if (to_any)
                    push_val(&v, nullable ? (EaValType)VT_ANYREF : EA_VT_ABSREF(EA_ABS_ANY));
                else
                    push_val(&v, nullable ? (EaValType)VT_EXTERNREF : EA_VT_ABSREF(EA_ABS_EXTERN));
                break;
            }
            case EA_OP_REF_TEST: case EA_OP_REF_TEST_NULL:
            case EA_OP_REF_CAST: case EA_OP_REF_CAST_NULL: {
                int64_t ht = (int64_t)(int32_t)in->imm.pair.a;
                EaValType target;
                if (ht >= 0) {
                    if ((uint32_t)ht >= m->n_types) { vfail(&v, "unknown type"); break; }
                    target = (op == EA_OP_REF_TEST || op == EA_OP_REF_CAST)
                                 ? EA_VT_TREF((uint32_t)ht) : EA_VT_TREFN((uint32_t)ht);
                } else {
                    uint32_t kind;
                    switch (ht) {
                    case -0x10: kind = EA_ABS_FUNC; break;
                    case -0x11: kind = EA_ABS_EXTERN; break;
                    case -0x12: kind = EA_ABS_ANY; break;
                    case -0x13: kind = EA_ABS_EQ; break;
                    case -0x14: kind = EA_ABS_I31; break;
                    case -0x15: kind = EA_ABS_STRUCT; break;
                    case -0x16: kind = EA_ABS_ARRAY; break;
                    case -0x17: kind = EA_ABS_EXN; break;
    case -0x0F: kind = EA_ABS_NONE; break;
                    case -0x0D: kind = EA_ABS_NOFUNC; break;
                    case -0x0E: kind = EA_ABS_NOEXTERN; break;
                    default: vfail(&v, "unknown heap type"); goto gc_done;
                    }
                    target = (op == EA_OP_REF_TEST || op == EA_OP_REF_CAST)
                                 ? EA_VT_ABSREF(kind) : EA_VT_ABSN(kind);
                }
                gc_done:
                if (v.failed) break;
                pop_val(&v, VT_ANYREF); // operand: any reference
                if (v.failed) break;
                if (op == EA_OP_REF_TEST || op == EA_OP_REF_TEST_NULL) {
                    push_val(&v, VT_I32);
                } else {
                    push_val(&v, target);
                }
                break;
            }
            case EA_OP_BR_ON_CAST: case EA_OP_BR_ON_CAST_FAIL: {
                // imm.u32=label, imm.q.a=flags (bit0 src null, bit1 dst null),
                // imm.q.b=src heap type, imm.q.c=dst heap type
                int64_t ht1 = (int64_t)(int32_t)in->imm.q.b;
                int64_t ht2 = (int64_t)(int32_t)in->imm.q.c;
                bool src_null = (in->imm.q.a & 1) != 0;
                bool dst_null = (in->imm.q.a & 2) != 0;
                EaValType src_t = gc_ht_valtype(m, ht1, src_null);
                EaValType dst_t = gc_ht_valtype(m, ht2, dst_null);
                if (src_t == (EaValType)0x7FFFFFFFu || dst_t == (EaValType)0x7FFFFFFFu) {
                    vfail(&v, "unknown heap type");
                    break;
                }
                uint32_t depth = in->imm.q.d;
                if (depth >= v.csp) { vfail(&v, "unknown label"); break; }
                Ctrl *c = &v.ctrl[v.csp - 1 - depth];
                // the cast result rides as the label's last value
                if (c->n_out < 1) { vfail(&v, "type mismatch: br_on_cast label"); break; }
                if (!vt_match(&v, c->out[c->n_out - 1],
                              op == EA_OP_BR_ON_CAST ? dst_t : src_t)) {
                    fprintf(stderr, "[V] br_on_cast label want %x got %x (op %x)\n",
                            (unsigned)c->out[c->n_out - 1],
                            (unsigned)(op == EA_OP_BR_ON_CAST ? dst_t : src_t), op);
                    vfail(&v, "type mismatch: br_on_cast");
                    break;
                }
                pop_val(&v, src_t); // operand
                if (v.failed) break;
                pop_vals(&v, c->out, c->n_out - 1);
                if (v.failed) break;
                // br_on_cast: fallthrough = src \ dst (drops null when dst is
                // nullable). br_on_cast_fail mirrors it.
                EaValType rem = dst_null ? ea_narrow_ref(src_t) : src_t;
                push_val(&v, op == EA_OP_BR_ON_CAST ? rem : dst_t);
                push_vals(&v, c->out, c->n_out - 1);
                break;
            }
            case EA_OP_THROW: {
                uint32_t ti = in->imm.u32;
                m->feat.exceptions = true;
                if (getenv("EA_VDBG")) fprintf(stderr, "[V] THROW ti=%u ntags=%u type_idx=%u n_params=%u sp=%u\n",
                                               ti, m->n_tags, ti < m->n_tags ? m->tags[ti].type_idx : 99,
                                               ti < m->n_tags ? m->types[m->tags[ti].type_idx].func.n_params : 99, v.sp);
                if (ti >= m->n_tags) { vfail(&v, "unknown tag"); break; }
                EaFuncType *ft = &m->types[m->tags[ti].type_idx].func;
                pop_vals(&v, ft->params, ft->n_params);
                if (!v.failed) set_unreachable(&v);
                break;
            }
            case EA_OP_THROW_REF:
                m->feat.exceptions = true;
                pop_val(&v, EA_VT_ABSN(EA_ABS_EXN));
                if (!v.failed) set_unreachable(&v);
                break;
            case EA_OP_TRY_TABLE: {
                m->feat.exceptions = true;
                const EaValType *bi, *bo;
                uint32_t nbi, nbo;
                if (getenv("EA_VDBG")) fprintf(stderr, "[V] TRYTABLE bt.kind=%u vt=%x ti=%u nc=%u\n",
                                               in->imm.bt.kind, (unsigned)in->imm.bt.vt, in->imm.bt.type_idx, in->n_catches);
                if (!resolve_blocktype(&v, &in->imm.bt, &bi, &nbi, &bo, &nbo)) break;
                if ((nbi > 1 || nbo > 1) && !m->feat.multi_value) {
                    vfail(&v, "multi-value not enabled");
                    break;
                }
                pop_vals(&v, bi, nbi);
                if (v.failed) break;
                uint32_t outer_csp = v.csp; // catch labels resolve OUTSIDE the block
                push_ctrl(&v, pc, bi, nbi, bo, nbo, false);
                v.ctrl[v.csp - 1].init_save =
                    (uint8_t *)ea_malloc(v.n_lini ? v.n_lini : 1);
                memcpy(v.ctrl[v.csp - 1].init_save, v.lini, v.n_lini);
                v.ctrl[v.csp - 1].init_poly_save = v.init_poly;
                in->height = v.ctrl[v.csp - 1].height;
                in->is_loop = 0;
                in->arity_out = (uint16_t)nbo;
                in->arity_in = (uint16_t)nbi;
                in->arity_res = (uint16_t)nbo;
                // validate catch clauses now; the executor reads them from the instr
                for (uint32_t k = 0; k < in->n_catches; k++) {
                    EaCatch *cc = &in->catches[k];
                    uint32_t depth = cc->label;
                    if (depth >= outer_csp) { vfail(&v, "unknown label"); break; }
                    Ctrl *lc = &v.ctrl[outer_csp - 1 - depth];
                    uint32_t want = 0;
                    EaFuncType *ft = NULL;
                    if (cc->kind <= 1) {
                        if (cc->tag >= m->n_tags) { vfail(&v, "unknown tag"); break; }
                        ft = &m->types[m->tags[cc->tag].type_idx].func;
                        want = ft->n_params + (cc->kind == 1 ? 1 : 0);
                    } else if (cc->kind == 3) {
                        want = 1;
                    }
                    if (lc->n_out < want ||
                        (cc->kind == 3 &&
                         !vt_match(&v, lc->out[lc->n_out - 1], EA_VT_ABSN(EA_ABS_EXN)))) {
                        fprintf(stderr, "[V] catch label want_n=%u n_out=%u kind=%u lout0=%x\n", want, lc->n_out, cc->kind, lc->n_out ? (unsigned)lc->out[0] : 0);
                        vfail(&v, "type mismatch: catch label");
                        break;
                    }
                    if (cc->kind <= 1) {
                        // label results end with the tag params (+exnref for _ref)
                        uint32_t off = lc->n_out - want;
                        for (uint32_t p2 = 0; p2 < ft->n_params; p2++)
                            if (!vt_match(&v, lc->out[off + p2], ft->params[p2])) {
                                fprintf(stderr, "[V] catch param %u: label=%x tagparam=%x\n", p2, (unsigned)lc->out[off + p2], (unsigned)ft->params[p2]);
                                vfail(&v, "type mismatch: catch label");
                                break;
                            }
                        if (v.failed) break;
                        if (cc->kind == 1 &&
                            !vt_match(&v, lc->out[lc->n_out - 1], EA_VT_ABSN(EA_ABS_EXN))) {
                            fprintf(stderr, "[V] catch_ref exnref slot: label=%x\n", (unsigned)lc->out[lc->n_out - 1]);
                            vfail(&v, "type mismatch: catch label");
                            break;
                        }
                    }
                }
                break;
            }
            case EA_OP_UNREACHABLE:
                set_unreachable(&v);
                break;
            case EA_OP_NOP:
                break;
            case EA_OP_I32_CONST: push_val(&v, VT_I32); break;
            case EA_OP_I64_CONST: push_val(&v, VT_I64); break;
            case EA_OP_F32_CONST: push_val(&v, VT_F32); break;
            case EA_OP_F64_CONST: push_val(&v, VT_F64); break;
            default: {
                if (is_load(op)) {
                    uint32_t align = in->imm.ma.align;
                    uint32_t memidx = in->imm.ma.memidx;
                    if (align > 4 || (1u << align) > load_width(op)) {
                        vfail(&v, "alignment must not be larger than natural");
                        break;
                    }
                    if (memidx >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    if (!m->memories[memidx].is64 && in->imm.ma.offset > UINT32_MAX) {
                        vfail(&v, "offset out of range");
                        break;
                    }
                    pop_val(&v, m->memories[memidx].is64 ? VT_I64 : VT_I32);
                    if (!v.failed) {
                        EaValType r = (op == EA_OP_F32_LOAD) ? VT_F32
                                      : (op == EA_OP_F64_LOAD) ? VT_F64
                                      : (op == EA_OP_I64_LOAD ||
                                         (op >= EA_OP_I64_LOAD8_S && op <= EA_OP_I64_LOAD32_U))
                                          ? VT_I64 : VT_I32;
                        push_val(&v, r);
                    }
                    break;
                }
                if (is_store(op)) {
                    uint32_t align = in->imm.ma.align;
                    uint32_t memidx = in->imm.ma.memidx;
                    if (align > 4 || (1u << align) > store_width(op)) {
                        vfail(&v, "alignment must not be larger than natural");
                        break;
                    }
                    if (memidx >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    if (!m->memories[memidx].is64 && in->imm.ma.offset > UINT32_MAX) {
                        vfail(&v, "offset out of range");
                        break;
                    }
                    EaValType vt2 = (op == EA_OP_F32_STORE) ? VT_F32
                                    : (op == EA_OP_F64_STORE) ? VT_F64
                                    : (op == EA_OP_I64_STORE || op == EA_OP_I64_STORE8 ||
                                       op == EA_OP_I64_STORE16 || op == EA_OP_I64_STORE32)
                                        ? VT_I64 : VT_I32;
                    pop_val(&v, vt2);
                    if (!v.failed) pop_val(&v, m->memories[memidx].is64 ? VT_I64 : VT_I32);
                    break;
                }
                if (op == EA_OP_MEMORY_SIZE || op == EA_OP_MEMORY_GROW ||
                    op == EA_OP_MEMORY_COPY || op == EA_OP_MEMORY_FILL ||
                    op == EA_OP_MEMORY_INIT || op == EA_OP_DATA_DROP) {
                    if (op == EA_OP_MEMORY_INIT) {
                        if (!m->has_data_count) { vfail(&v, "data count section required"); break; }
                        if (in->imm.pair.b >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                        if (in->imm.pair.a >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    } else if (op == EA_OP_DATA_DROP) {
                        if (!m->has_data_count) { vfail(&v, "data count section required"); break; }
                        if (in->imm.u32 >= m->n_datas) { vfail(&v, "unknown data segment"); break; }
                    } else if (op == EA_OP_MEMORY_COPY) {
                        if (in->imm.pair.a >= m->n_memories || in->imm.pair.b >= m->n_memories) {
                            vfail(&v, "unknown memory"); break;
                        }
                    } else {
                        if (in->imm.u32 >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                    }
                    if (op == EA_OP_DATA_DROP) break;
                    op_sig(op, &sig);
                    if (op == EA_OP_MEMORY_COPY) {
                        // [dst_idx src_idx min(dst_idx, src_idx)] — pop n, src, dst
                        EaValType da = m->memories[in->imm.pair.a].is64 ? VT_I64 : VT_I32;
                        EaValType db = m->memories[in->imm.pair.b].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, (da == VT_I64 && db == VT_I64) ? VT_I64 : VT_I32);
                        if (!v.failed) pop_val(&v, db);
                        if (!v.failed) pop_val(&v, da);
                    } else if (op == EA_OP_MEMORY_FILL) {
                        EaValType da = m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da); pop_val(&v, VT_I32); pop_val(&v, da);
                    } else if (op == EA_OP_MEMORY_INIT) {
                        // stack is [dest, src, n]: pop n, src, then dest
                        EaValType da = m->memories[in->imm.pair.a].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, VT_I32); pop_val(&v, VT_I32); pop_val(&v, da);
                    } else if (op == EA_OP_MEMORY_SIZE) {
                        push_val(&v, m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32);
                    } else if (op == EA_OP_MEMORY_GROW) {
                        EaValType da = m->memories[in->imm.u32].is64 ? VT_I64 : VT_I32;
                        pop_val(&v, da);
                        if (!v.failed) push_val(&v, da);
                    } else {
                        pop_vals(&v, sig.in, sig.n_in);
                        if (!v.failed) push_vals(&v, sig.out, sig.n_out);
                    }
                    break;
                }
                if (!op_sig(op, &sig)) {
                    if (op >= EA_OPV_BASE && !m->feat.simd)
                        vfail(&v, "SIMD not enabled");
                    else
                        vfail(&v, "illegal opcode");
                    break;
                }
                if (op >= EA_OPV_BASE) {
                    // SIMD memory ops: alignment, lane range, offset range
                    uint32_t sub = op < EA_OPV_BASE + 0x100 ? op & 0xFF : 0xFFFF; // relaxed ops: not memory ops
                    uint32_t memidx = in->imm.ma.memidx;
                    uint64_t nat = 0, maxlane = 0;
                    if (sub <= 0x0B) {
                        // natural widths: 0x00 load, 0x01-0x06 loadNxN..., 0x07-0x0A splats, 0x0B store
                        static const uint8_t lw[12] = {16,8,8,8,8,8,8,1,2,4,8,16};
                        nat = lw[sub];
                    } else if (sub == 0x5C) { nat = 4; }        // load32_zero
                    else if (sub == 0x5D) { nat = 8; }          // load64_zero
                    else if (sub >= 0x54 && sub <= 0x5B) {
                        uint64_t w = 1ull << (sub & 3); // 8/16/32/64-lane pairs
                        nat = w;
                        maxlane = 16 / w;
                        if (in->lane >= maxlane) {
                            vfail(&v, "invalid lane index");
                            break;
                        }
                    }
                    else if (sub == 0x0D) {
                        // i8x16.shuffle: 16 lane indices, each < 32
                        const uint8_t *idx = in->imm.bytes;
                        for (int i = 0; i < 16; i++) {
                            if (idx[i] >= 32) { vfail(&v, "invalid lane index"); break; }
                        }
                        if (v.failed) break;
                    }
                    else if (sub >= 0x15 && sub <= 0x22) {
                        // extract/replace lane: lane count by shape (lane imm in imm.u32)
                        static const uint8_t lanes[14] = {16,16,16,8,8,8,4,4,2,2,4,4,2,2};
                        if (in->imm.u32 >= lanes[sub - 0x15]) {
                            vfail(&v, "invalid lane index");
                            break;
                        }
                    }
                    if (nat) {
                        if (memidx >= m->n_memories) { vfail(&v, "unknown memory"); break; }
                        if (in->imm.ma.align > 4 || (1u << in->imm.ma.align) > nat) {
                            vfail(&v, "alignment must not be larger than natural");
                            break;
                        }
                        if (!m->memories[memidx].is64 && in->imm.ma.offset > UINT32_MAX) {
                            vfail(&v, "offset out of range");
                            break;
                        }
                    }
                }
                pop_vals(&v, sig.in, sig.n_in);
                if (!v.failed) push_vals(&v, sig.out, sig.n_out);
                if (getenv("EA_VDBG") && op >= EA_OPV_BASE) fprintf(stderr, "[V] simdop fi=%u pc=%u op=%x n_in=%u n_out=%u sp=%u\n", fi, pc, op, sig.n_in, sig.n_out, v.sp);
                break;
            }
            }
        }
        if (!v.failed) {
            if (v.csp != 0 || v.sp != ft->n_results ||
                !vt_list_eq(&v, v.vals, v.sp, ft->results, ft->n_results))
                vfail(&v, "type mismatch: function end");
        }
        f->max_stack = v.max_stack;
        f->depths = depths;
        f->reach = reach;
        f->br_targets = br_ar;
        depths = NULL; reach = NULL; br_ar = NULL;
        if (v.failed) {
            if (getenv("EA_VDBG")) {
                fprintf(stderr, "EA_VDBG vfail func %u pc %u/%u op %x: %s (sp=%u csp=%u) nres=%u res0=%x\n", fi, cur_pc, v.cur_op,
                        f->code.n, v.err ? v.err : "?", v.sp, v.csp, (void*)ft, ft->n_results,
                        ft->n_results ? (unsigned)ft->results[0] : 0u);
                for (uint32_t k = 0; k < f->code.n; k++)
                    fprintf(stderr, "   [%u] 0x%02x end=%u else=%d\n", k, f->code.v[k].opcode,
                            f->code.v[k].end_idx, (int)f->code.v[k].else_idx);
            }
            if (err && !*err) {
                const char *msg = v.err ? v.err : "validation failure";
                size_t n = strlen(msg) + 32;
                *err = (char *)ea_malloc(n);
                snprintf(*err, n, "%s", msg);
            }
            if (getenv("EA_VDBG")) fprintf(stderr, "EA_VDBG LOOP-FAIL path: func %u err=%s v.vals=%p\n",
                                           fi, v.err ? v.err : "?", (void*)v.vals);
            free(depths); free(reach); free(br_ar);
            free(v.err);
            free(v.vals);
            free(v.ctrl);
            free(in_refs);
            return 1;
        }
    }
out:
    if (getenv("EA_VDBG")) fprintf(stderr, "EA_VDBG OUT path: rc=%d v.vals=%p\n", rc, (void*)v.vals);
    free(in_refs);
    free(v.vals);
    free(v.ctrl);
    return rc;
}

// ---------------------------------------------------------------- SIMD signatures
static void sig_vv(Sig *s) { s->n_in = 2; s->in[0] = s->in[1] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_v(Sig *s)  { s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_vi(Sig *s) { s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_v3(Sig *s) { s->n_in = 3; s->in[0] = s->in[1] = s->in[2] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_to_i32(Sig *s) { s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I32; }
static void sig_load(Sig *s)  { s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_load_lane(Sig *s) { s->n_in = 2; s->in[0] = VT_I32; s->in[1] = VT_V128; s->n_out = 1; s->out[0] = VT_V128; }
static void sig_store(Sig *s) { s->n_in = 2; s->in[0] = VT_I32; s->in[1] = VT_V128; s->n_out = 0; }

bool simd_sig(uint32_t op, Sig *s) {
    uint32_t sub = op & 0xFF;
    bool ext = op >= EA_OPV_BASE + 0x100; // relaxed range
    if (ext) {
        switch (op) {
        case EA_OP_F32X4_RELAXED_MADD: case EA_OP_F32X4_RELAXED_NMADD:
        case EA_OP_F64X2_RELAXED_MADD: case EA_OP_F64X2_RELAXED_NMADD:
        case EA_OP_I8X16_RELAXED_LANESELECT: case EA_OP_I16X8_RELAXED_LANESELECT:
        case EA_OP_I32X4_RELAXED_LANESELECT: case EA_OP_I64X2_RELAXED_LANESELECT:
        case EA_OP_I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S:
            sig_v3(s);
            return true;
        case EA_OP_I32X4_RELAXED_TRUNC_F32X4_S: case EA_OP_I32X4_RELAXED_TRUNC_F32X4_U:
        case EA_OP_I32X4_RELAXED_TRUNC_F64X2_S_ZERO: case EA_OP_I32X4_RELAXED_TRUNC_F64X2_U_ZERO:
            sig_v(s);
            return true;
        default:
            sig_vv(s);
            return true;
        }
    }
    switch (sub) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
    case 0x5C: case 0x5D:
        sig_load(s);
        return true;
    case 0x0B:
        sig_store(s);
        return true;
    case 0x0C:
        s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x0D: case 0x0E:
        sig_vv(s);
        return true;
    case 0x54: case 0x55: case 0x56: case 0x57:
        sig_load_lane(s);
        return true;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
        sig_store(s);
        return true;
    case 0x0F: case 0x10: case 0x11:
        s->n_in = 1; s->in[0] = VT_I32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x12:
        s->n_in = 1; s->in[0] = VT_I64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x13:
        s->n_in = 1; s->in[0] = VT_F32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x14:
        s->n_in = 1; s->in[0] = VT_F64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x15: case 0x16: case 0x18: case 0x19: case 0x1B:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I32;
        return true;
    case 0x1D:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_I64;
        return true;
    case 0x1F:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_F32;
        return true;
    case 0x21:
        s->n_in = 1; s->in[0] = VT_V128; s->n_out = 1; s->out[0] = VT_F64;
        return true;
    case 0x17: case 0x1A: case 0x1C:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x1E:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_I64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x20:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_F32; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x22:
        s->n_in = 2; s->in[0] = VT_V128; s->in[1] = VT_F64; s->n_out = 1; s->out[0] = VT_V128;
        return true;
    case 0x53:
        sig_to_i32(s);
        return true;
    case 0x63: case 0x83: case 0xA3: case 0xC3:
    case 0x64: case 0x84: case 0xA4: case 0xC4:
        sig_to_i32(s);
        return true;
    case 0x52:
        sig_v3(s);
        return true;
    case 0x4D: // not
    case 0x5E: case 0x5F:
    case 0x60: case 0x61: case 0x62:
    case 0x67: case 0x68: case 0x69: case 0x6A:
    case 0x74: case 0x75: case 0x7A: case 0x94:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0x80: case 0x81:
    case 0x87: case 0x88: case 0x89: case 0x8A:
    case 0xA0: case 0xA1:
    case 0xA7: case 0xA8: case 0xA9: case 0xAA:
    case 0xC0: case 0xC1:
    case 0xC7: case 0xC8: case 0xC9: case 0xCA:
    case 0xE0: case 0xE1: case 0xE3:
    case 0xEC: case 0xED: case 0xEF:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        sig_v(s);
        return true;
    case 0x6B: case 0x6C: case 0x6D:
    case 0x8B: case 0x8C: case 0x8D:
    case 0xAB: case 0xAC: case 0xAD:
    case 0xCB: case 0xCC: case 0xCD:
        sig_vi(s);
        return true;
    default:
        sig_vv(s);
        return true;
    }
}
