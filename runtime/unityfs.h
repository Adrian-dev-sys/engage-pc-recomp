// Unity asset bundles (UnityFS, Unity 2020.3) for the Cobalt mod merge (cobalt.c): read LZ4 / LZMA / stored bundles,
// find a serialized file's objects, read and replace a TextAsset, read an AssetBundle's container, and write an
// uncompressed bundle back.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { char name[128]; uint64_t offset, size; uint32_t flags; } UfsNode;

typedef struct {
    uint8_t *data;   // every block, decompressed
    size_t size;
    UfsNode *nodes;
    int nnodes;
} Ufs;

int ufs_read(const uint8_t *file, size_t n, Ufs *out);   // 1 on success; ufs_free afterwards
void ufs_free(Ufs *u);
int ufs_node_names(const uint8_t *file, size_t n, char (*names)[128], int cap);   // header only; the count, -1 bad
const UfsNode *ufs_serialized(const Ufs *u);   // the serialized-file node (name without '.'), or NULL

typedef struct { int64_t path_id; uint64_t start, at; uint32_t size; int class_id; } SfObject;   // start: data-relative
typedef struct {
    const uint8_t *b;
    size_t n;
    uint32_t version;
    uint64_t data_offset;
    SfObject *objs;
    int nobjs;
    char (*ext)[256];   // external file paths ("archive:/CAB-.../CAB-...")
    int next;
} Sf;

int sf_parse(const uint8_t *b, size_t n, Sf *out);   // 1 on success; sf_free afterwards
void sf_free(Sf *s);
const SfObject *sf_find(const Sf *s, int class_id);   // the first object of that class
// The TextAsset's name and script bytes (pointers into s->b); 1 on success.
int sf_text_asset(const Sf *s, const SfObject *o, const char **name, uint32_t *name_len, const uint8_t **data,
                  uint32_t *len);
// A copy of the serialized file with object o's bytes replaced by a TextAsset of name + data; *out_n its size.
uint8_t *sf_with_text_asset(const Sf *s, const SfObject *o, const char *name, uint32_t name_len, const uint8_t *data,
                            size_t len, size_t *out_n);
int sf_container_last(const Sf *s, const SfObject *bundle, char *out, size_t cap);   // AssetBundle m_Container's last key

// An uncompressed bundle holding one serialized file named cab; malloc'd, *out_n its size.
uint8_t *ufs_write(const char *cab, const uint8_t *sfile, size_t n, size_t *out_n);

int lzma_unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t out);   // Unity's LZMA block: 5 props bytes + stream
int lz4_unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t out);
