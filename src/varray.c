/*
* Copyright (c) 2021 Calvin Rose & contributors
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include <janet.h>
#include <tgmath.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "varray.h"

#define TA_COUNT_TYPES (JANET_VARRAY_TYPE_F64 + 1)

/* Row table, one X(...) per element type, P forwarded to X unchanged
 * TAG   enum suffix         field  view->as member         CT  element C type
 * UT    wrapping arithmetic type: unsigned, never narrower than int, so promotion
 *       cannot overflow and results wrap; float rows use CT
 * ACC   64-bit reduction typ,, double on float rows */
#define TA_EACH_INT(X, P) \
    X(U8,  u8,  uint8_t,  unsigned, uint64_t, P) \
    X(S8,  s8,  int8_t,   unsigned, int64_t,  P) \
    X(U16, u16, uint16_t, unsigned, uint64_t, P) \
    X(S16, s16, int16_t,  unsigned, int64_t,  P) \
    X(U32, u32, uint32_t, uint32_t, uint64_t, P) \
    X(S32, s32, int32_t,  uint32_t, int64_t,  P) \
    X(U64, u64, uint64_t, uint64_t, uint64_t, P) \
    X(S64, s64, int64_t,  uint64_t, int64_t,  P)
#define TA_EACH_FLOAT(X, P) \
    X(F32, f32, float,  float,  double, P) \
    X(F64, f64, double, double, double, P)
#define TA_EACH(X, P) TA_EACH_INT(X, P) TA_EACH_FLOAT(X, P)
#define TA_EACH_DST(X) \
    X(U8, u8, uint8_t) X(S8, s8, int8_t) X(U16, u16, uint16_t) X(S16, s16, int16_t) \
    X(U32, u32, uint32_t) X(S32, s32, int32_t) X(U64, u64, uint64_t) X(S64, s64, int64_t) \
    X(F32, f32, float) X(F64, f64, double)
#define ROW_TAG(TAG, ...) #TAG
_Static_assert(sizeof(TA_EACH(ROW_TAG, _)) == sizeof(TA_EACH_DST(ROW_TAG)), "row tables diverged");
#define TA_ISFLOAT(T)  ((T) 0.5 != 0)
#define TA_ISSIGNED(T) ((T) -1 < (T) 0)
#define TA_IS64(T)     (!TA_ISFLOAT(T) && sizeof(T) == 8)
#define TA_ISNAN(x)    ((double)(x) != (double)(x))

static const char *ta_type_names[] = {
    "uint8", "int8", "uint16", "int16", "uint32", "int32",
    "uint64", "int64", "float32", "float64"
};
#define ROW_SIZE(TAG, field, CT, ...) [JANET_VARRAY_TYPE_##TAG] = sizeof(CT),
static const size_t ta_type_sizes[] = { TA_EACH(ROW_SIZE, _) };
_Static_assert(sizeof(ta_type_names) / sizeof(*ta_type_names) == TA_COUNT_TYPES, "names table out of sync");
_Static_assert(sizeof(ta_type_sizes) / sizeof(*ta_type_sizes) == TA_COUNT_TYPES, "sizes table out of sync");
#define ROW_HI(TAG, field, CT, ...) [JANET_VARRAY_TYPE_##TAG] = TA_ISFLOAT(CT) ? INFINITY \
    : 2.0 * (double)(UINT64_C(1) << (8 * sizeof(CT) - 1 - TA_ISSIGNED(CT))),
#define ROW_LO(TAG, field, CT, ...) [JANET_VARRAY_TYPE_##TAG] = TA_ISFLOAT(CT) ? -INFINITY \
    : TA_ISSIGNED(CT) ? -(double)(UINT64_C(1) << (8 * sizeof(CT) - 1)) : 0,
static const double ta_hi[] = { TA_EACH(ROW_HI, _) };
static const double ta_lo[] = { TA_EACH(ROW_LO, _) };

_Static_assert(sizeof(ta_lo) / sizeof(*ta_lo) == TA_COUNT_TYPES, "lo table out of sync");
_Static_assert(sizeof(ta_hi) / sizeof(*ta_hi) == TA_COUNT_TYPES, "hi table out of sync");
static JanetVArrayType ta_type_by_name(const uint8_t *name) {
    for (int i = 0; i < TA_COUNT_TYPES; i++)
        if (!janet_cstrcmp(name, ta_type_names[i])) return i;
    janet_panicf("invalid typed array type %S", name);
    return 0;
}

static void ta_check_scalar(JanetVArrayType t, double s) {
    if (t >= JANET_VARRAY_TYPE_F32) return;
    if (s != floor(s) || s < ta_lo[t] || s >= ta_hi[t])
        janet_panicf("%v is not representable as %s", janet_wrap_number(s), ta_type_names[t]);
}

static size_t ta_span(JanetVArrayType type, size_t size, size_t offset) {
    size_t atom = ta_type_sizes[type];
    /* buffer data is malloc aligned, so misaligned element pointers would only come from offset */
    if (offset % atom) janet_panic("typed array byte offset is not a multiple of element size");
    if (size > (SIZE_MAX - offset) / atom) janet_panic("typed array size overflow");
    return offset + atom * size;
}

static JanetVArrayBuffer *ta_buffer_init(JanetVArrayBuffer *buf, size_t size, int zero) {
    buf->data = zero ? calloc(size ? size : 1, 1) : malloc(size ? size : 1);
    if (!buf->data) janet_panic("out of memory");
    janet_gcpressure(size);
    buf->size = size;
    return buf;
}

static int ta_buffer_gc(void *p, size_t s) {
    (void) s;
    free(((JanetVArrayBuffer *)p)->data);
    return 0;
}

static void ta_buffer_marshal(void *p, JanetMarshalContext *ctx) {
    JanetVArrayBuffer *buf = p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, buf->size);
    janet_marshal_bytes(ctx, buf->data, buf->size);
}
static void *ta_buffer_unmarshal(JanetMarshalContext *ctx) {
    JanetVArrayBuffer *buf = janet_unmarshal_abstract(ctx, sizeof(JanetVArrayBuffer));
    buf->data = NULL;
    size_t size = janet_unmarshal_size(ctx);
    ta_buffer_init(buf, size, 0);
    janet_unmarshal_bytes(ctx, buf->data, size);
    return buf;
}

#ifdef JANET_ATEND_LENGTH
static size_t ta_view_length(void *p, size_t s) { (void) s; return ((JanetVArrayView *)p)->size; }
static size_t ta_buffer_length(void *p, size_t s) { (void) s; return ((JanetVArrayBuffer *)p)->size; }
#endif

const JanetAbstractType janet_ta_buffer_type = {
    .name = "va/buffer",
    .gc = ta_buffer_gc,
    .marshal = ta_buffer_marshal,
    .unmarshal = ta_buffer_unmarshal,
#ifdef JANET_ATEND_LENGTH
    .length = ta_buffer_length,
#endif
};

static int ta_mark(void *p, size_t s) {
    (void) s;
    janet_mark(janet_wrap_abstract(((JanetVArrayView *)p)->buffer));
    return 0;
}

static void ta_view_marshal(void *p, JanetMarshalContext *ctx) {
    JanetVArrayView *view = p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, view->size);
    janet_marshal_int(ctx, view->type);
    janet_marshal_size(ctx, (size_t)(view->as.u8 - view->buffer->data));
    janet_marshal_janet(ctx, janet_wrap_abstract(view->buffer));
}

static void *ta_view_unmarshal(JanetMarshalContext *ctx) {
    JanetVArrayView *view = janet_unmarshal_abstract(ctx, sizeof(JanetVArrayView));
    view->size = janet_unmarshal_size(ctx);
    int32_t atype = janet_unmarshal_int(ctx);
    if (atype < 0 || atype >= TA_COUNT_TYPES) janet_panic("bad typed array type");
    view->type = atype;
    size_t offset = janet_unmarshal_size(ctx);
    Janet buffer = janet_unmarshal_janet(ctx);
    if (!janet_checktype(buffer, JANET_ABSTRACT) ||
            janet_abstract_type(janet_unwrap_abstract(buffer)) != &janet_ta_buffer_type)
        janet_panicf("expected typed array buffer");
    view->buffer = janet_unwrap_abstract(buffer);
    if (view->buffer->size < ta_span(view->type, view->size, offset))
        janet_panic("bad typed array offset in marshalled data");
    view->as.u8 = view->buffer->data + offset;
    return view;
}

static JanetMethod ta_view_methods[6];

/* element in and out of a Janet value: 64-bit integer rows go through the
 * int64 abstracts to stay exact, everything else fits a double */
#define TA_WRAP(CT, x) \
    (!TA_IS64(CT) ? janet_wrap_number_safe((double)(x)) \
     : TA_ISSIGNED(CT) ? janet_wrap_s64((int64_t)(x)) : janet_wrap_u64((uint64_t)(x)))
#define GET_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: *out = TA_WRAP(CT, v->as.field[i]); break;
#define PUT_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: \
        v->as.field[i] = !TA_IS64(CT) ? (CT) janet_unwrap_number(x) \
                       : TA_ISSIGNED(CT) ? (CT) janet_unwrap_s64(x) : (CT) janet_unwrap_u64(x); \
        break;

static int ta_getter(void *p, Janet key, Janet *out) {
    JanetVArrayView *v = p;
    if (janet_checktype(key, JANET_KEYWORD))
        return janet_getmethod(janet_unwrap_keyword(key), ta_view_methods, out);
    if (!janet_checksize(key)) janet_panic("expected size as key");
    size_t i = (size_t) janet_unwrap_number(key);
    if (i >= v->size) return 0;
    switch (v->type) { TA_EACH(GET_CASE, _) }
    return 1;
}

static void ta_put(JanetVArrayView *v, size_t i, Janet x) {
    if (janet_checktype(x, JANET_NUMBER)) ta_check_scalar(v->type, janet_unwrap_number(x));
    else if (v->type != JANET_VARRAY_TYPE_U64 && v->type != JANET_VARRAY_TYPE_S64)
        janet_panic("expected number value");
    switch (v->type) { TA_EACH(PUT_CASE, _) }
}
static void ta_setter(void *p, Janet key, Janet x) {
    JanetVArrayView *v = p;
    if (!janet_checksize(key)) janet_panic("expected size as key");
    size_t i = (size_t) janet_unwrap_number(key);
    if (i >= v->size) janet_panic("index out of bounds");
    ta_put(v, i, x);
}

static Janet ta_view_next(void *p, Janet key) {
    JanetVArrayView *view = p;
    size_t index = 0;
    if (!janet_checktype(key, JANET_NIL)) {
        if (!janet_checksize(key)) janet_panic("expected size as key");
        index = (size_t) janet_unwrap_number(key) + 1;
    }
    return index < view->size ? janet_wrap_number((double) index) : janet_wrap_nil();
}

const JanetAbstractType janet_ta_view_type = {
    .name = "va/view",
    .gcmark = ta_mark,
    .get = ta_getter,
    .put = ta_setter,
    .marshal = ta_view_marshal,
    .unmarshal = ta_view_unmarshal,
    .next = ta_view_next,
#ifdef JANET_ATEND_LENGTH
    .length = ta_view_length
#endif
};

JanetVArrayBuffer *janet_varray_buffer(size_t size) {
    return ta_buffer_init(janet_abstract(&janet_ta_buffer_type, sizeof(JanetVArrayBuffer)), size, 1);
}

JanetVArrayView *janet_varray_view(JanetVArrayType type, size_t size, size_t offset, JanetVArrayBuffer *buffer) {
    JanetVArrayView *view = janet_abstract(&janet_ta_view_type, sizeof(JanetVArrayView));
    size_t buf_size = ta_span(type, size, offset);
    if (!buffer) buffer = janet_varray_buffer(buf_size);
    if (buffer->size < buf_size)
        janet_panicf("bad buffer size, %v bytes allocated < %v required", janet_wrap_number((double) buffer->size), janet_wrap_number((double) buf_size));
    view->buffer = buffer;
    view->size = size;
    view->as.u8 = buffer->data + offset;
    view->type = type;
    return view;
}

static JanetVArrayView *ta_is_view(Janet x) {
    if (!janet_checktype(x, JANET_ABSTRACT)) return NULL;
    void *abst = janet_unwrap_abstract(x);
    return janet_abstract_type(abst) == &janet_ta_view_type ? abst : NULL;
}

int janet_is_varray_view(Janet x, JanetVArrayType type) {
    JanetVArrayView *v = ta_is_view(x);
    return v && v->type == type;
}

JanetVArrayBuffer *janet_getvarray_buffer(const Janet *argv, int32_t n) {
    return janet_getabstract(argv, n, &janet_ta_buffer_type);
}

JanetVArrayView *janet_getvarray_any(const Janet *argv, int32_t n) {
    return janet_getabstract(argv, n, &janet_ta_view_type);
}

JanetVArrayView *janet_getvarray_view(const Janet *argv, int32_t n, JanetVArrayType type) {
    JanetVArrayView *view = janet_getvarray_any(argv, n);
    if (view->type != type)
        janet_panicf("bad slot #%d, expected typed array of type %s, got %v", n, ta_type_names[type], argv[n]);
    return view;
}

static Janet cfun_ta_new(int32_t argc, Janet *argv) {
    janet_arity(argc, 2, 4);
    size_t offset = 0; /* offset counts elements of the source view, not bytes */
    JanetVArrayBuffer *buffer = NULL;
    JanetVArrayType type = ta_type_by_name(janet_getkeyword(argv, 0));
    size_t size = janet_getsize(argv, 1);
    if (argc > 2) offset = janet_getsize(argv, 2);
    if (argc > 3) {
        int32_t blen;
        const uint8_t *bytes;
        JanetVArrayView *view;
        if (janet_bytes_view(argv[3], &bytes, &blen)) {
            buffer = janet_varray_buffer((size_t) blen);
            memcpy(buffer->data, bytes, blen);
        } else if ((view = ta_is_view(argv[3]))) {
            size_t base = (size_t)(view->as.u8 - view->buffer->data);
            size_t atom = ta_type_sizes[view->type];
            if (offset > (SIZE_MAX - base) / atom) janet_panic("typed array offset overflow");
            offset = base + offset * atom;
            buffer = view->buffer;
        } else {
            buffer = janet_getvarray_buffer(argv, 3);
        }
    }
    return janet_wrap_abstract(janet_varray_view(type, size, offset, buffer));
}

static Janet cfun_ta_buffer(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetVArrayView *view = ta_is_view(argv[0]);
    if (view) return janet_wrap_abstract(view->buffer);
    return janet_wrap_abstract(janet_varray_buffer(janet_getsize(argv, 0)));
}

static Janet cfun_ta_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetVArrayView *view = ta_is_view(argv[0]);
    if (view) return janet_wrap_number((double) view->size);
    return janet_wrap_number((double) janet_getvarray_buffer(argv, 0)->size);
}

static Janet cfun_ta_properties(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetVArrayView *view = ta_is_view(argv[0]);
    if (view) {
        JanetKV *props = janet_struct_begin(5);
        janet_struct_put(props, janet_ckeywordv("size"), janet_wrap_number((double) view->size));
        janet_struct_put(props, janet_ckeywordv("byte-offset"),
                         janet_wrap_number((double)(view->as.u8 - view->buffer->data)));
        janet_struct_put(props, janet_ckeywordv("type"), janet_ckeywordv(ta_type_names[view->type]));
        janet_struct_put(props, janet_ckeywordv("type-size"), janet_wrap_number((double) ta_type_sizes[view->type]));
        janet_struct_put(props, janet_ckeywordv("buffer"), janet_wrap_abstract(view->buffer));
        return janet_wrap_struct(janet_struct_end(props));
    }
    JanetVArrayBuffer *buffer = janet_getvarray_buffer(argv, 0);
    JanetKV *props = janet_struct_begin(1);
    janet_struct_put(props, janet_ckeywordv("size"), janet_wrap_number((double) buffer->size));
    return janet_wrap_struct(janet_struct_end(props));
}

static Janet cfun_ta_slice(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 3);
    JanetVArrayView *src = janet_getvarray_any(argv, 0);
    if (src->size > INT32_MAX) janet_panic("view too large for a janet array");
    int32_t length = (int32_t) src->size;
    int32_t start = argc > 1 ? janet_gethalfrange(argv, 1, length, "start") : 0;
    int32_t end = argc > 2 ? janet_gethalfrange(argv, 2, length, "end") : length;
    if (end < start) end = start;
    JanetArray *array = janet_array(end - start);
    for (int32_t i = start; i < end; i++)
        if (!ta_getter(src, janet_wrap_number(i), &array->data[i - start]))
            array->data[i - start] = janet_wrap_nil();
    array->count = end - start;
    return janet_wrap_array(array);
}

static size_t ta_runs(int32_t argc, Janet *argv, uint8_t **ps, uint8_t **pd, size_t *atom) {
    janet_arity(argc, 4, 5);
    JanetVArrayView *src = janet_getvarray_any(argv, 0), *dst = janet_getvarray_any(argv, 2);
    size_t is = janet_getsize(argv, 1), id = janet_getsize(argv, 3);
    size_t count = argc == 5 ? janet_getsize(argv, 4) : 1;
    *atom = ta_type_sizes[src->type];
    if (ta_type_sizes[dst->type] != *atom) janet_panic("element size mismatch");
    if (is > src->size || count > src->size - is || id > dst->size || count > dst->size - id)
        janet_panic("typed array copy out of bounds");
    *ps = src->as.u8 + is * *atom;
    *pd = dst->as.u8 + id * *atom;
    return count;
}

static Janet cfun_ta_copy_bytes(int32_t argc, Janet *argv) {
    uint8_t *ps, *pd;
    size_t atom;
    size_t n = ta_runs(argc, argv, &ps, &pd, &atom);
    memmove(pd, ps, n * atom);
    return janet_wrap_nil();
}

static Janet cfun_ta_swap_bytes(int32_t argc, Janet *argv) {
    uint8_t *ps, *pd;
    size_t atom;
    size_t len = ta_runs(argc, argv, &ps, &pd, &atom) * atom;
    uintptr_t s = (uintptr_t) ps, d = (uintptr_t) pd;
    if (s != d && s < d + len && d < s + len) janet_panic("swap-bytes: ranges overlap");
    for (size_t i = 0; i < len; i++) { uint8_t tmp = ps[i]; ps[i] = pd[i]; pd[i] = tmp; }
    return janet_wrap_nil();
}

// Kernels //
static JanetVArrayBuffer *ta_new_buffer(size_t size) {
    return ta_buffer_init(janet_abstract(&janet_ta_buffer_type, sizeof(JanetVArrayBuffer)), size, 0);
}
static JanetVArrayView *ta_new_view(JanetVArrayType t, size_t n) {
    return janet_varray_view(t, n, 0, ta_new_buffer(ta_span(t, n, 0)));
}
static JanetVArrayView *ta_from_indexed(JanetVArrayType t, Janet x) {
    const Janet *items;
    int32_t len;
    if (!janet_indexed_view(x, &items, &len)) janet_panicf("expected array or tuple, got %v", x);
    JanetVArrayView *v = ta_new_view(t, (size_t) len);
    for (int32_t i = 0; i < len; i++) ta_put(v, (size_t) i, items[i]);
    return v;
}
/* index operand is a uint32 view*/
static JanetVArrayView *ta_get_u32(Janet x) {
    JanetVArrayView *v = ta_is_view(x);
    if (!v) return ta_from_indexed(JANET_VARRAY_TYPE_U32, x);
    if (v->type != JANET_VARRAY_TYPE_U32) janet_panic("index view must be of type uint32");
    return v;
}
static void ta_same(JanetVArrayView *a, JanetVArrayView *b) {
    if (a->type != b->type)
        janet_panicf("type mismatch: %s vs %s, convert one with varray/cast",
                     ta_type_names[a->type], ta_type_names[b->type]);
    if (a->size != b->size) janet_panic("size mismatch");
}
static size_t ta_count(const uint8_t *ms, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) k += ms[i] != 0;
    return k;
}

typedef enum { TA_ARITH, TA_LT, TA_LE, TA_GT, TA_GE, TA_EQ, TA_NE } ta_op;
static const ta_op ta_mirror[] = {
    [TA_LT] = TA_GT, [TA_LE] = TA_GE, [TA_GT] = TA_LT, [TA_GE] = TA_LE, [TA_EQ] = TA_EQ, [TA_NE] = TA_NE
};

/* integer x OP s equals x OP k for k an element of the row, else a constant */
static int ta_threshold(JanetVArrayType t, ta_op op, double s, double *k) {
    if (s != s) return op == TA_NE;
    switch (op) {
        case TA_LT: case TA_GE: *k = ceil(s); break;
        case TA_LE: case TA_GT: *k = floor(s); break;
        default:
            if (s != floor(s) || s < ta_lo[t] || s >= ta_hi[t]) return op == TA_NE;
            *k = s;
            return -1;
    }
    int below = op == TA_LT || op == TA_LE;
    if (*k >= ta_hi[t]) return below;
    if (*k < ta_lo[t]) return !below;
    return -1;
}

static int ta_operands(const Janet *argv, int32_t first, ta_op op,
                       JanetVArrayView **xv, double *xs, JanetVArrayView **yv, double *ys,
                       JanetVArrayType *t, size_t *n) {
    *xv = ta_is_view(argv[first]);
    *yv = ta_is_view(argv[first + 1]);
    *xs = *ys = 0;
    if (!*xv && !*yv) janet_panic("expected at least one ta/view argument");
    if (*xv && *yv) {
        ta_same(*xv, *yv);
        *t = (*xv)->type;
        *n = (*xv)->size;
        return -1;
    }
    int left = !*xv;
    JanetVArrayView *v = left ? *yv : *xv;
    double *s = left ? xs : ys;
    *t = v->type;
    *n = v->size;
    *s = janet_getnumber(argv, first + !left);
    if (op == TA_ARITH) { ta_check_scalar(*t, *s); return -1; }
    if (*t >= JANET_VARRAY_TYPE_F32) return -1;
    return ta_threshold(*t, left ? ta_mirror[op] : op, *s, s);
}

// xv/yv     operand views, NULL on scalar operand
// xk/yk     cast scalar to element type
// xs/ys/os  input and output element pointers
// E/U       CT and UT bound as types and issigned/isfloat classify the row
#define MAP2_LOOPS(STMT) \
    if (xs && ys) { for (size_t i = 0; i < n; i++) { E x = xs[i], y = ys[i]; STMT; } } \
    else if (xs)  { for (size_t i = 0; i < n; i++) { E x = xs[i], y = yk;    STMT; } } \
    else          { for (size_t i = 0; i < n; i++) { E x = xk,    y = ys[i]; STMT; } }

#define MAP2_BIND(field, CT, UT) \
    typedef CT E; typedef UT U; (void) sizeof(U); \
    const int issigned = TA_ISSIGNED(E), isfloat = TA_ISFLOAT(E); \
    (void) issigned; (void) isfloat; \
    const E xk = (E) xscalar, yk = (E) yscalar; \
    const E *xs = xv ? xv->as.field : NULL; \
    const E *ys = yv ? yv->as.field : NULL;

#define MAP2_CASE(TAG, field, CT, UT, ACC, EXPR) \
    case JANET_VARRAY_TYPE_##TAG: { \
        MAP2_BIND(field, CT, UT) \
        E *os = out->as.field; \
        MAP2_LOOPS(os[i] = (E)(EXPR)) \
    } break;

#define CMP_CASE(TAG, field, CT, UT, ACC, EXPR) \
    case JANET_VARRAY_TYPE_##TAG: { \
        MAP2_BIND(field, CT, UT) \
        uint8_t *os = out->as.u8; \
        MAP2_LOOPS(os[i] = (EXPR) ? 1 : 0) \
    } break;

#define DEF_MAP2(name, ...) \
    static Janet cfun_ta_##name(int32_t argc, Janet *argv) { \
        janet_fixarity(argc, 2); \
        JanetVArrayView *xv, *yv; double xscalar, yscalar; \
        JanetVArrayType t; size_t n; \
        ta_operands(argv, 0, TA_ARITH, &xv, &xscalar, &yv, &yscalar, &t, &n); \
        JanetVArrayView *out = ta_new_view(t, n); \
        switch (t) { __VA_ARGS__ } \
        return janet_wrap_abstract(out); \
    }
#define DEF_CMP(name, OP, EXPR) \
    static Janet cfun_ta_##name(int32_t argc, Janet *argv) { \
        janet_fixarity(argc, 2); \
        JanetVArrayView *xv, *yv; double xscalar, yscalar; \
        JanetVArrayType t; size_t n; \
        int constant = ta_operands(argv, 0, OP, &xv, &xscalar, &yv, &yscalar, &t, &n); \
        JanetVArrayView *out = ta_new_view(JANET_VARRAY_TYPE_U8, n); \
        if (constant >= 0) memset(out->as.u8, constant, n); \
        else switch (t) { TA_EACH(CMP_CASE, EXPR) } \
        return janet_wrap_abstract(out); \
    }
#define TA_DIV(x, y) \
    ((!isfloat && (y) == 0) ? (E) 0 : \
     (!isfloat && issigned && (y) == (E) -1) ? (E)((U) 0 - (U)(x)) : (E)((x) / (y)))

DEF_MAP2(add, TA_EACH(MAP2_CASE, (U) x + (U) y))
DEF_MAP2(sub, TA_EACH(MAP2_CASE, (U) x - (U) y))
DEF_MAP2(mul, TA_EACH(MAP2_CASE, (U) x * (U) y))
DEF_MAP2(div, TA_EACH(MAP2_CASE, TA_DIV(x, y)))
DEF_MAP2(minimum, TA_EACH(MAP2_CASE, (x < y || TA_ISNAN(x)) ? x : y))
DEF_MAP2(maximum, TA_EACH(MAP2_CASE, (x > y || TA_ISNAN(x)) ? x : y))

DEF_CMP(lt, TA_LT, x < y)
DEF_CMP(le, TA_LE, x <= y)
DEF_CMP(gt, TA_GT, x > y)
DEF_CMP(ge, TA_GE, x >= y)
DEF_CMP(eq, TA_EQ, x == y)
DEF_CMP(neq, TA_NE, x != y)

#define MAP1_CASE(TAG, field, CT, UT, ACC, EXPR) \
    case JANET_VARRAY_TYPE_##TAG: { \
        typedef UT U; (void) sizeof(U); \
        const CT *xs = v->as.field; \
        CT *os = out->as.field; \
        for (size_t i = 0; i < n; i++) { CT x = xs[i]; (void) x; os[i] = (CT)(EXPR); } \
    } break;

#define DEF_MAP1(name, CASES) \
    static Janet cfun_ta_##name(int32_t argc, Janet *argv) { \
        janet_fixarity(argc, 1); \
        JanetVArrayView *v = janet_getvarray_any(argv, 0); \
        size_t n = v->size; \
        JanetVArrayView *out = ta_new_view(v->type, n); \
        switch (v->type) { \
            CASES \
            default: janet_panicf("%s: expected float view, got %s", #name, ta_type_names[v->type]); \
        } \
        return janet_wrap_abstract(out); \
    }

DEF_MAP1(neg, TA_EACH(MAP1_CASE, TA_ISFLOAT(U) ? -x : (U) 0 - (U) x))
DEF_MAP1(abs, TA_EACH(MAP1_CASE, x > 0 ? (U) x : (U) 0 - (U) x))
DEF_MAP1(reverse, TA_EACH(MAP1_CASE, xs[n - 1 - i]))
DEF_MAP1(sqrt, TA_EACH_FLOAT(MAP1_CASE, sqrt(x)))
DEF_MAP1(exp, TA_EACH_FLOAT(MAP1_CASE, exp(x)))
DEF_MAP1(log, TA_EACH_FLOAT(MAP1_CASE, log(x)))
DEF_MAP1(sin, TA_EACH_FLOAT(MAP1_CASE, sin(x)))
DEF_MAP1(cos, TA_EACH_FLOAT(MAP1_CASE, cos(x)))
DEF_MAP1(floor, TA_EACH_FLOAT(MAP1_CASE, floor(x)))
DEF_MAP1(ceil, TA_EACH_FLOAT(MAP1_CASE, ceil(x)))
DEF_MAP1(round, TA_EACH_FLOAT(MAP1_CASE, round(x)))

#define RUNNING_SUM__CASE(TAG, field, CT, UT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        typedef UT U; \
        const CT *xs = v->as.field; \
        CT *os = out->as.field; \
        U run = 0; \
        for (size_t i = 0; i < n; i++) { run = (U)(run + (U) xs[i]); os[i] = (CT) run; } \
    } break;

DEF_MAP1(running_sum, TA_EACH(RUNNING_SUM__CASE, _))

#define FOLD_CASE_INT(TAG, field, CT, UT, ACC, T) \
    case JANET_VARRAY_TYPE_##TAG: { \
        typedef ACC A; \
        const CT *xs = a->as.field, *ys = b ? b->as.field : xs; (void) ys; \
        uint64_t acc = 0; \
        for (size_t i = 0; i < n; i++) acc += T(i); \
        return janet_wrap_number((double)(A) acc); \
    }
#define FOLD_CASE_FLOAT(TAG, field, CT, UT, ACC, T) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = a->as.field, *ys = b ? b->as.field : xs; (void) ys; \
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0; \
        size_t i = 0; \
        for (; i + 4 <= n; i += 4) { a0 += T(i); a1 += T(i + 1); a2 += T(i + 2); a3 += T(i + 3); } \
        for (; i < n; i++) a0 += T(i); \
        return janet_wrap_number((a0 + a1) + (a2 + a3)); \
    }
#define SUM_I(i) ((uint64_t)(A) xs[i])
#define SUM_F(i) ((double) xs[i])
#define DOT_I(i) ((uint64_t)(A) xs[i] * (uint64_t)(A) ys[i])
#define DOT_F(i) ((double) xs[i] * ys[i])

#define DEF_FOLD(name, arity, TI, TF) \
    static Janet cfun_ta_##name(int32_t argc, Janet *argv) { \
        janet_fixarity(argc, arity); \
        JanetVArrayView *a = janet_getvarray_any(argv, 0), *b = NULL; \
        if (arity > 1) ta_same(a, b = janet_getvarray_any(argv, 1)); \
        size_t n = a->size; \
        switch (a->type) { \
            TA_EACH_INT(FOLD_CASE_INT, TI) \
            TA_EACH_FLOAT(FOLD_CASE_FLOAT, TF) \
        } \
        return janet_wrap_nil(); \
    }

DEF_FOLD(sum, 1, SUM_I, SUM_F)
DEF_FOLD(dot, 2, DOT_I, DOT_F)

#define RED_CASE(TAG, field, CT, UT, ACC, CMP) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = v->as.field; \
        CT b0 = xs[0], b1 = b0, b2 = b0, b3 = b0; \
        int nan = 0; \
        size_t i = 0; \
        for (; i + 4 <= n; i += 4) { \
            CT x0 = xs[i], x1 = xs[i + 1], x2 = xs[i + 2], x3 = xs[i + 3]; \
            b0 = x0 CMP b0 ? x0 : b0; \
            b1 = x1 CMP b1 ? x1 : b1; \
            b2 = x2 CMP b2 ? x2 : b2; \
            b3 = x3 CMP b3 ? x3 : b3; \
            nan |= TA_ISNAN(x0) | TA_ISNAN(x1) | TA_ISNAN(x2) | TA_ISNAN(x3); \
        } \
        for (; i < n; i++) { b0 = xs[i] CMP b0 ? xs[i] : b0; nan |= TA_ISNAN(xs[i]); } \
        b0 = b1 CMP b0 ? b1 : b0; \
        b2 = b3 CMP b2 ? b3 : b2; \
        CT best = b2 CMP b0 ? b2 : b0; \
        if (nan) { \
            if (!want_index) return janet_wrap_number(NAN); \
            while (!TA_ISNAN(xs[bi])) bi++; \
        } else if (want_index) { \
            while (xs[bi] != best) bi++; \
        } \
        if (!want_index) return TA_WRAP(CT, best); \
    } break;

/* min/max return element like indexing */
#define DEF_RED(name, CMP, WANT_INDEX) \
    static Janet cfun_ta_##name(int32_t argc, Janet *argv) { \
        janet_fixarity(argc, 1); \
        JanetVArrayView *v = janet_getvarray_any(argv, 0); \
        size_t n = v->size, bi = 0; \
        const int want_index = WANT_INDEX; \
        if (n == 0) janet_panic(#name ": empty view"); \
        switch (v->type) { TA_EACH(RED_CASE, CMP) } \
        return janet_wrap_number((double) bi); \
    }

DEF_RED(min, <, 0)
DEF_RED(max, >, 0)
DEF_RED(argmin, <, 1)
DEF_RED(argmax, >, 1)

#define GATHER_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = src->as.field; \
        CT *os = out->as.field; \
        for (size_t i = 0; i < count; i++) os[i] = xs[idx[i]]; \
    } break;
static Janet cfun_ta_gather(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetVArrayView *src = janet_getvarray_any(argv, 0);
    JanetVArrayView *iv = ta_get_u32(argv[1]);
    size_t count = iv->size;
    const uint32_t *idx = iv->as.u32;
    uint32_t top = 0;
    for (size_t i = 0; i < count; i++) top = idx[i] > top ? idx[i] : top;
    if (count && top >= src->size) janet_panic("gather index out of bounds");
    JanetVArrayView *out = ta_new_view(src->type, count);
    switch (src->type) { TA_EACH(GATHER_CASE, _) }
    return janet_wrap_abstract(out);
}

static Janet cfun_ta_where(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetVArrayView *mask = janet_getvarray_view(argv, 0, JANET_VARRAY_TYPE_U8);
    size_t n = mask->size;
    if (n > UINT32_MAX) janet_panic("where: view too large for uint32 indices");
    const uint8_t *ms = mask->as.u8;
    while (n && !ms[n - 1]) n--;
    JanetVArrayView *out = ta_new_view(JANET_VARRAY_TYPE_U32, ta_count(ms, n));
    uint32_t *os = out->as.u32;
    for (size_t i = 0, j = 0; i < n; i++) { os[j] = (uint32_t) i; j += ms[i] != 0; }
    return janet_wrap_abstract(out);
}

#define SELECT_CASE(TAG, field, CT, UT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        MAP2_BIND(field, CT, UT) \
        E *os = out->as.field; \
        MAP2_LOOPS(os[i] = ms[i] ? x : y) \
    } break;

static Janet cfun_ta_select(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);
    JanetVArrayView *mask = janet_getvarray_view(argv, 0, JANET_VARRAY_TYPE_U8);
    JanetVArrayView *xv, *yv; double xscalar, yscalar;
    JanetVArrayType t; size_t n;
    ta_operands(argv, 1, TA_ARITH, &xv, &xscalar, &yv, &yscalar, &t, &n);
    if (mask->size != n) janet_panic("mask size mismatch");
    const uint8_t *ms = mask->as.u8;
    JanetVArrayView *out = ta_new_view(t, n);
    switch (t) { TA_EACH(SELECT_CASE, _) }
    return janet_wrap_abstract(out);
}

#define COMPRESS_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = src->as.field; \
        CT *os = out->as.field; \
        for (size_t i = 0, j = 0; i < n; i++) { os[j] = xs[i]; j += ms[i] != 0; } \
    } break;

static Janet cfun_ta_compress(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetVArrayView *src = janet_getvarray_any(argv, 0);
    JanetVArrayView *mask = janet_getvarray_view(argv, 1, JANET_VARRAY_TYPE_U8);
    if (mask->size != src->size) janet_panic("mask size mismatch");
    size_t n = src->size;
    const uint8_t *ms = mask->as.u8;
    while (n && !ms[n - 1]) n--;
    JanetVArrayView *out = ta_new_view(src->type, ta_count(ms, n));
    switch (src->type) { TA_EACH(COMPRESS_CASE, _) }
    return janet_wrap_abstract(out);
}

typedef struct { uint64_t k; uint32_t i; } ta_grade_pair;

/* for grade, for keys unsigned order on the key is the sort order
 Signed rows flip
 send every NaN to the top key so NaN sorts last, and fold -0 into +0 since the two compare equal */
static inline uint64_t ta_key_f64(double d) {
    if (d != d) return UINT64_MAX;
    d += 0.0;
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b >> 63 ? ~b : b | (UINT64_C(1) << 63);
}

static inline uint64_t ta_key_f32(float f) {
    if (f != f) return UINT32_MAX;
    f += 0.0f;
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b >> 31 ? (uint32_t) ~b : b | (UINT32_C(1) << 31);
}

#define GRADE_KEY(CT, x) \
    (TA_ISFLOAT(CT) ? (sizeof(CT) == 8 ? ta_key_f64((double)(x)) : ta_key_f32((float)(x))) \
     : (((uint64_t)(x) & (~UINT64_C(0) >> (64 - 8 * sizeof(CT)))) \
        ^ (TA_ISSIGNED(CT) ? UINT64_C(1) << (8 * sizeof(CT) - 1) : 0)))

static const ta_grade_pair *ta_radix(ta_grade_pair *a, ta_grade_pair *b, size_t n, int bytes) {
    size_t count[8][256] = {{0}};
    for (size_t i = 0; i < n; i++)
        for (int p = 0; p < bytes; p++) count[p][(a[i].k >> (8 * p)) & 255]++;
    for (int p = 0; p < bytes; p++) {
        size_t *c = count[p];
        if (c[(a[0].k >> (8 * p)) & 255] == n) continue;
        for (size_t d = 0, sum = 0; d < 256; d++) { size_t m = c[d]; c[d] = sum; sum += m; }
        for (size_t i = 0; i < n; i++) b[c[(a[i].k >> (8 * p)) & 255]++] = a[i];
        ta_grade_pair *t = a; a = b; b = t;
    }
    return a;
}

#define GRADE_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = v->as.field; \
        const uint64_t top = ~UINT64_C(0) >> (64 - 8 * sizeof(CT)); \
        for (size_t i = 0; i < n; i++) { \
            uint64_t k = GRADE_KEY(CT, xs[i]); \
            ps[i].k = desc && !TA_ISNAN(xs[i]) ? top ^ k : k; \
            ps[i].i = (uint32_t) i; \
        } \
        bytes = sizeof(CT); \
    } break;

static Janet cfun_ta_grade(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    JanetVArrayView *v = janet_getvarray_any(argv, 0);
    int desc = 0;
    if (argc > 1) {
        const uint8_t *dir = janet_getkeyword(argv, 1);
        if (!janet_cstrcmp(dir, "desc")) desc = 1;
        else if (janet_cstrcmp(dir, "asc")) janet_panicf("grade: expected :asc or :desc, got %v", argv[1]);
    }
    size_t n = v->size;
    if (n > UINT32_MAX) janet_panic("grade: view too large for uint32 indices");
    if (n > SIZE_MAX / (2 * sizeof(ta_grade_pair))) janet_panic("grade: view too large");
    JanetVArrayView *out = ta_new_view(JANET_VARRAY_TYPE_U32, n);
    if (n == 0) return janet_wrap_abstract(out);
    ta_grade_pair *ps = janet_smalloc(2 * n * sizeof(*ps));
    int bytes = 0;
    switch (v->type) { TA_EACH(GRADE_CASE, _) }
    const ta_grade_pair *sorted = ta_radix(ps, ps + n, n, bytes);
    uint32_t *os = out->as.u32;
    for (size_t i = 0; i < n; i++) os[i] = sorted[i].i;
    janet_sfree(ps);
    return janet_wrap_abstract(out);
}

#define TA_BEFORE(x, t) ((x) < (t) || (TA_ISNAN(t) && !TA_ISNAN(x)))
#define SEARCH_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = v->as.field, *ts = tv ? tv->as.field : NULL; \
        for (size_t j = 0; j < m; j++) { \
            size_t lo = 0, hi = n; \
            while (lo < hi) { \
                size_t mid = lo + (hi - lo) / 2; \
                if (ts ? TA_BEFORE(xs[mid], ts[j]) : TA_BEFORE((double) xs[mid], scalar)) lo = mid + 1; \
                else hi = mid; \
            } \
            os[j] = (uint32_t) lo; \
        } \
    } break;
static Janet cfun_ta_lower_bound(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetVArrayView *v = janet_getvarray_any(argv, 0);
    JanetVArrayView *tv = ta_is_view(argv[1]), *out = NULL;
    size_t n = v->size, m = 1;
    if (n > UINT32_MAX) janet_panic("lower-bound: view too large for uint32 indices");
    double scalar = 0;
    uint32_t first = 0;
    uint32_t *os = &first;
    if (tv) {
        if (tv->type != v->type)
            janet_panicf("lower-bound: targets are %s but view is %s",
                         ta_type_names[tv->type], ta_type_names[v->type]);
        m = tv->size;
        out = ta_new_view(JANET_VARRAY_TYPE_U32, m);
        os = out->as.u32;
    } else {
        scalar = janet_getnumber(argv, 1);
    }
    switch (v->type) { TA_EACH(SEARCH_CASE, _) }
    return out ? janet_wrap_abstract(out) : janet_wrap_number((double) first);
}

#define CAST_INNER(TAG2, field2, CT2) \
    case JANET_VARRAY_TYPE_##TAG2: { \
        CT2 *os = out->as.field2; \
        if (from_float && !TA_ISFLOAT(CT2)) { \
            for (size_t i = 0; i < n; i++) { \
                double d = trunc((double) xs[i]); \
                if (!(d >= ta_lo[out->type] && d < ta_hi[out->type])) \
                    janet_panicf("cast: %v is not representable as %s", \
                                 janet_wrap_number((double) xs[i]), ta_type_names[out->type]); \
                os[i] = (CT2) d; \
            } \
        } else { \
            for (size_t i = 0; i < n; i++) os[i] = (CT2) xs[i]; \
        } \
    } break;

#define CAST_OUTER(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        const CT *xs = src->as.field; \
        const int from_float = TA_ISFLOAT(CT); \
        switch (out->type) { TA_EACH_DST(CAST_INNER) } \
    } break;

static Janet cfun_ta_cast(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetVArrayView *src = janet_getvarray_any(argv, 0);
    JanetVArrayType dst = ta_type_by_name(janet_getkeyword(argv, 1));
    size_t n = src->size;
    JanetVArrayView *out = ta_new_view(dst, n);
    switch (src->type) { TA_EACH(CAST_OUTER, _) }
    return janet_wrap_abstract(out);
}

#define RANGE_CASE(TAG, field, CT, ...) \
    case JANET_VARRAY_TYPE_##TAG: { \
        CT *os = out->as.field; \
        for (size_t i = 0; i < n; i++) os[i] = (CT)(start + step * (double) i); \
    } break;

static Janet cfun_ta_range(int32_t argc, Janet *argv) {
    janet_arity(argc, 2, 4);
    JanetVArrayType t = ta_type_by_name(janet_getkeyword(argv, 0));
    double start = argc > 2 ? janet_getnumber(argv, 1) : 0;
    double end = janet_getnumber(argv, argc > 2 ? 2 : 1);
    double step = argc > 3 ? janet_getnumber(argv, 3) : 1;
    if (step == 0 || !isfinite(step)) janet_panic("range: step must be finite and nonzero");
    if (t < JANET_VARRAY_TYPE_F32) {
        ta_check_scalar(t, start);
        if (step != floor(step)) janet_panicf("range: step %v is not an integer", argv[3]);
    }
    double span = ceil((end - start) / step);
    if (!(span < 9007199254740992.0)) janet_panic("range: bounds must be finite and span fewer than 2^53 elements");
    size_t n = span > 0 ? (size_t) span : 0;
    while (n && !(step > 0 ? start + step * (double)(n - 1) < end : start + step * (double)(n - 1) > end)) n--;
    if (t < JANET_VARRAY_TYPE_F32 && n > 0) {
        double last = start + step * (double)(n - 1);
        if (!(last >= ta_lo[t] && last < ta_hi[t]))
            janet_panicf("range: values do not fit in %s", ta_type_names[t]);
    }
    JanetVArrayView *out = ta_new_view(t, n);
    switch (t) { TA_EACH(RANGE_CASE, _) }
    return janet_wrap_abstract(out);
}

static Janet cfun_ta_from(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    return janet_wrap_abstract(ta_from_indexed(ta_type_by_name(janet_getkeyword(argv, 0)), argv[1]));
}

static const JanetReg ta_cfuns[] = {
    {"new", cfun_ta_new, "(varray/new type size &opt offset v|buffer)\n\n"
        "Create view of size elements. Offset counts elements of given view, bytes of given buffer"},
    {"buffer", cfun_ta_buffer, "(varray/buffer v|size)\n\nReturn view's buffer, or create zeroed buffer of size bytes"},
    {"length", cfun_ta_length, "(varray/length v|buffer)\n\nReturn element count of view or byte size of buffer"},
    {"properties", cfun_ta_properties, "(varray/properties v|buffer)\n\nReturn view or buffer properties as struct"},
    {"copy-bytes", cfun_ta_copy_bytes, "(varray/copy-bytes src sindex dest dindex &opt count)\n\n"
        "Copy count elements (default 1) of src from index sindex to dest at index dindex"},
    {"swap-bytes", cfun_ta_swap_bytes, "(varray/swap-bytes src sindex dest dindex &opt count)\n\n"
        "Swap count elements (default 1) between src from index sindex and dest at index dindex"},
    {"slice", cfun_ta_slice, "(varray/slice v &opt start end)\n\n"
        "Return elements from start to end as Janet array. Negative indices count from end"},
    {"add", cfun_ta_add, "(varray/add v w)\n\nAdd v and w at each index, wrapping ints. Either may be a scalar"},
    {"sub", cfun_ta_sub, "(varray/sub v w)\n\nSubtract w from v at each index, wrapping ints. Either may be a scalar"},
    {"mul", cfun_ta_mul, "(varray/mul v w)\n\nMultiply v and w at each index, wrapping ints. Either may be a scalar"},
    {"div", cfun_ta_div, "(varray/div v w)\n\nDivide v by w at each index. Integer division truncates toward zero and gives 0 on division by zero"},
    {"minimum", cfun_ta_minimum, "(varray/minimum v w)\n\nTake smaller of v and w at each index, propagating NaN"},
    {"maximum", cfun_ta_maximum, "(varray/maximum v w)\n\nTake larger of v and w at each index, propagating NaN"},
    {"neg", cfun_ta_neg, "(varray/neg v)\n\nNegate each element (modular for integer types)"},
    {"abs", cfun_ta_abs, "(varray/abs v)\n\nTake absolute value of each element (modular for integer types)"},
    {"sqrt", cfun_ta_sqrt, "(varray/sqrt float-view)\n\nTake square root of each element"},
    {"exp", cfun_ta_exp, "(varray/exp float-view)\n\nExponentiate each element"},
    {"log", cfun_ta_log, "(varray/log float-view)\n\nTake natural log of each element"},
    {"sin", cfun_ta_sin, "(varray/sin float-view)\n\nTake sine of each element"},
    {"cos", cfun_ta_cos, "(varray/cos float-view)\n\nTake cosine of each element"},
    {"floor", cfun_ta_floor, "(varray/floor float-view)\n\nFloor each element"},
    {"ceil", cfun_ta_ceil, "(varray/ceil float-view)\n\nCeil each element"},
    {"round", cfun_ta_round, "(varray/round float-view)\n\nRound each element, halves away from zero"},
    {"lt", cfun_ta_lt, "(varray/lt v w)\n\nMark where v < w with mask"},
    {"le", cfun_ta_le, "(varray/le v w)\n\nMark where v <= w with mask"},
    {"gt", cfun_ta_gt, "(varray/gt v w)\n\nMark where v > w with mask"},
    {"ge", cfun_ta_ge, "(varray/ge v w)\n\nMark where v >= w with mask"},
    {"eq", cfun_ta_eq, "(varray/eq v w)\n\nMark where v = w with mask"},
    {"neq", cfun_ta_neq, "(varray/neq v w)\n\nMark where v != w with mask"},
    {"sum", cfun_ta_sum, "(varray/sum v)\n\nSum all elements. Integer sums wrap at 64 bits and return through double, exact below 2^53"},
    {"min", cfun_ta_min, "(varray/min v)\n\nReduce to smallest element, returned like index, propagating NaN, panicking on empty view"},
    {"max", cfun_ta_max, "(varray/max v)\n\nReduce to largest element, returned like index, propagating NaN, panicking on empty view"},
    {"argmin", cfun_ta_argmin, "(varray/argmin v)\n\nReturn index of smallest element or first NaN, panicking on empty view"},
    {"argmax", cfun_ta_argmax, "(varray/argmax v)\n\nReturn index of largest element or first NaN, panicking on empty view"},
    {"dot", cfun_ta_dot, "(varray/dot v w)\n\nCompute dot product. Integer products accumulate mod 2^64 and return through double, exact below 2^53"},
    {"running-sum", cfun_ta_running_sum, "(varray/running-sum v)\n\nCompute running sum, keeping element type, wrapping ints"},
    {"gather", cfun_ta_gather, "(varray/gather v indices)\n\nBuild new view of v's elements at given indices"},
    {"reverse", cfun_ta_reverse, "(varray/reverse v)\n\nReturn new view elements in reverse order"},
    {"where", cfun_ta_where, "(varray/where mask)\n\nReturn indices where mask is nonzero"},
    {"select", cfun_ta_select, "(varray/select mask v w)\n\nTake v where mask is nonzero and w elsewhere"},
    {"compress", cfun_ta_compress, "(varray/compress v mask)\n\nKeep elements of v where mask is nonzero"},
    {"grade", cfun_ta_grade, "(varray/grade v &opt dir)\n\nArgsort: return indices that stably sort v, "
        "ascending by default or descending with :desc, NaN last. "
        "Compose with gather to sort, or gather other columns to co-sort"},
    {"lower-bound", cfun_ta_lower_bound, "(varray/lower-bound sorted-view x)\n\nFind lower-bound insertion index "
        "of x in ascending sorted-view via binary search"},
    {"cast", cfun_ta_cast, "(varray/cast v type)\n\nConvert view to another element type, e.g. (varray/cast v :float64). "
        "Integer to integer wraps; float to integer truncates toward zero and panics if value does not fit"},
    {"range", cfun_ta_range, "(varray/range type end)\n(varray/range type start end &opt step)"},
    {"from", cfun_ta_from, "(varray/from type indexed)\n\nCreate view of given type from Janet array or tuple of numbers"},
    {NULL, NULL, NULL}
};

static JanetMethod ta_view_methods[] = {
    {"length", cfun_ta_length},
    {"properties", cfun_ta_properties},
    {"copy-bytes", cfun_ta_copy_bytes},
    {"swap-bytes", cfun_ta_swap_bytes},
    {"slice", cfun_ta_slice},
    {NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
    janet_cfuns(env, "varray", ta_cfuns);
    janet_register_abstract_type(&janet_ta_buffer_type);
    janet_register_abstract_type(&janet_ta_view_type);
}