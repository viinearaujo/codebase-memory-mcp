#include "pipeline/uipath.h"

#include "graph_buffer/graph_buffer.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_reason(char *reasons, size_t cap, const char *label) {
    if (!reasons || cap == 0 || !label) {
        return;
    }
    size_t used = strlen(reasons);
    size_t need = strlen(label) + (used ? 1 : 0);
    if (used + need + 1 >= cap) {
        return;
    }
    if (used) {
        reasons[used++] = ',';
        reasons[used] = '\0';
    }
    memcpy(reasons + used, label, strlen(label) + 1);
}

int uipath_selector_risk(const char *text, char *reasons, size_t reasons_cap) {
    if (reasons && reasons_cap) {
        reasons[0] = '\0';
    }
    if (!text || !text[0]) {
        return 0;
    }
    int score = 0;
    if (strstr(text, "idx=")) {
        score++;
        add_reason(reasons, reasons_cap, "idx");
    }
    if (strstr(text, "parentid=")) {
        score++;
        add_reason(reasons, reasons_cap, "parentid");
    }
    if (strstr(text, "tableRow") || strstr(text, "tableCol")) {
        score++;
        add_reason(reasons, reasons_cap, "table");
    }
    int stars = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '*') {
            stars++;
        }
    }
    if (stars > 2) {
        score++;
        add_reason(reasons, reasons_cap, "wild");
    }
    if (strstr(text, "{{")) {
        score++;
        add_reason(reasons, reasons_cap, "dynamic");
    }
    if (strstr(text, "aaname=") || strstr(text, "title=") || strstr(text, "name=")) {
        for (const char *p = text; *p; p++) {
            if (*p >= '0' && *p <= '9' && p[1] >= '0' && p[1] <= '9') {
                score++;
                add_reason(reasons, reasons_cap, "literal");
                break;
            }
        }
    }
    int anchored = strstr(text, "anchor") != NULL || strstr(text, "fuzzy") != NULL ||
                   strstr(text, "Fuzzy") != NULL;
    if (!anchored && (strstr(text, "<wnd") || strstr(text, "<ctrl")) && strstr(text, "idx=")) {
        score++;
        add_reason(reasons, reasons_cap, "no-anchor");
    }
    return score;
}

static int token_in(char toks[][64], int n, const char *t) {
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], t) == 0) {
            return 1;
        }
    }
    return 0;
}

static int split_skel(const char *s, char toks[][64], int cap) {
    int n = 0;
    if (!s) {
        return 0;
    }
    while (*s && n < cap) {
        while (*s == ',' || *s == ' ') {
            s++;
        }
        if (!*s) {
            break;
        }
        size_t k = 0;
        while (*s && *s != ',' && k + 1 < 64) {
            toks[n][k++] = *s++;
        }
        toks[n][k] = '\0';
        if (k) {
            n++;
        }
        if (*s == ',') {
            s++;
        }
    }
    return n;
}

static int jaccard_ge(const char *a, const char *b) {
    char ta[48][64];
    char tb[48][64];
    int na = split_skel(a, ta, 48);
    int nb = split_skel(b, tb, 48);
    if (na < 3 || nb < 3) {
        return 0;
    }
    int inter = 0;
    for (int i = 0; i < na; i++) {
        if (token_in(tb, nb, ta[i])) {
            inter++;
        }
    }
    int uni = na + nb - inter;
    if (uni <= 0) {
        return 0;
    }
    /* >= 0.8 */
    return inter * 10 >= uni * 8;
}

typedef struct {
    int64_t id;
    char qn[512];
    char skel[512];
    int count;
} sim_wf;

void uipath_emit_similarity(struct cbm_gbuf *gb) {
    if (!gb) {
        return;
    }
    const cbm_gbuf_node_t **nodes = NULL;
    int n = 0;
    if (cbm_gbuf_find_by_label(gb, "Workflow", &nodes, &n) != 0 || n < 2) {
        return;
    }
    sim_wf *wfs = calloc((size_t)n, sizeof(sim_wf));
    if (!wfs) {
        return;
    }
    int nw = 0;
    for (int i = 0; i < n && nw < 200; i++) {
        const cbm_gbuf_node_t *node = nodes[i];
        if (!node->properties_json || !node->qualified_name) {
            continue;
        }
        yyjson_doc *doc = yyjson_read(node->properties_json, strlen(node->properties_json), 0);
        if (!doc) {
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *sk = yyjson_obj_get(root, "skeleton");
        yyjson_val *ac = yyjson_obj_get(root, "activity_count");
        int count = ac && yyjson_is_int(ac) ? (int)yyjson_get_int(ac) : 0;
        const char *skel = sk && yyjson_is_str(sk) ? yyjson_get_str(sk) : NULL;
        if (skel && count >= 3 && count <= 80) {
            wfs[nw].id = node->id;
            snprintf(wfs[nw].qn, sizeof(wfs[nw].qn), "%s", node->qualified_name);
            snprintf(wfs[nw].skel, sizeof(wfs[nw].skel), "%s", skel);
            wfs[nw].count = count;
            nw++;
        }
        yyjson_doc_free(doc);
    }
    int emitted = 0;
    for (int i = 0; i < nw && emitted < 400; i++) {
        for (int j = i + 1; j < nw && emitted < 400; j++) {
            if (!jaccard_ge(wfs[i].skel, wfs[j].skel)) {
                continue;
            }
            char props[640];
            snprintf(props, sizeof(props),
                     "{\"strategy\":\"uipath_skeleton\",\"confidence\":0.80,\"kind\":\"workflow\"}");
            cbm_gbuf_insert_edge(gb, wfs[i].id, wfs[j].id, "SIMILAR_TO", props);
            emitted++;
        }
    }
    free(wfs);
}
