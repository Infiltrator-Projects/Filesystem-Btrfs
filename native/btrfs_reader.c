/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only Btrfs trees over partition-relative Common V2 I/O. Disk keys
 * are ordered tuples. Logical bytenrs are never physical device offsets.
 */
#include "btrfs_reader.h"
#include <infiltratr/endian.h>
#include <string.h>

#define le16 infiltratr_load_le16
#define le32 infiltratr_load_le32
#define le64 infiltratr_load_le64

enum {
    TREE_HEADER = 101,
    LEAF_SLOT = 25,
    NODE_SLOT = 33,
    KEY_SIZE = 17,
    INODE_ITEM = 1,
    DIR_ITEM = 84,
    DIR_INDEX = 96,
    EXTENT_DATA = 108,
    EXTENT_CSUM = 128,
    ROOT_ITEM = 132,
    CHUNK_ITEM = 228,
    STOP = 3
};
typedef struct {
    uint64_t id;
    uint8_t type;
    uint64_t offset;
} Key;
typedef struct {
    uint64_t logical, generation, owner;
    uint8_t level;
} Tree;
typedef struct {
    uint64_t logical, length, physical[2], flags;
    unsigned stripes;
} Mapping;
struct BtrfsReader {
    IfsFsHostRuntimeV2 host;
    uint8_t fsid[16], device_uuid[16];
    uint8_t label[256];
    uint32_t sector, node;
    uint64_t device, generation, root_inode;
    Tree roots, files, checksums;
    Mapping *map;
    size_t mappings, capacity;
    bool implicit_holes;
};
typedef int (*Visitor)(BtrfsReader *, Key, const uint8_t *, uint32_t, void *);

static Key disk_key(const uint8_t *p) { return (Key){le64(p), p[8], le64(p + 9)}; }
static int order(Key a, Key b)
{
    if (a.id != b.id)
        return a.id < b.id ? -1 : 1;
    if (a.type != b.type)
        return a.type < b.type ? -1 : 1;
    return a.offset < b.offset ? -1 : a.offset > b.offset;
}
static void *allocate(BtrfsReader *r, size_t size)
{
    return r->host.allocate(r->host.context, size);
}
static void release(BtrfsReader *r, void *p)
{
    if (p)
        r->host.release(r->host.context, p);
}

static int add_chunk(BtrfsReader *r, Key key, const uint8_t *p, uint32_t size, void *unused)
{
    (void)unused;
    if (key.id != 256 || key.type != CHUNK_ITEM || size < 48)
        return IFS_FS_CORRUPT;
    Mapping m = {
        .logical = key.offset, .length = le64(p), .flags = le64(p + 24), .stripes = le16(p + 44)};
    if (!m.stripes || size != 48u + 32u * m.stripes || !m.length || m.logical % r->sector ||
        m.length % r->sector || m.length > UINT64_MAX - m.logical || !le64(p + 16) ||
        !(m.flags & 7u))
        return IFS_FS_CORRUPT;
    /* Single and DUP each map the complete logical range onto a stripe.
     * Never apply this formula to RAID0, parity or another device. */
    if ((m.flags & ~UINT64_C(7)) != 0 && (m.flags & ~UINT64_C(7)) != 32)
        return IFS_FS_UNSUPPORTED;
    if (m.stripes != ((m.flags & 32) ? 2u : 1u))
        return IFS_FS_CORRUPT;
    for (unsigned i = 0; i < m.stripes; ++i) {
        const uint8_t *stripe = p + 48 + i * 32;
        if (le64(stripe) != r->device || memcmp(stripe + 16, r->device_uuid, 16))
            return IFS_FS_CORRUPT;
        m.physical[i] = le64(stripe + 8);
        if (m.physical[i] % r->sector || m.physical[i] > r->host.volume_bytes ||
            m.length > r->host.volume_bytes - m.physical[i])
            return IFS_FS_CORRUPT;
    }
    if (m.stripes == 2 && m.physical[0] < m.physical[1] + m.length &&
        m.physical[1] < m.physical[0] + m.length)
        return IFS_FS_CORRUPT;
    for (size_t i = 0; i < r->mappings; ++i) {
        const Mapping *old = &r->map[i];
        if (old->logical == m.logical && old->length == m.length && old->flags == m.flags &&
            old->physical[0] == m.physical[0] && old->physical[1] == m.physical[1])
            return IFS_FS_OK;
        if (m.logical < old->logical + old->length && old->logical < m.logical + m.length)
            return IFS_FS_CORRUPT;
        for (unsigned a = 0; a < m.stripes; ++a)
            for (unsigned b = 0; b < old->stripes; ++b)
                if (m.physical[a] < old->physical[b] + old->length &&
                    old->physical[b] < m.physical[a] + m.length)
                    return IFS_FS_CORRUPT;
    }
    if (r->mappings == r->capacity) {
        size_t count = r->capacity ? r->capacity * 2 : 16;
        if (count < r->capacity || count > SIZE_MAX / sizeof(m))
            return IFS_FS_NO_MEMORY;
        Mapping *next = allocate(r, count * sizeof(m));
        if (!next)
            return IFS_FS_NO_MEMORY;
        if (r->map)
            memcpy(next, r->map, r->mappings * sizeof(m));
        release(r, r->map);
        r->map = next;
        r->capacity = count;
    }
    r->map[r->mappings++] = m;
    return IFS_FS_OK;
}
/* DUP protects complete Btrfs tree blocks, not just raw reads.
 * Validate a candidate's checksum, identity, keys, and complete slot table
 * before accepting it; only then can the alternate physical replica be used.
 * Never fall back to an unrelated chunk or an unchecked metadata copy. */
static int logical_read_copy(BtrfsReader *r, uint64_t at, void *out,
                             uint32_t size, uint64_t kind, unsigned copy)
{
    for (size_t i = 0; i < r->mappings; ++i) {
        const Mapping *m = &r->map[i];
        if (at < m->logical || at - m->logical >= m->length)
            continue;
        const uint64_t delta = at - m->logical;
        if (!(m->flags & kind) || size > m->length - delta)
            return IFS_FS_CORRUPT;
        if (copy >= m->stripes)
            return IFS_FS_NOT_FOUND;
        return ifs_fs_host_read_bytes_v2(
            &r->host, m->physical[copy] + delta, out, size);
    }
    return IFS_FS_CORRUPT;
}
static int logical_read(BtrfsReader *r, uint64_t at, void *out, uint32_t size,
                        uint64_t kind)
{
    return logical_read_copy(r, at, out, size, kind, 0u);
}

/* Reject damaged metadata before traversing any key or returning a pointer.
 * The same full verification is required independently for each DUP copy. */
static int node_valid(BtrfsReader *r, Tree t, const Key *first,
                      const Key *limit, const uint8_t *b, uint32_t *count)
{
    const uint32_t n = le32(b + 96);
    const uint32_t stride = t.level ? NODE_SLOT : LEAF_SLOT;
    if (!btrfs_checksum_valid(b, r->node) || memcmp(b + 32, r->fsid, 16) ||
        le64(b + 48) != t.logical || le64(b + 80) != t.generation ||
        le64(b + 88) != t.owner || b[100] != t.level ||
        n > (r->node - TREE_HEADER) / stride ||
        (!n && (t.level || first)))
        return IFS_FS_CORRUPT;
    uint32_t end = r->node - TREE_HEADER;
    Key prev = {0};
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *slot = b + TREE_HEADER + i * stride;
        const Key k = disk_key(slot);
        if ((!i && first && order(k, *first)) ||
            (i && order(prev, k) >= 0) ||
            (limit && order(k, *limit) >= 0))
            return IFS_FS_CORRUPT;
        if (!t.level) {
            const uint32_t off = le32(slot + 17);
            const uint32_t size = le32(slot + 21);
            if (off < n * stride || off > end || size != end - off)
                return IFS_FS_CORRUPT;
            end = off;
        }
        prev = k;
    }
    *count = n;
    return IFS_FS_OK;
}
static int load_node(BtrfsReader *r, Tree t, const Key *first,
                     const Key *limit, uint8_t **out, uint32_t *count)
{
    if (!t.logical || t.logical % r->sector || t.level > 8 ||
        !t.generation || t.generation > r->generation)
        return IFS_FS_CORRUPT;
    uint8_t *b = allocate(r, r->node);
    if (!b)
        return IFS_FS_NO_MEMORY;
    int last = IFS_FS_CORRUPT;
    for (unsigned copy = 0u; copy < 2u; ++copy) {
        int rc = logical_read_copy(r, t.logical, b, r->node,
                                   t.owner == 3 ? 2u : 4u, copy);
        if (rc == IFS_FS_NOT_FOUND)
            break; /* SINGLE metadata has no second physical copy. */
        if (rc == IFS_FS_OK) {
            rc = node_valid(r, t, first, limit, b, count);
            if (rc == IFS_FS_OK) {
                *out = b;
                return IFS_FS_OK;
            }
        }
        last = rc;
    }
    release(r, b);
    return last;
}
/* floor selects the last key <= high (checksum items cover ranges); otherwise
 * visit every record in the closed interval [low, high], in disk-key order. */
static int walk(BtrfsReader *r, Tree t, const Key *first, const Key *limit, Key low, Key high,
                bool floor, Visitor visit, void *context)
{
    uint8_t *b = NULL;
    uint32_t n = 0;
    int rc = load_node(r, t, first, limit, &b, &n);
    if (rc != IFS_FS_OK)
        return rc;
    uint32_t stride = t.level ? NODE_SLOT : LEAF_SLOT;
    for (uint32_t i = 0; i < n && rc == IFS_FS_OK; ++i) {
        const uint8_t *s = b + TREE_HEADER + i * stride;
        Key key = disk_key(s), next;
        const Key *upper = limit;
        if (i + 1 < n) {
            next = disk_key(s + stride);
            upper = &next;
        }
        if (order(key, high) > 0)
            break;
        if (floor && upper && order(*upper, high) <= 0)
            continue;
        if (t.level) {
            if (!floor && upper && order(*upper, low) <= 0)
                continue;
            Tree child = {le64(s + 17), le64(s + 25), t.owner, t.level - 1};
            if (child.generation > t.generation) {
                rc = IFS_FS_CORRUPT;
                break;
            }
            rc = walk(r, child, &key, upper, low, high, floor, visit, context);
        } else if (order(key, low) >= 0) {
            rc = visit(r, key, b + TREE_HEADER + le32(s + 17), le32(s + 21), context);
        }
    }
    release(r, b);
    return rc;
}
static int range(BtrfsReader *r, Tree t, uint64_t id, uint8_t type, Visitor visit, void *context)
{
    return walk(r, t, NULL, NULL, (Key){id, type, 0}, (Key){id, type, UINT64_MAX}, false, visit,
                context);
}
typedef struct {
    Tree tree;
    uint64_t directory;
} Root;
static int read_root(BtrfsReader *r, Key k, const uint8_t *p, uint32_t size, void *ctx)
{
    (void)r;
    if (size < 239)
        return IFS_FS_CORRUPT;
    Root *out = ctx;
    out->tree = (Tree){le64(p + 176), le64(p + 160), k.id, p[238]};
    out->directory = le64(p + 168);
    return STOP;
}
static int lookup_root(BtrfsReader *r, uint64_t id, Root *out)
{
    int rc = walk(r, r->roots, NULL, NULL, (Key){id, ROOT_ITEM, 0},
                  (Key){id, ROOT_ITEM, UINT64_MAX}, true, read_root, out);
    return rc == STOP ? IFS_FS_OK : rc == IFS_FS_OK ? IFS_FS_CORRUPT : rc;
}

typedef struct {
    uint64_t size, flags;
    uint32_t mode, uid, gid;
} Inode;
static int read_inode(BtrfsReader *r, Key k, const uint8_t *p, uint32_t size, void *ctx)
{
    (void)r;
    (void)k;
    if (size != 160)
        return IFS_FS_CORRUPT;
    *(Inode *)ctx = (Inode){.size=le64(p + 16), .flags=le64(p + 64),
        .mode=le32(p + 52), .uid=le32(p + 44), .gid=le32(p + 48)};
    return STOP;
}
static int inode(BtrfsReader *r, Tree tree, uint64_t id, Inode *out)
{
    Key k = {id, INODE_ITEM, 0};
    int rc = walk(r, tree, NULL, NULL, k, k, false, read_inode, out);
    return rc == STOP ? IFS_FS_OK : rc == IFS_FS_OK ? IFS_FS_CORRUPT : rc;
}

typedef struct {
    const char *match;
    uint32_t length, skip;
    unsigned found; /* match-key uniqueness across every DIR_INDEX item */
    char name[256];
    Key location;
    uint8_t type;
} DirectorySearch;
static int dir_item(BtrfsReader *r, Key key, const uint8_t *p, uint32_t size, void *ctx)
{
    (void)r;
    DirectorySearch *find = ctx;
    while (size) {
        if (size < 30)
            return IFS_FS_CORRUPT;
        uint32_t data = le16(p + 25), n = le16(p + 27);
        if (!n || n > 255 || data || n > size - 30 || memchr(p + 30, 0, n) ||
            memchr(p + 30, '/', n))
            return IFS_FS_CORRUPT;
        if (key.type == DIR_INDEX && size != 30 + n)
            return IFS_FS_CORRUPT;
        bool match =
            find->match ? n == find->length && !memcmp(p + 30, find->match, n) : find->skip == 0;
        if (match) {
            /* Btrfs DIR_INDEX offsets are distinct, but a malformed tree
             * can still name the same file twice. A pathname must resolve
             * uniquely; scan every item instead of accepting the first. */
            if (find->match && find->found)
                return IFS_FS_CORRUPT;
            find->location = disk_key(p);
            if ((find->location.type != INODE_ITEM && find->location.type != ROOT_ITEM) ||
                !find->location.id)
                return IFS_FS_CORRUPT;
            find->type = p[29];
            memcpy(find->name, p + 30, n);
            find->name[n] = 0;
            find->length = n;
            find->found = 1u;
            if (!find->match)
                return STOP; /* indexed enumeration only needs one item */
        }
        if (!find->match)
            --find->skip;
        p += 30 + n;
        size -= 30 + n;
    }
    return IFS_FS_OK;
}
typedef struct {
    Tree tree;
    uint64_t id;
    Inode inode;
} File;
static int follow(BtrfsReader *r, File *f, const DirectorySearch *d)
{
    if (d->location.type == ROOT_ITEM)
        return IFS_FS_UNSUPPORTED; /* nested subvolume */
    if (d->location.offset || d->location.id < 256)
        return IFS_FS_CORRUPT;
    f->id = d->location.id;
    int rc = inode(r, f->tree, f->id, &f->inode);
    if (rc != IFS_FS_OK)
        return rc;
    uint32_t mode = f->inode.mode & 0170000u;
    static const uint32_t types[] = {0,       0100000, 0040000, 0020000,
                                     0060000, 0010000, 0140000, 0120000};
    if (d->type >= sizeof(types) / sizeof(types[0]) || (d->type && types[d->type] != mode))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int resolve(BtrfsReader *r, const char *path, File *f, const char **name, size_t *length)
{
    if (!path || path[0] != '/')
        return IFS_FS_INVALID;
    f->tree = r->files;
    f->id = r->root_inode;
    int rc = inode(r, f->tree, f->id, &f->inode);
    if (rc != IFS_FS_OK)
        return rc;
    *name = "/";
    *length = 1;
    if (!*++path)
        return IFS_FS_OK;
    for (;;) {
        const char *start = path;
        while (*path && *path != '/' && (size_t)(path - start) < 256)
            ++path;
        size_t n = (size_t)(path - start);
        if (!n || n > 255 || (n == 1 && start[0] == '.') || (n == 2 && !memcmp(start, "..", 2)))
            return IFS_FS_INVALID;
        if ((f->inode.mode & 0170000u) != 0040000u)
            return IFS_FS_NOT_FOUND;
        DirectorySearch d = {.match = start, .length = (uint32_t)n};
        rc = range(r, f->tree, f->id, DIR_INDEX, dir_item, &d);
        if (rc != IFS_FS_OK)
            return rc;
        if (!d.found)
            return IFS_FS_NOT_FOUND;
        rc = follow(r, f, &d);
        if (rc != IFS_FS_OK)
            return rc;
        *name = start;
        *length = n;
        if (!*path)
            return IFS_FS_OK;
        ++path;
    }
}
static int entry(const File *f, const char *name, size_t n, IfsFsEntryV2 *out)
{
    if (!out || out->structure_bytes < sizeof(*out) || !out->name)
        return IFS_FS_INVALID;
    if (out->name_capacity <= n)
        return IFS_FS_NO_SPACE;
    uint32_t type;
    switch (f->inode.mode & 0170000u) {
    case 0040000:
        type = IFS_FS_ENTRY_DIRECTORY;
        break;
    case 0100000:
        type = IFS_FS_ENTRY_FILE;
        break;
    case 0120000:
        type = IFS_FS_ENTRY_LINK;
        break;
    default:
        return IFS_FS_UNSUPPORTED;
    }
    memcpy(out->name, name, n);
    out->name[n] = 0;
    out->name_bytes = (uint32_t)n;
    out->size_bytes = f->inode.size;
    out->attributes = 0;
    out->type = type;
    return IFS_FS_OK;
}
/* Return native inode ownership and permissions from the same verified
 * namespace traversal used by stat. No synthetic modes, no mutation. */
/* The label is immutable for the lifetime of this read-only session. */
int btrfs_reader_label(const BtrfsReader *r,char *out,uint32_t capacity,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    if(!r || !out || !capacity)return IFS_FS_INVALID;
    uint32_t n=0u;
    while(n<sizeof(r->label) && r->label[n])++n;
    if(n>=capacity)return IFS_FS_NO_SPACE;
    memcpy(out,r->label,n);
    out[n]=0;*bytes=n;
    return IFS_FS_OK;
}
int btrfs_reader_metadata(BtrfsReader *r,const char *path,IfsFsMetadataV2 *out)
{
    if(!out || out->structure_bytes<sizeof(*out)) return IFS_FS_INVALID;
    File f;
    const char *name;
    size_t length;
    int rc=resolve(r,path,&f,&name,&length);
    if(rc) return rc;
    *out=(IfsFsMetadataV2){.structure_bytes=sizeof(*out),.mask=IFS_FS_META_GET_MODE,
        .mode=f.inode.mode,.owner=f.inode.uid,.group=f.inode.gid};
    return IFS_FS_OK;
}
int btrfs_reader_stat(BtrfsReader *r, const char *path, IfsFsEntryV2 *out)
{
    File f;
    const char *name;
    size_t n;
    int rc = resolve(r, path, &f, &name, &n);
    return rc == IFS_FS_OK ? entry(&f, name, n, out) : rc;
}
int btrfs_reader_list(BtrfsReader *r, const char *path, uint32_t index, IfsFsEntryV2 *out)
{
    File f;
    const char *name;
    size_t n;
    int rc = resolve(r, path, &f, &name, &n);
    if (rc != IFS_FS_OK)
        return rc;
    if ((f.inode.mode & 0170000u) != 0040000u)
        return IFS_FS_INVALID;
    DirectorySearch d = {.skip = index};
    rc = range(r, f.tree, f.id, DIR_INDEX, dir_item, &d);
    if (rc != STOP)
        return rc == IFS_FS_OK ? IFS_FS_END : rc;
    rc = follow(r, &f, &d);
    return rc == IFS_FS_OK ? entry(&f, d.name, d.length, out) : rc;
}

typedef struct {
    uint64_t at, end;
    uint8_t *dest, *sector;
} DataRead;
/* Each checksum item covers successive full filesystem sectors. Even a
 * one-byte caller read checks the complete sector before copying its slice. */
static int checked_sectors(BtrfsReader *r, Key k, const uint8_t *p, uint32_t size, void *ctx)
{
    DataRead *d = ctx;
    if (!size || size % 4 || k.offset % r->sector)
        return IFS_FS_CORRUPT;
    uint64_t span = (uint64_t)(size / 4) * r->sector;
    if (span > UINT64_MAX - k.offset || d->at < k.offset || d->at - k.offset >= span)
        return IFS_FS_CORRUPT;
    while (d->at < d->end && d->at - k.offset < span) {
        uint64_t skip = d->at % r->sector, base = d->at - skip;
        int rc = logical_read(r, base, d->sector, r->sector, 1);
        if (rc != IFS_FS_OK)
            return rc;
        if (btrfs_crc32c(d->sector, r->sector) != le32(p + 4 * ((base - k.offset) / r->sector)))
            return IFS_FS_CORRUPT;
        uint64_t n = r->sector - skip < d->end - d->at ? r->sector - skip : d->end - d->at;
        memcpy(d->dest, d->sector + skip, (size_t)n);
        d->dest += n;
        d->at += n;
    }
    return STOP;
}
static int checked_data(BtrfsReader *r, uint64_t at, uint8_t *dest, uint64_t length)
{
    if (length > UINT64_MAX - at)
        return IFS_FS_CORRUPT;
    DataRead d = {at, at + length, dest, allocate(r, r->sector)};
    if (!d.sector)
        return IFS_FS_NO_MEMORY;
    int rc = IFS_FS_OK;
    while (d.at < d.end) {
        Key low = {UINT64_MAX - 9, EXTENT_CSUM, 0},
            high = {low.id, low.type, d.at - d.at % r->sector};
        rc = walk(r, r->checksums, NULL, NULL, low, high, true, checked_sectors, &d);
        if (rc != STOP) {
            if (rc == IFS_FS_OK)
                rc = IFS_FS_CORRUPT;
            break;
        }
        rc = IFS_FS_OK;
    }
    release(r, d.sector);
    return rc;
}
typedef struct {
    uint64_t offset, end, previous_end, file_size, flags;
    uint8_t *dest;
} FileRead;
static int read_extent(BtrfsReader *r, Key k, const uint8_t *p, uint32_t size, void *ctx)
{
    FileRead *d = ctx;
    if (size < 21 || k.offset % r->sector || k.offset < d->previous_end)
        return IFS_FS_CORRUPT;
    uint8_t type = p[20];
    uint64_t ram = le64(p + 8), len;
    if (type == 0) {
        if (k.offset || !ram || ram != d->file_size)
            return IFS_FS_CORRUPT;
        len = ram;
    } else {
        if ((type != 1 && type != 2) || size != 53)
            return IFS_FS_CORRUPT;
        len = le64(p + 45);
        if (!len || len % r->sector)
            return IFS_FS_CORRUPT;
    }
    if (len > UINT64_MAX - k.offset)
        return IFS_FS_CORRUPT;
    if (!r->implicit_holes && k.offset > d->previous_end && k.offset > d->offset &&
        d->previous_end < d->end)
        return IFS_FS_CORRUPT;
    d->previous_end = k.offset + len;
    uint64_t lo = k.offset > d->offset ? k.offset : d->offset,
             hi = d->previous_end < d->end ? d->previous_end : d->end;
    if (hi <= lo)
        return IFS_FS_OK;
    if (p[16] || p[17] || le16(p + 18))
        return IFS_FS_UNSUPPORTED;
    uint8_t *dest = d->dest + (lo - d->offset);
    if (type == 0) {
        if (size - 21 != ram)
            return IFS_FS_CORRUPT;
        memcpy(dest, p + 21 + (lo - k.offset), (size_t)(hi - lo));
        return IFS_FS_OK;
    }
    uint64_t disk = le64(p + 21), disk_len = le64(p + 29), delta = le64(p + 37);
    if (!disk)
        return (!disk_len && !delta && type == 1) ? IFS_FS_OK : IFS_FS_CORRUPT;
    if (disk % r->sector || disk_len % r->sector || delta % r->sector || ram != disk_len ||
        delta > disk_len || len > disk_len - delta || disk_len > UINT64_MAX - disk)
        return IFS_FS_CORRUPT;
    if (type == 2)
        return IFS_FS_OK; /* unwritten reservation */
    if (d->flags & 1u)
        return IFS_FS_UNSUPPORTED; /* NODATASUM */
    return checked_data(r, disk + delta + (lo - k.offset), dest, hi - lo);
}
int btrfs_reader_read(BtrfsReader *r, const char *path, uint64_t offset, void *out, uint32_t bytes)
{
    File f;
    const char *name;
    size_t n;
    if (bytes && !out)
        return IFS_FS_INVALID;
    int rc = resolve(r, path, &f, &name, &n);
    if (rc != IFS_FS_OK)
        return rc;
    if ((f.inode.mode & 0170000u) != 0100000u)
        return IFS_FS_UNSUPPORTED;
    if (offset > f.inode.size || bytes > f.inode.size - offset)
        return IFS_FS_INVALID;
    if (!bytes)
        return IFS_FS_OK;
    /* Gaps between file extents and explicit/preallocated holes read as zero. */
    memset(out, 0, bytes);
    FileRead d = {offset, offset + bytes, 0, f.inode.size, f.inode.flags, out};
    rc = walk(r, f.tree, NULL, NULL, (Key){f.id, EXTENT_DATA, 0},
              (Key){f.id, EXTENT_DATA, d.end - 1}, false, read_extent, &d);
    if (rc == IFS_FS_OK && !r->implicit_holes && d.previous_end < d.end)
        return IFS_FS_CORRUPT;
    return rc;
}

int btrfs_reader_open(const IfsFsHostRuntimeV2 *host, BtrfsReader **out)
{
    *out = NULL;
    BtrfsReader *r = host->allocate(host->context, sizeof(*r));
    if (!r)
        return IFS_FS_NO_MEMORY;
    memset(r, 0, sizeof(*r));
    size_t supplied =
        host->structure_bytes < sizeof(r->host) ? host->structure_bytes : sizeof(r->host);
    memcpy(&r->host, host, supplied);
    r->host.structure_bytes = (uint32_t)supplied;
    uint8_t *s = allocate(r, 4096);
    int rc = s ? ifs_fs_host_read_bytes_v2(&r->host, 65536, s, 4096) : IFS_FS_NO_MEMORY;
    if (rc != IFS_FS_OK)
        goto done;
    if (memcmp(s + 64, "_BHRfS_M", 8) || le16(s + 196) || !btrfs_checksum_valid(s, 4096) ||
        le64(s + 48) != 65536 || s[198] >= 8u ||
        s[199] >= 8u || s[200] >= 8u) {
        rc = IFS_FS_CORRUPT;
        goto done;
    }
    r->sector = le32(s + 144);
    r->node = le32(s + 148);
    r->device = le64(s + 201);
    r->generation = le64(s + 72);
    if (!r->device || !r->generation || r->sector < 4096 || r->node < r->sector ||
        r->node > 65536 || (r->sector & (r->sector - 1)) || (r->node & (r->node - 1))) {
        rc = IFS_FS_CORRUPT;
        goto done;
    }
    /* Active tree logs need replay. Unsupported identity and layout features
     * cannot be inferred from a plausible CRC or the superblock signature. */
    if (le64(s + 96) || le64(s + 136) != 1 || (le64(s + 56) & ~UINT64_C(1)) ||
        (le64(s + 188) & ~UINT64_C(0x37f)) || (le64(s + 180) & ~UINT64_C(3))) {
        rc = IFS_FS_UNSUPPORTED;
        goto done;
    }
    r->implicit_holes = (le64(s + 188) & UINT64_C(0x200)) != 0;
    memcpy(r->fsid, s + 32, 16);
    /* Preserve only the label of the checksum-validated superblock. */
    memcpy(r->label,s+299u,sizeof(r->label));
    memcpy(r->device_uuid, s + 201 + 66, 16);
    if (memcmp(s + 201 + 82, r->fsid, 16)) {
        rc = IFS_FS_CORRUPT;
        goto done;
    }
    r->roots = (Tree){le64(s + 80), r->generation, 1, s[198]};
    Tree chunks = {le64(s + 88), le64(s + 164), 3, s[199]};
    uint32_t length = le32(s + 160), pos = 0;
    if (!length || length > 2048) {
        rc = IFS_FS_CORRUPT;
        goto done;
    }
    while (pos < length) {
        const uint8_t *k = s + 811 + pos, *p = k + KEY_SIZE;
        if (length - pos < KEY_SIZE + 48) {
            rc = IFS_FS_CORRUPT;
            goto done;
        }
        uint32_t bytes = 48 + 32u * le16(p + 44);
        if (bytes > length - pos - KEY_SIZE || !(le64(p + 24) & 2)) {
            rc = IFS_FS_CORRUPT;
            goto done;
        }
        rc = add_chunk(r, disk_key(k), p, bytes, NULL);
        if (rc != IFS_FS_OK)
            goto done;
        pos += KEY_SIZE + bytes;
    }
    rc = range(r, chunks, 256, CHUNK_ITEM, add_chunk, NULL);
    if (rc != IFS_FS_OK)
        goto done;
    Root root;
    rc = lookup_root(r, 7, &root);
    if (rc != IFS_FS_OK)
        goto done;
    r->checksums = root.tree;
    uint64_t fs_root = 5;
    if (le64(s + 188) & 2) { /* DEFAULT_SUBVOL: follow the recorded root-directory entry. */
        DirectorySearch d = {.match = "default", .length = 7};
        rc = range(r, r->roots, le64(s + 128), DIR_ITEM, dir_item, &d);
        if (rc != IFS_FS_OK || !d.found || d.location.type != ROOT_ITEM) {
            if (rc == IFS_FS_OK)
                rc = IFS_FS_CORRUPT;
            goto done;
        }
        fs_root = d.location.id;
    }
    rc = lookup_root(r, fs_root, &root);
    if (rc != IFS_FS_OK)
        goto done;
    r->files = root.tree;
    r->root_inode = root.directory;
    Inode st;
    rc = inode(r, r->files, r->root_inode, &st);
    if (rc == IFS_FS_OK && (st.mode & 0170000u) != 0040000u)
        rc = IFS_FS_CORRUPT;
done:
    release(r, s);
    if (rc == IFS_FS_OK)
        *out = r;
    else
        btrfs_reader_close(r);
    return rc;
}
void btrfs_reader_close(BtrfsReader *r)
{
    release(r, r->map);
    release(r, r);
}
