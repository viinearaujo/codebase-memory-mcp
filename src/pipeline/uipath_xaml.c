#include "pipeline/uipath.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { XAML_DEPTH = 512, XAML_ATTRS = 48, XAML_NS = 48, XAML_ACT_MAX = 20000 };

typedef struct {
    const char *s;
    size_t n;
    size_t i;
    int line;
    int trunc_attr;
} xsrc;

typedef struct {
    char name[128];
    char *value;
} xattr;

typedef struct {
    char prefix[40];
    char local[96];
    char id_ref[96];
    char display[160];
    char slot[48];
    char child_slot[48];
    char clr[200];
    char package[80];
    char annotation[200];
    char x_name[80];
    char facts[4096];
    char key_name[96];
    char type_arg[140];
    char dir[16];
    char expr_text[1200];
    char catches[140];
    int facts_len;
    int start_line;
    int end_line;
    int ordinal;
    int next_ord;
    int collapsed;
    int skip;
    int activity;
    int expr_carrier;
    int is_argument;
    int is_variable;
    int property;
    int has_vars;
    int expr_cs;
} xframe;

typedef struct {
    char prefix[32];
    char uri[180];
} xns;

static int xpeek(xsrc *s) {
    return s->i < s->n ? (unsigned char)s->s[s->i] : -1;
}

static int xget(xsrc *s) {
    if (s->i >= s->n) {
        return -1;
    }
    unsigned char c = (unsigned char)s->s[s->i++];
    if (c == '\n') {
        s->line++;
    }
    return c;
}

static void fact_add(xframe *f, const char *line) {
    if (!f || !line || !line[0]) {
        return;
    }
    size_t n = strlen(line);
    if ((size_t)f->facts_len + n + 2 >= sizeof(f->facts)) {
        return;
    }
    if (f->facts_len) {
        f->facts[f->facts_len++] = '\n';
    }
    memcpy(f->facts + f->facts_len, line, n);
    f->facts_len += (int)n;
    f->facts[f->facts_len] = '\0';
}

static void decode_entities(const char *in, size_t in_len, char *out, size_t cap, int *clipped) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < cap;) {
        if (in[i] != '&') {
            out[o++] = in[i++];
            continue;
        }
        if (i + 5 <= in_len && strncmp(in + i, "&amp;", 5) == 0) {
            out[o++] = '&';
            i += 5;
        } else if (i + 4 <= in_len && strncmp(in + i, "&lt;", 4) == 0) {
            out[o++] = '<';
            i += 4;
        } else if (i + 4 <= in_len && strncmp(in + i, "&gt;", 4) == 0) {
            out[o++] = '>';
            i += 4;
        } else if (i + 6 <= in_len && strncmp(in + i, "&quot;", 6) == 0) {
            out[o++] = '"';
            i += 6;
        } else if (i + 6 <= in_len && strncmp(in + i, "&apos;", 6) == 0) {
            out[o++] = '\'';
            i += 6;
        } else if (i + 2 < in_len && in[i + 1] == '#') {
            int hex = in[i + 2] == 'x' || in[i + 2] == 'X';
            const char *p = in + i + (hex ? 3 : 2);
            char *end = NULL;
            unsigned long cp = strtoul(p, &end, hex ? 16 : 10);
            if (end && *end == ';' && end < in + in_len) {
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800 && o + 2 < cap) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp <= 0x10FFFF && o + 3 < cap) {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                i = (size_t)(end - in) + 1;
            } else {
                out[o++] = in[i++];
            }
        } else {
            out[o++] = in[i++];
        }
    }
    if (o + 1 >= cap && in_len > 0) {
        *clipped = 1;
    }
    out[o] = '\0';
}

static int wrapper_name(const char *s) {
    return s && (strcmp(s, "Then") == 0 || strcmp(s, "Else") == 0 || strcmp(s, "Try") == 0 ||
                 strcmp(s, "Body") == 0 || strcmp(s, "Do") == 0 || strcmp(s, "Catch") == 0 ||
                 strcmp(s, "Finally") == 0);
}

static int xmeta_local(const char *local) {
    static const char *meta[] = {"Class",    "Members", "Property", "String", "Boolean", "Int32",
                                 "Int64",    "Double",  "Object",   "Reference", "Null",  "Array",
                                 "Type",     "Code",    "Key",      "Name",      NULL};
    for (int i = 0; meta[i]; i++) {
        if (strcmp(local, meta[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int expr_local(const char *local) {
    return strcmp(local, "InArgument") == 0 || strcmp(local, "OutArgument") == 0 ||
           strcmp(local, "InOutArgument") == 0 || strcmp(local, "CSharpValue") == 0 ||
           strcmp(local, "VisualBasicValue") == 0 || strcmp(local, "CSharpReference") == 0 ||
           strcmp(local, "VisualBasicReference") == 0 || strcmp(local, "Literal") == 0;
}

static void split_name(const char *full, char *prefix, size_t pc, char *local, size_t lc) {
    const char *col = strchr(full, ':');
    if (col && (size_t)(col - full) < pc) {
        memcpy(prefix, full, (size_t)(col - full));
        prefix[col - full] = '\0';
        snprintf(local, lc, "%s", col + 1);
    } else {
        prefix[0] = '\0';
        snprintf(local, lc, "%s", full);
    }
}

static const char *ns_uri(xns *ns, int n, const char *prefix) {
    for (int i = n - 1; i >= 0; i--) {
        if (strcmp(ns[i].prefix, prefix ? prefix : "") == 0) {
            return ns[i].uri;
        }
    }
    return "";
}

static int designer_ns(const char *uri, const char *prefix, const char *local) {
    if (prefix && (strcmp(prefix, "sap") == 0 || strcmp(prefix, "sap2010") == 0 ||
                   strcmp(prefix, "sads") == 0)) {
        return 1;
    }
    if (uri && strstr(uri, "activities/presentation")) {
        return 1;
    }
    if (local && (strcmp(local, "ViewStateManager") == 0 ||
                  strcmp(local, "VirtualizedContainerService") == 0 ||
                  strcmp(local, "HintSize") == 0 || strcmp(local, "WorkflowViewState") == 0 ||
                  strcmp(local, "CollectionViewState") == 0)) {
        return 1;
    }
    return 0;
}

static void map_clr(const char *uri, const char *local, char *clr, size_t cc, char *pkg, size_t pc) {
    clr[0] = '\0';
    pkg[0] = '\0';
    if (!uri) {
        snprintf(clr, cc, "%s", local ? local : "");
        return;
    }
    const char *clrns = strstr(uri, "clr-namespace:");
    if (clrns) {
        clrns += strlen("clr-namespace:");
        const char *semi = strchr(clrns, ';');
        char nsbuf[160];
        size_t n = semi ? (size_t)(semi - clrns) : strlen(clrns);
        if (n >= sizeof(nsbuf)) {
            n = sizeof(nsbuf) - 1;
        }
        memcpy(nsbuf, clrns, n);
        nsbuf[n] = '\0';
        snprintf(clr, cc, "%s.%s", nsbuf, local ? local : "");
        const char *asm = strstr(uri, "assembly=");
        if (asm) {
            snprintf(pkg, pc, "%s", asm + 9);
        }
        return;
    }
    if (strstr(uri, "schemas.uipath.com/workflow/activities")) {
        snprintf(clr, cc, "UiPath.Core.Activities.%s", local ? local : "");
        snprintf(pkg, pc, "UiPath.System.Activities");
        return;
    }
    if (strstr(uri, "netfx/2009/xaml/activities")) {
        snprintf(clr, cc, "System.Activities.Statements.%s", local ? local : "");
        snprintf(pkg, pc, "System.Activities");
        return;
    }
    snprintf(clr, cc, "%s", local ? local : "");
}

static const char *attr_get(xattr *attrs, int n, const char *suffix) {
    size_t sl = strlen(suffix);
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(attrs[i].name);
        if (nl >= sl && strcmp(attrs[i].name + nl - sl, suffix) == 0) {
            if (nl == sl || attrs[i].name[nl - sl - 1] == ':' || attrs[i].name[nl - sl - 1] == '.') {
                return attrs[i].value ? attrs[i].value : "";
            }
        }
    }
    return NULL;
}

static int skip_comment_or_pi(xsrc *s) {
    if (xpeek(s) != '<') {
        return 0;
    }
    if (s->i + 4 <= s->n && strncmp(s->s + s->i, "<!--", 4) == 0) {
        s->i += 4;
        while (s->i + 2 < s->n && !(s->s[s->i] == '-' && s->s[s->i + 1] == '-' && s->s[s->i + 2] == '>')) {
            xget(s);
        }
        if (s->i + 2 < s->n) {
            s->i += 3;
        }
        return 1;
    }
    if (s->i + 9 <= s->n && strncmp(s->s + s->i, "<![CDATA[", 9) == 0) {
        s->i += 9;
        while (s->i + 2 < s->n && !(s->s[s->i] == ']' && s->s[s->i + 1] == ']' && s->s[s->i + 2] == '>')) {
            xget(s);
        }
        if (s->i + 2 < s->n) {
            s->i += 3;
        }
        return 1;
    }
    if (s->i + 2 <= s->n && s->s[s->i] == '<' && (s->s[s->i + 1] == '?' || s->s[s->i + 1] == '!')) {
        xget(s);
        xget(s);
        while (xpeek(s) != -1 && xpeek(s) != '>') {
            xget(s);
        }
        if (xpeek(s) == '>') {
            xget(s);
        }
        return 1;
    }
    return 0;
}

static int parse_tag(xsrc *s, char *name, size_t name_cap, xattr *attrs, int *attr_n, int *self_close,
                     int *end_tag) {
    *attr_n = 0;
    *self_close = 0;
    *end_tag = 0;
    name[0] = '\0';
    if (xget(s) != '<') {
        return -1;
    }
    if (xpeek(s) == '/') {
        xget(s);
        *end_tag = 1;
    }
    size_t ni = 0;
    while (xpeek(s) != -1 && !isspace(xpeek(s)) && xpeek(s) != '>' && xpeek(s) != '/') {
        if (ni + 1 < name_cap) {
            name[ni++] = (char)xget(s);
        } else {
            xget(s);
        }
    }
    name[ni] = '\0';
    while (xpeek(s) != -1 && xpeek(s) != '>') {
        while (isspace(xpeek(s))) {
            xget(s);
        }
        if (xpeek(s) == '/' ) {
            xget(s);
            *self_close = 1;
            continue;
        }
        if (xpeek(s) == '>' || xpeek(s) == -1) {
            break;
        }
        char an[128];
        size_t ai = 0;
        while (xpeek(s) != -1 && xpeek(s) != '=' && !isspace(xpeek(s)) && xpeek(s) != '>' &&
               xpeek(s) != '/') {
            if (ai + 1 < sizeof(an)) {
                an[ai++] = (char)xget(s);
            } else {
                xget(s);
            }
        }
        an[ai] = '\0';
        while (isspace(xpeek(s))) {
            xget(s);
        }
        if (xpeek(s) != '=') {
            continue;
        }
        xget(s);
        while (isspace(xpeek(s))) {
            xget(s);
        }
        int quote = xpeek(s);
        if (quote != '"' && quote != '\'') {
            continue;
        }
        xget(s);
        size_t cap = 8192;
        char *raw = malloc(cap);
        if (!raw) {
            return -1;
        }
        size_t rn = 0;
        int clipped = 0;
        while (xpeek(s) != -1 && xpeek(s) != quote) {
            if (rn + 1 >= cap) {
                clipped = 1;
                xget(s);
                continue;
            }
            raw[rn++] = (char)xget(s);
        }
        raw[rn] = '\0';
        if (xpeek(s) == quote) {
            xget(s);
        }
        if (*attr_n < XAML_ATTRS && an[0]) {
            char *dec = malloc(cap);
            if (!dec) {
                free(raw);
                return -1;
            }
            decode_entities(raw, rn, dec, cap, &clipped);
            attrs[*attr_n].value = dec;
            snprintf(attrs[*attr_n].name, sizeof(attrs[*attr_n].name), "%s", an);
            (*attr_n)++;
        }
        if (clipped) {
            s->trunc_attr = 1;
        }
        free(raw);
    }
    if (xpeek(s) == '>') {
        xget(s);
    }
    return 0;
}

static void free_attrs(xattr *attrs, int n) {
    for (int i = 0; i < n; i++) {
        free(attrs[i].value);
        attrs[i].value = NULL;
    }
}

static int seen_has(char (*seen)[96], int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(seen[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void fill_item_from_frame(uipath_xaml_item *it, const xframe *f, uipath_xaml_kind kind) {
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    snprintf(it->name, sizeof(it->name), "%s", f->display[0] ? f->display : f->local);
    snprintf(it->id_ref, sizeof(it->id_ref), "%s", f->id_ref);
    snprintf(it->id_source, sizeof(it->id_source), "%s", f->id_ref[0] ? "idref" : "path");
    snprintf(it->type_name, sizeof(it->type_name), "%s", f->local);
    snprintf(it->clr_type, sizeof(it->clr_type), "%s", f->clr);
    snprintf(it->package, sizeof(it->package), "%s", f->package);
    snprintf(it->slot, sizeof(it->slot), "%s", f->slot);
    snprintf(it->annotation, sizeof(it->annotation), "%s", f->annotation);
    snprintf(it->x_name, sizeof(it->x_name), "%s", f->x_name);
    snprintf(it->facts, sizeof(it->facts), "%s", f->facts);
    snprintf(it->direction, sizeof(it->direction), "%s", f->dir);
    it->start_line = f->start_line;
    it->end_line = f->end_line > 0 ? f->end_line : f->start_line;
    it->ordinal = f->ordinal;
}

static xframe *owning_activity(xframe *stack, int depth) {
    for (int i = depth - 1; i >= 0; i--) {
        if (stack[i].activity && !stack[i].collapsed && !stack[i].skip) {
            return &stack[i];
        }
    }
    return NULL;
}

static void attach_expr_facts(xframe *stack, int depth) {
    xframe *f = &stack[depth];
    if (!f->expr_carrier) {
        return;
    }
    xframe *owner = owning_activity(stack, depth);
    if (!owner) {
        return;
    }
    const char *text = f->expr_text;
    const char *lang = f->expr_cs ? "cs" : "vb";
    char line[1600];
    if (strcmp(f->slot, "WorkflowFileName") == 0) {
        int expr = text[0] == '[' || strchr(text, '(');
        snprintf(line, sizeof(line), "%s\t%s", expr ? "invoke_expr" : "invoke", text);
        fact_add(owner, line);
    } else if (strcmp(f->slot, "Arguments") == 0 || f->key_name[0]) {
        snprintf(line, sizeof(line), "bind\t%s\t%s\t%s", f->dir[0] ? f->dir : "In", f->key_name, text);
        fact_add(owner, line);
    } else if (strcmp(f->slot, "To") == 0) {
        snprintf(line, sizeof(line), "write\t%s\t%s\t", lang, text);
        fact_add(owner, line);
    } else if (text[0]) {
        snprintf(line, sizeof(line), "expr\t%s\t%s", lang, text);
        fact_add(owner, line);
    }
    if (strstr(text, "Config.json") || strstr(text, "Config.xlsx")) {
        snprintf(line, sizeof(line), "load\t%s", text);
        fact_add(owner, line);
    }
}

static void parent_id_of(xframe *stack, int depth, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (stack[i].activity && !stack[i].collapsed && stack[i].id_ref[0]) {
            snprintf(out, cap, "%s", stack[i].id_ref);
            return;
        }
    }
}

static void breadcrumb(xframe *stack, int depth, char *out, size_t cap) {
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i <= depth; i++) {
        if (!stack[i].activity || stack[i].collapsed) {
            continue;
        }
        const char *bit = stack[i].display[0] ? stack[i].display : stack[i].local;
        size_t n = strlen(bit);
        if (used && used + 3 < cap) {
            memcpy(out + used, " > ", 3);
            used += 3;
        }
        if (used + n + 1 >= cap) {
            break;
        }
        memcpy(out + used, bit, n);
        used += n;
        out[used] = '\0';
    }
}

static void protected_by(xframe *stack, int depth, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (stack[i].activity && (strcmp(stack[i].local, "TryCatch") == 0 ||
                                  strcmp(stack[i].local, "RetryScope") == 0)) {
            snprintf(out, cap, "%s", stack[i].id_ref);
            return;
        }
    }
}

static void note_resource_attrs(xframe *f, xattr *attrs, int nattr) {
    const char *wf = attr_get(attrs, nattr, "WorkflowFileName");
    if (wf && wf[0]) {
        char line[1400];
        int expr = wf[0] == '[' || strchr(wf, '(') != NULL;
        snprintf(line, sizeof(line), "%s\t%s", expr ? "invoke_expr" : "invoke", wf);
        fact_add(f, line);
    }
    const char *sels[] = {"FullSelectorArgument", "FuzzySelectorArgument", "ScopeSelectorArgument",
                          "Selector", NULL};
    for (int i = 0; sels[i]; i++) {
        const char *v = attr_get(attrs, nattr, sels[i]);
        if (!v || !v[0]) {
            continue;
        }
        char reasons[80];
        int risk = uipath_selector_risk(v, reasons, sizeof(reasons));
        char line[1500];
        const char *kind = strcmp(sels[i], "Selector") == 0 ? "classic" : sels[i];
        snprintf(line, sizeof(line), "sel\t%s\t%d\t%s\t%s", kind, risk, reasons, v);
        fact_add(f, line);
    }
    const char *asset = attr_get(attrs, nattr, "AssetName");
    if (asset && asset[0]) {
        char line[400];
        const char *op = "get";
        if (strstr(f->local, "SetAsset")) {
            op = "set";
        } else if (strstr(f->local, "Credential")) {
            op = "credential";
        }
        int via_cfg = asset[0] == '[' || strchr(asset, '(');
        snprintf(line, sizeof(line), "asset\t%s\t%s\t%s", op, asset, via_cfg ? "config" : "literal");
        fact_add(f, line);
    }
    const char *queue = attr_get(attrs, nattr, "QueueName");
    if (!queue) {
        queue = attr_get(attrs, nattr, "QueueType");
    }
    if (queue && queue[0]) {
        char line[400];
        const char *op = "enqueue";
        if (strstr(f->local, "GetTransaction") || strstr(f->local, "GetQueue")) {
            op = "dequeue";
        }
        int via_cfg = queue[0] == '[' || strchr(queue, '(');
        snprintf(line, sizeof(line), "queue\t%s\t%s\t%s", op, queue, via_cfg ? "config" : "literal");
        fact_add(f, line);
    }
    const char *proc = attr_get(attrs, nattr, "ProcessName");
    if (proc && proc[0]) {
        char line[300];
        snprintf(line, sizeof(line), "res\tprocess\t%s", proc);
        fact_add(f, line);
    }
    const char *conn = attr_get(attrs, nattr, "ConnectionId");
    if (conn && conn[0]) {
        char line[300];
        snprintf(line, sizeof(line), "res\tconnection\t%s", conn);
        fact_add(f, line);
    }
    const char *to = attr_get(attrs, nattr, "To");
    const char *val = attr_get(attrs, nattr, "Value");
    if (strcmp(f->local, "Assign") == 0 && (to || val)) {
        char line[1500];
        snprintf(line, sizeof(line), "write\tvb\t%s\t%s", to ? to : "", val ? val : "");
        fact_add(f, line);
    }
    for (int i = 0; i < nattr; i++) {
        const char *v = attrs[i].value;
        if (!v || v[0] != '[') {
            continue;
        }
        char line[1500];
        snprintf(line, sizeof(line), "expr\tvb\t%s", v);
        fact_add(f, line);
    }
    for (int i = 0; i < nattr; i++) {
        const char *v = attrs[i].value;
        if (!v) {
            continue;
        }
        if (strstr(v, "Config.json") || strstr(v, "Config.xlsx") || strstr(v, "Config.")) {
            char line[400];
            snprintf(line, sizeof(line), "load\t%s", v);
            fact_add(f, line);
        }
    }
}

static void structural_id(xframe *stack, int depth, char *out, size_t cap) {
    size_t used = 0;
    snprintf(out, cap, "~/");
    used = 2;
    for (int i = 0; i <= depth; i++) {
        if (!stack[i].activity || stack[i].collapsed) {
            continue;
        }
        char bit[80];
        snprintf(bit, sizeof(bit), "%s%s[%d]", used > 2 ? "/" : "", stack[i].local, stack[i].ordinal);
        size_t n = strlen(bit);
        if (used + n + 1 >= cap) {
            break;
        }
        memcpy(out + used, bit, n + 1);
        used += n;
    }
}

int uipath_xaml_scan(const char *src, size_t len, uipath_xaml_emit_fn emit, void *ud) {
    if (!src || !emit) {
        return 0;
    }
    xsrc xs = {.s = src, .n = len, .i = 0, .line = 1, .trunc_attr = 0};
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        xs.i = 3;
    }
    xframe *stack = calloc(XAML_DEPTH, sizeof(xframe));
    char (*seen)[96] = calloc(4096, 96);
    xns *ns = calloc(XAML_NS * XAML_DEPTH, sizeof(xns));
    int *ns_mark = calloc(XAML_DEPTH, sizeof(int));
    if (!stack || !seen || !ns || !ns_mark) {
        free(stack);
        free(seen);
        free(ns);
        free(ns_mark);
        return 0;
    }
    int depth = -1;
    int ns_count = 0;
    int seen_n = 0;
    int activities = 0;
    int parse_status = 0;
    int saw_vb = 0, saw_cs = 0;
    char root_kind[48] = "";
    char x_class[220] = "";
    char root_annotation[240] = "";
    char skeleton[1500];
    skeleton[0] = '\0';
    size_t skel_used = 0;

    while (xs.i < xs.n) {
        while (xs.i < xs.n && xpeek(&xs) != '<') {
            int ch = xget(&xs);
            if (depth >= 0 && stack[depth].expr_carrier && ch >= 0 && ch != '\r') {
                char *et = stack[depth].expr_text;
                size_t el = strlen(et);
                char add = (ch == '\n' || ch == '\t') ? ' ' : (char)ch;
                if (add == ' ' && (el == 0 || et[el - 1] == ' ')) {
                    continue;
                }
                if (el + 2 < sizeof(stack[depth].expr_text)) {
                    et[el] = add;
                    et[el + 1] = '\0';
                }
            }
        }
        if (xs.i >= xs.n) {
            break;
        }
        if (skip_comment_or_pi(&xs)) {
            continue;
        }
        if (xpeek(&xs) != '<') {
            break;
        }
        char tname[160];
        xattr attrs[XAML_ATTRS];
        memset(attrs, 0, sizeof(attrs));
        int nattr = 0, self_close = 0, end_tag = 0;
        int start_line = xs.line;
        if (parse_tag(&xs, tname, sizeof(tname), attrs, &nattr, &self_close, &end_tag) != 0) {
            parse_status = 1;
            free_attrs(attrs, nattr);
            break;
        }
        if (end_tag) {
            if (depth >= 0) {
                xframe *f = &stack[depth];
                f->end_line = xs.line;
                if (f->activity && !f->collapsed && !f->skip) {
                    uipath_xaml_item item;
                    fill_item_from_frame(&item, f, UIP_XAML_ACTIVITY);
                    parent_id_of(stack, depth, item.parent_id, sizeof(item.parent_id));
                    breadcrumb(stack, depth, item.breadcrumb, sizeof(item.breadcrumb));
                    protected_by(stack, depth, item.protected_by, sizeof(item.protected_by));
                    snprintf(item.id_source, sizeof(item.id_source), "%s",
                             strncmp(f->id_ref, "~/", 2) == 0 ? "path" : "idref");
                    emit(ud, &item);
                } else if (f->is_argument) {
                    uipath_xaml_item item;
                    fill_item_from_frame(&item, f, UIP_XAML_ARGUMENT);
                    snprintf(item.name, sizeof(item.name), "%s", f->key_name);
                    snprintf(item.type_name, sizeof(item.type_name), "%s", f->type_arg);
                    item.naming_ok = 1;
                    if (strcmp(f->dir, "In") == 0 && strncmp(f->key_name, "in_", 3) != 0) {
                        item.naming_ok = 0;
                    }
                    if (strcmp(f->dir, "Out") == 0 && strncmp(f->key_name, "out_", 4) != 0) {
                        item.naming_ok = 0;
                    }
                    if (strcmp(f->dir, "InOut") == 0 && strncmp(f->key_name, "io_", 3) != 0) {
                        item.naming_ok = 0;
                    }
                    emit(ud, &item);
                } else if (f->is_variable) {
                    uipath_xaml_item item;
                    fill_item_from_frame(&item, f, UIP_XAML_VARIABLE);
                    snprintf(item.name, sizeof(item.name), "%s", f->key_name);
                    snprintf(item.type_name, sizeof(item.type_name), "%s", f->type_arg);
                    parent_id_of(stack, depth, item.parent_id, sizeof(item.parent_id));
                    emit(ud, &item);
                } else if (f->expr_carrier && depth > 0) {
                    if (f->expr_cs) {
                        saw_cs = 1;
                    } else if (f->expr_text[0]) {
                        saw_vb = 1;
                    }
                    attach_expr_facts(stack, depth);
                }
                ns_count = ns_mark[depth];
                depth--;
            }
            free_attrs(attrs, nattr);
            continue;
        }

        if (depth + 1 >= XAML_DEPTH) {
            parse_status = 2;
            free_attrs(attrs, nattr);
            break;
        }
        depth++;
        xframe *f = &stack[depth];
        memset(f, 0, sizeof(*f));
        f->start_line = start_line;
        ns_mark[depth] = ns_count;
        split_name(tname, f->prefix, sizeof(f->prefix), f->local, sizeof(f->local));
        for (int i = 0; i < nattr; i++) {
            if (strncmp(attrs[i].name, "xmlns", 5) == 0 && ns_count < XAML_NS * XAML_DEPTH) {
                const char *pref = "";
                if (attrs[i].name[5] == ':') {
                    pref = attrs[i].name + 6;
                }
                snprintf(ns[ns_count].prefix, sizeof(ns[ns_count].prefix), "%s", pref);
                snprintf(ns[ns_count].uri, sizeof(ns[ns_count].uri), "%s",
                         attrs[i].value ? attrs[i].value : "");
                ns_count++;
            }
        }
        const char *uri = ns_uri(ns, ns_count, f->prefix);
        const char *idref = attr_get(attrs, nattr, "IdRef");
        const char *disp = attr_get(attrs, nattr, "DisplayName");
        const char *ann = attr_get(attrs, nattr, "AnnotationText");
        const char *xname = attr_get(attrs, nattr, "Name");
        const char *xclass = attr_get(attrs, nattr, "Class");
        if (disp) {
            snprintf(f->display, sizeof(f->display), "%s", disp);
        }
        if (ann) {
            snprintf(f->annotation, sizeof(f->annotation), "%s", ann);
            if (root_annotation[0] == '\0') {
                snprintf(root_annotation, sizeof(root_annotation), "%s", ann);
            }
        }
        if (xclass && x_class[0] == '\0') {
            snprintf(x_class, sizeof(x_class), "%s", xclass);
        }
        if (xname && strncmp(attrs[0].name, "x:", 2) != 0) {
            /* x:Name is preferred; Name on Variable is the variable name. */
        }
        const char *xname_attr = NULL;
        for (int i = 0; i < nattr; i++) {
            if (strcmp(attrs[i].name, "x:Name") == 0 || strcmp(attrs[i].name, "Name") == 0) {
                xname_attr = attrs[i].value;
            }
        }
        if (xname_attr) {
            snprintf(f->x_name, sizeof(f->x_name), "%s", xname_attr);
            snprintf(f->key_name, sizeof(f->key_name), "%s", xname_attr);
        }
        const char *xkey = attr_get(attrs, nattr, "Key");
        if (xkey) {
            snprintf(f->key_name, sizeof(f->key_name), "%s", xkey);
        }
        const char *targ = attr_get(attrs, nattr, "TypeArguments");
        const char *xtype = attr_get(attrs, nattr, "Type");
        if (targ) {
            snprintf(f->type_arg, sizeof(f->type_arg), "%s", targ);
        } else if (xtype) {
            snprintf(f->type_arg, sizeof(f->type_arg), "%s", xtype);
        }
        if (depth > 0 && stack[depth - 1].child_slot[0]) {
            snprintf(f->slot, sizeof(f->slot), "%s", stack[depth - 1].child_slot);
        }
        map_clr(uri, f->local, f->clr, sizeof(f->clr), f->package, sizeof(f->package));

        int parent_skip = depth > 0 && stack[depth - 1].skip;
        int is_designer = designer_ns(uri, f->prefix, f->local);
        if (strcmp(f->local, "WorkflowViewState") == 0 && idref && depth > 0) {
            for (int i = depth - 1; i >= 0; i--) {
                if (stack[i].activity && stack[i].id_ref[0] == '\0') {
                    snprintf(stack[i].id_ref, sizeof(stack[i].id_ref), "%s", idref);
                    break;
                }
                if (stack[i].activity) {
                    break;
                }
            }
        }
        if (parent_skip || is_designer) {
            f->skip = 1;
        }
        int prop = strchr(f->local, '.') != NULL;
        if (prop) {
            f->property = 1;
            const char *dot = strchr(f->local, '.');
            if (dot && dot[1]) {
                snprintf(f->child_slot, sizeof(f->child_slot), "%s", dot + 1);
            }
        }
        if (strcmp(f->local, "Property") == 0 &&
            (strcmp(f->prefix, "x") == 0 || strstr(uri, "winfx/2006/xaml"))) {
            f->is_argument = 1;
            if (strncmp(f->type_arg, "InOutArgument", 13) == 0) {
                snprintf(f->dir, sizeof(f->dir), "InOut");
            } else if (strncmp(f->type_arg, "OutArgument", 11) == 0) {
                snprintf(f->dir, sizeof(f->dir), "Out");
            } else if (strncmp(f->type_arg, "InArgument", 10) == 0) {
                snprintf(f->dir, sizeof(f->dir), "In");
            } else {
                snprintf(f->dir, sizeof(f->dir), "In");
            }
        } else if (strcmp(f->local, "Variable") == 0 && !f->skip) {
            f->is_variable = 1;
            if (depth > 0) {
                stack[depth - 1].has_vars = 1;
            }
        } else if (expr_local(f->local)) {
            f->expr_carrier = 1;
            f->expr_cs = strncmp(f->local, "CSharp", 6) == 0;
            if (strncmp(f->local, "Out", 3) == 0) {
                snprintf(f->dir, sizeof(f->dir), "Out");
            } else if (strncmp(f->local, "InOut", 5) == 0) {
                snprintf(f->dir, sizeof(f->dir), "InOut");
            } else if (strncmp(f->local, "In", 2) == 0) {
                snprintf(f->dir, sizeof(f->dir), "In");
            }
            const char *et = attr_get(attrs, nattr, "ExpressionText");
            if (et) {
                snprintf(f->expr_text, sizeof(f->expr_text), "%s", et);
            }
        } else if (!f->skip && !prop && !(depth == 0 && strcmp(f->local, "Activity") == 0) &&
                   !xmeta_local(f->local) && !(strcmp(f->prefix, "x") == 0 && xmeta_local(f->local))) {
            int in_decl = strcmp(f->slot, "Variables") == 0 || strcmp(f->slot, "Members") == 0;
            if (!in_decl && activities < XAML_ACT_MAX) {
                f->activity = 1;
                activities++;
                if (depth > 0) {
                    f->ordinal = stack[depth - 1].next_ord++;
                }
                if (idref && idref[0]) {
                    snprintf(f->id_ref, sizeof(f->id_ref), "%s", idref);
                } else {
                    structural_id(stack, depth, f->id_ref, sizeof(f->id_ref));
                }
                if (seen_n < 4096 && seen_has(seen, seen_n, f->id_ref)) {
                    size_t L = strlen(f->id_ref);
                    if (L + 12 < sizeof(f->id_ref)) {
                        snprintf(f->id_ref + L, sizeof(f->id_ref) - L, "@%d", f->start_line);
                    }
                }
                if (seen_n < 4096) {
                    snprintf(seen[seen_n], 96, "%s", f->id_ref);
                    seen_n++;
                }
                int collapse = strcmp(f->local, "Sequence") == 0 && f->annotation[0] == '\0' &&
                               (wrapper_name(f->display) ||
                                (f->display[0] == '\0' && wrapper_name(f->slot)));
                f->collapsed = collapse;
                if (collapse) {
                    activities--;
                }
                if (!collapse) {
                    if (root_kind[0] == '\0' || strcmp(root_kind, "Activity") == 0) {
                        snprintf(root_kind, sizeof(root_kind), "%s", f->local);
                    }
                    if (skel_used + strlen(f->local) + 2 < sizeof(skeleton)) {
                        if (skel_used) {
                            skeleton[skel_used++] = ',';
                        }
                        memcpy(skeleton + skel_used, f->local, strlen(f->local));
                        skel_used += strlen(f->local);
                        skeleton[skel_used] = '\0';
                    }
                    note_resource_attrs(f, attrs, nattr);
                }
            } else if (activities >= XAML_ACT_MAX) {
                parse_status = 2;
            }
        }
        if (strcmp(f->slot, "Catches") == 0 && f->type_arg[0] && depth > 0) {
            for (int i = depth - 1; i >= 0; i--) {
                if (strcmp(stack[i].local, "TryCatch") == 0) {
                    snprintf(stack[i].catches, sizeof(stack[i].catches), "%s", f->type_arg);
                    char line[200];
                    snprintf(line, sizeof(line), "catch\t%s", f->type_arg);
                    fact_add(&stack[i], line);
                    break;
                }
            }
        }
        if (self_close) {
            f->end_line = xs.line;
            if (f->activity && !f->collapsed && !f->skip) {
                uipath_xaml_item item;
                fill_item_from_frame(&item, f, UIP_XAML_ACTIVITY);
                parent_id_of(stack, depth, item.parent_id, sizeof(item.parent_id));
                breadcrumb(stack, depth, item.breadcrumb, sizeof(item.breadcrumb));
                protected_by(stack, depth, item.protected_by, sizeof(item.protected_by));
                snprintf(item.id_source, sizeof(item.id_source), "%s",
                         strncmp(f->id_ref, "~/", 2) == 0 ? "path" : "idref");
                if (f->catches[0]) {
                    snprintf(item.catches, sizeof(item.catches), "%s", f->catches);
                }
                emit(ud, &item);
            } else if (f->is_argument || f->is_variable) {
                uipath_xaml_item item;
                fill_item_from_frame(&item, f, f->is_argument ? UIP_XAML_ARGUMENT : UIP_XAML_VARIABLE);
                snprintf(item.name, sizeof(item.name), "%s", f->key_name[0] ? f->key_name : f->local);
                snprintf(item.type_name, sizeof(item.type_name), "%s", f->type_arg);
                if (f->is_variable) {
                    parent_id_of(stack, depth, item.parent_id, sizeof(item.parent_id));
                }
                item.naming_ok = 1;
                emit(ud, &item);
            } else if (f->expr_carrier) {
                if (f->expr_cs) {
                    saw_cs = 1;
                } else if (f->expr_text[0]) {
                    saw_vb = 1;
                }
                attach_expr_facts(stack, depth);
            }
            ns_count = ns_mark[depth];
            depth--;
        }
        free_attrs(attrs, nattr);
    }

    /* Text nodes of expression carriers: a second lightweight pass is unnecessary
     * if ExpressionText was an attribute. Grab element text by scanning facts
     * already recorded. Bracket text inside elements is collected below when the
     * carrier frame is still open — handled in the end-tag path via expr_text
     * filled from attributes. Element body text is copied here only when the
     * scan left expr_text empty: walk is already finished, so body text was
     * skipped. Fill it during the main loop instead. */

    if (xs.trunc_attr && parse_status == 0) {
        parse_status = 1;
    }
    if (depth >= 0 && parse_status == 0) {
        parse_status = 1;
    }
    uipath_xaml_item meta;
    memset(&meta, 0, sizeof(meta));
    meta.kind = UIP_XAML_META;
    snprintf(meta.root_kind, sizeof(meta.root_kind), "%s", root_kind[0] ? root_kind : "Activity");
    snprintf(meta.x_class, sizeof(meta.x_class), "%s", x_class);
    snprintf(meta.annotation, sizeof(meta.annotation), "%s", root_annotation);
    snprintf(meta.facts, sizeof(meta.facts), "%s", skeleton);
    meta.activity_count = activities;
    meta.parse_status = parse_status;
    if (saw_vb && saw_cs) {
        snprintf(meta.expr_lang, sizeof(meta.expr_lang), "mixed");
    } else if (saw_cs) {
        snprintf(meta.expr_lang, sizeof(meta.expr_lang), "cs");
    } else {
        snprintf(meta.expr_lang, sizeof(meta.expr_lang), "vb");
    }
    emit(ud, &meta);
    free(stack);
    free(seen);
    free(ns);
    free(ns_mark);
    return 0;
}
