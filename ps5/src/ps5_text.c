/*
 * [ps5 port] Text changes: "Xbox" to "PS5", START to OPTIONS, a credit line.
 *
 * The game's text is in assets/misc.viv: a BIG4 archive whose .loc entries
 * (RefPack-compressed) hold every string in UTF-16. At start-up this reads the
 * player's own misc.viv from GameData, applies the rules in text/replace.txt to
 * every .loc entry, and writes the result to cache/misc.viv; the path hook then
 * hands the game that copy whenever it opens assets/misc.viv. Nothing in
 * GameData is changed. Changed entries are stored again at the end of the
 * archive (uncompressed RefPack, which every RefPack reader takes) and the
 * directory points at them; everything else keeps its bytes and offsets.
 *
 * replace.txt: "old => new" per line, matched exactly (case counts) inside
 * every string, in file order. [always] rules apply always; [playstation]
 * rules only with the PlayStation button style. Lines starting with // are
 * notes. In the game's text # starts a new line.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct { char *from, *to; } Rule;

static Rule *s_rules;
static int s_rule_count;
static char s_original[1024], s_patched[1024];

/* ---- RefPack ------------------------------------------------------------------------------------- */

static uint8_t *refpack_decode(const uint8_t *src, size_t n, size_t *out_len)
{
    size_t pos = 2, size, have = 0;
    unsigned sig, step;
    uint8_t *out;

    if (n < 5)
        return NULL;
    sig = (unsigned)src[0] << 8 | src[1];
    if ((sig & 0x3EFF) != 0x10FB)
        return NULL;
    step = (sig & 0x8000) ? 4 : 3;
    if (sig & 0x0100)
        pos += step;
    if (pos + step > n)
        return NULL;
    size = 0;
    for (unsigned i = 0; i < step; i++)
        size = size << 8 | src[pos + i];
    pos += step;
    if (size > 64u << 20)
        return NULL;
    out = malloc(size ? size : 1);
    if (!out)
        return NULL;
    while (pos < n) {
        unsigned b0 = src[pos++], run, length = 0, dist = 0;
        if (b0 < 0x80) {
            if (pos >= n) break;
            unsigned b1 = src[pos++];
            run = b0 & 3;
            length = ((b0 & 0x1C) >> 2) + 3;
            dist = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            if (pos + 2 > n) break;
            unsigned b1 = src[pos], b2 = src[pos + 1];
            pos += 2;
            run = (b1 >> 6) & 3;
            length = (b0 & 0x3F) + 4;
            dist = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            if (pos + 3 > n) break;
            unsigned b1 = src[pos], b2 = src[pos + 1], b3 = src[pos + 2];
            pos += 3;
            run = b0 & 3;
            length = ((b0 & 0x0C) << 6) + b3 + 5;
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            run = ((b0 & 0x1F) << 2) + 4;
        } else {
            run = b0 & 3;
        }
        if (pos + run > n || have + run + length > size || dist > have + run) {
            free(out);
            return NULL;
        }
        memcpy(out + have, src + pos, run);
        pos += run;
        have += run;
        for (unsigned i = 0; i < length; i++, have++)
            out[have] = out[have - dist];
        if (b0 >= 0xFC)
            break;
    }
    if (have != size) {
        free(out);
        return NULL;
    }
    *out_len = size;
    return out;
}

/* Stored, not compressed: literal blocks of up to 112 bytes and an end block. */
static uint8_t *refpack_store(const uint8_t *src, size_t n, size_t *out_len)
{
    size_t cap = 5 + n + n / 112 + 2, pos = 0, at = 0;
    uint8_t *out;

    if (n > 0xFFFFFF)
        return NULL;
    out = malloc(cap);
    if (!out)
        return NULL;
    out[pos++] = 0x10;
    out[pos++] = 0xFB;
    out[pos++] = (uint8_t)(n >> 16);
    out[pos++] = (uint8_t)(n >> 8);
    out[pos++] = (uint8_t)n;
    while (n - at >= 4) {
        size_t take = n - at;
        if (take > 112)
            take = 112;
        take &= ~(size_t)3;
        out[pos++] = (uint8_t)(0xE0 | ((take >> 2) - 1));
        memcpy(out + pos, src + at, take);
        pos += take;
        at += take;
    }
    out[pos++] = (uint8_t)(0xFC | (n - at));
    memcpy(out + pos, src + at, n - at);
    pos += n - at;
    *out_len = pos;
    return out;
}

/* ---- UTF-16 <-> UTF-8 ------------------------------------------------------------------------- */

static size_t utf16_to_utf8(const uint8_t *p, size_t units, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = p[i * 2] | (uint32_t)p[i * 2 + 1] << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            uint32_t d = p[i * 2 + 2] | (uint32_t)p[i * 2 + 3] << 8;
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | c >> 18);
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

/* Returns the number of UTF-16 units written to out (no terminator). */
static size_t utf8_to_utf16(const char *s, uint8_t *out)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) {
            c = (c & 0x07) << 18 | (uint32_t)(p[0] & 0x3F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            p += 3;
        } else if (c >= 0xE0 && p[0] && p[1]) {
            c = (c & 0x0F) << 12 | (uint32_t)(p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = (c & 0x1F) << 6 | (p[0] & 0x3F);
            p += 1;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            out[o * 2] = (uint8_t)hi; out[o * 2 + 1] = (uint8_t)(hi >> 8); o++;
            out[o * 2] = (uint8_t)lo; out[o * 2 + 1] = (uint8_t)(lo >> 8); o++;
        } else {
            out[o * 2] = (uint8_t)c; out[o * 2 + 1] = (uint8_t)(c >> 8); o++;
        }
    }
    return o;
}

/* ---- the rules -------------------------------------------------------------------------------- */

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static int read_rules(const char *path, int playstation)
{
    FILE *f = fopen(path, "rb");
    char line[2048];
    int on = 1, cap = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *arrow;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s = trim(s + 3);                                   /* a BOM from a Windows editor */
        if (!*s || (s[0] == '/' && s[1] == '/'))
            continue;
        if (s[0] == '[') {
            on = !strncasecmp(s, "[always]", 8) || (playstation && !strncasecmp(s, "[playstation]", 13));
            continue;
        }
        if (!on || !(arrow = strstr(s, "=>")))
            continue;
        *arrow = 0;
        {
            char *from = trim(s), *to = trim(arrow + 2);
            if (!*from)
                continue;
            if (s_rule_count == cap) {
                Rule *grown;
                cap = cap ? cap * 2 : 16;
                grown = realloc(s_rules, (size_t)cap * sizeof *grown);
                if (!grown)
                    break;
                s_rules = grown;
            }
            s_rules[s_rule_count].from = strdup(from);
            s_rules[s_rule_count].to = strdup(to);
            s_rule_count++;
        }
    }
    fclose(f);
    return s_rule_count;
}

/* Apply every rule to s (UTF-8). Returns a new string when something changed, else NULL. */
static char *apply_rules(const char *s)
{
    char *cur = NULL;
    for (int r = 0; r < s_rule_count; r++) {
        const char *src = cur ? cur : s, *hit = strstr(src, s_rules[r].from);
        size_t fl, tl, count = 0, len;
        char *out, *o;
        if (!hit)
            continue;
        fl = strlen(s_rules[r].from);
        tl = strlen(s_rules[r].to);
        for (const char *p = hit; p; p = strstr(p + fl, s_rules[r].from))
            count++;
        len = strlen(src) + count * (tl > fl ? tl - fl : 0) + 1;
        out = malloc(len);
        if (!out)
            return cur;
        o = out;
        for (const char *p = src;;) {
            const char *h = strstr(p, s_rules[r].from);
            if (!h) {
                strcpy(o, p);
                break;
            }
            memcpy(o, p, (size_t)(h - p));
            o += h - p;
            memcpy(o, s_rules[r].to, tl);
            o += tl;
            p = h + fl;
        }
        free(cur);
        cur = out;
    }
    return cur;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* A .loc with the rules applied, or NULL when nothing changed (or it is not one). */
static uint8_t *patch_loc(const uint8_t *d, size_t n, size_t *out_len, int *changed)
{
    uint32_t locl, count, chunk;
    const uint8_t *offs;
    uint8_t *out, *strings;
    size_t head, at, cap;
    char *utf8 = NULL;
    uint8_t *utf16 = NULL;

    *changed = 0;
    if (n < 20 || memcmp(d, "LOCH", 4))
        return NULL;
    locl = rd32(d + 16);
    if ((size_t)locl + 16 > n || memcmp(d + locl, "LOCL", 4))
        return NULL;
    chunk = rd32(d + locl + 4);
    count = rd32(d + locl + 12);
    if ((size_t)locl + chunk > n || 16 + (size_t)count * 4 > chunk)
        return NULL;
    offs = d + locl + 16;
    head = 16 + (size_t)count * 4;
    cap = n * 2 + 4096;
    out = malloc(locl + cap);
    utf8 = malloc(64 * 1024);
    utf16 = malloc(64 * 1024);
    if (!out || !utf8 || !utf16)
        goto fail;
    memcpy(out, d, locl + 16);
    strings = out + locl;
    at = head;
    for (uint32_t i = 0; i < count; i++) {
        size_t o = rd32(offs + i * 4), units = 0;
        const uint8_t *p = d + locl + o;
        char *patched;
        if (locl + o >= n)
            goto fail;
        while (locl + o + units * 2 + 1 < n && (p[units * 2] || p[units * 2 + 1]))
            units++;
        if (units > 8000)
            goto fail;
        utf16_to_utf8(p, units, utf8);
        patched = apply_rules(utf8);
        if (patched) {
            if (strlen(patched) > 15000) {
                free(patched);
                goto fail;
            }
            units = utf8_to_utf16(patched, utf16);
            p = utf16;
            free(patched);
            (*changed)++;
        }
        if (at + units * 2 + 2 > cap)
            goto fail;
        wr32(strings + 16 + i * 4, (uint32_t)at);
        memcpy(strings + at, p, units * 2);
        strings[at + units * 2] = strings[at + units * 2 + 1] = 0;
        at += units * 2 + 2;
    }
    while (at & 3)
        strings[at++] = 0;
    wr32(strings + 4, (uint32_t)at);
    free(utf8);
    free(utf16);
    if (!*changed) {
        free(out);
        return NULL;
    }
    *out_len = locl + at;
    return out;
fail:
    free(out);
    free(utf8);
    free(utf16);
    *changed = 0;
    return NULL;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 0 && size < (64L << 20) ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *n = (size_t)size;
    return data;
}

static void serve_patched(char *host_path, size_t size)
{
    if (s_patched[0] && !strcasecmp(host_path, s_original) && strlen(s_patched) < size)
        snprintf(host_path, size, "%s", s_patched);
}

/* Build cache/misc.viv and serve it. playstation: also the [playstation] rules. */
void ps5_text_prepare(int playstation)
{
    extern const char *ps5_root(void);
    extern const char *ps5_game_dir(void);
    extern void (*g_xbox_host_path_override)(char *host_path, size_t size);
    const char *root = ps5_root();
    char path[1024];
    uint8_t *viv, *out = NULL;
    size_t n, len, cap;
    uint32_t count, files = 0, strings = 0;
    size_t pos;

    snprintf(path, sizeof path, "%s/text/replace.txt", root);
    if (!read_rules(path, playstation)) {
        fprintf(stderr, "[TEXT] no rules in %s; the game's text is unchanged\n", path);
        return;
    }
    snprintf(s_original, sizeof s_original, "%s/assets/misc.viv", ps5_game_dir());
    viv = read_file(s_original, &n);
    if (!viv || n < 16 || memcmp(viv, "BIG", 3)) {
        fprintf(stderr, "[TEXT] could not read %s\n", s_original);
        free(viv);
        return;
    }
    count = rd32be(viv + 8);
    cap = n + (2u << 20);
    out = malloc(cap);
    if (!out) {
        free(viv);
        return;
    }
    memcpy(out, viv, n);
    len = n;
    pos = 16;
    for (uint32_t i = 0; i < count && pos + 9 < n; i++) {
        uint32_t off = rd32be(viv + pos), size = rd32be(viv + pos + 4);
        const char *name = (const char *)viv + pos + 8;
        size_t nl = strnlen(name, n - pos - 8);
        size_t entry = pos;
        pos += 8 + nl + 1;
        if (nl < 4 || strcasecmp(name + nl - 4, ".loc") || (size_t)off + size > n)
            continue;
        {
            size_t raw_len = 0, patched_len = 0, stored_len = 0;
            int changed = 0;
            uint8_t *raw = refpack_decode(viv + off, size, &raw_len), *patched, *stored;
            int packed = raw != NULL;
            if (!raw) {
                raw = malloc(size);
                if (!raw)
                    continue;
                memcpy(raw, viv + off, size);
                raw_len = size;
            }
            patched = patch_loc(raw, raw_len, &patched_len, &changed);
            free(raw);
            if (!patched)
                continue;
            stored = packed ? refpack_store(patched, patched_len, &stored_len) : patched;
            if (!packed)
                stored_len = patched_len;
            while (len & 127)
                out[len++] = 0;
            if (stored && len + stored_len <= cap) {
                memcpy(out + len, stored, stored_len);
                wr32be(out + entry, (uint32_t)len);
                wr32be(out + entry + 4, (uint32_t)stored_len);
                len += stored_len;
                files++;
                strings += (uint32_t)changed;
                fprintf(stderr, "[TEXT] %s: %d string(s) changed\n", name, changed);
            }
            if (stored != patched)
                free(stored);
            free(patched);
        }
    }
    free(viv);
    if (!files) {
        free(out);
        fprintf(stderr, "[TEXT] the rules matched nothing; the game's text is unchanged\n");
        return;
    }
    wr32(out + 4, (uint32_t)len);
    snprintf(path, sizeof path, "%s/cache", root);
    mkdir(path, 0777);
    snprintf(s_patched, sizeof s_patched, "%s/cache/misc.viv", root);
    {
        /* Only write when it differs: the same file every start otherwise. */
        size_t old_n = 0;
        uint8_t *old = read_file(s_patched, &old_n);
        int same = old && old_n == len && !memcmp(old, out, len);
        free(old);
        if (!same) {
            FILE *f = fopen(s_patched, "wb");
            if (!f || fwrite(out, 1, len, f) != len) {
                fprintf(stderr, "[TEXT] could not write %s\n", s_patched);
                if (f)
                    fclose(f);
                s_patched[0] = 0;
                free(out);
                return;
            }
            fclose(f);
        }
    }
    free(out);
    g_xbox_host_path_override = serve_patched;
    fprintf(stderr, "[TEXT] %u string(s) in %u file(s) changed (%d rule(s), %s buttons); the game reads %s\n",
            strings, files, s_rule_count, playstation ? "PlayStation" : "original", s_patched);
}
