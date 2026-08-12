// File Storage B+ Tree (BPT) key-value database
// - Keys: strings up to 64 bytes, Values: int32
// - Entries ordered by (key, value); same key may have many values (unique per key)
// - Data persisted in file "bpt.dat" (survives across runs; judge handles cleanup)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

// ---------------- Constants ----------------
static const uint32_t BLOCK_SZ = 8192;
static const uint32_t LEAF_T = 1;
static const uint32_t INTERNAL_T = 2;
static const int KEY_SZ = 68;      // 64 bytes max key + safety margin
static const int MAX_L = 110;      // max entries per leaf
static const int MIN_L = 55;       // min entries in non-root leaf
static const int MAX_I = 105;      // max separators per internal node
static const int MIN_I = 52;       // min separators in non-root internal

struct Entry { char key[KEY_SZ]; int32_t val; };

struct LeafNode {
    uint32_t type, count, next, pad;
    Entry e[MAX_L];
};
struct InternalNode {
    uint32_t type, count;
    uint32_t ch[MAX_I + 1];
    Entry sep[MAX_I];
};

static_assert(sizeof(LeafNode) <= BLOCK_SZ, "leaf node too big");
static_assert(sizeof(InternalNode) <= BLOCK_SZ, "internal node too big");
static_assert(sizeof(Entry) == 72, "entry size check");

// ---------------- Fast input ----------------
static char ibuf[1 << 20];
static size_t ipos = 0, ilen = 0;
static inline int gc() {
    if (ipos == ilen) {
        ilen = fread(ibuf, 1, sizeof(ibuf), stdin);
        ipos = 0;
        if (ilen == 0) return -1;
    }
    return (unsigned char)ibuf[ipos++];
}
static inline bool isws(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == -1; }
static long long read_ll(bool* ok = nullptr) {
    int c;
    do { c = gc(); } while (isws(c));
    if (ok) *ok = (c != -1);
    int sign = 1;
    if (c == '-') { sign = -1; c = gc(); }
    long long v = 0;
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = gc(); }
    return sign * v;
}
static inline int read_int() { return (int)read_ll(); }
static void read_token(char* out, int cap) {
    int c;
    do { c = gc(); } while (isws(c));
    int i = 0;
    while (!isws(c)) {
        if (i < cap - 1) out[i++] = (char)c;
        c = gc();
    }
    out[i] = 0;
}

// ---------------- Fast output ----------------
static char obuf[1 << 20];
static size_t opos = 0;
static inline void oflush() { if (opos) { fwrite(obuf, 1, opos, stdout); opos = 0; } }
static inline void oputc(char c) { if (opos == sizeof(obuf)) oflush(); obuf[opos++] = c; }
static void oint(int32_t v) {
    long long w = v;
    if (w == 0) { oputc('0'); return; }
    if (w < 0) { oputc('-'); w = -w; }
    char tmp[24]; int n = 0;
    while (w) { tmp[n++] = (char)('0' + w % 10); w /= 10; }
    while (n) oputc(tmp[--n]);
}

// ---------------- Buffer pool ----------------
static const int FRAMES = 4096;                 // 4096 * 8KB = 32MB cache
struct Frame {
    uint32_t block;
    bool dirty;
    Frame *prev, *next;
    unsigned char data[BLOCK_SZ];
};
static Frame* frames = nullptr;
static Frame* head = nullptr;                   // MRU
static Frame* tail = nullptr;                   // LRU
static Frame* freeList = nullptr;
static std::unordered_map<uint32_t, Frame*> idx;

static void detach(Frame* f) {
    if (f->prev) f->prev->next = f->next; else head = f->next;
    if (f->next) f->next->prev = f->prev; else tail = f->prev;
    f->prev = f->next = nullptr;
}
static void push_front(Frame* f) {
    f->prev = nullptr; f->next = head;
    if (head) head->prev = f; else tail = f;
    head = f;
}
static void init_frames() {
    frames = (Frame*)malloc(sizeof(Frame) * FRAMES);
    if (!frames) { exit(1); }
    freeList = nullptr;
    for (int i = FRAMES - 1; i >= 0; --i) {
        frames[i].block = 0xFFFFFFFFu;
        frames[i].dirty = false;
        frames[i].prev = nullptr;
        frames[i].next = freeList;
        freeList = &frames[i];
    }
    idx.reserve(8192);
    idx.max_load_factor(0.7f);
}

static int dbfd = -1;
static inline void io_read(uint32_t block, void* buf) {
    pread(dbfd, buf, BLOCK_SZ, (off_t)block * BLOCK_SZ);
}
static inline void io_write(uint32_t block, const void* buf) {
    pwrite(dbfd, buf, BLOCK_SZ, (off_t)block * BLOCK_SZ);
}

static Frame* get_frame(uint32_t block) {
    auto it = idx.find(block);
    if (it != idx.end()) {
        Frame* f = it->second;
        detach(f); push_front(f);
        return f;
    }
    Frame* f;
    if (freeList) {
        f = freeList; freeList = f->next;
    } else {
        f = tail; detach(f);
        if (f->dirty) { io_write(f->block, f->data); f->dirty = false; }
        idx.erase(f->block);
    }
    f->block = block;
    io_read(block, f->data);
    idx[block] = f;
    push_front(f);
    return f;
}

// ---------------- Header / global state ----------------
static const char MAGIC[8] = {'B','P','T','D','B','0','0','1'};
struct Header { char magic[8]; uint32_t root, total, free_head; uint32_t pad[4]; };
static uint32_t ROOT = 0, TOTAL = 0, FREEHEAD = 0;
static const char* DB_PATH = "bpt.dat";

static uint32_t alloc_block() {
    if (FREEHEAD) {
        uint32_t b = FREEHEAD;
        Frame* f = get_frame(b);
        FREEHEAD = *(uint32_t*)f->data;
        return b;
    }
    return TOTAL++;
}
static void free_block(uint32_t b) {
    Frame* f = get_frame(b);
    *(uint32_t*)f->data = FREEHEAD;
    f->dirty = true;
    FREEHEAD = b;
}

static void write_header() {
    Frame* f = get_frame(0);
    Header* h = (Header*)f->data;
    memcpy(h->magic, MAGIC, 8);
    h->root = ROOT; h->total = TOTAL; h->free_head = FREEHEAD;
    h->pad[0] = h->pad[1] = h->pad[2] = h->pad[3] = 0;
    f->dirty = true;
}

static void open_db() {
    dbfd = open(DB_PATH, O_RDWR | O_CREAT, 0644);
    if (dbfd < 0) { exit(1); }
    struct stat st;
    if (fstat(dbfd, &st) != 0) st.st_size = 0;
    bool ok = false;
    if (st.st_size >= (off_t)BLOCK_SZ) {
        Frame* f = get_frame(0);
        Header* h = (Header*)f->data;
        if (memcmp(h->magic, MAGIC, 8) == 0 && h->root != 0 && h->total > h->root) {
            ROOT = h->root; TOTAL = h->total; FREEHEAD = h->free_head;
            ok = true;
        }
    }
    if (!ok) {
        ftruncate(dbfd, 0);
        TOTAL = 1; FREEHEAD = 0;
        uint32_t rb = alloc_block();   // block 1: empty root leaf
        Frame* f = get_frame(rb);
        memset(f->data, 0, sizeof(LeafNode));
        LeafNode* ln = (LeafNode*)f->data;
        ln->type = LEAF_T; ln->count = 0; ln->next = 0;
        f->dirty = true;
        ROOT = rb;
        write_header();
    }
}

static void close_db() {
    write_header();
    for (Frame* f = head; f; f = f->next) {
        if (f->dirty) { io_write(f->block, f->data); f->dirty = false; }
    }
    close(dbfd);
}

// ---------------- Core helpers ----------------
static inline int cmp_entry(const char* k1, int32_t v1, const char* k2, int32_t v2) {
    int c = strcmp(k1, k2);
    if (c) return c < 0 ? -1 : 1;
    return (v1 > v2) - (v1 < v2);
}

// path from root to current node during insert/delete
static uint32_t pathB[32];
static int pathPos[32];
static int DEPTH = 0;

// ---------------- Insert ----------------
static void insert_up(int level, uint32_t rchild, const Entry& sep) {
    if (level < 0) {
        uint32_t nb = alloc_block();
        Frame* f = get_frame(nb);
        InternalNode* nn = (InternalNode*)f->data;
        nn->type = INTERNAL_T; nn->count = 1;
        nn->ch[0] = ROOT; nn->ch[1] = rchild;
        nn->sep[0] = sep;
        f->dirty = true;
        ROOT = nb;
        return;
    }
    uint32_t pb = pathB[level];
    int pos = pathPos[level];
    Frame* f = get_frame(pb);
    InternalNode* in = (InternalNode*)f->data;
    Entry tmpS[MAX_I + 1];
    uint32_t tmpC[MAX_I + 2];
    memcpy(tmpS, in->sep, sizeof(Entry) * in->count);
    memcpy(tmpC, in->ch, sizeof(uint32_t) * (in->count + 1));
    for (int i = in->count; i > pos; --i) tmpS[i] = tmpS[i - 1];
    tmpS[pos] = sep;
    for (int i = in->count + 1; i > pos + 1; --i) tmpC[i] = tmpC[i - 1];
    tmpC[pos + 1] = rchild;
    int n = in->count + 1;
    if (n <= MAX_I) {
        memcpy(in->sep, tmpS, sizeof(Entry) * n);
        memcpy(in->ch, tmpC, sizeof(uint32_t) * (n + 1));
        in->count = n;
        f->dirty = true;
        return;
    }
    int mid = n / 2;
    Entry up = tmpS[mid];
    memcpy(in->sep, tmpS, sizeof(Entry) * mid);
    memcpy(in->ch, tmpC, sizeof(uint32_t) * (mid + 1));
    in->count = mid;
    f->dirty = true;
    uint32_t rb = alloc_block();
    Frame* rf = get_frame(rb);
    InternalNode* rn = (InternalNode*)rf->data;
    rn->type = INTERNAL_T;
    int nrk = n - mid - 1;
    rn->count = nrk;
    memcpy(rn->sep, tmpS + mid + 1, sizeof(Entry) * nrk);
    memcpy(rn->ch, tmpC + mid + 1, sizeof(uint32_t) * (nrk + 1));
    rf->dirty = true;
    insert_up(level - 1, rb, up);
}

static void do_insert(const char* key, int32_t val) {
    uint32_t cur = ROOT;
    DEPTH = 0;
    for (;;) {
        Frame* f = get_frame(cur);
        uint32_t t = *(uint32_t*)f->data;
        if (t == INTERNAL_T) {
            InternalNode* in = (InternalNode*)f->data;
            int lo = 0, hi = (int)in->count;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cmp_entry(key, val, in->sep[mid].key, in->sep[mid].val) < 0) hi = mid;
                else lo = mid + 1;
            }
            pathB[DEPTH] = cur; pathPos[DEPTH] = lo; DEPTH++;
            cur = in->ch[lo];
        } else {
            LeafNode* ln = (LeafNode*)f->data;
            int lo = 0, hi = (int)ln->count;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cmp_entry(key, val, ln->e[mid].key, ln->e[mid].val) <= 0) hi = mid;
                else lo = mid + 1;
            }
            if (lo < (int)ln->count && ln->e[lo].val == val && strcmp(ln->e[lo].key, key) == 0)
                return; // duplicate (index,value): ignore
            pathB[DEPTH] = cur; pathPos[DEPTH] = lo; DEPTH++;
            Entry tmp[MAX_L + 1];
            memcpy(tmp, ln->e, sizeof(Entry) * ln->count);
            for (int i = ln->count; i > lo; --i) tmp[i] = tmp[i - 1];
            strcpy(tmp[lo].key, key);
            tmp[lo].val = val;
            int n = ln->count + 1;
            if (n <= MAX_L) {
                memcpy(ln->e, tmp, sizeof(Entry) * n);
                ln->count = n;
                f->dirty = true;
                return;
            }
            int nl = n / 2, nr = n - nl;
            uint32_t oldNext = ln->next;
            memcpy(ln->e, tmp, sizeof(Entry) * nl);
            ln->count = nl;
            f->dirty = true;
            uint32_t rb = alloc_block();
            ln->next = rb;
            Frame* rf = get_frame(rb);
            LeafNode* rn = (LeafNode*)rf->data;
            rn->type = LEAF_T; rn->count = nr; rn->next = oldNext; rn->pad = 0;
            memcpy(rn->e, tmp + nl, sizeof(Entry) * nr);
            rf->dirty = true;
            insert_up(DEPTH - 2, rb, tmp[nl]);
            return;
        }
    }
}

// ---------------- Delete ----------------
static void rebalance_after_delete() {
    int level = DEPTH - 1;
    for (;;) {
        bool isLeaf = (level == DEPTH - 1);
        if (level == 0) {
            if (!isLeaf) {
                Frame* f = get_frame(ROOT);
                InternalNode* in = (InternalNode*)f->data;
                if (in->count == 0) {
                    uint32_t nr = in->ch[0];
                    free_block(ROOT);
                    ROOT = nr;
                }
            }
            return;
        }
        uint32_t b = pathB[level];
        Frame* f = get_frame(b);
        int cnt;
        if (isLeaf) cnt = (int)((LeafNode*)f->data)->count;
        else cnt = (int)((InternalNode*)f->data)->count;
        bool under = isLeaf ? (cnt < MIN_L) : (cnt < MIN_I);
        if (!under) return;
        uint32_t pb = pathB[level - 1];
        int pos = pathPos[level - 1];
        Frame* pf = get_frame(pb);
        InternalNode* P = (InternalNode*)pf->data;
        if (pos > 0) {
            uint32_t sb = P->ch[pos - 1];
            Frame* sf = get_frame(sb);
            if (isLeaf) {
                LeafNode* S = (LeafNode*)sf->data;
                LeafNode* L = (LeafNode*)f->data;
                if (S->count > MIN_L) {
                    for (int i = L->count; i > 0; --i) L->e[i] = L->e[i - 1];
                    L->e[0] = S->e[S->count - 1];
                    L->count++; S->count--;
                    P->sep[pos - 1] = L->e[0];
                    sf->dirty = true; f->dirty = true; pf->dirty = true;
                    return;
                }
                memcpy(S->e + S->count, L->e, sizeof(Entry) * L->count);
                S->count += L->count;
                S->next = L->next;
                sf->dirty = true;
                for (int i = pos - 1; i + 1 < (int)P->count; ++i) P->sep[i] = P->sep[i + 1];
                for (int i = pos; i < (int)P->count; ++i) P->ch[i] = P->ch[i + 1];
                P->count--;
                pf->dirty = true;
                f->dirty = false;
                free_block(b);
            } else {
                InternalNode* S = (InternalNode*)sf->data;
                InternalNode* N = (InternalNode*)f->data;
                if (S->count > MIN_I) {
                    for (int i = N->count; i > 0; --i) N->sep[i] = N->sep[i - 1];
                    for (int i = N->count + 1; i > 0; --i) N->ch[i] = N->ch[i - 1];
                    N->sep[0] = P->sep[pos - 1];
                    N->ch[0] = S->ch[S->count];
                    N->count++;
                    P->sep[pos - 1] = S->sep[S->count - 1];
                    S->count--;
                    sf->dirty = true; f->dirty = true; pf->dirty = true;
                    return;
                }
                S->sep[S->count] = P->sep[pos - 1];
                memcpy(S->sep + S->count + 1, N->sep, sizeof(Entry) * N->count);
                memcpy(S->ch + S->count + 1, N->ch, sizeof(uint32_t) * (N->count + 1));
                S->count += N->count + 1;
                sf->dirty = true;
                for (int i = pos - 1; i + 1 < (int)P->count; ++i) P->sep[i] = P->sep[i + 1];
                for (int i = pos; i < (int)P->count; ++i) P->ch[i] = P->ch[i + 1];
                P->count--;
                pf->dirty = true;
                f->dirty = false;
                free_block(b);
            }
        } else {
            uint32_t sb = P->ch[pos + 1];
            Frame* sf = get_frame(sb);
            if (isLeaf) {
                LeafNode* S = (LeafNode*)sf->data;
                LeafNode* L = (LeafNode*)f->data;
                if (S->count > MIN_L) {
                    L->e[L->count] = S->e[0];
                    L->count++;
                    for (int i = 1; i < (int)S->count; ++i) S->e[i - 1] = S->e[i];
                    S->count--;
                    P->sep[pos] = S->e[0];
                    sf->dirty = true; f->dirty = true; pf->dirty = true;
                    return;
                }
                memcpy(L->e + L->count, S->e, sizeof(Entry) * S->count);
                L->count += S->count;
                L->next = S->next;
                f->dirty = true;
                for (int i = pos; i + 1 < (int)P->count; ++i) P->sep[i] = P->sep[i + 1];
                for (int i = pos + 1; i < (int)P->count; ++i) P->ch[i] = P->ch[i + 1];
                P->count--;
                pf->dirty = true;
                sf->dirty = false;
                free_block(sb);
            } else {
                InternalNode* S = (InternalNode*)sf->data;
                InternalNode* N = (InternalNode*)f->data;
                if (S->count > MIN_I) {
                    N->sep[N->count] = P->sep[pos];
                    N->ch[N->count + 1] = S->ch[0];
                    N->count++;
                    P->sep[pos] = S->sep[0];
                    for (int i = 1; i < (int)S->count; ++i) S->sep[i - 1] = S->sep[i];
                    for (int i = 1; i <= (int)S->count; ++i) S->ch[i - 1] = S->ch[i];
                    S->count--;
                    sf->dirty = true; f->dirty = true; pf->dirty = true;
                    return;
                }
                N->sep[N->count] = P->sep[pos];
                memcpy(N->sep + N->count + 1, S->sep, sizeof(Entry) * S->count);
                memcpy(N->ch + N->count + 1, S->ch, sizeof(uint32_t) * (S->count + 1));
                N->count += S->count + 1;
                f->dirty = true;
                for (int i = pos; i + 1 < (int)P->count; ++i) P->sep[i] = P->sep[i + 1];
                for (int i = pos + 1; i < (int)P->count; ++i) P->ch[i] = P->ch[i + 1];
                P->count--;
                pf->dirty = true;
                sf->dirty = false;
                free_block(sb);
            }
        }
        level--;
    }
}

static void do_delete(const char* key, int32_t val) {
    uint32_t cur = ROOT;
    DEPTH = 0;
    Frame* lf = nullptr;
    for (;;) {
        Frame* f = get_frame(cur);
        uint32_t t = *(uint32_t*)f->data;
        if (t == INTERNAL_T) {
            InternalNode* in = (InternalNode*)f->data;
            int lo = 0, hi = (int)in->count;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cmp_entry(key, val, in->sep[mid].key, in->sep[mid].val) < 0) hi = mid;
                else lo = mid + 1;
            }
            pathB[DEPTH] = cur; pathPos[DEPTH] = lo; DEPTH++;
            cur = in->ch[lo];
        } else {
            lf = f;
            pathB[DEPTH] = cur; pathPos[DEPTH] = 0; DEPTH++;
            break;
        }
    }
    LeafNode* ln = (LeafNode*)lf->data;
    int lo = 0, hi = (int)ln->count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (cmp_entry(key, val, ln->e[mid].key, ln->e[mid].val) <= 0) hi = mid;
        else lo = mid + 1;
    }
    if (lo >= (int)ln->count || ln->e[lo].val != val || strcmp(ln->e[lo].key, key) != 0)
        return; // entry does not exist
    for (int i = lo; i + 1 < (int)ln->count; ++i) ln->e[i] = ln->e[i + 1];
    ln->count--;
    lf->dirty = true;
    rebalance_after_delete();
}

// ---------------- Find ----------------
static void do_find(const char* key) {
    uint32_t cur = ROOT;
    for (;;) {
        Frame* f = get_frame(cur);
        if (*(uint32_t*)f->data == INTERNAL_T) {
            InternalNode* in = (InternalNode*)f->data;
            int lo = 0, hi = (int)in->count;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (cmp_entry(key, INT32_MIN, in->sep[mid].key, in->sep[mid].val) < 0) hi = mid;
                else lo = mid + 1;
            }
            cur = in->ch[lo];
        } else {
            LeafNode* ln = (LeafNode*)f->data;
            int lo = 0, hi = (int)ln->count;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (strcmp(ln->e[mid].key, key) < 0) lo = mid + 1;
                else hi = mid;
            }
            bool printed = false;
            for (int i = lo;; ++i) {
                if (i >= (int)ln->count) {
                    cur = ln->next;
                    if (!cur) break;
                    f = get_frame(cur);
                    ln = (LeafNode*)f->data;
                    i = -1;
                    continue;
                }
                int c = strcmp(ln->e[i].key, key);
                if (c == 0) {
                    if (printed) oputc(' ');
                    oint(ln->e[i].val);
                    printed = true;
                } else if (c > 0) {
                    break;
                }
            }
            if (!printed) { oputc('n'); oputc('u'); oputc('l'); oputc('l'); }
            oputc('\n');
            return;
        }
    }
}

// ---------------- Main ----------------
int main() {
    init_frames();
    open_db();
    bool ok = true;
    int n = (int)read_ll(&ok);
    if (!ok) n = 0;
    char key[KEY_SZ];
    char cmd[16];
    for (int i = 0; i < n; ++i) {
        read_token(cmd, sizeof(cmd));
        if (cmd[0] == 'i') {          // insert
            read_token(key, sizeof(key));
            int v = read_int();
            do_insert(key, (int32_t)v);
        } else if (cmd[0] == 'd') {   // delete
            read_token(key, sizeof(key));
            int v = read_int();
            do_delete(key, (int32_t)v);
        } else {                      // find
            read_token(key, sizeof(key));
            do_find(key);
        }
    }
    oflush();
    close_db();
    return 0;
}
