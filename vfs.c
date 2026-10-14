#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
 
/* ------------------------------------------------------------------ */
/* Constants and on-disk structures                                    */
/* ------------------------------------------------------------------ */
#define BS          4096
#define MAGIC       0x56465331u          /* "VFS1" */
#define JMAGIC      0x4A524E4Cu          /* "JRNL" */
#define NDIRECT     10
#define PPB         (BS / 4)             /* pointers per block */
#define ISIZE       128
#define IPB         (BS / ISIZE)
#define DESZ        64
#define DPB         (BS / DESZ)
#define NAMEMAX     59
#define JSTART      1
#define JBLKS       64
#define JCAP        (JBLKS - 2)          /* max blocks per journal txn */
#define NCACHE      256
#define NH          256
#define DC_SIZE     1024
#define ROOT_INO    1
#define MAXLBN      ((u64)NDIRECT + PPB + (u64)PPB * PPB)
 

struct super {
    u32 magic, version, block_size, total_blocks, total_inodes;
    u32 journal_start, journal_blocks;
    u32 ibmap_start, ibmap_blocks, dbmap_start, dbmap_blocks;
    u32 itable_start, itable_blocks, data_start;
    u32 free_blocks, free_inodes;
    u32 state;                /* 0 = clean, 1 = mounted/dirty */
    u32 mount_count, root_ino, alloc_hint;
    u64 created, last_mount;
};
 
struct inode {
    u64 size, atime, mtime, ctime;
    u32 uid, gid;
    u16 mode, links;
    u32 blocks;              
    u32 direct[NDIRECT];
    u32 ind, dind;
    u8  pad[32];
};
_Static_assert(sizeof(struct inode) == ISIZE, "inode must be 128 bytes");
 
struct dent { u32 ino; char name[60]; };
_Static_assert(sizeof(struct dent) == DESZ, "dirent must be 64 bytes");
 
struct jhdr {
    u32 magic, seq, count, commit, csum;
    u32 targets[JCAP];
};
 
/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static int fd = -1;
static struct super SB;
static int sb_dirty;
static u32 cur_uid, cur_gid, cwd_ino = ROOT_INO;
static int do_sync = 1;
static int crash_pt, crash_txn = 1, crash_armed, commits_seen;
static int in_commit;
static u32 jseq;
static char cwd_path[1024] = "/";
 
static struct {
    u64 rbytes, wbytes, rops, wops, syncs;
    double iotime;
    u64 chit, cmiss;
    u64 dlook, dhit, dscan;
    u64 commits, jblocks, replayed;
    u64 acalls, ascan, afail;
} ST;
 
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static u64 wall(void) { return (u64)time(NULL); }
/* ------------------------------------------------------------------ */
/* Raw block I/O                                                       */
/* ------------------------------------------------------------------ */
static void dread(u32 blk, void *buf)
{
    double t = now_sec();
    ssize_t r = pread(fd, buf, BS, (off_t)blk * BS);
    if (r < 0) { perror("pread"); exit(1); }
    if (r < BS) memset((u8 *)buf + r, 0, BS - r);
    ST.iotime += now_sec() - t; ST.rops++; ST.rbytes += BS;
}

static void dwrite(u32 blk, const void *buf)
{
    double t = now_sec();
    if (pwrite(fd, buf, BS, (off_t)blk * BS) != BS) { perror("pwrite"); exit(1); }

    ST.iotime += now_sec() - t; ST.wops++; ST.wbytes += BS;
}

static void dsync(void)
{
    if (!do_sync) return;
    double t = now_sec();

    fsync(fd);
    ST.iotime += now_sec() - t; ST.syncs++;
}
 
/* ------------------------------------------------------------------ */
/* Block cache (LRU, hashed). Metadata is write-back via the journal;  */
/* file data is write-through (ordered mode).                          */
/* ------------------------------------------------------------------ */
struct cbuf {
    u32 blk; int valid, dirty, hnext; u64 lru;
    u8 d[BS];
};
static struct cbuf cache[NCACHE];
static int hhead[NH];
static u64 tick;
 
static void cache_init(void)
{
    for (int i = 0; i < NH; i++) hhead[i] = -1;
    for (int i = 0; i < NCACHE; i++) { cache[i].valid = 0; cache[i].dirty = 0; }
}
static struct cbuf *cfind(u32 blk)
{
    for (int i = hhead[blk % NH]; i >= 0; i = cache[i].hnext)
        if (cache[i].blk == blk) return &cache[i];
    return NULL;
}
static void txn_commit(void);
 
/* mode 0: read from disk if absent; mode 1: return zero-filled buffer */
static struct cbuf *bget_ex(u32 blk, int mode)
{
    struct cbuf *c = cfind(blk);
    if (c) {
        if (mode == 0) ST.chit++;
        c->lru = ++tick;
        if (mode == 1) memset(c->d, 0, BS);
        return c;
    }
    if (mode == 0) ST.cmiss++;
retry:;
    int v = -1; u64 best = ~0ULL;
    for (int i = 0; i < NCACHE; i++) {
        if (!cache[i].valid) { v = i; break; }
        if (!cache[i].dirty && cache[i].lru < best) { best = cache[i].lru; v = i; }
    }
    if (v < 0) {
        if (in_commit) { fprintf(stderr, "fatal: cache full of dirty buffers\n"); exit(1); }
        txn_commit();
        goto retry;
    }
    c = &cache[v];
    if (c->valid) {                       
        int *p = &hhead[c->blk % NH];
        while (*p != v) p = &cache[*p].hnext;
        *p = c->hnext;
    }
    c->blk = blk; c->valid = 1; c->dirty = 0; c->lru = ++tick;
    c->hnext = hhead[blk % NH]; hhead[blk % NH] = v;
    if (mode == 1) memset(c->d, 0, BS); else dread(blk, c->d);
    return c;
}
static struct cbuf *bget(u32 blk) { return bget_ex(blk, 0); }
static struct cbuf *bnew(u32 blk) { return bget_ex(blk, 1); }
static void bdirty(struct cbuf *c) { c->dirty = 1; }
 
/* ------------------------------------------------------------------ */
/* Journal (write-ahead log for metadata blocks)                       */
/* ------------------------------------------------------------------ */

static u32 fnv(u32 h, const void *p, size_t n)
{
    const u8 *b = p;

    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}
static void crash_point(int pt)
{


    if (crash_armed && crash_pt == pt && commits_seen == crash_txn) {
        fprintf(stderr, "*** simulated power failure at journal point %d (txn %d) ***\n",
                pt, crash_txn);
        _exit(99);

    }
}

static void write_hdr(const struct jhdr *h)
{
    u8 buf[BS]; memset(buf, 0, BS); memcpy(buf, h, sizeof *h);
    dwrite(JSTART, buf);
}
static void journal_chunk(struct cbuf **l, int k)
{
    struct jhdr h; memset(&h, 0, sizeof h);
    h.magic = JMAGIC; h.seq = ++jseq; h.count = k;
    u32 cs = 2166136261u;
    for (int i = 0; i < k; i++) {
        h.targets[i] = l[i]->blk;
        dwrite(JSTART + 1 + i, l[i]->d);
        cs = fnv(cs, l[i]->d, BS);
    }
    cs = fnv(cs, h.targets, sizeof(u32) * k);
    h.csum = cs; h.commit = 0;
    write_hdr(&h); dsync();                         /* log body durable   */
    commits_seen++;
    crash_point(1);                                 /* before commit rec  */
    h.commit = 1; write_hdr(&h); dsync();           /* commit record      */
    crash_point(2);                                 /* before checkpoint  */
    for (int i = 0; i < k; i++) dwrite(l[i]->blk, l[i]->d);
    dsync();                                        /* home locations     */
    crash_point(3);
    memset(&h, 0, sizeof h); write_hdr(&h);         /* retire journal     */
    for (int i = 0; i < k; i++) l[i]->dirty = 0;
    ST.commits++; ST.jblocks += k;
}
static int cmp_cbuf(const void *a, const void *b)
{
    u32 x = (*(struct cbuf *const *)a)->blk, y = (*(struct cbuf *const *)b)->blk;
    return x < y ? -1 : x > y;
}
static void sb_sync(void)
{
    if (!sb_dirty) return;
    sb_dirty = 0;
    struct cbuf *c = bget(0);
    memcpy(c->d, &SB, sizeof SB);
    bdirty(c);
}
static void txn_commit(void)
{
    if (in_commit) return;
    sb_sync();
    struct cbuf *list[NCACHE]; int n = 0;
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].valid && cache[i].dirty) list[n++] = &cache[i];
    if (!n) return;
    in_commit = 1;
    /* sorted: bitmaps -> inode table -> data/dir/indirect, so a split txn
     * can only leak blocks, never double-allocate them */
    qsort(list, n, sizeof list[0], cmp_cbuf);
    for (int off = 0; off < n; off += JCAP)
        journal_chunk(list + off, n - off < JCAP ? n - off : JCAP);
    in_commit = 0;
}
static void journal_recover(void)
{
    u8 buf[BS]; struct jhdr h;
    dread(JSTART, buf); memcpy(&h, buf, sizeof h);
    if (h.magic != JMAGIC) return;

    if (h.commit == 1 && h.count <= JCAP) {
        static u8 body[JCAP][BS];
        u32 cs = 2166136261u;
        for (u32 i = 0; i < h.count; i++) { dread(JSTART + 1 + i, body[i]); cs = fnv(cs, body[i], BS); }
        cs = fnv(cs, h.targets, sizeof(u32) * h.count);
        if (cs == h.csum) {

            for (u32 i = 0; i < h.count; i++) dwrite(h.targets[i], body[i]);
            dsync();
            ST.replayed = h.count;
        }
    }
    memset(&h, 0, sizeof h); write_hdr(&h); dsync();
}

/* ------------------------------------------------------------------ */
/* Bitmaps, inode table, allocation                                    */
/* ------------------------------------------------------------------ */
static int bit_get(u32 start, u32 i)

{
    struct cbuf *c = bget(start + i / (BS * 8));
    return (c->d[(i % (BS * 8)) / 8] >> (i % 8)) & 1;
}
static void bit_set(u32 start, u32 i, int v)
{
    struct cbuf *c = bget(start + i / (BS * 8));
    u8 *p = &c->d[(i % (BS * 8)) / 8];
    if (v) *p |= 1 << (i % 8); else *p &= ~(1 << (i % 8));
    bdirty(c);
}
 
/* next-fit block allocator; goal==0 means "continue after last allocation" */
static u32 alloc_block(u32 goal)
{
    ST.acalls++;
    u32 total = SB.total_blocks, ds = SB.data_start, span = total - ds;
    if (!goal) goal = SB.alloc_hint;
    if (goal < ds || goal >= total) goal = ds;
    struct cbuf *c = NULL; u32 curbm = ~0u, b = goal;
    for (u32 n = 0; n < span; n++, b++) {
        if (b >= total) b = ds;
        ST.ascan++;
        u32 bm = b / (BS * 8);
        if (!c || curbm != bm) { c = bget(SB.dbmap_start + bm); curbm = bm; }
        u8 *p = &c->d[(b % (BS * 8)) / 8];
        if (!((*p >> (b % 8)) & 1)) {
            *p |= 1 << (b % 8); bdirty(c);
            SB.free_blocks--; SB.alloc_hint = b + 1; sb_dirty = 1;
            return b;
        }
    }
    ST.afail++;
    return 0;
}
static void free_block(u32 b)
{
    bit_set(SB.dbmap_start, b, 0);
    SB.free_blocks++; sb_dirty = 1;
}
static int iget(u32 ino, struct inode *ip)
{
    if (ino == 0 || ino >= SB.total_inodes) return -EINVAL;
    struct cbuf *c = bget(SB.itable_start + ino / IPB);
    memcpy(ip, c->d + (ino % IPB) * ISIZE, ISIZE);
    return 0;
}
static void iput(u32 ino, const struct inode *ip)
{
    struct cbuf *c = bget(SB.itable_start + ino / IPB);
    memcpy(c->d + (ino % IPB) * ISIZE, ip, ISIZE);
    bdirty(c);
}
static u32 alloc_inode(void)
{
    for (u32 i = 1; i < SB.total_inodes; i++) {
        if (!bit_get(SB.ibmap_start, i)) {
            bit_set(SB.ibmap_start, i, 1);
            SB.free_inodes--; sb_dirty = 1;
            return i;
        }
    }
    return 0;
}


static void free_inode(u32 ino)
{
    struct inode z; memset(&z, 0, sizeof z);
    iput(ino, &z);
    bit_set(SB.ibmap_start, ino, 0);
    SB.free_inodes++; sb_dirty = 1;
}
 
/* ------------------------------------------------------------------ */
/* Block mapping: logical file block -> physical block                  */
/* ------------------------------------------------------------------ */
/* returns 0 ok, 1 hole (alloc==0), <0 error */
static int ensure(u32 *slot, int alloc, u32 goal, int ismeta, struct inode *ip, int *isnew)
{
    if (*slot) return 0;
    if (!alloc) return 1;
    u32 b = alloc_block(goal);

    if (!b) return -ENOSPC;
    *slot = b; ip->blocks++;

    if (ismeta) { struct cbuf *c = bnew(b); bdirty(c); } else *isnew = 1;
    return 0;
}
static int bmap(struct inode *ip, u32 lbn, int alloc, u32 *out, int *isnew)
{
    int r, nw; struct cbuf *c;
    *isnew = 0; *out = 0;
    if (lbn < NDIRECT) {
        r = ensure(&ip->direct[lbn], alloc, lbn ? ip->direct[lbn - 1] + 1 : 0, 0, ip, isnew);
        if (r) return r < 0 ? r : 0;
        *out = ip->direct[lbn]; return 0;
    }
    u64 l = lbn - NDIRECT;
    if (l < PPB) {
        r = ensure(&ip->ind, alloc, 0, 1, ip, &nw);
        if (r) return r < 0 ? r : 0;
        c = bget(ip->ind);
        u32 *s = &((u32 *)c->d)[l];
        r = ensure(s, alloc, 0, 0, ip, isnew);
        if (r) return r < 0 ? r : 0;
        if (*isnew) { c = bget(ip->ind); bdirty(c); }
        *out = ((u32 *)bget(ip->ind)->d)[l]; return 0;
    }
    l -= PPB;
    if (l >= (u64)PPB * PPB) return -EFBIG;
    r = ensure(&ip->dind, alloc, 0, 1, ip, &nw);
    if (r) return r < 0 ? r : 0;
    u32 i1 = l / PPB, i2 = l % PPB;
    c = bget(ip->dind);
    r = ensure(&((u32 *)c->d)[i1], alloc, 0, 1, ip, &nw);
    if (r) return r < 0 ? r : 0;
    bdirty(bget(ip->dind));
    u32 l2 = ((u32 *)bget(ip->dind)->d)[i1];
    c = bget(l2);
    r = ensure(&((u32 *)c->d)[i2], alloc, 0, 0, ip, isnew);
    if (r) return r < 0 ? r : 0;
    if (*isnew) bdirty(bget(l2));
    *out = ((u32 *)bget(l2)->d)[i2];
    return 0;
}
static void free_tree(u32 b, int depth)
{
    if (!b) return;
    if (depth > 0) {
        u32 tmp[PPB];
        memcpy(tmp, bget(b)->d, BS);
        for (int i = 0; i < PPB; i++) free_tree(tmp[i], depth - 1);
    }
    free_block(b);
}
static void inode_trunc(struct inode *ip)
{
    for (int i = 0; i < NDIRECT; i++) { free_tree(ip->direct[i], 0); ip->direct[i] = 0; }
    free_tree(ip->ind, 1);  ip->ind = 0;
    free_tree(ip->dind, 2); ip->dind = 0;
    ip->size = 0; ip->blocks = 0;
}
 
/* byte-level file read/write. meta=1 -> journaled (directories). */


static long file_rw(struct inode *ip, u64 off, void *buf, u32 len, int wr, int meta)
{
    u32 done = 0;
    if (!wr) {
        if (off >= ip->size) return 0;
        if (off + len > ip->size) len = ip->size - off;

    }
    while (done < len) {
        u32 lbn = (off + done) / BS, boff = (off + done) % BS;
        u32 n = BS - boff; if (n > len - done) n = len - done;
        u32 b; int isnew;
        int r = bmap(ip, lbn, wr, &b, &isnew);
        if (r < 0) return done ? (long)done : r;
        if (wr) {
            struct cbuf *c = (isnew || (boff == 0 && n == BS)) ? bnew(b) : bget(b);
            memcpy(c->d + boff, (u8 *)buf + done, n);
            if (meta) bdirty(c); else dwrite(b, c->d);
        } else if (!b) {
            memset((u8 *)buf + done, 0, n);
        } else {
            memcpy((u8 *)buf + done, bget(b)->d + boff, n);
        }
        done += n;
    }


    if (wr && off + done > ip->size) ip->size = off + done;
    return done;
}
 
/* ------------------------------------------------------------------ */
/* Directories: entry scan + hash-indexed dentry cache                  */
/* ------------------------------------------------------------------ */
static struct dc { u32 dir, ino; char name[60]; } dcache[DC_SIZE];
 
static u32 dhash(u32 dir, const char *n)
{
    u32 h = 2166136261u ^ dir;
    for (; *n; n++) { h ^= (u8)*n; h *= 16777619u; }
    return h % DC_SIZE;
}
static void dc_put(u32 dir, const char *n, u32 ino)
{
    struct dc *e = &dcache[dhash(dir, n)];
    e->dir = dir; e->ino = ino; strncpy(e->name, n, 59); e->name[59] = 0;
}
static void dc_del(u32 dir, const char *n)
{
    struct dc *e = &dcache[dhash(dir, n)];
    if (e->ino && e->dir == dir && !strcmp(e->name, n)) e->ino = 0;
}
static void dc_clear(void) { memset(dcache, 0, sizeof dcache); }
static int dir_find(struct inode *dip, const char *name, u32 *ino, u32 *pblk, int *pslot)
{
    for (u32 l = 0; (u64)l * BS < dip->size; l++) {
        u32 b; int nw;
        bmap(dip, l, 0, &b, &nw);
        if (!b) continue;
        ST.dscan++;
        struct dent *d = (struct dent *)bget(b)->d;
        for (int i = 0; i < DPB; i++)
            if (d[i].ino && !strcmp(d[i].name, name)) {
                if (ino) *ino = d[i].ino;
                if (pblk) *pblk = b;
                if (pslot) *pslot = i;
                return 0;
            }
    }
    return -ENOENT;
}
static int lookup(u32 dir, const char *name, u32 *ino)
{
    ST.dlook++;
    struct dc *e = &dcache[dhash(dir, name)];
    if (e->ino && e->dir == dir && !strcmp(e->name, name)) { ST.dhit++; *ino = e->ino; return 0; }
    struct inode dip;
    int r = iget(dir, &dip);
    if (r) return r;
    r = dir_find(&dip, name, ino, NULL, NULL);
    if (!r) dc_put(dir, name, *ino);
    return r;
}


static int dir_add(u32 dino, struct inode *dip, const char *name, u32 ino)
{
    (void)dino;
    for (u32 l = 0; (u64)l * BS < dip->size; l++) {
        u32 b; int nw;

        bmap(dip, l, 0, &b, &nw);
        if (!b) continue;
        struct cbuf *c = bget(b);
        struct dent *d = (struct dent *)c->d;
        for (int i = 0; i < DPB; i++)
            if (!d[i].ino) {
                d[i].ino = ino; memset(d[i].name, 0, 60); strncpy(d[i].name, name, NAMEMAX);
                bdirty(c); dc_put(dino, name, ino);
                return 0;
            }
    }
    u32 b; int nw;
    int r = bmap(dip, dip->size / BS, 1, &b, &nw);
    if (r) return r;
    struct cbuf *c = bnew(b);
    struct dent *d = (struct dent *)c->d;
    d[0].ino = ino; strncpy(d[0].name, name, NAMEMAX);
    bdirty(c);
    dip->size += BS;
    dc_put(dino, name, ino);
    return 0;
}

static int dir_remove(u32 dino, struct inode *dip, const char *name)
{
    u32 b; int slot;
    int r = dir_find(dip, name, NULL, &b, &slot);
    if (r) return r;
    struct cbuf *c = bget(b);
    memset(&((struct dent *)c->d)[slot], 0, DESZ);
    bdirty(c); dc_del(dino, name);
    return 0;
}
typedef int (*dir_cb)(const char *name, u32 ino, void *ctx);
static int dir_iter(struct inode *dip, dir_cb cb, void *ctx)
{
    u8 tmp[BS];
    for (u32 l = 0; (u64)l * BS < dip->size; l++) {
        u32 b; int nw;
        bmap(dip, l, 0, &b, &nw);
        if (!b) continue;
        memcpy(tmp, bget(b)->d, BS);
        struct dent *d = (struct dent *)tmp;
        for (int i = 0; i < DPB; i++)
            if (d[i].ino && cb(d[i].name, d[i].ino, ctx)) return 1;
    }
    return 0;
}
/* ------------------------------------------------------------------ */
/* Permissions and path resolution                                      */
/* ------------------------------------------------------------------ */
static int perm_ok(const struct inode *ip, int want)
{
    if (cur_uid == 0) return 1;
    int sh = ip->uid == cur_uid ? 6 : (ip->gid == cur_gid ? 3 : 0);
    return (((ip->mode & 0777) >> sh) & want) == want;
}
static int is_dir(const struct inode *ip) { return (ip->mode & S_IFMT) == S_IFDIR; }
 
/* want_parent=0: resolve full path. want_parent=1: resolve parent dir and
 * return final component in 'last'. */
static int path_walk(const char *path, int want_parent, u32 *out, char *last)
{
    char buf[1024];
    if (strlen(path) >= sizeof buf) return -ENAMETOOLONG;
    strcpy(buf, path);
    u32 cur = path[0] == '/' ? (u32)ROOT_INO : cwd_ino;
    char *save, *tok = strtok_r(buf, "/", &save);
    if (!tok) { if (want_parent) return -EINVAL; *out = cur; return 0; }
    while (tok) {
        char *next = strtok_r(NULL, "/", &save);
        if (strlen(tok) > NAMEMAX) return -ENAMETOOLONG;
        if (!next && want_parent) { strcpy(last, tok); *out = cur; return 0; }
        struct inode ip;
        int r = iget(cur, &ip);

        if (r) return r;
        if (!is_dir(&ip)) return -ENOTDIR;
        if (!perm_ok(&ip, 1)) return -EACCES;
        u32 n;
        r = lookup(cur, tok, &n);
        if (r) return r;


        cur = n; tok = next;
    }
    *out = cur;
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* File-system operations                                               */
/* ------------------------------------------------------------------ */
static int fs_create(const char *path, u16 perm, int isdir)
{
    u32 pino, t; char name[64];
    int r = path_walk(path, 1, &pino, name);
    if (r) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -EEXIST;
    struct inode p;
    iget(pino, &p);
    if (!is_dir(&p)) return -ENOTDIR;
    if (!perm_ok(&p, 3)) return -EACCES;
    if (lookup(pino, name, &t) == 0) return -EEXIST;
    u32 ino = alloc_inode();
    if (!ino) return -ENOSPC;
    struct inode n; memset(&n, 0, sizeof n);
    n.mode = (isdir ? S_IFDIR : S_IFREG) | perm;
    n.uid = cur_uid; n.gid = cur_gid; n.links = isdir ? 2 : 1;

    n.atime = n.mtime = n.ctime = wall();
    if (isdir) {
        r = dir_add(ino, &n, ".", ino);
        if (!r) r = dir_add(ino, &n, "..", pino);
        if (r) goto fail;
    }
    r = dir_add(pino, &p, name, ino);
    if (r) goto fail;
    if (isdir) p.links++;
    p.mtime = p.ctime = wall();
    iput(pino, &p); iput(ino, &n);
    txn_commit();
    return 0;
fail:
    inode_trunc(&n); free_inode(ino); txn_commit();
    return r;
}
static int fs_unlink(const char *path)
{
    u32 pino, ino; char name[64];
    int r = path_walk(path, 1, &pino, name);
    if (r) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    struct inode p, ip;
    iget(pino, &p);
    if (!perm_ok(&p, 3)) return -EACCES;
    if ((r = lookup(pino, name, &ino))) return r;
    iget(ino, &ip);
    if (is_dir(&ip)) return -EISDIR;
    dir_remove(pino, &p, name);
    p.mtime = p.ctime = wall(); iput(pino, &p);
    if (--ip.links == 0) { inode_trunc(&ip); free_inode(ino); }
    else { ip.ctime = wall(); iput(ino, &ip); }
    txn_commit();
    return 0;
}
static int empty_cb(const char *n, u32 ino, void *ctx)
{
    (void)ino;
    if (strcmp(n, ".") && strcmp(n, "..")) { *(int *)ctx = 0; return 1; }
    return 0;
}
static int fs_rmdir(const char *path)
{
    u32 pino, ino; char name[64];
    int r = path_walk(path, 1, &pino, name);
    if (r) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    struct inode p, ip;
    iget(pino, &p);
    if (!perm_ok(&p, 3)) return -EACCES;
    if ((r = lookup(pino, name, &ino))) return r;

    iget(ino, &ip);

    if (!is_dir(&ip)) return -ENOTDIR;
    if (ino == ROOT_INO) return -EBUSY;

    int empty = 1;
    dir_iter(&ip, empty_cb, &empty);
    if (!empty) return -ENOTEMPTY;
    dir_remove(pino, &p, name);
    dc_del(ino, "."); dc_del(ino, "..");
    p.links--; p.mtime = p.ctime = wall(); iput(pino, &p);
    inode_trunc(&ip); free_inode(ino);
    txn_commit();
    return 0;
}
static int fs_link(const char *oldp, const char *newp)
{
    u32 ino, pino; char name[64];
    int r = path_walk(oldp, 0, &ino, NULL);
    if (r) return r;
    struct inode ip, p;
    iget(ino, &ip);
    if (is_dir(&ip)) return -EPERM;
    if ((r = path_walk(newp, 1, &pino, name))) return r;
    iget(pino, &p);
    if (!is_dir(&p)) return -ENOTDIR;
    if (!perm_ok(&p, 3)) return -EACCES;
    u32 t;
    if (lookup(pino, name, &t) == 0) return -EEXIST;
    if ((r = dir_add(pino, &p, name, ino))) return r;
    p.mtime = p.ctime = wall(); iput(pino, &p);

    ip.links++; ip.ctime = wall(); iput(ino, &ip);


    txn_commit();
    return 0;
}
static int fs_rename(const char *src, const char *dst)
{
    u32 sp, dp, ino, t; char sn[64], dn[64];
    int r = path_walk(src, 1, &sp, sn);
    if (r) return r;
    if ((r = path_walk(dst, 1, &dp, dn))) return r;
    if (!strcmp(sn, ".") || !strcmp(sn, "..") || !strcmp(dn, ".") || !strcmp(dn, "..")) return -EINVAL;
    if ((r = lookup(sp, sn, &ino))) return r;
    struct inode ip; iget(ino, &ip);
    int dirmv = is_dir(&ip);
    if (dirmv) {                                   /* forbid moving into itself */
        u32 u = dp;
        for (;;) {
            if (u == ino) return -EINVAL;
            if (u == ROOT_INO) break;
            if (lookup(u, "..", &u)) break;
        }
    }
    if (lookup(dp, dn, &t) == 0) {
        if (t == ino) return 0;
        struct inode ti; iget(t, &ti);
        if (dirmv || is_dir(&ti)) return -EEXIST;
        if ((r = fs_unlink(dst))) return r;
    }
    struct inode A, B, *spi = &A, *dpi = &B;
    iget(sp, spi);

    if (dp == sp) dpi = spi; else iget(dp, dpi);
    if (!is_dir(dpi)) return -ENOTDIR;
    if (!perm_ok(spi, 3) || !perm_ok(dpi, 3)) return -EACCES;
    if ((r = dir_add(dp, dpi, dn, ino))) return r;
    dir_remove(sp, spi, sn);
    if (dirmv && sp != dp) {
        u32 b; int slot;
        if (!dir_find(&ip, "..", NULL, &b, &slot)) {
            struct cbuf *c = bget(b);
            ((struct dent *)c->d)[slot].ino = dp; bdirty(c);
            dc_put(ino, "..", dp);
        }
        spi->links--; dpi->links++;
    }
    spi->mtime = spi->ctime = dpi->mtime = dpi->ctime = wall();
    iput(sp, spi); if (dpi != spi) iput(dp, dpi);
    ip.ctime = wall(); iput(ino, &ip);
    txn_commit();
    return 0;
}



static int fs_chmod(const char *path, u16 perm)
{


    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    if (cur_uid && cur_uid != ip.uid) return -EPERM;
    ip.mode = (ip.mode & S_IFMT) | (perm & 07777); ip.ctime = wall();
    iput(ino, &ip); txn_commit();
    return 0;
}
static int fs_chown(const char *path, u32 uid, u32 gid)
{
    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    if (cur_uid) return -EPERM;
    ip.uid = uid; ip.gid = gid; ip.ctime = wall();
    iput(ino, &ip); txn_commit();
    return 0;
}
/* open (create/truncate) a file for writing */
static int open_for_write(const char *path, u32 *ino, struct inode *ip)
{
    int r = path_walk(path, 0, ino, NULL);
    if (r == -ENOENT) {
        if ((r = fs_create(path, 0644, 0))) return r;
        r = path_walk(path, 0, ino, NULL);
    }
    if (r) return r;


    iget(*ino, ip);
    if (is_dir(ip)) return -EISDIR;
    if (!perm_ok(ip, 2)) return -EACCES;
    if (ip->size) inode_trunc(ip);
    return 0;
}
static int fs_put_host(const char *host, const char *path)
{
    FILE *f = fopen(host, "rb");
    if (!f) return -errno;
    u32 ino; struct inode ip;
    int r = open_for_write(path, &ino, &ip);
    if (r) { fclose(f); return r; }
    static u8 buf[65536]; size_t n; u64 off = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        long w = file_rw(&ip, off, buf, n, 1, 0);
        if (w < 0) { r = (int)w; break; }
        off += w;
        if ((size_t)w < n) { r = -ENOSPC; break; }
    }
    fclose(f);
    ip.mtime = ip.ctime = wall(); iput(ino, &ip); txn_commit();
    return r;
}
static int fs_write_bytes(const char *path, const void *data, u32 len)
{
    u32 ino; struct inode ip;
    int r = open_for_write(path, &ino, &ip);
    if (r) return r;
    long w = file_rw(&ip, 0, (void *)data, len, 1, 0);

    if (w < 0) r = (int)w;
    ip.mtime = ip.ctime = wall(); iput(ino, &ip); txn_commit();
    return r;
}
static int fs_read_to(const char *path, FILE *out, u64 *total)
{
    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    if (is_dir(&ip)) return -EISDIR;
    if (!perm_ok(&ip, 4)) return -EACCES;
    static u8 buf[65536]; u64 off = 0; long n;
    while ((n = file_rw(&ip, off, buf, sizeof buf, 0, 0)) > 0) {
        if (out) fwrite(buf, 1, n, out);
        off += n;
    }
    if (total) *total = off;
    return n < 0 ? (int)n : 0;
}
/* ------------------------------------------------------------------ */
/* Inspection: ls / tree / stat / df / stats                            */
/* ------------------------------------------------------------------ */
static void modestr(u16 m, char *s)
{
    s[0] = (m & S_IFMT) == S_IFDIR ? 'd' : '-';
    const char *r = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) s[1 + i] = (m & (1 << (8 - i))) ? r[i] : '-';
    s[10] = 0;
}
static void file_extents(struct inode *ip, u32 *nblk, u32 *next)
{
    u32 nb = 0, ne = 0, prev = 0;
    for (u32 l = 0; (u64)l * BS < ip->size; l++) {
        u32 b; int nw;
        bmap(ip, l, 0, &b, &nw);
        if (!b) { prev = 0; continue; }
        nb++;
        if (!prev || b != prev + 1) ne++;
        prev = b;
    }
    *nblk = nb; *next = ne;
}
struct lsctx { int lng; };
static int ls_cb(const char *name, u32 ino, void *ctx)
{
    struct lsctx *c = ctx;
    if (!c->lng) { printf("%s\n", name); return 0; }
    struct inode ip; iget(ino, &ip);
    char m[11], ts[32]; time_t t = ip.mtime;
    modestr(ip.mode, m);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&t));

    printf("%s %2u %5u %5u %10llu %s %s%s  (ino %u)\n", m, ip.links, ip.uid, ip.gid,
           (unsigned long long)ip.size, ts, name, is_dir(&ip) ? "/" : "", ino);
    return 0;
}

static int cmd_ls(const char *path, int lng)
{
    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    struct lsctx c = { lng };
    if (!is_dir(&ip)) { ls_cb(path, ino, &c); return 0; }
    if (!perm_ok(&ip, 4)) return -EACCES;
    dir_iter(&ip, ls_cb, &c);
    return 0;
}
struct treectx { int depth; u64 files, dirs; };
static void tree_rec(u32 ino, struct treectx *t);
static int tree_cb(const char *name, u32 ino, void *ctx)
{
    struct treectx *t = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    struct inode ip; iget(ino, &ip);
    printf("%*s%s%s\n", t->depth * 3, "", name, is_dir(&ip) ? "/" : "");
    if (is_dir(&ip)) {
        t->dirs++;
        if (t->depth < 24) { t->depth++; tree_rec(ino, t); t->depth--; }
    } else t->files++;
    return 0;
}

static void tree_rec(u32 ino, struct treectx *t)
{
    struct inode ip; iget(ino, &ip);
    dir_iter(&ip, tree_cb, t);
}
static int cmd_tree(const char *path)
{
    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    if (!is_dir(&ip)) return -ENOTDIR;
    if (!perm_ok(&ip, 5)) return -EACCES;
    struct treectx t = { 1, 0, 0 };
    printf("%s\n", path);
    tree_rec(ino, &t);
    printf("\n%llu directories, %llu files\n", (unsigned long long)t.dirs, (unsigned long long)t.files);
    return 0;
}


static int cmd_stat(const char *path)
{
    u32 ino; struct inode ip;
    int r = path_walk(path, 0, &ino, NULL);
    if (r) return r;
    iget(ino, &ip);
    char m[11]; modestr(ip.mode, m);
    u32 nb, ne; file_extents(&ip, &nb, &ne);
    time_t a = ip.atime, mt = ip.mtime, c = ip.ctime;
    printf("  Inode: %u   Type: %s   Mode: %04o (%s)\n", ino, is_dir(&ip) ? "directory" : "regular file",
           ip.mode & 07777, m);
    printf("  Links: %u   Uid: %u   Gid: %u\n", ip.links, ip.uid, ip.gid);
    printf("  Size: %llu bytes   Blocks allocated: %u (incl. indirect)   Extents: %u\n",
           (unsigned long long)ip.size, ip.blocks, ne);
    printf("  Direct: ");
    for (int i = 0; i < NDIRECT; i++) printf("%u ", ip.direct[i]);
    printf("\n  Indirect: %u   Double-indirect: %u\n", ip.ind, ip.dind);
    printf("  Created: %s", ctime(&c));
    printf("  Modified: %s", ctime(&mt));
    printf("  Accessed: %s", ctime(&a));
    return 0;
}
 
struct usage { u64 files, dirs, bytes, dblocks, extents, fblocks; };
static void scan_usage(struct usage *u)
{
    memset(u, 0, sizeof *u);
    for (u32 i = 1; i < SB.total_inodes; i++) {
        if (!bit_get(SB.ibmap_start, i)) continue;
        struct inode ip; iget(i, &ip);
        if (is_dir(&ip)) { u->dirs++; u->dblocks += ip.blocks; continue; }
        u->files++; u->bytes += ip.size; u->dblocks += ip.blocks;
        u32 nb, ne; file_extents(&ip, &nb, &ne);
        u->extents += ne; u->fblocks += nb;
    }
}


static void cmd_df(void)
{
    u32 used = SB.total_blocks - SB.free_blocks, uin = SB.total_inodes - SB.free_inodes;
    printf("Filesystem geometry\n");
    printf("  block size        : %u bytes\n", SB.block_size);
    printf("  total blocks      : %u (%.1f MiB)\n", SB.total_blocks, SB.total_blocks * (double)BS / 1048576);
    printf("  journal           : blocks %u-%u\n", SB.journal_start, SB.journal_start + SB.journal_blocks - 1);
    printf("  inode bitmap      : blocks %u-%u\n", SB.ibmap_start, SB.ibmap_start + SB.ibmap_blocks - 1);
    printf("  block bitmap      : blocks %u-%u\n", SB.dbmap_start, SB.dbmap_start + SB.dbmap_blocks - 1);
    printf("  inode table       : blocks %u-%u (%u inodes)\n", SB.itable_start,
           SB.itable_start + SB.itable_blocks - 1, SB.total_inodes);
    printf("  data region       : blocks %u-%u\n", SB.data_start, SB.total_blocks - 1);
    printf("Utilization\n");


    printf("  blocks used/free  : %u / %u  (%.2f%% used)\n", used, SB.free_blocks, 100.0 * used / SB.total_blocks);
    printf("  inodes used/free  : %u / %u  (%.2f%% used)\n", uin, SB.free_inodes, 100.0 * uin / SB.total_inodes);
    printf("  mounts: %u   state: %s\n", SB.mount_count, SB.state ? "mounted" : "clean");
}

static void cmd_stats(void)
{
    struct usage u; scan_usage(&u);
    u32 used = SB.total_blocks - SB.free_blocks;
    printf("== Storage utilization ==\n");

    printf("  capacity used        : %.2f%%  (%u of %u blocks, metadata incl.)\n",
           100.0 * used / SB.total_blocks, used, SB.total_blocks);
    printf("  files / directories  : %llu / %llu\n", (unsigned long long)u.files, (unsigned long long)u.dirs);
    printf("  logical file bytes   : %llu\n", (unsigned long long)u.bytes);
    u64 alloc = u.dblocks * BS;
    printf("  allocated data bytes : %llu\n", (unsigned long long)alloc);
    if (u.bytes) printf("  space efficiency     : %.2f%%  (internal fragmentation %.2f%%)\n",
                      100.0 * u.bytes / alloc, 100.0 - 100.0 * u.bytes / alloc);
    if (u.files) printf("  avg extents per file : %.2f   contiguity: %.1f%% of file blocks\n",
                        (double)u.extents / u.files,
                        u.fblocks ? 100.0 * (u.fblocks - u.extents) / u.fblocks : 100.0);
    printf("== Allocator ==\n");
    printf("  allocations: %llu   failures: %llu   avg bitmap bits scanned/alloc: %.2f\n",
           (unsigned long long)ST.acalls, (unsigned long long)ST.afail,
           ST.acalls ? (double)ST.ascan / ST.acalls : 0.0);
    printf("== Block I/O (this session) ==\n");
    printf("  reads : %llu ops, %.2f MiB\n", (unsigned long long)ST.rops, ST.rbytes / 1048576.0);
    printf("  writes: %llu ops, %.2f MiB   fsyncs: %llu\n", (unsigned long long)ST.wops,
           ST.wbytes / 1048576.0, (unsigned long long)ST.syncs);

    if (ST.iotime > 0)
        printf("  device throughput    : %.1f MiB/s (%.4f s in I/O)\n",
               (ST.rbytes + ST.wbytes) / 1048576.0 / ST.iotime, ST.iotime);
    printf("== Caches ==\n");

    u64 ba = ST.chit + ST.cmiss;
    printf("  block cache (%d x %d B): hits %llu, misses %llu, hit rate %.1f%%\n", NCACHE, BS,
           (unsigned long long)ST.chit, (unsigned long long)ST.cmiss, ba ? 100.0 * ST.chit / ba : 0.0);
    printf("  dentry hash cache    : lookups %llu, hits %llu, hit rate %.1f%%, dir blocks scanned %llu\n",
           (unsigned long long)ST.dlook, (unsigned long long)ST.dhit,
           ST.dlook ? 100.0 * ST.dhit / ST.dlook : 0.0, (unsigned long long)ST.dscan);
    printf("== Journal ==\n");
    printf("  transactions: %llu   metadata blocks logged: %llu   replayed at mount: %llu\n",
           (unsigned long long)ST.commits, (unsigned long long)ST.jblocks, (unsigned long long)ST.replayed);
}
 
/* ------------------------------------------------------------------ */
/* fsck                                                                 */
/* ------------------------------------------------------------------ */
struct fck {
    u8 *bused, *vis; u32 *refs, *stack; u32 sp;
    u32 errs, nblocks;
};
static void mark_tree(struct fck *f, u32 ino, u32 b, int depth)
{
    if (!b) return;
    if (b < SB.data_start || b >= SB.total_blocks) {
        printf("  inode %u: block pointer %u out of range\n", ino, b); f->errs++; return;
    }
    if (f->bused[b]) { printf("  inode %u: block %u already owned (duplicate)\n", ino, b); f->errs++; return; }
    f->bused[b] = 1; f->nblocks++;
    if (depth) {
        u32 tmp[PPB]; memcpy(tmp, bget(b)->d, BS);
        for (int i = 0; i < PPB; i++) mark_tree(f, ino, tmp[i], depth - 1);
    }
}




static void fsck_visit(struct fck *f, u32 ino)
{
    struct inode ip; iget(ino, &ip);
    int t = ip.mode & S_IFMT;
    if (t != S_IFDIR && t != S_IFREG) {
        printf("  inode %u: referenced but has invalid type\n", ino); f->errs++; return;
    }
    f->nblocks = 0;
    for (int i = 0; i < NDIRECT; i++) mark_tree(f, ino, ip.direct[i], 0);
    mark_tree(f, ino, ip.ind, 1); mark_tree(f, ino, ip.dind, 2);
    if (f->nblocks != ip.blocks) {
        printf("  inode %u: block count %u but %u reachable\n", ino, ip.blocks, f->nblocks); f->errs++;
    }
    if (t == S_IFDIR) f->stack[f->sp++] = ino;
}
static int fsck_cb(const char *n, u32 ino, void *ctx)
{
    struct fck *f = ctx;
    if (ino >= SB.total_inodes) { printf("  entry '%s' -> bad inode %u\n", n, ino); f->errs++; return 0; }
    f->refs[ino]++;
    if (strcmp(n, ".") && strcmp(n, "..") && !f->vis[ino]) { f->vis[ino] = 1; fsck_visit(f, ino); }
    return 0;
}
static int cmd_fsck(int repair)
{
    u32 N = SB.total_inodes, T = SB.total_blocks;
    struct fck f; memset(&f, 0, sizeof f);

    f.bused = calloc(T, 1); f.vis = calloc(N, 1);
    f.refs = calloc(N, 4); f.stack = malloc((size_t)N * 4);
    printf("fsck: checking superblock...\n");
    if (SB.magic != MAGIC || SB.block_size != BS) { printf("  bad superblock\n"); return 2; }
    if (SB.state) printf("  note: filesystem was not cleanly unmounted\n");
    for (u32 b = 0; b < SB.data_start; b++) f.bused[b] = 1;
    printf("fsck: walking namespace from root...\n");
    f.vis[ROOT_INO] = 1; fsck_visit(&f, ROOT_INO);
    while (f.sp) {
        struct inode d; u32 ino = f.stack[--f.sp];
        iget(ino, &d);
        dir_iter(&d, fsck_cb, &f);
    }
    u32 errs = f.errs, links = 0, orphans = 0, ibad = 0, bleak = 0, bmiss = 0;
    printf("fsck: checking link counts and orphans...\n");
    for (u32 i = 1; i < N; i++) {
        struct inode ip; iget(i, &ip);
        if (f.vis[i]) {

            if (ip.links != f.refs[i]) {
                printf("  inode %u: link count %u, expected %u\n", i, ip.links, f.refs[i]);
                links++;
                if (repair) { ip.links = f.refs[i]; iput(i, &ip); }
            }
        } else if (ip.mode) {
            printf("  inode %u: orphan (allocated, unreachable)\n", i); orphans++;
            if (repair) { struct inode z; memset(&z, 0, sizeof z); iput(i, &z); }
        }
    }

    printf("fsck: checking bitmaps...\n");
    for (u32 i = 0; i < N; i++) {
        int want = f.vis[i] || i == 0;
        if (bit_get(SB.ibmap_start, i) != want) { ibad++; if (repair) bit_set(SB.ibmap_start, i, want); }
    }
    u32 nfree = 0;
    for (u32 b = 0; b < T; b++) {
        int want = f.bused[b], have = bit_get(SB.dbmap_start, b);
        if (have && !want) bleak++; else if (!have && want) bmiss++;
        if (have != want && repair) bit_set(SB.dbmap_start, b, want);
        if (!want) nfree++;
    }
    u32 nfi = 0;
    for (u32 i = 1; i < N; i++) if (!f.vis[i]) nfi++;
    u32 sbbad = (nfree != SB.free_blocks) + (nfi != SB.free_inodes);
    if (ibad) printf("  inode bitmap: %u wrong bit(s)\n", ibad);
    if (bleak) printf("  block bitmap: %u leaked block(s) (marked used, unreferenced)\n", bleak);
    if (bmiss) printf("  block bitmap: %u referenced block(s) marked free\n", bmiss);
    if (sbbad) printf("  superblock free counters wrong (blocks %u vs %u, inodes %u vs %u)\n",
                      SB.free_blocks, nfree, SB.free_inodes, nfi);
    errs += links + orphans + ibad + bleak + bmiss + sbbad;
    if (repair && errs) {
        SB.free_blocks = nfree; SB.free_inodes = nfi; sb_dirty = 1;
        txn_commit();
        printf("fsck: %u problem(s) found and repaired\n", errs);
    } else if (errs) printf("fsck: %u problem(s) found (run 'fsck -r' to repair)\n", errs);
    else printf("fsck: clean (%u inodes in use, %u blocks in use)\n", N - nfi - 1, T - nfree);
    free(f.bused); free(f.vis); free(f.refs); free(f.stack);


    return errs && !repair ? 1 : 0;
}
 
/* ------------------------------------------------------------------ */
/* Benchmark                                                            */
/* ------------------------------------------------------------------ */
static int cmd_bench(int mb, int nfiles)
{
    char p[128]; int r;
    u64 total = (u64)mb << 20;
    static u8 buf[65536];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (u8)(i * 31 + 7);
    printf("Benchmark: %d MiB sequential file, %d small files (fsync %s)\n", mb, nfiles, do_sync ? "on" : "off");
    if ((r = fs_create("/bench", 0755, 1)) && r != -EEXIST) return r;

 
    double t0 = now_sec();
    u32 ino; struct inode ip;
    if ((r = open_for_write("/bench/big", &ino, &ip))) return r;
    for (u64 off = 0; off < total; off += sizeof buf) {
        long w = file_rw(&ip, off, buf, sizeof buf, 1, 0);
        if (w < (long)sizeof buf) { iput(ino, &ip); txn_commit(); return w < 0 ? (int)w : -ENOSPC; }
    }
    iput(ino, &ip); txn_commit();
    double tw = now_sec() - t0;
 
    u32 nb, ne; file_extents(&ip, &nb, &ne);
    t0 = now_sec();
    u64 off = 0, sum = 0; long n; u8 rb[65536];
    while ((n = file_rw(&ip, off, rb, sizeof rb, 0, 0)) > 0) { for (long i = 0; i < n; i += 512) sum += rb[i]; off += n; }
    double tr = now_sec() - t0;
    printf("  sequential write : %8.1f MiB/s  (%.3f s)\n", mb / tw, tw);
    printf("  sequential read  : %8.1f MiB/s  (%.3f s, checksum %llu)\n", mb / tr, tr, (unsigned long long)sum);
    printf("  big file layout  : %u data blocks in %u extent(s) -> %.1f%% contiguous\n", nb, ne,
           nb ? 100.0 * (nb - ne) / nb : 100.0);
 
    if ((r = fs_create("/bench/d", 0755, 1))) return r;
    t0 = now_sec();
    for (int i = 0; i < nfiles; i++) {

        snprintf(p, sizeof p, "/bench/d/f%d", i);
        if ((r = fs_write_bytes(p, "hello, storage engine\n", 22))) return r;
    }
    double tc = now_sec() - t0;
    printf("  small-file create: %8.0f files/s (%.3f s, each is a journaled txn)\n", nfiles / tc, tc);
 
    dc_clear();
    u64 s0 = ST.dscan;
    t0 = now_sec();
    for (int i = 0; i < nfiles; i++) { snprintf(p, sizeof p, "/bench/d/f%d", i); path_walk(p, 0, &ino, NULL); }
    double tl1 = now_sec() - t0;
    u64 scanned = ST.dscan - s0;
    t0 = now_sec();
    for (int i = 0; i < nfiles; i++) { snprintf(p, sizeof p, "/bench/d/f%d", i); path_walk(p, 0, &ino, NULL); }
    double tl2 = now_sec() - t0;
    printf("  dir lookup cold  : %8.2f us/op (%llu dir blocks scanned)\n", tl1 / nfiles * 1e6, (unsigned long long)scanned);
    printf("  dir lookup warm  : %8.2f us/op (dentry cache)\n", tl2 / nfiles * 1e6);
    for (int i = 0; i < nfiles; i++) { snprintf(p, sizeof p, "/bench/d/f%d", i); fs_unlink(p); }
    fs_rmdir("/bench/d"); fs_unlink("/bench/big"); fs_rmdir("/bench");
    printf("  (benchmark files removed)\n");
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* mkfs / mount / unmount                                               */
/* ------------------------------------------------------------------ */
static u32 ceil_div(u32 a, u32 b) { return (a + b - 1) / b; }
 
static int do_mkfs(const char *img, u32 mb, u32 inodes)
{
    if (mb < 2) { fprintf(stderr, "image must be at least 2 MiB\n"); return 1; }
    fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(img); return 1; }
    u32 total = mb * 256;
    if (!inodes) inodes = total / 4;
    inodes = ceil_div(inodes, IPB) * IPB;
    memset(&SB, 0, sizeof SB);
    SB.magic = MAGIC; SB.version = 1; SB.block_size = BS;
    SB.total_blocks = total; SB.total_inodes = inodes;
    SB.journal_start = JSTART; SB.journal_blocks = JBLKS;

    SB.ibmap_start = JSTART + JBLKS; SB.ibmap_blocks = ceil_div(inodes, BS * 8);
    SB.dbmap_start = SB.ibmap_start + SB.ibmap_blocks; SB.dbmap_blocks = ceil_div(total, BS * 8);

    SB.itable_start = SB.dbmap_start + SB.dbmap_blocks; SB.itable_blocks = inodes / IPB;
    SB.data_start = SB.itable_start + SB.itable_blocks;
    if (SB.data_start + 8 >= total) { fprintf(stderr, "too many inodes for image size\n"); return 1; }
    SB.free_blocks = total - SB.data_start; SB.free_inodes = inodes - 2;
    SB.root_ino = ROOT_INO; SB.alloc_hint = SB.data_start;
    SB.created = wall();
    if (ftruncate(fd, (off_t)total * BS)) { perror("ftruncate"); return 1; }
    cache_init();

    for (u32 b = 0; b < SB.data_start; b++) bit_set(SB.dbmap_start, b, 1);
    bit_set(SB.ibmap_start, 0, 1); bit_set(SB.ibmap_start, ROOT_INO, 1);

    struct inode r; memset(&r, 0, sizeof r);
    r.mode = S_IFDIR | 0755; r.uid = cur_uid; r.gid = cur_gid; r.links = 2;
    r.atime = r.mtime = r.ctime = wall();
    dir_add(ROOT_INO, &r, ".", ROOT_INO);
    dir_add(ROOT_INO, &r, "..", ROOT_INO);
    iput(ROOT_INO, &r);
    sb_dirty = 1; txn_commit();
    close(fd);
    printf("Created %s: %u MiB, %u blocks, %u inodes, data region starts at block %u\n",
           img, mb, total, inodes, SB.data_start);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command dispatcher / shell                                           */
/* ------------------------------------------------------------------ */

static int need(int argc, int n, const char *syntax) {
    if (argc >= n) return 1;
    fprintf(stderr, "usage: %s\n", syntax);
    return 0;
}
static void usage_text(void) {
    puts("Commands: mkdir touch rm rmdir mv ln ls tree stat cat put get write chmod chown su cd pwd df info stats fsck bench help exit");
}
static int fs_mount(const char *img) {
    fd = open(img, O_RDWR);
    if (fd < 0) { perror(img); return -1; }
    u8 buf[BS];
    if (pread(fd, buf, BS, 0) != BS) { perror("superblock"); close(fd); return -1; }
    memcpy(&SB, buf, sizeof SB);
    struct stat st;
    if (fstat(fd, &st) || SB.magic != MAGIC || SB.version != 1 ||
        SB.block_size != BS || SB.total_blocks < 512 ||
        (u64)st.st_size < (u64)SB.total_blocks * BS ||
        SB.root_ino != ROOT_INO || SB.data_start >= SB.total_blocks) {
        fprintf(stderr, "Invalid virtual disk image\n"); close(fd); return -1;
    }
    journal_recover();
    cache_init(); dc_clear();
    cwd_ino = ROOT_INO; strcpy(cwd_path, "/");
    SB.state = 1; SB.mount_count++; SB.last_mount = wall();
    sb_dirty = 1; txn_commit();
    return 0;
}
static void fs_unmount(void) {
    txn_commit(); SB.state = 0; sb_dirty = 1; txn_commit();
    dsync(); close(fd); fd = -1;
}
static int run_cmd(int argc, char **argv);
static void shell(void) {
    char line[8192];
    usage_text();
    while (1) {
        printf("vfs:%s$ ", cwd_path); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        char *args[128]; int n = 0;
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\r\n", &save); t && n < 128;
             t = strtok_r(NULL, " \t\r\n", &save)) args[n++] = t;
        if (!n) continue;
        if (!strcmp(args[0], "exit") || !strcmp(args[0], "quit")) break;
        run_cmd(n, args);
    }
}

static int run_cmd(int argc, char **argv)
{
    const char *c = argv[0]; int r = 0;
    if (!strcmp(c, "mkdir")) { if (!need(argc, 2, "mkdir <path>")) return 1; r = fs_create(argv[1], 0755, 1); }
    else if (!strcmp(c, "touch")) {
        if (!need(argc, 2, "touch <path>")) return 1;
        u32 i; r = path_walk(argv[1], 0, &i, NULL);


        if (r == -ENOENT) r = fs_create(argv[1], 0644, 0);
    }
    else if (!strcmp(c, "rm")) { if (!need(argc, 2, "rm <path>")) return 1; r = fs_unlink(argv[1]); }

    else if (!strcmp(c, "rmdir")) { if (!need(argc, 2, "rmdir <path>")) return 1; r = fs_rmdir(argv[1]); }
    else if (!strcmp(c, "mv")) { if (!need(argc, 3, "mv <src> <dst>")) return 1; r = fs_rename(argv[1], argv[2]); }
    else if (!strcmp(c, "ln")) { if (!need(argc, 3, "ln <existing> <newname>")) return 1; r = fs_link(argv[1], argv[2]); }
    else if (!strcmp(c, "ls")) {
        int lng = 0, i = 1;
        if (argc > 1 && !strcmp(argv[1], "-l")) { lng = 1; i++; }
        r = cmd_ls(i < argc ? argv[i] : ".", lng);
    }
    else if (!strcmp(c, "tree")) r = cmd_tree(argc > 1 ? argv[1] : ".");
    else if (!strcmp(c, "stat")) { if (!need(argc, 2, "stat <path>")) return 1; r = cmd_stat(argv[1]); }
    else if (!strcmp(c, "cat")) { if (!need(argc, 2, "cat <path>")) return 1; r = fs_read_to(argv[1], stdout, NULL); fflush(stdout); }
    else if (!strcmp(c, "put")) { if (!need(argc, 3, "put <hostfile> <path>")) return 1; r = fs_put_host(argv[1], argv[2]); }
    else if (!strcmp(c, "get")) {
        if (!need(argc, 3, "get <path> <hostfile>")) return 1;


        FILE *o = fopen(argv[2], "wb");
        if (!o) { perror(argv[2]); return 1; }
        u64 n = 0; r = fs_read_to(argv[1], o, &n); fclose(o);
        if (!r) printf("copied %llu bytes to %s\n", (unsigned long long)n, argv[2]);
    }
    else if (!strcmp(c, "write")) {
        if (!need(argc, 3, "write <path> <text...>")) return 1;
        char text[4096] = ""; size_t l = 0;
        for (int i = 2; i < argc && l < sizeof text - 2; i++)
            l += snprintf(text + l, sizeof text - l, "%s%s", i > 2 ? " " : "", argv[i]);
        text[l++] = '\n';
        r = fs_write_bytes(argv[1], text, l);
    }
    else if (!strcmp(c, "chmod")) { if (!need(argc, 3, "chmod <octal> <path>")) return 1; r = fs_chmod(argv[2], strtol(argv[1], NULL, 8)); }
    else if (!strcmp(c, "chown")) { if (!need(argc, 4, "chown <uid> <gid> <path>")) return 1; r = fs_chown(argv[3], atoi(argv[1]), atoi(argv[2])); }
    else if (!strcmp(c, "su")) { if (!need(argc, 3, "su <uid> <gid>")) return 1; cur_uid = atoi(argv[1]); cur_gid = atoi(argv[2]); printf("now uid=%u gid=%u\n", cur_uid, cur_gid); }
    else if (!strcmp(c, "cd")) {
        u32 i; struct inode ip;
        r = path_walk(argc > 1 ? argv[1] : "/", 0, &i, NULL);
        if (!r) { iget(i, &ip); if (!is_dir(&ip)) r = -ENOTDIR; else if (!perm_ok(&ip, 1)) r = -EACCES; }
        if (!r) {
            cwd_ino = i;
            if (argc > 1 && argv[1][0] == '/') strncpy(cwd_path, argv[1], sizeof cwd_path - 1);
            else if (argc > 1 && strlen(cwd_path) + strlen(argv[1]) + 2 < sizeof cwd_path) {
                if (strcmp(cwd_path, "/")) strcat(cwd_path, "/");
                strcat(cwd_path, argv[1]);
            } else strcpy(cwd_path, "/");
        }
    }

    else if (!strcmp(c, "pwd")) puts(cwd_path);
    else if (!strcmp(c, "df") || !strcmp(c, "info")) cmd_df();
    else if (!strcmp(c, "stats")) cmd_stats();
    else if (!strcmp(c, "fsck")) r = cmd_fsck(argc > 1 && !strcmp(argv[1], "-r"));
    else if (!strcmp(c, "bench")) r = cmd_bench(argc > 1 ? atoi(argv[1]) : 16, argc > 2 ? atoi(argv[2]) : 200);
    else if (!strcmp(c, "help")) usage_text();
    else { fprintf(stderr, "vfs: unknown command '%s' (try 'help')\n", c); return 1; }
    txn_commit();
    if (r < 0) { fprintf(stderr, "vfs: %s: %s\n", c, strerror(-r)); return 1; }
    return r;
}
int main(int argc, char **argv)
{
    cur_uid = getuid(); cur_gid = getgid();
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--nosync")) do_sync = 0;
        else if (!strcmp(argv[i], "--crash") && i + 1 < argc) {
            crash_pt = atoi(argv[++i]);
            char *col = strchr(argv[i], ':'); if (col) crash_txn = atoi(col + 1);
        }
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) cur_uid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) cur_gid = atoi(argv[++i]);
        else break;
    }
    if (i >= argc || !strcmp(argv[i], "help")) {
        fprintf(stderr,
                "usage: %s [--nosync] [--crash P[:T]] [-u uid] [-g gid] <image> [command...]\n"
                "       %s mkfs <image> <size_MiB> [inodes]\n"

                "  --crash P:T  simulate power loss at journal point P (1=before commit record,\n"

                "               2=after commit/before checkpoint, 3=after checkpoint) in the T-th txn\n", argv[0], argv[0]);
        if (i < argc) usage_text();

        return 1;
    }
    if (!strcmp(argv[i], "mkfs")) {
        if (i + 2 >= argc) { fprintf(stderr, "usage: mkfs <image> <size_MiB> [inodes]\n"); return 1; }
        return do_mkfs(argv[i + 1], atoi(argv[i + 2]), i + 3 < argc ? atoi(argv[i + 3]) : 0);
    }
    if (fs_mount(argv[i]) < 0) return 1;
    int rc = 0;
    if (i + 1 >= argc || !strcmp(argv[i + 1], "shell")) shell();
    else rc = run_cmd(argc - i - 1, argv + i + 1);
    fs_unmount();
    return rc;
}

