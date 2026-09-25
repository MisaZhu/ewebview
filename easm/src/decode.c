// easm binary decoder: sections + instruction pre-decode
#include "easm.h"
#include "opcodes.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------- reader
typedef struct {
    const uint8_t *p, *end, *start;
} Rd;

static bool rd_u32(Rd *r, uint32_t *out) { // LEB128 unsigned, max 5 bytes
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        if (i == 4 && (b & 0xF0)) return false;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = (uint32_t)v; return true; }
        shift += 7;
    }
    return false;
}
static bool rd_u64(Rd *r, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        if (i == 9 && (b & 0xFE)) return false;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return true; }
        shift += 7;
    }
    return false;
}
static bool rd_s33(Rd *r, int64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        if (i == 4 && (b & 0x78)) return false; // beyond 33 bits
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= ((uint64_t)-1) << shift;
            *out = (int64_t)v;
            return true;
        }
    }
    return false;
}
static bool rd_s32(Rd *r, int32_t *out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        if (i == 4) {
            uint8_t hi = (uint8_t)(b & 0xF0);
            bool sign = (b & 0x08) != 0;
            if (sign ? (hi != 0x70) : (hi != 0x00)) return false;
        }
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= ((uint64_t)-1) << shift;
            *out = (int32_t)(uint32_t)v;
            return true;
        }
    }
    return false;
}
static bool rd_s64(Rd *r, int64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        if (i == 9) {
            uint8_t vv = (uint8_t)(b & 0x7F);
            bool sign = (vv & 0x01) != 0;
            uint8_t expect = sign ? 0x7E : 0x00;
            if ((vv & 0x7E) != expect || (b & 0x80)) return false;
        }
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= ((uint64_t)-1) << shift;
            *out = (int64_t)v;
            return true;
        }
    }
    return false;
}
static bool rd_byte(Rd *r, uint8_t *out) {
    if (r->p >= r->end) return false;
    *out = *r->p++;
    return true;
}
static bool rd_bytes(Rd *r, uint32_t n, const uint8_t **out) {
    if ((uint64_t)n > (uint64_t)(r->end - r->p)) return false;
    *out = r->p;
    r->p += n;
    return true;
}
static const char *g_rd_err; // one-shot error message from immediate readers (name/memarg)

// spec UTF-8 validation: rejects overlongs, surrogates, > U+10FFFF
static bool utf8_valid(const uint8_t *p, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) { i++; continue; }
        uint32_t need;
        uint32_t cp;
        if ((b & 0xE0) == 0xC0) { need = 1; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; }
        else return false;
        if (i + need >= n) return false; // continuation bytes beyond end
        for (uint32_t k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (need == 1 && cp < 0x80) return false;       // overlong
        if (need == 2 && cp < 0x800) return false;      // overlong
        if (need == 3 && cp < 0x10000) return false;    // overlong
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false; // surrogate
        i += need + 1;
    }
    return true;
}
static bool rd_name_len(Rd *r, char **out, uint32_t *n_out) {
    uint32_t n;
    if (!rd_u32(r, &n)) return false;
    const uint8_t *p;
    if (!rd_bytes(r, n, &p)) return false;
    g_rd_err = NULL;
    if (!utf8_valid(p, n)) { g_rd_err = "malformed UTF-8 encoding"; return false; }
    *out = ea_strndup((const char *)p, n);
    *n_out = n;
    return true;
}
static bool rd_name(Rd *r, char **out) {
    uint32_t n;
    if (!rd_u32(r, &n)) return false;
    const uint8_t *p;
    if (!rd_bytes(r, n, &p)) return false;
    g_rd_err = NULL;
    if (!utf8_valid(p, n)) { g_rd_err = "malformed UTF-8 encoding"; return false; }
    *out = ea_strndup((const char *)p, n);
    return true;
}
static bool rd_heaptype(Rd *r, EaValType *out, bool nullable);
static bool read_type_entry_form(Rd *r, uint8_t form, EaType *t);
static bool read_type_entry_form_inner(Rd *r, uint8_t form, EaType *t);
static bool rd_valtype(Rd *r, EaValType *out) {
    if (r->p >= r->end) return false;
    uint8_t b = *r->p;
    switch (b) {
    case 0x7F: *out = VT_I32; r->p++; return true;
    case 0x7E: *out = VT_I64; r->p++; return true;
    case 0x7D: *out = VT_F32; r->p++; return true;
    case 0x7C: *out = VT_F64; r->p++; return true;
    case 0x7B: *out = VT_V128; r->p++; return true;
    case 0x70: *out = VT_FUNCREF; r->p++; return true;
    case 0x6F: *out = VT_EXTERNREF; r->p++; return true;
    case 0x6E: *out = VT_ANYREF; r->p++; return true;
    case 0x6D: *out = EA_VT_ABSN(EA_ABS_EQ); r->p++; return true;
    case 0x6C: *out = EA_VT_ABSN(EA_ABS_I31); r->p++; return true;
    case 0x6B: *out = EA_VT_ABSN(EA_ABS_STRUCT); r->p++; return true;
    case 0x6A: *out = EA_VT_ABSN(EA_ABS_ARRAY); r->p++; return true;
    case 0x71: *out = EA_VT_ABSN(EA_ABS_NONE); r->p++; return true;
    case 0x73: *out = EA_VT_ABSN(EA_ABS_NOFUNC); r->p++; return true;   // nullfuncref
    case 0x72: *out = EA_VT_ABSN(EA_ABS_NOEXTERN); r->p++; return true; // nullexternref
    case 0x69: *out = EA_VT_ABSN(EA_ABS_EXN); r->p++; return true; // exnref
    case 0x74: *out = EA_VT_ABSN(EA_ABS_EXN); r->p++; return true; // nullexnref
    case 0x63: case 0x64: { // (ref null ht) / (ref ht) with a heap type
        r->p++;
        return rd_heaptype(r, out, b == 0x63);
    }
    default: return false;
    }
}

static bool rd_heaptype(Rd *r, EaValType *out, bool nullable) {
    int64_t ht;
    if (!rd_s33(r, &ht)) return false;
    if (ht >= 0) { // type index
        *out = nullable ? EA_VT_TREFN((uint32_t)ht) : EA_VT_TREF((uint32_t)ht);
        return true;
    }
    switch (ht) {
    case -0x10: *out = nullable ? (EaValType)VT_FUNCREF : EA_VT_ABSREF(EA_ABS_FUNC); return true;
    case -0x11: *out = nullable ? (EaValType)VT_EXTERNREF : EA_VT_ABSREF(EA_ABS_EXTERN); return true;
    case -0x12: *out = nullable ? (EaValType)VT_ANYREF : EA_VT_ABSREF(EA_ABS_ANY); return true;
    case -0x13: *out = EA_VT_ABSN(EA_ABS_EQ); return true;      // eqref
    case -0x14: *out = EA_VT_ABSN(EA_ABS_I31); return true;     // i31ref
    case -0x15: *out = EA_VT_ABSN(EA_ABS_STRUCT); return true;  // structref
    case -0x16: *out = EA_VT_ABSN(EA_ABS_ARRAY); return true;   // arrayref
    case -0x17: *out = EA_VT_ABSN(EA_ABS_EXN); return true;    // exnref
    case -0x0F: *out = EA_VT_ABSN(EA_ABS_NONE); return true;   // nullref
    case -0x0D: *out = EA_VT_ABSN(EA_ABS_NOFUNC); return true;  // nullfuncref
    case -0x0E: *out = EA_VT_ABSN(EA_ABS_NOEXTERN); return true; // nullexternref
    case -0x09: case -0x0C: *out = EA_VT_ABSN(EA_ABS_EXN); return true; // exnref/null exnref
    default: return false;
    }
}
static bool rd_heaptype(Rd *r, EaValType *out, bool nullable);
static bool rd_reftype(Rd *r, EaValType *out) {
    if (r->p >= r->end) return false;
    uint8_t b = *r->p;
    if ((b >= 0x69 && b <= 0x73)) { // abstract reftype shorthands (incl. GC kinds)
        r->p++;
        switch (b) {
        case 0x70: *out = VT_FUNCREF; return true;
        case 0x6F: *out = VT_EXTERNREF; return true;
        case 0x6E: *out = VT_ANYREF; return true;
        case 0x6D: *out = EA_VT_ABSN(EA_ABS_EQ); return true;
        case 0x6C: *out = EA_VT_ABSN(EA_ABS_I31); return true;
        case 0x6B: *out = EA_VT_ABSN(EA_ABS_STRUCT); return true;
        case 0x6A: *out = EA_VT_ABSN(EA_ABS_ARRAY); return true;
        case 0x71: *out = EA_VT_ABSN(EA_ABS_NONE); return true;
        case 0x73: *out = EA_VT_ABSN(EA_ABS_NOFUNC); return true;
        case 0x72: *out = EA_VT_ABSN(EA_ABS_NOEXTERN); return true;
        case 0x69: case 0x74: *out = EA_VT_ABSN(EA_ABS_EXN); return true;
        default: return false;
        }
    }
    if (b == 0x63 || b == 0x64) { r->p++; return rd_heaptype(r, out, b == 0x63); }
    return false;
}
static bool rd_blocktype(Rd *r, EaBlockType *out) {
    if (r->p >= r->end) return false;
    uint8_t b = *r->p;
    if (b == 0x40) { out->kind = 0; out->vt = 0; out->type_idx = 0; r->p++; return true; }
    const uint8_t *save = r->p;
    EaValType vt;
    if (rd_valtype(r, &vt)) { out->kind = 1; out->vt = vt; out->type_idx = 0; return true; }
    r->p = save;
    int64_t idx;
    if (!rd_s33(r, &idx) || idx < 0) return false;
    out->kind = 2;
    out->type_idx = (uint32_t)idx;
    return true;
}
static bool rd_limits(Rd *r, uint64_t *min, uint64_t *max, bool *has_max, uint8_t allow_mask, uint8_t *flag_out) {
    uint8_t flag;
    if (!rd_byte(r, &flag)) return false;
    if (flag & ~allow_mask) return false;
    if (flag_out) *flag_out = flag;
    *has_max = (flag & 1) != 0;
    if (!rd_u64(r, min)) return false;
    if (*has_max) {
        if (!rd_u64(r, max)) return false;
    } else {
        *max = UINT64_MAX;
    }
    return true;
}

static bool read_functype(Rd *r, EaFuncType *ft);

// composite type body: 0x60 func / 0x5F struct / 0x5E array
static bool read_comp_body(Rd *r, uint8_t form, EaType *t) {
    if (form == 0x60) {
        t->kind = CT_FUNC;
        return read_functype(r, &t->func);
    }
    if (form == 0x5F || form == 0x5E) {
        t->kind = (form == 0x5F) ? CT_STRUCT : CT_ARRAY;
        uint32_t nf;
        if (form == 0x5E) nf = 1;
        else if (!rd_u32(r, &nf) || nf > (1u << 16)) return false;
        t->n_fields = nf;
        t->fields = (EaFieldType *)ea_zalloc((nf ? nf : 1) * sizeof(EaFieldType));
        for (uint32_t i = 0; i < nf; i++) {
            EaValType vt;
            uint8_t packed = 0;
            uint8_t st;
            if (!rd_byte(r, &st)) return false;
            if (st == 0x78) { vt = VT_I32; packed = 1; }        // i8
            else if (st == 0x77) { vt = VT_I32; packed = 2; }   // i16
            else {
                r->p--; // regular valtype: re-read through the valtype reader
                if (!rd_valtype(r, &vt)) return false;
            }
            uint8_t mut;
            if (!rd_byte(r, &mut) || mut > 1) return false;
            t->fields[i].vt = vt;
            t->fields[i].mut = mut;
            t->fields[i].packed_ = packed;
        }
        return true;
    }
    return false;
}

// one type-space entry: comptype, or sub / sub final wrapping a comptype
static bool read_type_entry(Rd *r, EaType *t) {
    uint8_t form;
    if (!rd_byte(r, &form)) return false;
    return read_type_entry_form(r, form, t);
}
static bool read_type_entry_form(Rd *r, uint8_t form, EaType *t) {
    bool _ok = read_type_entry_form_inner(r, form, t);
    if (!_ok && getenv("EA_VDBG2")) fprintf(stderr, "type entry FAIL form=%02x at off=%ld\n", form, (long)(r->p - r->start));
    return _ok;
}
static bool read_type_entry_form_inner(Rd *r, uint8_t form, EaType *t) {
    if (form == 0x50 || form == 0x4F) {
        t->is_final = (form == 0x4F);
        uint32_t ns;
        if (!rd_u32(r, &ns) || ns > 2) return false;
        t->n_sup = (uint8_t)ns;
        for (uint32_t i = 0; i < ns; i++)
            if (!rd_u32(r, &t->sup[i])) return false;
        uint8_t cf;
        if (!rd_byte(r, &cf)) return false;
        return read_comp_body(r, cf, t);
    }
    if (form == 0x60 || form == 0x5F || form == 0x5E)
        t->is_final = 1; // plain comptype == (sub final ...)
    return read_comp_body(r, form, t);
}

// form byte (0x60) already consumed by read_comp_body
static bool read_functype(Rd *r, EaFuncType *ft) {
    uint32_t np;
    if (!rd_u32(r, &np) || np > (1u << 20)) return false;
    ft->n_params = np;
    ft->params = (EaValType *)ea_malloc((np ? np : 1) * sizeof(EaValType));
    for (uint32_t i = 0; i < np; i++)
        if (!rd_valtype(r, &ft->params[i])) return false;
    uint32_t nr;
    if (!rd_u32(r, &nr) || nr > (1u << 20)) return false;
    ft->n_results = nr;
    if (getenv("EA_VDBG2")) fprintf(stderr, "  functype np=%u nr=%u\n", np, nr);
    ft->results = (EaValType *)ea_malloc((nr ? nr : 1) * sizeof(EaValType));
    for (uint32_t i = 0; i < nr; i++)
        if (!rd_valtype(r, &ft->results[i])) return false;
    return true;
}

// ---------------------------------------------------------------- instruction pre-decode
typedef struct {
    EaInstr *v;
    uint32_t n, cap;
    uint32_t *pool;
    uint32_t pool_n, pool_cap;
    uint32_t *nest;
    uint32_t nest_sp, nest_cap;
    bool too_deep;
} InsCtx;

static EaInstr *ins_new(InsCtx *c, uint32_t op, uint32_t imm_off) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->v = (EaInstr *)ea_realloc(c->v, c->cap * sizeof(EaInstr));
    }
    EaInstr *in = &c->v[c->n++];
    memset(in, 0, sizeof(*in));
    in->opcode = op;
    in->imm_off = imm_off;
    in->else_idx = UINT32_MAX;
    in->end_idx = UINT32_MAX;
    return in;
}
static bool ins_open(InsCtx *c, uint32_t idx) {
    if (c->nest_sp == c->nest_cap) {
        c->nest_cap = c->nest_cap ? c->nest_cap * 2 : 64;
        c->nest = (uint32_t *)ea_realloc(c->nest, c->nest_cap * 4);
    }
    if (getenv("EA_DDBG")) fprintf(stderr, "OPEN idx=%u n=%u sp=%u\n", idx, c->n, c->nest_sp);
    c->nest[c->nest_sp++] = idx;
    return true;
}
static uint32_t pool_put(InsCtx *c, const uint8_t *bytes, uint32_t n_bytes) {
    uint32_t words = (n_bytes + 3) / 4;
    while (c->pool_n + words > c->pool_cap) {
        c->pool_cap = c->pool_cap ? c->pool_cap * 2 : 16;
        c->pool = (uint32_t *)ea_realloc(c->pool, (size_t)c->pool_cap * 4);
    }
    uint32_t base = c->pool_n;
    memset(&c->pool[base], 0, (size_t)words * 4);
    memcpy(&c->pool[base], bytes, n_bytes);
    c->pool_n += words;
    return base;
}
static bool rd_memarg(Rd *r, EaInstr *in, bool multi_memory) {
    uint32_t align;
    if (!rd_u32(r, &align)) return false;
    if (align > 0x7F) { g_rd_err = "malformed memop flags"; return false; } // bit 7+ reserved
    uint32_t memidx = 0;
    if (multi_memory && (align & 0x40)) {
        if (!rd_u32(r, &memidx)) return false;
        align &= ~0x40u;
    }
    uint64_t offset;
    if (!rd_u64(r, &offset)) return false; // u64 LEB (memory64: up to 2^64-1)
    in->imm.ma.align = align;
    in->imm.ma.offset = offset;
    in->imm.ma.memidx = memidx;
    return true;
}

// Pre-decode instructions until region end, or until the top-level `end`
// (stop_at_end=true, used for const exprs). Returns false on malformed.
static bool predecode(Rd *r, InsCtx *c, bool stop_at_end, bool *hit_end) {
    if (hit_end) *hit_end = false;
    while (r->p < r->end) {
        uint32_t imm_off = (uint32_t)(r->p - r->start);
        uint8_t b;
        if (!rd_byte(r, &b)) return false;
        uint32_t op = b;
        EaInstr *in;
        if (b != 0xFC && b != 0xFD && b != 0xFE && b != 0xFB) {
            in = ins_new(c, b, imm_off);
        } else {
            uint32_t sub;
            if (!rd_u32(r, &sub)) return false;
            if (b == 0xFC) op = EA_OPX_BASE + sub;
            else if (b == 0xFD) op = EA_OPV_BASE + sub;
            else if (b == 0xFB) op = EA_OPF_BASE + sub;
            else op = EA_OPT_BASE + sub;
            in = ins_new(c, op, imm_off);
        }
        switch (op) {
        case EA_OP_END:
            if (c->nest_sp == 0) {
                if (stop_at_end) {
                    if (hit_end) *hit_end = true;
                    c->n--; // drop the end marker for exprs
                    return true;
                }
                break; // function-final end: keep, continue
            }
            c->nest_sp--;
            c->v[c->nest[c->nest_sp]].end_idx = c->n - 1;
            if (getenv("EA_DDBG")) fprintf(stderr, "CLOSE pop=%u end=%u n=%u\n", c->nest[c->nest_sp], c->v[c->nest[c->nest_sp]].end_idx, c->n);
            break;
        case EA_OP_ELSE:
            if (c->nest_sp == 0) return false;
            if (c->v[c->nest[c->nest_sp - 1]].opcode != EA_OP_IF) return false;
            c->v[c->nest[c->nest_sp - 1]].else_idx = c->n - 1;
            break;
        case EA_OP_UNREACHABLE: case EA_OP_NOP: case EA_OP_RETURN:
        case EA_OP_DROP: case EA_OP_SELECT:
        case EA_OP_REF_IS_NULL: case EA_OP_REF_EQ: case EA_OP_REF_AS_NON_NULL:
            break;
        case EA_OP_BLOCK: case EA_OP_LOOP: case EA_OP_IF:
            if (!rd_blocktype(r, &in->imm.bt)) return false;
            if (!ins_open(c, c->n - 1)) { c->too_deep = true; return false; }
            break;
        case EA_OP_TRY_TABLE: {
            if (getenv("EA_DDBG")) fprintf(stderr, "TRYTABLE decode\n");
            if (!rd_blocktype(r, &in->imm.bt)) return false;
            if (!ins_open(c, c->n - 1)) { c->too_deep = true; return false; }
            uint32_t nc;
            if (!rd_u32(r, &nc) || nc > 64) return false;
            EaCatch *cs = (EaCatch *)ea_malloc((nc ? nc : 1) * sizeof(EaCatch));
            for (uint32_t k = 0; k < nc; k++) {
                uint8_t kind;
                if (!rd_byte(r, &kind) || kind > 3) return false;
                cs[k].kind = kind;
                if (kind <= 1) {
                    if (!rd_u32(r, &cs[k].tag)) return false;
                } else {
                    cs[k].tag = UINT32_MAX;
                }
                if (!rd_u32(r, &cs[k].label)) return false;
            }
            in->n_catches = nc;
            in->catches = cs;
            break;
        }
        case EA_OP_THROW:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_THROW_REF:
            break;
        case EA_OP_BR: case EA_OP_BR_IF:
        case EA_OP_BR_ON_NULL: case EA_OP_BR_ON_NON_NULL:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_BR_TABLE: {
            uint32_t n;
            if (!rd_u32(r, &n)) return false;
            if (n > (1u << 20)) return false;
            while (c->pool_n + n + 1 > c->pool_cap) {
                c->pool_cap = c->pool_cap ? c->pool_cap * 2 : 16;
                c->pool = (uint32_t *)ea_realloc(c->pool, (size_t)c->pool_cap * 4);
            }
            uint32_t base = c->pool_n;
            for (uint32_t i = 0; i < n; i++)
                if (!rd_u32(r, &c->pool[c->pool_n++])) return false;
            if (!rd_u32(r, &c->pool[c->pool_n++])) return false;
            in->imm.pair.a = base;
            in->imm.pair.b = n;
            break;
        }
        case EA_OP_CALL: case EA_OP_RETURN_CALL: case EA_OP_REF_FUNC:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_CALL_INDIRECT: case EA_OP_RETURN_CALL_INDIRECT:
            if (!rd_u32(r, &in->imm.pair.a)) return false;
            if (!rd_u32(r, &in->imm.pair.b)) return false;
            break;
        case EA_OP_CALL_REF: case EA_OP_RETURN_CALL_REF:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_SELECT_T: {
            uint32_t n;
            if (!rd_u32(r, &n) || n != 1) return false;
            EaValType vt;
            if (!rd_valtype(r, &vt)) return false;
            in->imm.u32 = (uint32_t)vt;
            break;
        }
        case EA_OP_LOCAL_GET: case EA_OP_LOCAL_SET: case EA_OP_LOCAL_TEE:
        case EA_OP_GLOBAL_GET: case EA_OP_GLOBAL_SET:
        case EA_OP_TABLE_GET: case EA_OP_TABLE_SET:
        case EA_OP_TABLE_GROW: case EA_OP_TABLE_SIZE: case EA_OP_TABLE_FILL:
        case EA_OP_ELEM_DROP:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_TABLE_COPY:
            if (!rd_u32(r, &in->imm.pair.a)) return false; // dst
            if (!rd_u32(r, &in->imm.pair.b)) return false; // src
            break;
        case EA_OP_TABLE_INIT:
            // binary order is [elemidx, tableidx]; store tableidx in a
            if (!rd_u32(r, &in->imm.pair.b)) return false;
            if (!rd_u32(r, &in->imm.pair.a)) return false;
            break;
        case EA_OP_I32_LOAD: case EA_OP_I64_LOAD: case EA_OP_F32_LOAD: case EA_OP_F64_LOAD:
        case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U: case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
        case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U:
        case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U:
        case EA_OP_I32_STORE: case EA_OP_I64_STORE: case EA_OP_F32_STORE: case EA_OP_F64_STORE:
        case EA_OP_I32_STORE8: case EA_OP_I32_STORE16:
        case EA_OP_I64_STORE8: case EA_OP_I64_STORE16: case EA_OP_I64_STORE32:
            if (!rd_memarg(r, in, true)) return false;
            break;
        case EA_OP_MEMORY_SIZE: case EA_OP_MEMORY_GROW: case EA_OP_MEMORY_FILL:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_MEMORY_COPY:
            if (!rd_u32(r, &in->imm.pair.a)) return false;
            if (!rd_u32(r, &in->imm.pair.b)) return false;
            break;
        case EA_OP_MEMORY_INIT:
            if (!rd_u32(r, &in->imm.pair.b)) return false;
            if (!rd_u32(r, &in->imm.pair.a)) return false;
            break;
        case EA_OP_DATA_DROP:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_I32_CONST: {
            int32_t v;
            if (!rd_s32(r, &v)) return false;
            in->imm.u32 = (uint32_t)v;
            break;
        }
        case EA_OP_I64_CONST: {
            int64_t v;
            if (!rd_s64(r, &v)) return false;
            in->imm.u64 = (uint64_t)v;
            break;
        }
        case EA_OP_F32_CONST: {
            const uint8_t *p;
            if (!rd_bytes(r, 4, &p)) return false;
            memcpy(&in->imm.f32, p, 4);
            break;
        }
        case EA_OP_F64_CONST: {
            const uint8_t *p;
            if (!rd_bytes(r, 8, &p)) return false;
            memcpy(&in->imm.f64, p, 8);
            break;
        }
        case EA_OP_REF_NULL: {
            int64_t ht;
            if (!rd_s33(r, &ht)) return false;
            if (ht >= 0) in->imm.u32 = (uint32_t)EA_VT_TREFN((uint32_t)ht);
            else if (ht == -0x10) in->imm.u32 = (uint32_t)VT_FUNCREF;
            else if (ht == -0x11) in->imm.u32 = (uint32_t)VT_EXTERNREF;
            else in->imm.u32 = (uint32_t)VT_ANYREF;
            break;
        }
        // ---- GC (0xFB)
        case EA_OP_STRUCT_NEW: case EA_OP_STRUCT_NEW_DEFAULT:
        case EA_OP_ARRAY_NEW: case EA_OP_ARRAY_NEW_DEFAULT:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_STRUCT_GET: case EA_OP_STRUCT_GET_S: case EA_OP_STRUCT_GET_U:
        case EA_OP_STRUCT_SET:
            if (!rd_u32(r, &in->imm.pair.a)) return false;  // typeidx
            if (!rd_u32(r, &in->imm.pair.b)) return false;  // field idx
            break;
        case EA_OP_ARRAY_NEW_FIXED:
            if (!rd_u32(r, &in->imm.pair.a)) return false;  // typeidx
            if (!rd_u32(r, &in->imm.pair.b)) return false;  // N
            break;
        case EA_OP_ARRAY_NEW_DATA: case EA_OP_ARRAY_INIT_DATA:
            if (!rd_u32(r, &in->imm.pair.a)) return false;  // typeidx
            if (!rd_u32(r, &in->imm.pair.b)) return false;  // dataidx
            break;
        case EA_OP_ARRAY_NEW_ELEM: case EA_OP_ARRAY_INIT_ELEM:
            if (!rd_u32(r, &in->imm.pair.a)) return false;  // typeidx
            if (!rd_u32(r, &in->imm.pair.b)) return false;  // elemidx
            break;
        case EA_OP_ARRAY_GET: case EA_OP_ARRAY_GET_S: case EA_OP_ARRAY_GET_U:
        case EA_OP_ARRAY_SET: case EA_OP_ARRAY_FILL:
            if (!rd_u32(r, &in->imm.u32)) return false;
            break;
        case EA_OP_ARRAY_COPY:
            if (!rd_u32(r, &in->imm.pair.a)) return false;  // dst type
            if (!rd_u32(r, &in->imm.pair.b)) return false;  // src type
            break;
        case EA_OP_REF_TEST: case EA_OP_REF_TEST_NULL:
        case EA_OP_REF_CAST: case EA_OP_REF_CAST_NULL: {
            int64_t ht;
            if (!rd_s33(r, &ht)) return false;
            in->imm.pair.a = (uint32_t)(int32_t)ht; // >=0: typeidx; <0: abstract kind
            break;
        }
        case EA_OP_BR_ON_CAST: case EA_OP_BR_ON_CAST_FAIL: {
            uint8_t flags;
            if (!rd_byte(r, &flags) || (flags & ~3u)) return false;
            in->imm.q.a = flags;                            // bit0: src null, bit1: dst null
            if (!rd_u32(r, &in->imm.q.d)) return false;     // labelidx (union-safe slot)
            int64_t ht;
            if (!rd_s33(r, &ht)) return false;
            in->imm.q.b = (uint32_t)(int32_t)ht;            // src heap type
            if (!rd_s33(r, &ht)) return false;
            in->imm.q.c = (uint32_t)(int32_t)ht;            // dst heap type
            break;
        }
        case EA_OP_ANY_CONVERT_EXTERN: case EA_OP_EXTERN_CONVERT_ANY:
        case EA_OP_REF_I31: case EA_OP_I31_GET_S: case EA_OP_I31_GET_U:
            break;

        // ---- SIMD
        case EA_OP_V128_LOAD: case EA_OP_V128_LOAD8X8_S: case EA_OP_V128_LOAD8X8_U:
        case EA_OP_V128_LOAD16X4_S: case EA_OP_V128_LOAD16X4_U:
        case EA_OP_V128_LOAD32X2_S: case EA_OP_V128_LOAD32X2_U:
        case EA_OP_V128_LOAD8_SPLAT: case EA_OP_V128_LOAD16_SPLAT:
        case EA_OP_V128_LOAD32_SPLAT: case EA_OP_V128_LOAD64_SPLAT:
        case EA_OP_V128_STORE:
        case EA_OP_V128_LOAD32_ZERO: case EA_OP_V128_LOAD64_ZERO:
            if (!rd_memarg(r, in, true)) return false;
            break;
        case EA_OP_V128_LOAD8_LANE: case EA_OP_V128_LOAD16_LANE:
        case EA_OP_V128_LOAD32_LANE: case EA_OP_V128_LOAD64_LANE:
        case EA_OP_V128_STORE8_LANE: case EA_OP_V128_STORE16_LANE:
        case EA_OP_V128_STORE32_LANE: case EA_OP_V128_STORE64_LANE: {
            uint8_t lane;
            if (!rd_memarg(r, in, true)) return false;
            if (!rd_byte(r, &lane)) return false;
            in->lane = lane;
            break;
        }
        case EA_OP_V128_CONST: case EA_OP_I8X16_SHUFFLE: {
            const uint8_t *p;
            if (!rd_bytes(r, 16, &p)) return false;
            memcpy(in->imm.bytes, p, 16);
            break;
        }
        case EA_OP_I8X16_EXTRACT_LANE_S: case EA_OP_I8X16_EXTRACT_LANE_U:
        case EA_OP_I8X16_REPLACE_LANE:
        case EA_OP_I16X8_EXTRACT_LANE_S: case EA_OP_I16X8_EXTRACT_LANE_U:
        case EA_OP_I16X8_REPLACE_LANE:
        case EA_OP_I32X4_EXTRACT_LANE: case EA_OP_I32X4_REPLACE_LANE:
        case EA_OP_I64X2_EXTRACT_LANE: case EA_OP_I64X2_REPLACE_LANE:
        case EA_OP_F32X4_EXTRACT_LANE: case EA_OP_F32X4_REPLACE_LANE:
        case EA_OP_F64X2_EXTRACT_LANE: case EA_OP_F64X2_REPLACE_LANE: {
            uint8_t lane;
            if (!rd_byte(r, &lane)) return false;
            in->imm.u32 = lane;
            break;
        }
        default:
            if (op >= EA_OPV_BASE) break; // remaining simd/GC ops: no immediates
            // numeric ops 0x45..0xC4 and FC trunc_sat have no immediates
            if ((op >= 0x45 && op <= 0xC4) ||
                (op >= EA_OP_I32_TRUNC_SAT_F32_S && op <= EA_OP_I64_TRUNC_SAT_F64_U))
                break;
            c->n--;
            fprintf(stderr, "EA_DBG unknown op 0x%x at imm_off %u\n", op, imm_off);
            return false; // unknown opcode
        }
    }
    return !stop_at_end;
}

static bool predecode_region(const uint8_t *p, uint32_t len, InsList *out) {
    Rd r = {p, p + len, p};
    InsCtx c;
    memset(&c, 0, sizeof(c));
    bool ok = predecode(&r, &c, false, NULL);
    if (ok && c.nest_sp != 0) ok = false; // unbalanced
    free(c.nest);
    if (!ok) {
        free(c.v);
        free(c.pool);
        memset(out, 0, sizeof(*out));
        return ok;
    }
    out->v = c.v;
    out->n = c.n;
    out->pool = c.pool;
    out->pool_n = c.pool_n;
    if (getenv("EA_DDBG") && c.n >= 2)
        fprintf(stderr, "REGIONX n=%u v0.end=%u v1.end=%u\n", c.n, c.v[0].end_idx, c.v[1].end_idx);
    if (ok) {
        // resolve nesting
        uint32_t *stack = (uint32_t *)ea_malloc((c.n ? c.n : 1) * 4);
        uint32_t sp = 0;
        for (uint32_t i = 0; i < c.n; i++) {
            EaInstr *in = &out->v[i];
            switch (in->opcode) {
            case EA_OP_BLOCK: case EA_OP_LOOP: case EA_OP_IF: case EA_OP_TRY_TABLE:
                stack[sp++] = i;
                break;
            case EA_OP_END:
                if (sp == 0) break; // function-final end (terminator)
                sp--;
                out->v[stack[sp]].end_idx = i;
                break;
            default: break;
            }
            if (!ok) break;
        }
        if (ok && sp != 0) ok = false;
        free(stack);
    }
    if (!ok) {
        free(c.v);
        free(c.pool);
        memset(out, 0, sizeof(*out));
    }
    return ok;
}

// decode one const expr starting at r->p; stops after matching end.
static bool predecode_expr(Rd *r, InsList *out) {
    InsCtx c;
    memset(&c, 0, sizeof(c));
    bool hit_end = false;
    bool ok = predecode(r, &c, true, &hit_end);
    if (ok && !hit_end) ok = false;
    free(c.nest);
    if (!ok) {
        free(c.v);
        free(c.pool);
        memset(out, 0, sizeof(*out));
        return ok;
    }
    out->v = c.v;
    out->n = c.n;
    out->pool = c.pool;
    out->pool_n = c.pool_n;
    if (getenv("EA_DDBG") && c.n >= 2)
        fprintf(stderr, "PREDECOD exit n=%u v0.end=%u v1.end=%u\n", c.n, c.v[0].end_idx, c.v[1].end_idx);
    return ok;
}

// ---------------------------------------------------------------- module decode
static void free_inslist(InsList *l) {
    free(l->v);
    free(l->pool);
}

void ea_module_free(EaModule *m) {
    for (uint32_t i = 0; i < m->n_imports; i++) {
        free(m->imports[i].module);
        free(m->imports[i].name);
    }
    free(m->imports);
    free(m->types);
    for (uint32_t i = 0; i < m->n_funcs; i++) {
        EaFunc *f = &m->funcs[i];
        free(f->locals);
        free_inslist(&f->code);
    }
    free(m->funcs);
    for (uint32_t i = 0; i < m->n_tables; i++)
        if (m->tables[i].has_init) free_inslist(&m->tables[i].init);
    free(m->tables);
    free(m->memories);
    for (uint32_t i = 0; i < m->n_globals_def; i++) free_inslist(&m->globals_def[i].init);
    free(m->globals_def);
    for (uint32_t i = 0; i < m->n_exports; i++) free(m->exports[i].name);
    free(m->exports);
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        free_inslist(&e->offset);
        free(e->func_idx);
        if (e->items)
            for (uint32_t j = 0; j < e->n_items; j++) free_inslist(&e->items[j]);
        free(e->items);
    }
    free(m->elems);
    for (uint32_t i = 0; i < m->n_datas; i++) free_inslist(&m->datas[i].offset);
    free(m->datas);
    free(m->tags);
    free(m->owned_bytes);
    memset(m, 0, sizeof(*m));
}

int ea_decode_module(EaModule *m, const uint8_t *bytes, size_t len, char **err) {
    memset(m, 0, sizeof(*m));
#define FAIL(msg) do { if (err && !*err) *err = ea_strndup(msg, strlen(msg)); return -1; } while (0)
#define FAIL_V(msg) do { if (err && !*err) *err = ea_strndup(msg, strlen(msg)); return 1; } while (0)
    if (len < 8) FAIL("unexpected end");
    if (memcmp(bytes, "\0asm", 4) != 0) FAIL("magic header not detected");
    uint32_t version;
    memcpy(&version, bytes + 4, 4);
    if (version != 1) FAIL("unknown binary version");

    // keep our own copy so pointers stay valid regardless of caller's buffer
    m->owned_bytes = (uint8_t *)ea_malloc(len);
    memcpy(m->owned_bytes, bytes, len);
    m->owned_len = len;

    m->feat.simd = m->feat.bulk_memory = m->feat.reference_types = m->feat.sign_ext =
        m->feat.nontrap_convs = m->feat.multi_value = m->feat.tail_call = true;

    Rd r = {bytes + 8, bytes + len, bytes + 8};

    // temporaries
    uint32_t *def_func_types = NULL; uint32_t n_def_func_types = 0;
    EaTable *def_tables = NULL; uint32_t n_def_tables = 0;
    InsList *table_inits = NULL;
    EaMemory *def_mems = NULL; uint32_t n_def_mems = 0;
    EaGlobal *def_globals = NULL; uint32_t n_def_globals = 0;
    EaElem *elems = NULL; uint32_t n_elems = 0;
    EaData *datas = NULL; uint32_t n_datas = 0;
    uint32_t *imp_funcs = NULL;
    EaTable *imp_tables = NULL;
    EaMemory *imp_mems = NULL;
    EaGlobal *imp_globals = NULL;
    EaTag *imp_tags = NULL, *def_tags = NULL;
    uint32_t n_imp_tags_cap = 0, n_def_tags = 0;
    struct BodyRef { const uint8_t *p; uint32_t len; uint32_t off; } *bodies = NULL;
    uint32_t start_func_idx = 0; bool has_start = false;

    int last_rank = 0;
    static const int sec_rank[14] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 11, 6};
    // id:      0(custom,any) 1  2  3  4  5  6  7  8  9  10->12  11->13  12->11

    while (r.p < r.end) {
        uint8_t sec_id;
        if (!rd_byte(&r, &sec_id)) FAIL("unexpected end of section or function");
        uint32_t sec_len;
        if (!rd_u32(&r, &sec_len)) FAIL("unexpected end of section or function");
        if ((uint64_t)sec_len > (uint64_t)(r.end - r.p)) FAIL("unexpected end of section or function");
        if (sec_id > 13) FAIL("malformed section id");
        if (sec_id != 0) {
            if (sec_rank[sec_id] <= last_rank) FAIL("unexpected content after last section");
            last_rank = sec_rank[sec_id];
        }
        Rd s = {r.p, r.p + sec_len, r.p};
        r.p += sec_len;
        switch (sec_id) {
        case 0: { // custom
            char *nm;
            if (!rd_name(&s, &nm)) FAIL(g_rd_err ? g_rd_err : "malformed custom section");
            free(nm);
            break; // remaining custom bytes ignored (e.g. name section)
        }
        case 1: { // type
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed type section");
            if (n > (1u << 20)) FAIL("malformed type section");
            // type indices advance per contained type, so a rec of k entries
            // consumes k indices; pre-count then fill
            {
                Rd scan = s;
                uint32_t total = 0;
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t f;
                    if (!rd_byte(&scan, &f)) FAIL_V("malformed type section");
                    if (getenv("EA_VDBG2")) fprintf(stderr, "SCAN i=%u f=%02x\n", i, f);
                    if (f == 0x4E) {
                        uint32_t g;
                        if (!rd_u32(&scan, &g)) FAIL_V("malformed type section");
                        total += g;
                        for (uint32_t k = 0; k < g; k++) {
                            uint8_t f2;
                            EaType tmp;
                            memset(&tmp, 0, sizeof(tmp));
                            if (!rd_byte(&scan, &f2)) FAIL_V("malformed type section");
                            if (!read_type_entry_form(&scan, f2, &tmp)) FAIL_V("unsupported type form (gc type forms)");
                        }
                    } else {
                        total++;
                        EaType tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        if (!read_type_entry_form(&scan, f, &tmp)) FAIL_V("unsupported type form (gc type forms)");
                    }
                }
                m->types = (EaType *)ea_zalloc((total ? total : 1) * sizeof(EaType));
                m->n_types = total;
                uint32_t idx = 0;
                for (uint32_t i = 0; i < n && idx < total; i++) {
                    uint8_t f;
                    if (!rd_byte(&s, &f)) FAIL_V("malformed type section");
                    if (f == 0x4E) {
                        uint32_t g;
                        if (!rd_u32(&s, &g)) FAIL_V("malformed type section");
                        for (uint32_t k = 0; k < g && idx < total; k++, idx++) {
                            uint8_t f2;
                            if (!rd_byte(&s, &f2)) FAIL_V("malformed type section");
                            if (!read_type_entry_form(&s, f2, &m->types[idx])) FAIL_V("unsupported type form (gc type forms)");
                            m->types[idx].rec_pos = k;
                            m->types[idx].rec_size = g;
                        }
                    } else {
                        if (!read_type_entry_form(&s, f, &m->types[idx])) FAIL_V("unsupported type form (gc type forms)");
                        m->types[idx].rec_pos = 0;
                        m->types[idx].rec_size = 1;
                        idx++;
                    }
                }
                m->n_types = idx;
            }
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 2: { // import
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed import section");
            if (n > (1u << 20)) FAIL("malformed import section");
            m->imports = (EaImport *)ea_zalloc((n ? n : 1) * sizeof(EaImport));
            m->n_imports = n;
            imp_funcs = (uint32_t *)ea_zalloc((n ? n : 1) * 4);
            imp_tables = (EaTable *)ea_zalloc((n ? n : 1) * sizeof(EaTable));
            imp_mems = (EaMemory *)ea_zalloc((n ? n : 1) * sizeof(EaMemory));
            imp_globals = (EaGlobal *)ea_zalloc((n ? n : 1) * sizeof(EaGlobal));
            for (uint32_t i = 0; i < n; i++) {
                EaImport *im = &m->imports[i];
                g_rd_err = NULL;
                if (!rd_name(&s, &im->module) || !rd_name(&s, &im->name))
                    FAIL(g_rd_err ? g_rd_err : "malformed import section");
                uint8_t kind;
                if (!rd_byte(&s, &kind)) FAIL("malformed import section");
                im->kind = kind;
                switch (kind) {
                case EAK_FUNC:
                    if (!rd_u32(&s, &im->idx)) FAIL("malformed import section");
                    imp_funcs[m->n_imp_funcs++] = im->idx;
                    break;
                case EAK_TABLE: {
                    EaValType rt;
                    if (!rd_reftype(&s, &rt)) FAIL("malformed import section");
                    uint64_t mn, mx;
                    bool hm;
                    uint8_t limflag;
                    if (!rd_limits(&s, &mn, &mx, &hm, 0x07, &limflag)) FAIL("malformed import section");
                    if (mn > UINT32_MAX || (hm && mx > UINT32_MAX)) FAIL("malformed import section");
                    EaTable *t = &imp_tables[m->n_imp_tables++];
                    t->ref_type = rt;
                    t->min = (uint32_t)mn;
                    t->max = hm ? (uint32_t)mx : UINT32_MAX;
                    t->has_max = hm;
                    t->is64 = (limflag & 0x04) != 0;
                    im->table = *t;
                    break;
                }
                case EAK_MEMORY: {
                    uint64_t mn, mx;
                    bool hm;
                    uint8_t limflag;
                    if (!rd_limits(&s, &mn, &mx, &hm, 0x07, &limflag)) FAIL("malformed import section");
                    if ((limflag & 4) ? (mn > (1ull << 48) || (hm && mx > (1ull << 48)))
                                      : (mn > 65536 || (hm && mx > 65536)))
                        FAIL("memory size must be at most 65536 pages");
                    if (hm && mx < mn) FAIL("size minimum must not be greater than maximum");
                    EaMemory *mm = &imp_mems[m->n_imp_memories++];
                    mm->min = mn; mm->max = mx; mm->has_max = hm;
                    mm->is64 = (limflag & 4) != 0;
                    mm->shared = (limflag & 2) != 0;
                    mm->page_size = 65536;
                    im->memory = *mm;
                    break;
                }
                case EAK_GLOBAL: {
                    EaValType vt;
                    if (!rd_valtype(&s, &vt)) FAIL("malformed import section");
                    uint8_t mut;
                    if (!rd_byte(&s, &mut) || mut > 1) FAIL("malformed import section");
                    EaGlobal *g = &imp_globals[m->n_imp_globals++];
                    g->type = vt;
                    g->mutable_ = mut != 0;
                    im->global.type = vt;
                    im->global.mutable_ = mut != 0;
                    break;
                }
                case EAK_TAG: {
                    uint32_t attr, tidx;
                    if (!rd_u32(&s, &attr) || attr != 0) FAIL("malformed import section");
                    if (!rd_u32(&s, &tidx)) FAIL("malformed import section");
                    if (n_imp_tags_cap == m->n_imp_tags) {
                        n_imp_tags_cap = n_imp_tags_cap ? n_imp_tags_cap * 2 : 4;
                        imp_tags = (EaTag *)ea_realloc(imp_tags, n_imp_tags_cap * sizeof(EaTag));
                    }
                    imp_tags[m->n_imp_tags].type_idx = tidx;
                    im->kind = EAK_TAG;
                    im->idx = m->n_imp_tags++;
                    m->feat.exceptions = true;
                    break;
                }
                default:
                    FAIL("malformed import kind");
                }
            }
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 3: { // function
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed function section");
            if (n > (1u << 20)) FAIL("malformed function section");
            def_func_types = (uint32_t *)ea_malloc((n ? n : 1) * 4);
            for (uint32_t i = 0; i < n; i++)
                if (!rd_u32(&s, &def_func_types[i])) FAIL("malformed function section");
            n_def_func_types = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 4: { // table
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed table section");
            if (getenv("EA_DDBG")) fprintf(stderr, "TBL count=%u\n", n);
            if (n > 1024) FAIL("malformed table section");
            def_tables = (EaTable *)ea_zalloc((n ? n : 1) * sizeof(EaTable));
            table_inits = (InsList *)ea_zalloc((n ? n : 1) * sizeof(InsList));
            for (uint32_t i = 0; i < n; i++) {
                EaValType rt;
                // "0x40 0x00" prefix marks a table with an init expression
                bool init_prefix = false;
                {
                    uint8_t first;
                    if (!rd_byte(&s, &first)) FAIL("malformed table section");
                    if (first == 0x40) {
                        uint8_t zero;
                        if (!rd_byte(&s, &zero)) FAIL("malformed table section");
                        init_prefix = true;
                        if (!rd_reftype(&s, &rt)) FAIL("malformed table section");
                    } else {
                        // any reftype (funcref/externref/typed refs): re-read
                        s.p--;
                        if (!rd_reftype(&s, &rt)) FAIL("malformed table section");
                    }
                }
                uint64_t mn, mx;
                bool hm;
                uint8_t limflag;
                if (!rd_limits(&s, &mn, &mx, &hm, 0x05, &limflag)) FAIL("malformed table section");
                def_tables[i].ref_type = rt;
                def_tables[i].min = (uint32_t)mn;
                def_tables[i].max = hm ? (uint32_t)mx : UINT32_MAX;
                def_tables[i].has_max = hm;
                def_tables[i].is64 = (limflag & 0x04) != 0;
                if (init_prefix) {
                    InsList init_expr;
                    memset(&init_expr, 0, sizeof(init_expr));
                    if (!predecode_expr(&s, &init_expr)) FAIL("malformed table section");
                    table_inits[i] = init_expr;
                    def_tables[i].has_init = true;
                }
            }
            n_def_tables = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 5: { // memory
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed memory section");
            if (n > 1024) FAIL("malformed memory section");
            def_mems = (EaMemory *)ea_zalloc((n ? n : 1) * sizeof(EaMemory));
            for (uint32_t i = 0; i < n; i++) {
                uint64_t mn, mx;
                bool hm;
                uint8_t limflag;
                    if (!rd_limits(&s, &mn, &mx, &hm, 0x07, &limflag)) FAIL("malformed memory section");
                if ((limflag & 4) ? (mn > (1ull << 48) || (hm && mx > (1ull << 48)))
                                  : (mn > 65536 || (hm && mx > 65536)))
                    FAIL("memory size must be at most 65536 pages");
                if (hm && mx < mn) FAIL("size minimum must not be greater than maximum");
                def_mems[i].min = mn; def_mems[i].max = mx; def_mems[i].has_max = hm;
                def_mems[i].is64 = (limflag & 4) != 0;
                def_mems[i].shared = (limflag & 2) != 0;
                def_mems[i].page_size = 65536;
            }
            n_def_mems = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 6: { // global
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed global section");
            if (n > (1u << 20)) FAIL("malformed global section");
            def_globals = (EaGlobal *)ea_zalloc((n ? n : 1) * sizeof(EaGlobal));
            for (uint32_t i = 0; i < n; i++) {
                EaValType vt;
                if (!rd_valtype(&s, &vt)) FAIL("malformed global section");
                uint8_t mut;
                if (!rd_byte(&s, &mut) || mut > 1) FAIL("malformed global section");
                def_globals[i].type = vt;
                def_globals[i].mutable_ = mut != 0;
                if (!predecode_expr(&s, &def_globals[i].init)) FAIL("malformed global init expr");
            }
            n_def_globals = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 7: { // export
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed export section");
            if (n > (1u << 20)) FAIL("malformed export section");
            m->exports = (EaExport *)ea_zalloc((n ? n : 1) * sizeof(EaExport));
            m->n_exports = n;
            for (uint32_t i = 0; i < n; i++) {
                EaExport *ex = &m->exports[i];
                uint32_t name_len;
                if (!rd_name_len(&s, &ex->name, &name_len))
                    FAIL(g_rd_err ? g_rd_err : "malformed export section");
                ex->name_len = name_len;
                for (uint32_t q = 0; q < i; q++) {
                    if (m->exports[q].name_len == name_len &&
                        memcmp(m->exports[q].name, ex->name, name_len) == 0)
                        FAIL("duplicate export name");
                }
                uint8_t kind;
                if (!rd_byte(&s, &kind) || kind > 4) FAIL("malformed export section");
                ex->kind = kind;
                if (!rd_u32(&s, &ex->idx)) FAIL("malformed export section");
            }
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 8: { // start
            if (!rd_u32(&s, &start_func_idx)) FAIL("malformed start section");
            has_start = true;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 9: { // element
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed element section");
            if (n > (1u << 20)) FAIL("malformed element section");
            elems = (EaElem *)ea_zalloc((n ? n : 1) * sizeof(EaElem));
            for (uint32_t i = 0; i < n; i++) {
                EaElem *e = &elems[i];
                uint32_t flags;
                if (!rd_u32(&s, &flags) || flags > 7) FAIL("malformed element section");
                if (getenv("EA_DDBG")) fprintf(stderr, "ELEM[%u] flags=%u off=%ld\n", i, flags, (long)(s.p - s.start));
                e->mode = (flags & 1) ? ((flags & 2) ? SEG_DECLARATIVE : SEG_PASSIVE) : SEG_ACTIVE;
                bool uses_exprs = (flags & 4) != 0;
                if (e->mode == SEG_ACTIVE) {
                    if (flags == 2 || flags == 6) {
                        if (!rd_u32(&s, &e->table_idx)) FAIL("malformed element section");
                    }
                    if (!predecode_expr(&s, &e->offset)) FAIL("malformed element offset expr");
                }
                if (uses_exprs) {
                    if (e->mode != SEG_ACTIVE || flags == 6) {
                        EaValType rt;
                        if (!rd_reftype(&s, &rt)) FAIL("malformed element section");
                        e->ref_type = rt;
                    } else {
                        e->ref_type = VT_FUNCREF;
                    }
                } else {
                    // funcidx forms: flags 1/2/3 carry an elemkind byte, flags 0 does not
                    if (flags == 1 || flags == 2 || flags == 3) {
                        uint8_t ek;
                        if (!rd_byte(&s, &ek) || ek != 0) FAIL("malformed element section");
                    }
                    e->ref_type = EA_VT_ABSREF(EA_ABS_FUNC); // (ref func), non-null
                }
                uint32_t cnt;
                if (!rd_u32(&s, &cnt)) FAIL("malformed element section");
                if (getenv("EA_DDBG")) fprintf(stderr, "  cnt=%u off=%ld\n", cnt, (long)(s.p - s.start));
                if (cnt > (1u << 24)) FAIL("malformed element section");
                e->n_items = cnt;
                if (uses_exprs) {
                    e->items = (InsList *)ea_zalloc((cnt ? cnt : 1) * sizeof(InsList));
                    for (uint32_t j = 0; j < cnt; j++)
                        if (!predecode_expr(&s, &e->items[j])) FAIL("malformed element item expr");
                } else {
                    e->func_idx = (uint32_t *)ea_malloc((cnt ? cnt : 1) * 4);
                    for (uint32_t j = 0; j < cnt; j++)
                        if (!rd_u32(&s, &e->func_idx[j])) FAIL("malformed element section");
                }
            }
            n_elems = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 12: { // data count
            if (!rd_u32(&s, &m->data_count)) FAIL("malformed data count section");
            m->has_data_count = true;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 10: { // code
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed code section");
            if (n != n_def_func_types) FAIL("function and code section have inconsistent lengths");
            bodies = (struct BodyRef *)ea_zalloc((n ? n : 1) * sizeof(*bodies));
            for (uint32_t i = 0; i < n; i++) {
                uint32_t sz;
                if (!rd_u32(&s, &sz)) FAIL("malformed code section");
                if ((uint64_t)sz > (uint64_t)(s.end - s.p)) FAIL("unexpected end of section or function");
                bodies[i].p = s.p;
                bodies[i].len = sz;
                bodies[i].off = (uint32_t)(s.p - bytes);
                s.p += sz;
            }
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 11: { // data
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed data section");
            if (m->has_data_count && n != m->data_count)
                FAIL("data count and data section have inconsistent lengths");
            if (n > (1u << 20)) FAIL("malformed data section");
            datas = (EaData *)ea_zalloc((n ? n : 1) * sizeof(EaData));
            for (uint32_t i = 0; i < n; i++) {
                EaData *d = &datas[i];
                uint32_t flags;
                if (!rd_u32(&s, &flags) || flags > 2) FAIL("malformed data section");
                d->mode = (flags == 1) ? SEG_PASSIVE : SEG_ACTIVE;
                if (flags == 2) {
                    if (!rd_u32(&s, &d->mem_idx)) FAIL("malformed data section");
                }
                if (flags != 1) {
                    if (!predecode_expr(&s, &d->offset)) FAIL("malformed data offset expr");
                }
                uint32_t blen;
                if (!rd_u32(&s, &blen)) FAIL("malformed data section");
                const uint8_t *p;
                if (!rd_bytes(&s, blen, &p)) FAIL("malformed data section");
                d->data_off = (uint32_t)(p - bytes);
                d->data_len = blen;
            }
            n_datas = n;
            if (s.p != s.end) FAIL("section size mismatch");
            break;
        }
        case 13: { // tag
            uint32_t n;
            if (!rd_u32(&s, &n)) FAIL("malformed tag section");
            if (n > (1u << 20)) FAIL("malformed tag section");
            def_tags = (EaTag *)ea_zalloc((n ? n : 1) * sizeof(EaTag));
            for (uint32_t i = 0; i < n; i++) {
                uint32_t attr, tidx;
                if (!rd_u32(&s, &attr) || attr != 0) FAIL("malformed tag section");
                if (!rd_u32(&s, &tidx)) FAIL("malformed tag section");
                def_tags[i].type_idx = tidx;
            }
            n_def_tags = n;
            if (s.p != s.end) FAIL("section size mismatch");
            m->feat.exceptions = true;
            break;
        }
        default:
            FAIL("malformed section id");
        }
    }

    // ---- assemble combined arrays (imports first)
    uint32_t n_funcs = m->n_imp_funcs + n_def_func_types;
    m->n_funcs = n_funcs;
    m->funcs = (EaFunc *)ea_zalloc((n_funcs ? n_funcs : 1) * sizeof(EaFunc));
    for (uint32_t i = 0; i < n_funcs; i++)
            for (uint32_t i = 0; i < m->n_imp_funcs; i++)
        m->funcs[i].type_idx = imp_funcs[i];
    for (uint32_t j = 0; j < n_def_func_types; j++) {
        EaFunc *f = &m->funcs[m->n_imp_funcs + j];
        f->type_idx = def_func_types[j];
        f->code_off = bodies ? bodies[j].off : 0;
        // pre-decode body (skip locals decl)
        if (bodies) {
            const uint8_t *bp = bodies[j].p;
            size_t blen = bodies[j].len;
            Rd br = {bp, bp + blen, bp};
            uint32_t n_groups;
            if (!rd_u32(&br, &n_groups)) FAIL("malformed locals");
            uint64_t total = 0;
            uint32_t cap = 0;
            EaValType *locals = NULL;
            for (uint32_t g = 0; g < n_groups; g++) {
                uint64_t cnt64;
                EaValType vt;
                uint32_t cnt;
                if (!rd_u32(&br, &cnt)) { free(locals); FAIL("malformed locals"); }
                cnt64 = cnt;
                if (!rd_valtype(&br, &vt)) { free(locals); FAIL("malformed locals"); }
                if (ea_tref_real(vt) && ea_tref_idx(vt) >= m->n_types)
                    FAIL_V("unknown type");
                if (total + cnt64 > (1u << 24)) { free(locals); FAIL("too many locals"); }
                if (total + cnt64 > cap) {
                    cap = (uint32_t)(total + cnt64);
                    locals = (EaValType *)ea_realloc(locals, cap * sizeof(EaValType));
                }
                for (uint32_t k = 0; k < cnt; k++) locals[total++] = vt;
            }
            f->locals = locals;
            f->n_locals = (uint32_t)total;
            f->body = br.p;
            f->body_len = (uint32_t)(blen - (size_t)(br.p - bp));
            g_rd_err = NULL;
            if (!predecode_region((const uint8_t *)f->body, f->body_len, &f->code)) {
                free(locals);
                f->locals = NULL;
                FAIL(g_rd_err ? g_rd_err : "illegal opcode in function body");
            }
        }
    }
    free(bodies);

    uint32_t n_tables = m->n_imp_tables + n_def_tables;
    m->n_tables = n_tables;
    m->tables = (EaTable *)ea_malloc((n_tables ? n_tables : 1) * sizeof(EaTable));
    for (uint32_t i = 0; i < m->n_imp_tables; i++) m->tables[i] = imp_tables[i];
    for (uint32_t j = 0; j < n_def_tables; j++) {
        m->tables[m->n_imp_tables + j] = def_tables[j];
        if (table_inits && table_inits[j].n) {
            m->tables[m->n_imp_tables + j].init = table_inits[j];
            m->tables[m->n_imp_tables + j].has_init = true;
            table_inits[j].v = NULL; // ownership moved
        }
    }

    uint32_t n_mems = m->n_imp_memories + n_def_mems;
    m->n_memories = n_mems;
    m->memories = (EaMemory *)ea_malloc((n_mems ? n_mems : 1) * sizeof(EaMemory));
    for (uint32_t i = 0; i < m->n_imp_memories; i++) m->memories[i] = imp_mems[i];
    for (uint32_t j = 0; j < n_def_mems; j++) m->memories[m->n_imp_memories + j] = def_mems[j];

    uint32_t n_globals = m->n_imp_globals + n_def_globals;
    m->n_globals_def = n_globals; // combined array (imported have empty init)
    m->globals_def = (EaGlobal *)ea_malloc((n_globals ? n_globals : 1) * sizeof(EaGlobal));
    for (uint32_t i = 0; i < m->n_imp_globals; i++) m->globals_def[i] = imp_globals[i];
    for (uint32_t j = 0; j < n_def_globals; j++) m->globals_def[m->n_imp_globals + j] = def_globals[j];
    m->n_tags = m->n_imp_tags + n_def_tags;
    m->tags = (EaTag *)ea_malloc((m->n_tags ? m->n_tags : 1) * sizeof(EaTag));
    for (uint32_t i = 0; i < m->n_imp_tags; i++) m->tags[i] = imp_tags[i];
    for (uint32_t j = 0; j < n_def_tags; j++) m->tags[m->n_imp_tags + j] = def_tags[j];

    m->elems = elems;
    m->n_elems = n_elems;
    m->datas = datas;
    m->n_datas = n_datas;
    m->start_func = start_func_idx;
    m->has_start = has_start;

    // a data count section pins the exact data segment count, including
    // the "count without any data section" case
    if (m->has_data_count && n_datas != m->data_count)
        FAIL("data count and data section have inconsistent lengths");

    if (r.p != r.end) FAIL("junk after last section");

    if (table_inits)
        for (uint32_t j = 0; j < n_def_tables; j++)
            if (table_inits[j].v) free_inslist(&table_inits[j]);
    free(table_inits);
    free(def_func_types);
    free(def_tables);
    free(def_mems);
    free(def_globals);
    free(imp_funcs);
    free(imp_tables);
    free(imp_mems);
    free(imp_globals);
    return 0;
#undef FAIL
#undef FAIL_V
}
