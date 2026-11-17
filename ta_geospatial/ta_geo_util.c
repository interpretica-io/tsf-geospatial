/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: strings, options, errors, random numbers
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_geo_int.h"

/* ---- growing string --------------------------------------------- */

static bool
sb_grow(sbuf *b, size_t extra)
{
    size_t need;
    char *p;

    if (b->oom)
        return false;
    need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    if (b->cap == 0)
        b->cap = 256;
    while (b->cap < need)
        b->cap *= 2;
    p = realloc(b->p, b->cap);
    if (p == NULL)
    {
        b->oom = true;
        return false;
    }
    b->p = p;
    return true;
}

void
sb_vprintf(sbuf *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;

    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0 || !sb_grow(b, (size_t)n))
        return;
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void
sb_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void
sb_put(sbuf *b, const char *s, size_t n)
{
    if (!sb_grow(b, n))
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void
sb_puts(sbuf *b, const char *s)
{
    if (s != NULL)
        sb_put(b, s, strlen(s));
}

void
sb_field(sbuf *b, const char *s)
{
    const char *p;

    if (s == NULL)
        return;
    for (p = s; *p != '\0'; p++)
    {
        switch (*p)
        {
            case '%': sb_puts(b, "%25"); break;
            case '\t': sb_puts(b, "%09"); break;
            case '\n': sb_puts(b, "%0A"); break;
            case '\r': sb_puts(b, "%0D"); break;
            default: sb_put(b, p, 1); break;
        }
    }
}

void
sb_num(sbuf *b, double v)
{
    if (isnan(v))
        sb_puts(b, "nan");
    else if (isinf(v))
        sb_puts(b, v > 0 ? "inf" : "-inf");
    else if (v == floor(v) && fabs(v) < 1e15)
        sb_printf(b, "%.0f", v);
    else
        sb_printf(b, "%.17g", v);
}

void
sb_kv_num(sbuf *b, const char *key, double v)
{
    sb_printf(b, "%s=", key);
    sb_num(b, v);
    sb_puts(b, "\n");
}

void
sb_kv_int(sbuf *b, const char *key, long long v)
{
    sb_printf(b, "%s=%lld\n", key, v);
}

void
sb_kv_str(sbuf *b, const char *key, const char *v)
{
    const char *p;

    sb_printf(b, "%s=", key);
    for (p = v != NULL ? v : ""; *p != '\0'; p++)
        sb_put(b, (*p == '\n' || *p == '\r') ? " " : p, 1);
    sb_puts(b, "\n");
}

char *
sb_take(sbuf *b)
{
    char *p;

    if (b->oom || b->p == NULL)
    {
        free(b->p);
        p = strdup("");
    }
    else
    {
        p = b->p;
    }
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
    return p;
}

void
sb_free(sbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

/* ---- errors ------------------------------------------------------ */

int
seterr(char **err, int rc, const char *fmt, ...)
{
    va_list ap;
    sbuf b = SBUF_INIT;

    if (err == NULL || *err != NULL)
        return rc;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    *err = sb_take(&b);
    return rc;
}

/* ---- options ---------------------------------------------------- */

int
kv_parse(const char *text, kv *out, char **err)
{
    const char *p = text != NULL ? text : "";

    out->v = NULL;
    out->n = 0;
    while (*p != '\0')
    {
        const char *eol = strchr(p, '\n');
        const char *eq;
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        kv_pair *nv;

        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len == 0 || p[0] == '#')
            goto next;
        eq = memchr(p, '=', len);
        if (eq == NULL || eq == p)
        {
            kv_free(out);
            return seterr(err, TA_GEO_EINVAL,
                          "option line without key=value: '%.*s'",
                          (int)(len > 60 ? 60 : len), p);
        }
        nv = realloc(out->v, (out->n + 1) * sizeof(*nv));
        if (nv == NULL)
        {
            kv_free(out);
            return seterr(err, TA_GEO_ENOMEM, "out of memory");
        }
        out->v = nv;
        nv[out->n].key = strndup(p, (size_t)(eq - p));
        nv[out->n].val = strndup(eq + 1, len - (size_t)(eq - p) - 1);
        nv[out->n].used = false;
        out->n++;
next:
        if (eol == NULL)
            break;
        p = eol + 1;
    }
    return TA_GEO_OK;
}

void
kv_free(kv *k)
{
    size_t i;

    for (i = 0; i < k->n; i++)
    {
        free(k->v[i].key);
        free(k->v[i].val);
    }
    free(k->v);
    k->v = NULL;
    k->n = 0;
}

const char *
kv_get(kv *k, const char *key)
{
    size_t i;
    const char *v = NULL;

    if (k == NULL)
        return NULL;
    /* The last occurrence wins, as with command-line options. */
    for (i = 0; i < k->n; i++)
    {
        if (strcmp(k->v[i].key, key) == 0)
        {
            k->v[i].used = true;
            v = k->v[i].val;
        }
    }
    return v;
}

const char *
kv_str(kv *k, const char *key, const char *def)
{
    const char *v = kv_get(k, key);

    return v != NULL ? v : def;
}

int
kv_dbl(kv *k, const char *key, double def, double *out, char **err)
{
    const char *v = kv_get(k, key);
    char *end;

    if (v == NULL || *v == '\0')
    {
        *out = def;
        return TA_GEO_OK;
    }
    errno = 0;
    *out = strtod(v, &end);
    if (errno != 0 || *end != '\0')
        return seterr(err, TA_GEO_EINVAL, "option %s: not a number: '%s'",
                      key, v);
    return TA_GEO_OK;
}

int
kv_long(kv *k, const char *key, long long def, long long *out, char **err)
{
    const char *v = kv_get(k, key);
    char *end;
    double mult = 1;
    double d;

    if (v == NULL || *v == '\0')
    {
        *out = def;
        return TA_GEO_OK;
    }
    errno = 0;
    d = strtod(v, &end);
    if (*end == 'k' || *end == 'K')
        mult = 1e3, end++;
    else if (*end == 'm' || *end == 'M')
        mult = 1e6, end++;
    if (errno != 0 || *end != '\0' || d * mult != floor(d * mult) ||
        fabs(d * mult) > 9e18)
        return seterr(err, TA_GEO_EINVAL, "option %s: not an integer: '%s'",
                      key, v);
    *out = (long long)(d * mult);
    return TA_GEO_OK;
}

int
kv_bool(kv *k, const char *key, bool def, bool *out, char **err)
{
    const char *v = kv_get(k, key);

    if (v == NULL || *v == '\0')
        *out = def;
    else if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
             strcmp(v, "yes") == 0)
        *out = true;
    else if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
             strcmp(v, "no") == 0)
        *out = false;
    else
        return seterr(err, TA_GEO_EINVAL, "option %s: not a boolean: '%s'",
                      key, v);
    return TA_GEO_OK;
}

int
kv_unused(kv *k, char **err)
{
    size_t i;

    for (i = 0; i < k->n; i++)
    {
        if (!k->v[i].used)
            return seterr(err, TA_GEO_EINVAL, "unknown option '%s'",
                          k->v[i].key);
    }
    return TA_GEO_OK;
}

/* ---- random numbers ---------------------------------------------- */

static uint64_t
splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void
geo_rng_seed(geo_rng *r, uint64_t seed)
{
    int i;

    for (i = 0; i < 4; i++)
        r->s[i] = splitmix64(&seed);
}

static inline uint64_t
rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

uint64_t
geo_rng_next(geo_rng *r)
{
    uint64_t *s = r->s;
    uint64_t res = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return res;
}

double
geo_rng_unit(geo_rng *r)
{
    return (double)(geo_rng_next(r) >> 11) * 0x1.0p-53;
}

double
geo_rng_range(geo_rng *r, double lo, double hi)
{
    return lo + (hi - lo) * geo_rng_unit(r);
}

double
geo_rng_normal(geo_rng *r)
{
    double u1 = geo_rng_unit(r);
    double u2 = geo_rng_unit(r);

    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---- hashing ------------------------------------------------------ */

uint64_t
geo_hash(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;

    for (i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int
hmap_grow(geo_hmap *m)
{
    geo_hmap n;
    size_t i;

    n.cap = m->cap == 0 ? 1024 : m->cap * 2;
    n.n = 0;
    n.keys = calloc(n.cap, sizeof(*n.keys));
    n.vals = calloc(n.cap, sizeof(*n.vals));
    if (n.keys == NULL || n.vals == NULL)
    {
        free(n.keys);
        free(n.vals);
        return -1;
    }
    for (i = 0; i < m->cap; i++)
    {
        if (m->keys[i] != 0)
            geo_hmap_put(&n, m->keys[i], m->vals[i], NULL);
    }
    free(m->keys);
    free(m->vals);
    *m = n;
    return 0;
}

int
geo_hmap_put(geo_hmap *m, uint64_t key, int64_t val, int64_t *old)
{
    size_t i;

    if (key == 0)
        key = 1;
    if ((m->n + 1) * 2 > m->cap && hmap_grow(m) != 0)
        return -1;
    for (i = key & (m->cap - 1); m->keys[i] != 0; i = (i + 1) & (m->cap - 1))
    {
        if (m->keys[i] == key)
        {
            if (old != NULL)
                *old = m->vals[i];
            return 0;
        }
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->n++;
    return 1;
}

bool
geo_hmap_get(const geo_hmap *m, uint64_t key, int64_t *val)
{
    size_t i;

    if (m->cap == 0)
        return false;
    if (key == 0)
        key = 1;
    for (i = key & (m->cap - 1); m->keys[i] != 0; i = (i + 1) & (m->cap - 1))
    {
        if (m->keys[i] == key)
        {
            if (val != NULL)
                *val = m->vals[i];
            return true;
        }
    }
    return false;
}

void
geo_hmap_free(geo_hmap *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof(*m));
}

double
geo_us_since(uint64_t t0)
{
    return (double)(geo_now_ns() - t0) / 1000.0;
}
