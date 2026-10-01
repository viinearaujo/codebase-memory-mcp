#include "pipeline/uipath.h"

#include "foundation/sha256.h"
#include "graph_buffer/graph_buffer.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

enum { CFG_PART_MAX = 4 * 1024 * 1024, CFG_ROWS_MAX = 10000, CFG_STR_MAX = 8000 };

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int secret_name(const char *name) {
    if (!name) {
        return 0;
    }
    char buf[128];
    size_t n = strlen(name);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        buf[i] = c;
    }
    buf[n] = '\0';
    return strstr(buf, "password") || strstr(buf, "secret") || strstr(buf, "token") ||
           strstr(buf, "credential") || strstr(buf, "apikey") || strstr(buf, "api_key") ||
           strstr(buf, "passwd");
}

static void preview_value(const char *name, const char *value, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    if (!value) {
        value = "";
    }
    if (secret_name(name) || strncmp(value, "sk-", 3) == 0 || strncmp(value, "AKIA", 4) == 0 ||
        strstr(value, "BEGIN") != NULL) {
        snprintf(out, cap, "[redacted]");
        return;
    }
    size_t n = strlen(value);
    if (n >= cap) {
        n = cap - 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (value[i] == '\n' || value[i] == '\r') ? ' ' : value[i];
    }
    out[n] = '\0';
    if (strlen(value) > 80 && cap > 80) {
        out[77] = '.';
        out[78] = '.';
        out[79] = '.';
        out[80] = '\0';
    }
}

static char *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 32 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static void json_escape(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) {
        return;
    }
    size_t o = 0;
    if (!src) {
        src = "";
    }
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static int64_t upsert(struct cbm_gbuf *gb, const char *label, const char *name, const char *qn,
                      const char *file, int line, const char *props) {
    if (!name || !name[0]) {
        name = label;
    }
    return cbm_gbuf_upsert_node(gb, label, name, qn, file ? file : "", line, line,
                                props ? props : "{}");
}

static const char *env_from_name(const char *rel) {
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    /* Config.Dev.json -> Dev. Config.json -> empty. */
    if (strncmp(base, "Config.", 7) != 0) {
        return "";
    }
    const char *dot = strrchr(base, '.');
    if (!dot || strcmp(dot, ".json") != 0) {
        return "";
    }
    static char env[64];
    size_t n = (size_t)(dot - (base + 7));
    if (n == 0 || n >= sizeof(env)) {
        return "";
    }
    memcpy(env, base + 7, n);
    env[n] = '\0';
    return env;
}

static void emit_key(struct cbm_gbuf *gb, const char *rel, const char *project_qn, const char *qn,
                     const char *section, const char *lookup, const char *value, const char *desc,
                     const char *asset, const char *folder, int line, long mtime, long size,
                     const char *sha, const char *format, const char *shape, const char *env) {
    (void)mtime;
    (void)size;
    (void)sha;
    (void)format;
    (void)shape;
    (void)env;
    char prev[96];
    preview_value(lookup, value, prev, sizeof(prev));
    char e_sec[80], e_look[160], e_prev[120], e_desc[160], e_asset[120], e_folder[80];
    json_escape(e_sec, sizeof(e_sec), section ? section : "");
    json_escape(e_look, sizeof(e_look), lookup ? lookup : "");
    json_escape(e_prev, sizeof(e_prev), prev);
    json_escape(e_desc, sizeof(e_desc), desc ? desc : "");
    json_escape(e_asset, sizeof(e_asset), asset ? asset : "");
    json_escape(e_folder, sizeof(e_folder), folder ? folder : "");
    char props[1400];
    snprintf(props, sizeof(props),
             "{\"domain\":\"uipath\",\"strategy\":\"uipath_config\",\"section\":\"%s\","
             "\"lookup_key\":\"%s\",\"value_preview\":\"%s\",\"description\":\"%s\","
             "\"project_qn\":\"%s\",\"asset_name\":\"%s\",\"asset_folder\":\"%s\",\"docstring\":\"%s\"}",
             e_sec, e_look, e_prev, e_desc, project_qn ? project_qn : "", e_asset, e_folder, e_desc);
    char name[160];
    snprintf(name, sizeof(name), "%s", lookup && lookup[0] ? lookup : qn);
    upsert(gb, "ConfigKey", name, qn, rel, line, props);
    (void)rel;
}

static void emit_file_node(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                           const char *format, const char *shape, const char *env, long mtime,
                           long size, const char *sha) {
    char qn[640];
    snprintf(qn, sizeof(qn), "%s", rel);
    char props[800];
    snprintf(props, sizeof(props),
             "{\"domain\":\"uipath\",\"strategy\":\"uipath_config\",\"format\":\"%s\","
             "\"shape\":\"%s\",\"is_loaded\":false,\"environment\":\"%s\",\"mtime\":%ld,"
             "\"size\":%ld,\"sha\":\"%s\",\"project_qn\":\"%s\"}",
             format, shape ? shape : "unknown", env ? env : "", mtime, size, sha ? sha : "",
             project_qn ? project_qn : "");
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    upsert(gb, "ConfigFile", base, qn, rel, 1, props);
}

static int line_of(const char *src, const char *hit) {
    int line = 1;
    if (!src || !hit || hit < src) {
        return 1;
    }
    for (const char *p = src; p < hit; p++) {
        if (*p == '\n') {
            line++;
        }
    }
    return line;
}

static void index_json_value(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                             const char *src, yyjson_val *node, const char *section,
                             const char *prefix, const char *pointer, int is_asset_section);

static void emit_scalar_key(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                            const char *src, const char *section, const char *lookup,
                            const char *pointer, yyjson_val *value, const char *desc,
                            const char *asset, const char *folder) {
    char rendered[256];
    rendered[0] = '\0';
    if (yyjson_is_str(value)) {
        snprintf(rendered, sizeof(rendered), "%s", yyjson_get_str(value));
    } else if (yyjson_is_int(value)) {
        snprintf(rendered, sizeof(rendered), "%lld", (long long)yyjson_get_sint(value));
    } else if (yyjson_is_real(value)) {
        snprintf(rendered, sizeof(rendered), "%g", yyjson_get_real(value));
    } else if (yyjson_is_bool(value)) {
        snprintf(rendered, sizeof(rendered), "%s", yyjson_get_bool(value) ? "true" : "false");
    } else if (yyjson_is_null(value)) {
        snprintf(rendered, sizeof(rendered), "null");
    }
    char qn[900];
    snprintf(qn, sizeof(qn), "%s#%s", rel, pointer);
    const char *hit = NULL;
    if (src && lookup && lookup[0]) {
        char pat[200];
        snprintf(pat, sizeof(pat), "\"%s\"", lookup);
        hit = strstr(src, pat);
    }
    emit_key(gb, rel, project_qn, qn, section, lookup, rendered, desc, asset, folder,
             line_of(src, hit), 0, 0, "", "json", "", "");
}

static void index_json_object_fields(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                                     const char *src, yyjson_val *obj, const char *section,
                                     const char *prefix, const char *pointer, int asset_section) {
    yyjson_obj_iter it;
    yyjson_obj_iter_init(obj, &it);
    yyjson_val *key;
    yyjson_val *val;
    while ((key = yyjson_obj_iter_next(&it))) {
        val = yyjson_obj_iter_get_val(key);
        const char *ks = yyjson_get_str(key);
        if (!ks) {
            continue;
        }
        char look[200];
        if (prefix && prefix[0]) {
            snprintf(look, sizeof(look), "%s.%s", prefix, ks);
        } else {
            snprintf(look, sizeof(look), "%s", ks);
        }
        char ptr[400];
        snprintf(ptr, sizeof(ptr), "%s/%s", pointer ? pointer : "", ks);
        if (yyjson_is_obj(val) || yyjson_is_arr(val)) {
            index_json_value(gb, rel, project_qn, src, val, section ? section : ks, look, ptr,
                             asset_section || strcmp(ks, "Assets") == 0);
        } else {
            const char *use_section = section;
            int as_asset = asset_section || (section && strcmp(section, "Assets") == 0);
            emit_scalar_key(gb, rel, project_qn, src, use_section, look, ptr, val, "",
                            as_asset ? (yyjson_is_str(val) ? yyjson_get_str(val) : "") : "", "");
        }
    }
}

static void index_json_value(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                             const char *src, yyjson_val *node, const char *section,
                             const char *prefix, const char *pointer, int is_asset_section) {
    if (yyjson_is_arr(node)) {
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(node, idx, max, el) {
            if (!yyjson_is_obj(el)) {
                continue;
            }
            yyjson_val *name = yyjson_obj_get(el, "Name");
            if (!name) {
                name = yyjson_obj_get(el, "name");
            }
            if (!yyjson_is_str(name)) {
                continue;
            }
            const char *ns = yyjson_get_str(name);
            yyjson_val *value = yyjson_obj_get(el, "Value");
            if (!value) {
                value = yyjson_obj_get(el, "value");
            }
            yyjson_val *desc = yyjson_obj_get(el, "Description");
            yyjson_val *asset = yyjson_obj_get(el, "Asset");
            if (!asset) {
                asset = yyjson_obj_get(el, "asset");
            }
            yyjson_val *folder = yyjson_obj_get(el, "Folder");
            char ptr[400];
            snprintf(ptr, sizeof(ptr), "%s/%s", pointer ? pointer : "", ns);
            char rendered[256];
            rendered[0] = '\0';
            if (value && yyjson_is_str(value)) {
                snprintf(rendered, sizeof(rendered), "%s", yyjson_get_str(value));
            } else if (value && yyjson_is_int(value)) {
                snprintf(rendered, sizeof(rendered), "%lld", (long long)yyjson_get_sint(value));
            }
            const char *asset_s = asset && yyjson_is_str(asset) ? yyjson_get_str(asset) : "";
            const char *folder_s = folder && yyjson_is_str(folder) ? yyjson_get_str(folder) : "";
            const char *desc_s = desc && yyjson_is_str(desc) ? yyjson_get_str(desc) : "";
            if (is_asset_section && asset_s[0] == '\0') {
                asset_s = rendered;
            }
            char qn[900];
            snprintf(qn, sizeof(qn), "%s#%s", rel, ptr);
            char pat[200];
            snprintf(pat, sizeof(pat), "\"%s\"", ns);
            emit_key(gb, rel, project_qn, qn, section, ns, rendered[0] ? rendered : asset_s, desc_s,
                     asset_s, folder_s, line_of(src, strstr(src, pat)), 0, 0, "", "json", "", "");
        }
        return;
    }
    if (yyjson_is_obj(node)) {
        index_json_object_fields(gb, rel, project_qn, src, node, section, prefix, pointer,
                                 is_asset_section);
    }
}

static const char *json_shape(yyjson_val *root) {
    if (!yyjson_is_obj(root)) {
        return "unknown";
    }
    yyjson_val *settings = yyjson_obj_get(root, "Settings");
    yyjson_val *constants = yyjson_obj_get(root, "Constants");
    yyjson_val *assets = yyjson_obj_get(root, "Assets");
    yyjson_val *probe = settings ? settings : (constants ? constants : assets);
    if (probe) {
        if (yyjson_is_arr(probe)) {
            return "sectioned_list";
        }
        if (yyjson_is_obj(probe)) {
            return "sectioned_map";
        }
    }
    int nested = 0;
    int scalar = 0;
    yyjson_obj_iter it;
    yyjson_obj_iter_init(root, &it);
    yyjson_val *k;
    while ((k = yyjson_obj_iter_next(&it))) {
        yyjson_val *v = yyjson_obj_iter_get_val(k);
        if (yyjson_is_obj(v) || yyjson_is_arr(v)) {
            nested++;
        } else {
            scalar++;
        }
    }
    if (nested && !scalar) {
        return "nested";
    }
    if (nested && scalar) {
        return "nested";
    }
    return "flat";
}

static int index_json(struct cbm_gbuf *gb, const char *abs_path, const char *rel,
                      const char *project_qn, const char *text, size_t len, long mtime, long size) {
    char sha[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(text, len, sha);
    yyjson_doc *doc = yyjson_read(text, len, YYJSON_READ_ALLOW_TRAILING_COMMAS);
    if (!doc) {
        emit_file_node(gb, rel, project_qn, "json", "unknown", env_from_name(rel), mtime, size, sha);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *shape = json_shape(root);
    emit_file_node(gb, rel, project_qn, "json", shape, env_from_name(rel), mtime, size, sha);
    if (yyjson_is_obj(root)) {
        int sectioned = strcmp(shape, "sectioned_list") == 0 || strcmp(shape, "sectioned_map") == 0;
        if (sectioned) {
            static const char *secs[] = {"Settings", "Constants", "Assets", NULL};
            for (int i = 0; secs[i]; i++) {
                yyjson_val *sec = yyjson_obj_get(root, secs[i]);
                if (!sec) {
                    continue;
                }
                char ptr[64];
                snprintf(ptr, sizeof(ptr), "/%s", secs[i]);
                index_json_value(gb, rel, project_qn, text, sec, secs[i], "", ptr,
                                 strcmp(secs[i], "Assets") == 0);
            }
        } else {
            index_json_value(gb, rel, project_qn, text, root, "", "", "", 0);
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

static char *inflate_part(const unsigned char *src, size_t src_len, size_t expect) {
    size_t cap = expect > 0 && expect < CFG_PART_MAX ? expect + 64 : 64 * 1024;
    if (cap > CFG_PART_MAX) {
        cap = CFG_PART_MAX;
    }
    char *out = malloc(cap + 1);
    if (!out) {
        return NULL;
    }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        free(out);
        return NULL;
    }
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    size_t used = 0;
    int rc;
    do {
        if (used + 4096 > cap) {
            if (cap >= CFG_PART_MAX) {
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > CFG_PART_MAX) {
                ncap = CFG_PART_MAX;
            }
            char *grown = realloc(out, ncap + 1);
            if (!grown) {
                break;
            }
            out = grown;
            cap = ncap;
        }
        strm.next_out = (Bytef *)(out + used);
        strm.avail_out = (uInt)(cap - used);
        rc = inflate(&strm, Z_NO_FLUSH);
        used = cap - strm.avail_out;
        if (rc != Z_OK && rc != Z_STREAM_END) {
            break;
        }
    } while (rc != Z_STREAM_END && used < CFG_PART_MAX);
    inflateEnd(&strm);
    out[used] = '\0';
    return out;
}

typedef struct {
    char *name;
    char *data;
} zip_part;

static void free_parts(zip_part *p, int n) {
    for (int i = 0; i < n; i++) {
        free(p[i].name);
        free(p[i].data);
    }
    free(p);
}

static zip_part *unzip_all(const unsigned char *b, size_t n, int *out_n) {
    *out_n = 0;
    if (n < 22) {
        return NULL;
    }
    size_t start = n > 66000 ? n - 66000 : 0;
    size_t eocd = (size_t)-1;
    for (size_t i = n - 22;; i--) {
        if (b[i] == 0x50 && b[i + 1] == 0x4b && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            eocd = i;
            break;
        }
        if (i == start) {
            break;
        }
    }
    if (eocd == (size_t)-1) {
        return NULL;
    }
    int nent = rd16(b + eocd + 10);
    uint32_t cd_off = rd32(b + eocd + 16);
    if (cd_off >= n || nent < 0 || nent > 256) {
        return NULL;
    }
    zip_part *parts = calloc((size_t)nent, sizeof(zip_part));
    if (!parts) {
        return NULL;
    }
    size_t p = cd_off;
    int got = 0;
    for (int i = 0; i < nent && p + 46 < n; i++) {
        if (rd32(b + p) != 0x02014b50) {
            break;
        }
        uint16_t method = rd16(b + p + 10);
        uint32_t comp = rd32(b + p + 20);
        uint32_t uncomp = rd32(b + p + 24);
        uint16_t namelen = rd16(b + p + 28);
        uint16_t extra = rd16(b + p + 30);
        uint16_t comment = rd16(b + p + 32);
        uint32_t local = rd32(b + p + 42);
        if (p + 46 + namelen > n) {
            break;
        }
        char *name = malloc((size_t)namelen + 1);
        if (!name) {
            break;
        }
        memcpy(name, b + p + 46, namelen);
        name[namelen] = '\0';
        p += 46 + namelen + extra + comment;
        char *data = NULL;
        if (local + 30 < n && rd32(b + local) == 0x04034b50) {
            uint16_t ln = rd16(b + local + 26);
            uint16_t le = rd16(b + local + 28);
            size_t data_off = local + 30 + ln + le;
            if (data_off + comp <= n && uncomp <= CFG_PART_MAX) {
                if (method == 0) {
                    data = malloc((size_t)comp + 1);
                    if (data) {
                        memcpy(data, b + data_off, comp);
                        data[comp] = '\0';
                    }
                } else if (method == 8) {
                    data = inflate_part(b + data_off, comp, uncomp);
                }
            }
        }
        parts[got].name = name;
        parts[got].data = data ? data : strdup("");
        got++;
    }
    *out_n = got;
    return parts;
}

static const char *part_get(zip_part *parts, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (parts[i].name && strcmp(parts[i].name, name) == 0) {
            return parts[i].data ? parts[i].data : "";
        }
        /* Target paths in rels are relative to xl/. */
        if (parts[i].name && name[0] && strstr(parts[i].name, name)) {
            return parts[i].data ? parts[i].data : "";
        }
    }
    return NULL;
}

static char **parse_shared(const char *xml, int *out_n) {
    *out_n = 0;
    if (!xml) {
        return NULL;
    }
    int cap = 64;
    char **arr = calloc((size_t)cap, sizeof(char *));
    if (!arr) {
        return NULL;
    }
    const char *p = xml;
    while ((p = strstr(p, "<si")) != NULL && *out_n < CFG_STR_MAX) {
        const char *end = strstr(p, "</si>");
        if (!end) {
            break;
        }
        char *acc = calloc(1024, 1);
        if (!acc) {
            break;
        }
        size_t used = 0;
        const char *t = p;
        while ((t = strstr(t, "<t")) && t < end) {
            const char *gt = strchr(t, '>');
            if (!gt || gt > end) {
                break;
            }
            const char *te = strstr(gt, "</t>");
            if (!te || te > end) {
                break;
            }
            size_t len = (size_t)(te - (gt + 1));
            if (used + len + 1 < 1024) {
                memcpy(acc + used, gt + 1, len);
                used += len;
                acc[used] = '\0';
            }
            t = te + 4;
        }
        if (*out_n >= cap) {
            cap *= 2;
            char **grown = realloc(arr, (size_t)cap * sizeof(char *));
            if (!grown) {
                free(acc);
                break;
            }
            arr = grown;
        }
        arr[*out_n] = acc;
        (*out_n)++;
        p = end + 5;
    }
    return arr;
}

static const char *cell_text(const char *cell_open, const char *cell_end, char **shared, int ns,
                             char *buf, size_t cap) {
    buf[0] = '\0';
    int shared_idx = cell_open && strstr(cell_open, "t=\"s\"") != NULL;
    const char *v = strstr(cell_open, "<v>");
    if (!v || v > cell_end) {
        const char *is = strstr(cell_open, "<is>");
        if (is && is < cell_end) {
            const char *gt = strstr(is, "<t");
            if (gt && gt < cell_end) {
                gt = strchr(gt, '>');
            }
            const char *te = gt ? strstr(gt, "</t>") : NULL;
            if (gt && te && te < cell_end) {
                size_t n = (size_t)(te - (gt + 1));
                if (n >= cap) {
                    n = cap - 1;
                }
                memcpy(buf, gt + 1, n);
                buf[n] = '\0';
            }
        }
        return buf;
    }
    v += 3;
    const char *ve = strstr(v, "</v>");
    if (!ve || ve > cell_end) {
        return buf;
    }
    char tmp[64];
    size_t n = (size_t)(ve - v);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, v, n);
    tmp[n] = '\0';
    if (shared_idx) {
        int idx = atoi(tmp);
        if (idx >= 0 && idx < ns && shared[idx]) {
            snprintf(buf, cap, "%s", shared[idx]);
        }
    } else {
        snprintf(buf, cap, "%s", tmp);
    }
    return buf;
}

static int col_index(const char *r) {
    if (!r) {
        return -1;
    }
    int c = 0;
    while (*r >= 'A' && *r <= 'Z') {
        c = c * 26 + (*r - 'A' + 1);
        r++;
    }
    return c - 1;
}

static void index_sheet(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                        const char *sheet_name, const char *xml, char **shared, int ns) {
    if (!xml) {
        return;
    }
    int name_col = 0, value_col = 1, desc_col = 2, asset_col = -1, folder_col = -1;
    int header_done = 0;
    int rows = 0;
    const char *p = xml;
    while ((p = strstr(p, "<row")) && rows < CFG_ROWS_MAX) {
        const char *row_end = strstr(p, "</row>");
        if (!row_end) {
            break;
        }
        char cells[8][256];
        memset(cells, 0, sizeof(cells));
        const char *c = p;
        while ((c = strstr(c, "<c ")) && c < row_end) {
            const char *cend = strstr(c, "</c>");
            if (!cend || cend > row_end) {
                const char *sc = strstr(c, "/>");
                cend = sc && sc < row_end ? sc : NULL;
                if (!cend) {
                    break;
                }
            }
            const char *rattr = strstr(c, " r=\"");
            int col = 0;
            if (rattr && rattr < cend) {
                col = col_index(rattr + 4);
            }
            if (col >= 0 && col < 8) {
                cell_text(c, cend, shared, ns, cells[col], sizeof(cells[col]));
            }
            c = cend + 1;
        }
        if (!header_done) {
            for (int i = 0; i < 8; i++) {
                if (strcmp(cells[i], "Name") == 0) {
                    name_col = i;
                } else if (strcmp(cells[i], "Value") == 0) {
                    value_col = i;
                } else if (strcmp(cells[i], "Description") == 0) {
                    desc_col = i;
                } else if (strcmp(cells[i], "Asset") == 0) {
                    asset_col = i;
                } else if (strcmp(cells[i], "Folder") == 0) {
                    folder_col = i;
                }
            }
            header_done = 1;
        } else if (cells[name_col][0]) {
            char qn[900];
            snprintf(qn, sizeof(qn), "%s#%s!%s", rel, sheet_name, cells[name_col]);
            const char *asset = asset_col >= 0 ? cells[asset_col] : "";
            if (strcmp(sheet_name, "Assets") == 0 && asset[0] == '\0') {
                asset = cells[value_col];
            }
            emit_key(gb, rel, project_qn, qn, sheet_name, cells[name_col], cells[value_col],
                     desc_col >= 0 ? cells[desc_col] : "", asset,
                     folder_col >= 0 ? cells[folder_col] : "", rows + 2, 0, 0, "", "xlsx", "", "");
        }
        rows++;
        p = row_end + 6;
    }
}

static int index_xlsx(struct cbm_gbuf *gb, const char *abs_path, const char *rel,
                      const char *project_qn, const unsigned char *bytes, size_t len, long mtime,
                      long size) {
    char sha[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(bytes, len, sha);
    int nparts = 0;
    zip_part *parts = unzip_all(bytes, len, &nparts);
    emit_file_node(gb, rel, project_qn, "xlsx", "sheets", "", mtime, size, sha);
    if (!parts) {
        return 0;
    }
    const char *sst = part_get(parts, nparts, "xl/sharedStrings.xml");
    int ns = 0;
    char **shared = parse_shared(sst, &ns);
    const char *wb = part_get(parts, nparts, "xl/workbook.xml");
    const char *rels = part_get(parts, nparts, "xl/_rels/workbook.xml.rels");
    if (wb) {
        const char *s = wb;
        while ((s = strstr(s, "<sheet ")) != NULL) {
            const char *name_at = strstr(s, "name=\"");
            const char *id_at = strstr(s, "r:id=\"");
            if (!name_at || !id_at) {
                break;
            }
            name_at += 6;
            const char *name_end = strchr(name_at, '"');
            id_at += 6;
            const char *id_end = strchr(id_at, '"');
            if (!name_end || !id_end) {
                break;
            }
            char sheet[64];
            char rid[32];
            size_t nl = (size_t)(name_end - name_at);
            if (nl >= sizeof(sheet)) {
                nl = sizeof(sheet) - 1;
            }
            memcpy(sheet, name_at, nl);
            sheet[nl] = '\0';
            size_t il = (size_t)(id_end - id_at);
            if (il >= sizeof(rid)) {
                il = sizeof(rid) - 1;
            }
            memcpy(rid, id_at, il);
            rid[il] = '\0';
            const char *target = NULL;
            if (rels) {
                char pat[64];
                snprintf(pat, sizeof(pat), "Id=\"%s\"", rid);
                const char *relhit = strstr(rels, pat);
                if (relhit) {
                    const char *tg = strstr(relhit, "Target=\"");
                    if (tg) {
                        tg += 8;
                        const char *te = strchr(tg, '"');
                        if (te) {
                            char tbuf[128];
                            size_t tn = (size_t)(te - tg);
                            if (tn >= sizeof(tbuf)) {
                                tn = sizeof(tbuf) - 1;
                            }
                            memcpy(tbuf, tg, tn);
                            tbuf[tn] = '\0';
                            char full[160];
                            if (strncmp(tbuf, "xl/", 3) == 0) {
                                snprintf(full, sizeof(full), "%s", tbuf);
                            } else if (strncmp(tbuf, "/xl/", 4) == 0) {
                                snprintf(full, sizeof(full), "%s", tbuf + 1);
                            } else {
                                snprintf(full, sizeof(full), "xl/%s", tbuf);
                            }
                            target = part_get(parts, nparts, full);
                        }
                    }
                }
            }
            if (target) {
                index_sheet(gb, rel, project_qn, sheet, target, shared, ns);
            }
            s = name_end;
        }
    }
    if (shared) {
        for (int i = 0; i < ns; i++) {
            free(shared[i]);
        }
        free(shared);
    }
    free_parts(parts, nparts);
    return 0;
}

static int index_appconfig(struct cbm_gbuf *gb, const char *rel, const char *project_qn,
                           const char *text) {
    emit_file_node(gb, rel, project_qn, "appconfig", "add", "", 0, (long)strlen(text), "");
    const char *p = text;
    while ((p = strstr(p, "<add ")) != NULL) {
        const char *k = strstr(p, "key=\"");
        const char *v = strstr(p, "value=\"");
        if (!k || !v) {
            break;
        }
        k += 5;
        v += 7;
        const char *ke = strchr(k, '"');
        const char *ve = strchr(v, '"');
        if (!ke || !ve) {
            break;
        }
        char key[128];
        char val[256];
        size_t kn = (size_t)(ke - k);
        size_t vn = (size_t)(ve - v);
        if (kn >= sizeof(key)) {
            kn = sizeof(key) - 1;
        }
        if (vn >= sizeof(val)) {
            vn = sizeof(val) - 1;
        }
        memcpy(key, k, kn);
        key[kn] = '\0';
        memcpy(val, v, vn);
        val[vn] = '\0';
        char qn[700];
        snprintf(qn, sizeof(qn), "%s#%s", rel, key);
        emit_key(gb, rel, project_qn, qn, "appSettings", key, val, "", "", "", line_of(text, p), 0,
                 0, "", "appconfig", "", "");
        p = ve;
    }
    return 0;
}

int uipath_config_index_file(struct cbm_gbuf *gb, const char *abs_path, const char *rel,
                             const char *project_qn) {
    if (!gb || !abs_path || !rel) {
        return 0;
    }
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        return 0;
    }
    size_t len = 0;
    char *bytes = read_all(abs_path, &len);
    if (!bytes) {
        return 0;
    }
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    size_t bl = strlen(base);
    if (bl > 5 && strcmp(base + bl - 5, ".json") == 0) {
        index_json(gb, abs_path, rel, project_qn, bytes, len, (long)st.st_mtime, (long)st.st_size);
    } else if (bl > 5 && strcmp(base + bl - 5, ".xlsx") == 0) {
        index_xlsx(gb, abs_path, rel, project_qn, (const unsigned char *)bytes, len,
                   (long)st.st_mtime, (long)st.st_size);
    } else if (bl > 7 && strcmp(base + bl - 7, ".config") == 0) {
        index_appconfig(gb, rel, project_qn, bytes);
    }
    free(bytes);
    return 0;
}
