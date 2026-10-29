// cpu/pdftext.c — Nyx OS PDF Text Extractor
//
// Parses simple PDF-1.x files with uncompressed content streams and
// extracts readable text. Designed for the Nyx filesystem's 512-byte
// file limit — handles small PDFs (memos, notes, short documents).
//
// Limitations:
//   - Does NOT decompress FlateDecode / LZW streams
//   - Ignores encodings other than ASCII / PDFDocEncoding
//   - Does not reorder text by position; outputs in stream order
//   - Ignores images, annotations, forms, metadata, etc.
//
#include <pdftext.h>
#include <fs.h>
#include <stdint.h>

// Reusable buffer for extracting PDF stream data.
// Must be file-scope static — the kernel stack is only 16 KB and
// a local would overflow.  Capped to 64 KiB (PDF streams are small).
#define PDF_BUF_SIZE 65536
static char pdf_stream_buf[PDF_BUF_SIZE];

// ─── String helpers (self-contained, no stdlib) ──────────────────
static int p_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static int p_ncmp(const char* a, const char* b, int n) {
    for (int i=0; i<n; i++) if (a[i] != b[i]) return 1;
    return 0;
}

// ─── Skip whitespace / comments ──────────────────────────────────
static int p_isspace(char c) {
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';
}

static void skip_ws(const PdfDoc* doc, int* pos) {
    while (*pos < doc->size) {
        char c = doc->data[*pos];
        if (p_isspace(c)) { (*pos)++; continue; }
        if (c == '%') {
            while (*pos < doc->size && doc->data[*pos] != '\n') (*pos)++;
            continue;
        }
        break;
    }
}

// ─── PDF numeric parsing ─────────────────────────────────────────
static int parse_int(const PdfDoc* doc, int* pos, int* val) {
    skip_ws(doc, pos);
    if (*pos >= doc->size) return 0;
    int neg = 0;
    if (doc->data[*pos] == '-') { neg = 1; (*pos)++; }
    else if (doc->data[*pos] == '+') (*pos)++;
    if (*pos >= doc->size || doc->data[*pos] < '0' || doc->data[*pos] > '9')
        return 0;
    int v = 0;
    while (*pos < doc->size && doc->data[*pos] >= '0' && doc->data[*pos] <= '9') {
        v = v * 10 + (doc->data[*pos] - '0');
        (*pos)++;
    }
    *val = neg ? -v : v;
    return 1;
}

// ─── Compare a parsed name at (off, len) to a literal string ─────
static int name_eq(const PdfDoc* doc, int off, int len, const char* lit) {
    int i = 0;
    while (i < len && lit[i] && doc->data[off + i] == lit[i]) i++;
    return i == len && lit[i] == '\0';
}

// ─── Skip a complete PDF object value (dict, array, string, num) ─
static int skip_value(const PdfDoc* doc, int* pos) {
    skip_ws(doc, pos);
    if (*pos >= doc->size) return 0;
    char c = doc->data[*pos];

    // Dictionary
    if (c == '<' && *pos+1 < doc->size && doc->data[*pos+1] == '<') {
        *pos += 2;
        int depth = 1;
        while (*pos < doc->size && depth > 0) {
            skip_ws(doc, pos);
            if (*pos >= doc->size) break;
            if (doc->data[*pos] == '>' && *pos+1 < doc->size && doc->data[*pos+1] == '>') {
                depth--;
                *pos += 2;
            } else if (doc->data[*pos] == '<' && *pos+1 < doc->size && doc->data[*pos+1] == '<') {
                depth++;
                *pos += 2;
            } else {
                skip_value(doc, pos);
            }
        }
        return 1;
    }

    // Array
    if (c == '[') {
        (*pos)++;
        int depth = 1;
        while (*pos < doc->size && depth > 0) {
            skip_ws(doc, pos);
            if (*pos >= doc->size) break;
            if (doc->data[*pos] == ']') { depth--; (*pos)++; }
            else skip_value(doc, pos);
        }
        return 1;
    }

    // String
    if (c == '(') {
        int parens = 1;
        (*pos)++;
        while (*pos < doc->size && parens > 0) {
            if (doc->data[*pos] == '\\') { *pos += 2; continue; }
            if (doc->data[*pos] == '(') parens++;
            if (doc->data[*pos] == ')') parens--;
            (*pos)++;
        }
        return 1;
    }

    // Hex string
    if (c == '<') {
        while (*pos < doc->size && doc->data[*pos] != '>') (*pos)++;
        if (*pos < doc->size) (*pos)++;
        return 1;
    }

    // Number (int or real)
    if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.') {
        while (*pos < doc->size) {
            char cc = doc->data[*pos];
            if ((cc >= '0' && cc <= '9') || cc == '.' || cc == '-' || cc == '+' ||
                cc == 'e' || cc == 'E') (*pos)++;
            else break;
        }
        return 1;
    }

    // Name
    if (c == '/') {
        (*pos)++;
        while (*pos < doc->size) {
            char cc = doc->data[*pos];
            if (cc == '#' && *pos+2 < doc->size) { *pos += 3; continue; }
            if (cc == '/' || p_isspace(cc) || cc == '<' || cc == '>' ||
                cc == '[' || cc == ']' || cc == '(' || cc == ')' ||
                cc == '%') break;
            (*pos)++;
        }
        return 1;
    }

    // Keyword: null, true, false, R (indirect reference trailing)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        while (*pos < doc->size && ((doc->data[*pos] >= 'a' && doc->data[*pos] <= 'z') ||
                                     (doc->data[*pos] >= 'A' && doc->data[*pos] <= 'Z'))) (*pos)++;
        return 1;
    }

    return 0;
}

// ─── Find a dictionary value for a given key ─────────────────────
// Scans region [start, end) for /KeyName followed by a value.
// Returns 1 with *val_start and *val_end set.
static int find_dict_val(const PdfDoc* doc, int start, int end,
                          const char* key, int* val_start, int* val_end) {
    int klen = p_len(key);
    int pos = start;

    while (pos < end) {
        if (doc->data[pos] == '/') {
            int nstart = pos + 1;
            int nend = nstart;
            while (nend < end) {
                char c = doc->data[nend];
                if (c == '/' || p_isspace(c) || c == '<' || c == '>' ||
                    c == '[' || c == ']' || c == '(' || c == ')' || c == '%')
                    break;
                nend++;
            }
            int nlen = nend - nstart;
            if (nlen == klen && p_ncmp(doc->data + nstart, key, nlen) == 0) {
                pos = nend;
                *val_start = pos;
                skip_value(doc, &pos);
                *val_end = pos;
                return 1;
            }
            pos = nend;
        } else {
            pos++;
        }
    }
    return 0;
}

// ─── Read an indirect reference (N G R) at a position ────────────
// Returns obj_num or -1.
static int read_ref_at(const PdfDoc* doc, int pos) {
    skip_ws(doc, &pos);
    int obj = 0;
    if (!parse_int(doc, &pos, &obj)) return -1;
    int gen = 0; (void)gen;
    if (!parse_int(doc, &pos, &gen)) return -1;
    skip_ws(doc, &pos);
    if (pos + 1 < doc->size && doc->data[pos] == 'R') return obj;
    return -1;
}

// ─── Find all indirect objects in the PDF ────────────────────────
static void find_objects(PdfDoc* doc) {
    doc->object_count = 0;
    doc->root_obj = -1;
    doc->pages_obj = -1;
    int i = 0;
    while (i < doc->size - 6 && doc->object_count < PDF_MAX_OBJECTS) {
        int num = 0, gen = 0;
        int saved = i;

        // Try to parse object number
        if (doc->data[i] >= '0' && doc->data[i] <= '9') {
            int pos = i;
            while (pos < doc->size && doc->data[pos] >= '0' && doc->data[pos] <= '9') {
                num = num * 10 + (doc->data[pos] - '0');
                pos++;
            }
            if (pos >= doc->size || !p_isspace(doc->data[pos])) { i++; continue; }
            while (pos < doc->size && p_isspace(doc->data[pos])) pos++;

            gen = 0;
            while (pos < doc->size && doc->data[pos] >= '0' && doc->data[pos] <= '9') {
                gen = gen * 10 + (doc->data[pos] - '0');
                pos++;
            }
            if (pos >= doc->size || !p_isspace(doc->data[pos])) { i++; continue; }
            while (pos < doc->size && p_isspace(doc->data[pos])) pos++;

            if (pos + 2 < doc->size && doc->data[pos] == 'o' &&
                doc->data[pos+1] == 'b' && doc->data[pos+2] == 'j') {
                doc->objects[doc->object_count].obj_num = num;
                doc->objects[doc->object_count].gen_num = gen;
                doc->objects[doc->object_count].offset = saved;
                int endpos = pos + 3;
                while (endpos < doc->size - 5) {
                    if (doc->data[endpos] == 'e' && doc->data[endpos+1] == 'n' &&
                        doc->data[endpos+2] == 'd' && doc->data[endpos+3] == 'o' &&
                        doc->data[endpos+4] == 'b' && doc->data[endpos+5] == 'j') {
                        endpos += 6;
                        break;
                    }
                    endpos++;
                }
                doc->objects[doc->object_count].length = endpos - saved;
                doc->object_count++;
                i = endpos;
                continue;
            }
        }
        i++;
    }
}

// ─── Get the content between "obj" and "endobj" for object N ─────
static int get_object_body(const PdfDoc* doc, int obj_num, int* start, int* end) {
    for (int i = 0; i < doc->object_count; i++) {
        if (doc->objects[i].obj_num == obj_num) {
            int pos = doc->objects[i].offset;
            while (pos < doc->size) {
                if (pos + 2 < doc->size && doc->data[pos] == 'o' &&
                    doc->data[pos+1] == 'b' && doc->data[pos+2] == 'j') {
                    pos += 3;
                    break;
                }
                pos++;
            }
            *start = pos;
            *end = doc->objects[i].offset + doc->objects[i].length - 6;
            return 1;
        }
    }
    return 0;
}

// ─── Extract numeric reference from a value position ─────────────
static int value_as_ref(const PdfDoc* doc, int start, int end) {
    int pos = start;
    skip_ws(doc, &pos);
    int obj = 0;
    if (!parse_int(doc, &pos, &obj)) return -1;
    skip_ws(doc, &pos);
    int gen = 0; (void)gen;
    if (!parse_int(doc, &pos, &gen)) return -1;
    skip_ws(doc, &pos);
    if (pos < end && doc->data[pos] == 'R') return obj;
    return -1;
}

// ─── Check if a value at (start,end) looks like a dictionary ─────
static int is_dict(const PdfDoc* doc, int start, int end) {
    int pos = start;
    skip_ws(doc, &pos);
    return (pos + 1 < end && doc->data[pos] == '<' && doc->data[pos+1] == '<');
}

// ─── Extract stream data ────────────────────────────────────────
static int extract_stream_data(const PdfDoc* doc, int body_start, int body_end,
                                 char* out, int max_out) {
    int pos = body_start;

    // Skip the dict
    skip_ws(doc, &pos);
    if (pos + 1 < body_end && doc->data[pos] == '<' && doc->data[pos+1] == '<') {
        int depth = 1;
        pos += 2;
        while (pos < body_end && depth > 0) {
            skip_ws(doc, &pos);
            if (pos >= body_end) break;
            if (doc->data[pos] == '>' && pos+1 < body_end && doc->data[pos+1] == '>') {
                depth--;
                pos += 2;
            } else if (doc->data[pos] == '<' && pos+1 < body_end && doc->data[pos+1] == '<') {
                depth++;
                pos += 2;
            } else {
                skip_value(doc, &pos);
            }
        }
    }

    skip_ws(doc, &pos);
    if (pos + 5 >= body_end) return 0;
    if (doc->data[pos] != 's' || doc->data[pos+1] != 't' ||
        doc->data[pos+2] != 'r' || doc->data[pos+3] != 'e' ||
        doc->data[pos+4] != 'a' || doc->data[pos+5] != 'm')
        return 0;
    pos += 6;

    if (pos < body_end && doc->data[pos] == '\r') pos++;
    if (pos < body_end && doc->data[pos] == '\n') pos++;

    int data_start = pos;
    while (pos < body_end - 8) {
        if (doc->data[pos] == 'e' && doc->data[pos+1] == 'n' &&
            doc->data[pos+2] == 'd' && doc->data[pos+3] == 's' &&
            doc->data[pos+4] == 't' && doc->data[pos+5] == 'r' &&
            doc->data[pos+6] == 'e' && doc->data[pos+7] == 'a' &&
            doc->data[pos+8] == 'm') break;
        pos++;
    }
    int data_end = pos;
    while (data_end > data_start && p_isspace(doc->data[data_end-1]))
        data_end--;

    int len = data_end - data_start;
    if (len > max_out - 1) len = max_out - 1;
    for (int i = 0; i < len; i++) out[i] = doc->data[data_start + i];
    out[len] = '\0';
    return len;
}

// ─── Parse a content stream for text ─────────────────────────────
static void parse_content_stream(const char* stream, int stream_len,
                                  char* output, int* out_pos, int max_out) {
    int i = 0;
    while (i < stream_len && *out_pos < max_out - 1) {
        while (i < stream_len && p_isspace(stream[i])) i++;
        if (i >= stream_len) break;

        if (stream[i] == '(') {
            int str_start = i;
            int depth = 1;
            i++;
            while (i < stream_len && depth > 0) {
                if (stream[i] == '\\') { i += 2; continue; }
                if (stream[i] == '(') depth++;
                if (stream[i] == ')') depth--;
                i++;
            }

            while (i < stream_len && p_isspace(stream[i])) i++;

            if (i + 1 < stream_len && stream[i] == 'T' && stream[i+1] == 'j') {
                int spos = str_start + 1;
                int sdepth = 1;
                while (spos < i && sdepth > 0) {
                    if (stream[spos] == '\\') { spos += 2; continue; }
                    if (stream[spos] == '(') sdepth++;
                    if (stream[spos] == ')') sdepth--;
                    if (sdepth > 0) {
                        char c = stream[spos];
                        if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                            if (*out_pos < max_out - 1) output[(*out_pos)++] = c;
                        }
                    }
                    spos++;
                }
                i += 2;
            }
            else if (i < stream_len && stream[i] == '\'') {
                int spos = str_start + 1;
                int sdepth = 1;
                while (spos < i && sdepth > 0) {
                    if (stream[spos] == '\\') { spos += 2; continue; }
                    if (stream[spos] == '(') sdepth++;
                    if (stream[spos] == ')') sdepth--;
                    if (sdepth > 0) {
                        char c = stream[spos];
                        if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                            if (*out_pos < max_out - 1) output[(*out_pos)++] = c;
                        }
                    }
                    spos++;
                }
                if (*out_pos < max_out - 1) output[(*out_pos)++] = '\n';
                i++;
            }
        }
        else if (stream[i] == '[') {
            int arr_start = i;
            i++;
            while (i < stream_len && stream[i] != ']') {
                if (stream[i] == '\\') { i += 2; continue; }
                if (stream[i] == '(') {
                    i++;
                    while (i < stream_len && stream[i] != ')') {
                        if (stream[i] == '\\') { i += 2; continue; }
                        i++;
                    }
                    if (i < stream_len) i++;
                } else {
                    i++;
                }
            }
            if (i < stream_len) i++;

            while (i < stream_len && p_isspace(stream[i])) i++;
            if (i + 1 < stream_len && stream[i] == 'T' && stream[i+1] == 'J') {
                int apos = arr_start + 1;
                while (apos < i && stream[apos] != ']') {
                    while (apos < i && p_isspace(stream[apos])) apos++;
                    if (apos >= i || stream[apos] == ']') break;
                    if (stream[apos] == '(') {
                        apos++;
                        while (apos < i && stream[apos] != ')') {
                            if (stream[apos] == '\\') { apos += 2; continue; }
                            char c = stream[apos];
                            if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                                if (*out_pos < max_out - 1) output[(*out_pos)++] = c;
                            }
                            apos++;
                        }
                        if (apos < i) apos++;
                    } else {
                        while (apos < i) {
                            char c = stream[apos];
                            if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9') ||
                                c == 'e' || c == 'E') apos++;
                            else break;
                        }
                    }
                }
                i += 2;
            }
        }
        else if (stream[i] == '"') {
            i++;
            while (i < stream_len && p_isspace(stream[i])) i++;
            while (i < stream_len && (stream[i] == '-' || stream[i] == '+' || stream[i] == '.' ||
                   (stream[i] >= '0' && stream[i] <= '9'))) i++;
            while (i < stream_len && p_isspace(stream[i])) i++;
            while (i < stream_len && (stream[i] == '-' || stream[i] == '+' || stream[i] == '.' ||
                   (stream[i] >= '0' && stream[i] <= '9'))) i++;
            while (i < stream_len && p_isspace(stream[i])) i++;
            if (i < stream_len && stream[i] == '(') {
                i++;
                while (i < stream_len && stream[i] != ')') {
                    if (stream[i] == '\\') { i += 2; continue; }
                    char c = stream[i];
                    if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                        if (*out_pos < max_out - 1) output[(*out_pos)++] = c;
                    }
                    i++;
                }
                if (i < stream_len) i++;
            }
            if (*out_pos < max_out - 1) output[(*out_pos)++] = '\n';
        }
        else {
            i++;
        }
    }
}

// ─── Main extraction entry point ─────────────────────────────────
int pdftext_extract(const char* pdf_data, int data_size, char* output, int max_out) {
    if (!pdf_data || data_size < 10 || !output || max_out < 1) return -1;
    output[0] = '\0';

    PdfDoc doc;
    doc.data = pdf_data;
    doc.size = data_size;
    find_objects(&doc);

    if (doc.object_count == 0) return -1;

    // Check for valid PDF header
    if (pdf_data[0] != '%' || pdf_data[1] != 'P' || pdf_data[2] != 'D' ||
        pdf_data[3] != 'F') {
        return -1;
    }

    // Find Root/Catalog and Pages objects
    int pages_obj = -1;

    for (int i = 0; i < doc.object_count; i++) {
        int body_start, body_end;
        if (!get_object_body(&doc, doc.objects[i].obj_num, &body_start, &body_end))
            continue;

        if (is_dict(&doc, body_start, body_end)) {
            int v_start, v_end;

            if (find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) {
                int pos2 = v_start;
                skip_ws(&doc, &pos2);
                if (pos2 < v_end && doc.data[pos2] == '/') {
                    pos2++;
                    int ns = pos2;
                    while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                            (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
                    int nlen = pos2 - ns;

                    if (name_eq(&doc, ns, nlen, "Catalog")) {
                        if (find_dict_val(&doc, body_start, body_end, "Pages", &v_start, &v_end)) {
                            int ref = value_as_ref(&doc, v_start, v_end);
                            if (ref > 0) pages_obj = ref;
                        }
                    }
                    else if (name_eq(&doc, ns, nlen, "Pages")) {
                        pages_obj = doc.objects[i].obj_num;
                    }
                }
            }
        }
    }

    // Fallback: scan all objects for /Type /Pages
    if (pages_obj < 0) {
        for (int i = 0; i < doc.object_count; i++) {
            int body_start, body_end;
            if (!get_object_body(&doc, doc.objects[i].obj_num, &body_start, &body_end))
                continue;
            if (is_dict(&doc, body_start, body_end)) {
                int v_start, v_end;
                if (find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) {
                    int pos2 = v_start;
                    skip_ws(&doc, &pos2);
                    if (pos2 < v_end && doc.data[pos2] == '/') {
                        pos2++;
                        int ns = pos2;
                        while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                                (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
                        if (name_eq(&doc, ns, pos2 - ns, "Pages")) {
                            pages_obj = doc.objects[i].obj_num;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (pages_obj < 0) return -1;

    // Collect all page object numbers via the page tree
    int page_objs[32];
    int page_count = 0;
    int stack[32];
    int stack_top = 0;
    int visited[PDF_MAX_OBJECTS];
    for (int i = 0; i < PDF_MAX_OBJECTS; i++) visited[i] = 0;

    stack[stack_top++] = pages_obj;

    while (stack_top > 0) {
        int obj_num = stack[--stack_top];
        if (obj_num < 0 || obj_num >= PDF_MAX_OBJECTS) continue;
        if (visited[obj_num]) continue;
        visited[obj_num] = 1;

        int body_start, body_end;
        if (!get_object_body(&doc, obj_num, &body_start, &body_end)) continue;
        if (!is_dict(&doc, body_start, body_end)) continue;

        int v_start, v_end;
        if (!find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) continue;

        int pos2 = v_start;
        skip_ws(&doc, &pos2);
        if (pos2 >= v_end || doc.data[pos2] != '/') continue;
        pos2++;
        int ns = pos2;
        while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
        int tlen = pos2 - ns;

        if (name_eq(&doc, ns, tlen, "Pages")) {
            if (find_dict_val(&doc, body_start, body_end, "Kids", &v_start, &v_end)) {
                int kpos = v_start;
                skip_ws(&doc, &kpos);
                if (kpos < v_end && doc.data[kpos] == '[') {
                    kpos++;
                    while (kpos < v_end && doc.data[kpos] != ']') {
                        int ref = read_ref_at(&doc, kpos);
                        if (ref > 0 && stack_top < 32) stack[stack_top++] = ref;
                        skip_value(&doc, &kpos);
                    }
                }
            }
        } else if (name_eq(&doc, ns, tlen, "Page")) {
            if (page_count < 32) page_objs[page_count++] = obj_num;
        }
    }

    if (page_count == 0) return -1;

    // Extract text from each page's content streams
    int out_pos = 0;

    for (int pi = 0; pi < page_count && out_pos < max_out - 1; pi++) {
        int body_start, body_end;
        if (!get_object_body(&doc, page_objs[pi], &body_start, &body_end))
            continue;
        if (!is_dict(&doc, body_start, body_end)) continue;

        if (page_count > 1 && out_pos < max_out - 20) {
            // Draw styled page separator: "\n── Page N ──\n"
            output[out_pos++] = '\n';
            output[out_pos++] = 0xC4;  // ─
            output[out_pos++] = 0xC4;  // ─
            output[out_pos++] = ' ';
            output[out_pos++] = 'P';
            output[out_pos++] = 'a';
            output[out_pos++] = 'g';
            output[out_pos++] = 'e';
            output[out_pos++] = ' ';
            // Page number (pi starts at 0, so page = pi + 1)
            int pn = pi + 1;
            char num_buf[8];
            int np = 0;
            if (pn == 0) { num_buf[np++] = '0'; }
            else {
                while (pn > 0 && np < 6) { num_buf[np++] = '0' + (pn % 10); pn /= 10; }
            }
            for (int ni = np - 1; ni >= 0; ni--)
                output[out_pos++] = num_buf[ni];
            output[out_pos++] = ' ';
            output[out_pos++] = 0xC4;  // ─
            output[out_pos++] = 0xC4;  // ─
            output[out_pos++] = '\n';
        }

        int v_start, v_end;
        if (find_dict_val(&doc, body_start, body_end, "Contents", &v_start, &v_end)) {
            int ref = read_ref_at(&doc, v_start);
            if (ref > 0) {
                int sv = 0;
                while (ref >= 0 && sv < doc.object_count) {
                    if (doc.objects[sv].obj_num == ref) break;
                    sv++;
                }
                if (sv < doc.object_count) {
                    int sbody_start, sbody_end;
                    if (get_object_body(&doc, ref, &sbody_start, &sbody_end)) {
                        int stream_len = extract_stream_data(&doc,
                            sbody_start, sbody_end,
                            pdf_stream_buf, PDF_BUF_SIZE);
                        if (stream_len > 0) {
                            parse_content_stream(pdf_stream_buf, stream_len, output, &out_pos, max_out);
                        }
                    }
                }
            } else {
                int arr_pos = v_start;
                skip_ws(&doc, &arr_pos);
                if (arr_pos < v_end && doc.data[arr_pos] == '[') {
                    arr_pos++;
                    while (arr_pos < v_end && doc.data[arr_pos] != ']') {
                        int aref = read_ref_at(&doc, arr_pos);
                        if (aref > 0) {
                            int sbody_start, sbody_end;
                            if (get_object_body(&doc, aref, &sbody_start, &sbody_end)) {
                                int stream_len = extract_stream_data(&doc,
                                    sbody_start, sbody_end,
                                    pdf_stream_buf, PDF_BUF_SIZE);
                                if (stream_len > 0) {
                                    parse_content_stream(pdf_stream_buf, stream_len, output, &out_pos, max_out);
                                }
                            }
                        }
                        skip_value(&doc, &arr_pos);
                    }
                }
            }
        }
    }

    output[out_pos] = '\0';
    return out_pos;
}

// ─── Lightweight page counter ────────────────────────────────────
// Returns page count, or -1 if the PDF cannot be parsed.
int pdftext_get_page_count(const char* pdf_data, int data_size) {
    if (!pdf_data || data_size < 10) return -1;

    // Check PDF header
    if (pdf_data[0] != '%' || pdf_data[1] != 'P' || pdf_data[2] != 'D' ||
        pdf_data[3] != 'F') {
        return -1;
    }

    PdfDoc doc;
    doc.data = pdf_data;
    doc.size = data_size;
    find_objects(&doc);
    if (doc.object_count == 0) return -1;

    // Find Pages object via Catalog or direct scan
    int pages_obj = -1;
    for (int i = 0; i < doc.object_count; i++) {
        int body_start, body_end;
        if (!get_object_body(&doc, doc.objects[i].obj_num, &body_start, &body_end))
            continue;
        if (!is_dict(&doc, body_start, body_end)) continue;

        int v_start, v_end;
        if (find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) {
            int pos2 = v_start;
            skip_ws(&doc, &pos2);
            if (pos2 < v_end && doc.data[pos2] == '/') {
                pos2++;
                int ns = pos2;
                while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                        (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
                int nlen = pos2 - ns;

                if (name_eq(&doc, ns, nlen, "Catalog")) {
                    if (find_dict_val(&doc, body_start, body_end, "Pages", &v_start, &v_end)) {
                        int ref = value_as_ref(&doc, v_start, v_end);
                        if (ref > 0) { pages_obj = ref; break; }
                    }
                } else if (name_eq(&doc, ns, nlen, "Pages")) {
                    pages_obj = doc.objects[i].obj_num;
                }
            }
        }
    }

    // Fallback: scan all objects for /Type /Pages
    if (pages_obj < 0) {
        for (int i = 0; i < doc.object_count; i++) {
            int body_start, body_end;
            if (!get_object_body(&doc, doc.objects[i].obj_num, &body_start, &body_end))
                continue;
            if (!is_dict(&doc, body_start, body_end)) continue;
            int v_start, v_end;
            if (find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) {
                int pos2 = v_start;
                skip_ws(&doc, &pos2);
                if (pos2 < v_end && doc.data[pos2] == '/') {
                    pos2++;
                    int ns = pos2;
                    while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                            (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
                    if (name_eq(&doc, ns, pos2 - ns, "Pages")) {
                        pages_obj = doc.objects[i].obj_num;
                        break;
                    }
                }
            }
        }
    }

    if (pages_obj < 0) return -1;

    // Traverse page tree
    int page_count = 0;
    int stack[32];
    int stack_top = 0;
    int visited[PDF_MAX_OBJECTS];
    for (int i = 0; i < PDF_MAX_OBJECTS; i++) visited[i] = 0;

    stack[stack_top++] = pages_obj;

    while (stack_top > 0) {
        int obj_num = stack[--stack_top];
        if (obj_num < 0 || obj_num >= PDF_MAX_OBJECTS) continue;
        if (visited[obj_num]) continue;
        visited[obj_num] = 1;

        int body_start, body_end;
        if (!get_object_body(&doc, obj_num, &body_start, &body_end)) continue;
        if (!is_dict(&doc, body_start, body_end)) continue;

        int v_start, v_end;
        if (!find_dict_val(&doc, body_start, body_end, "Type", &v_start, &v_end)) continue;

        int pos2 = v_start;
        skip_ws(&doc, &pos2);
        if (pos2 >= v_end || doc.data[pos2] != '/') continue;
        pos2++;
        int ns = pos2;
        while (pos2 < v_end && ((doc.data[pos2] >= 'a' && doc.data[pos2] <= 'z') ||
                                (doc.data[pos2] >= 'A' && doc.data[pos2] <= 'Z'))) pos2++;
        int tlen = pos2 - ns;

        if (name_eq(&doc, ns, tlen, "Pages")) {
            if (find_dict_val(&doc, body_start, body_end, "Kids", &v_start, &v_end)) {
                int kpos = v_start;
                skip_ws(&doc, &kpos);
                if (kpos < v_end && doc.data[kpos] == '[') {
                    kpos++;
                    while (kpos < v_end && doc.data[kpos] != ']') {
                        int ref = read_ref_at(&doc, kpos);
                        if (ref > 0 && stack_top < 32) stack[stack_top++] = ref;
                        skip_value(&doc, &kpos);
                    }
                }
            }
        } else if (name_eq(&doc, ns, tlen, "Page")) {
            page_count++;
        }
    }

    return page_count > 0 ? page_count : -1;
}
