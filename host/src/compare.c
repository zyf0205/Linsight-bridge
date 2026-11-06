#include "compare.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "canlog.h"

/* Candidate frame plus its original position. 24 bytes on a 64-bit host. */
typedef struct {
    uint32_t id;
    uint8_t flags;
    uint8_t dlc;
    uint8_t data[8];
    uint32_t idx;
} cand_entry_t;

typedef struct {
    size_t start;  /* first entry of the key group */
    size_t end;    /* one past the last entry */
    size_t cursor; /* next unused entry, FIFO by original index */
} group_t;

static int key_cmp(const linsight_can_record_t *r, const cand_entry_t *e)
{
    if (r->id != e->id) {
        return r->id < e->id ? -1 : 1;
    }
    if (r->flags != e->flags) {
        return r->flags < e->flags ? -1 : 1;
    }
    if (r->dlc != e->dlc) {
        return r->dlc < e->dlc ? -1 : 1;
    }
    return memcmp(r->data, e->data, r->dlc);
}

/* Compare only the match key (not the original index). */
static int entry_key_cmp(const cand_entry_t *a, const cand_entry_t *b)
{
    if (a->id != b->id) {
        return a->id < b->id ? -1 : 1;
    }
    if (a->flags != b->flags) {
        return a->flags < b->flags ? -1 : 1;
    }
    if (a->dlc != b->dlc) {
        return a->dlc < b->dlc ? -1 : 1;
    }
    return memcmp(a->data, b->data, a->dlc);
}

/* Sort by key, then by original index so each group stays FIFO. */
static int entry_cmp(const void *pa, const void *pb)
{
    const cand_entry_t *a = pa;
    const cand_entry_t *b = pb;
    const int c = entry_key_cmp(a, b);
    if (c != 0) {
        return c;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static int load_candidate(const char *path, cand_entry_t **out, size_t *out_n)
{
    lsc_reader_t rd;
    if (lsc_reader_open(&rd, path) != 0) {
        return -1;
    }

    size_t cap = 1024;
    size_t n = 0;
    cand_entry_t *v = malloc(cap * sizeof(*v));
    if (!v) {
        lsc_reader_close(&rd);
        return -1;
    }

    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        if (n == cap) {
            if (cap > SIZE_MAX / 2 / sizeof(*v)) {
                free(v);
                lsc_reader_close(&rd);
                errno = ENOMEM;
                return -1;
            }
            cap *= 2;
            cand_entry_t *grown = realloc(v, cap * sizeof(*v));
            if (!grown) {
                free(v);
                lsc_reader_close(&rd);
                return -1;
            }
            v = grown;
        }
        v[n] = (cand_entry_t){
            .id = rec.id,
            .flags = rec.flags,
            .dlc = rec.dlc,
            .idx = (uint32_t)n,
        };
        memcpy(v[n].data, rec.data, sizeof(v[n].data));
        n++;
    }
    lsc_reader_close(&rd);
    if (rc < 0) {
        free(v);
        return -1;
    }
    *out = v;
    *out_n = n;
    return 0;
}

static size_t find_group(const group_t *groups, size_t ngroups, const cand_entry_t *ent,
                         const linsight_can_record_t *r)
{
    size_t lo = 0;
    size_t hi = ngroups;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (key_cmp(r, &ent[groups[mid].start]) > 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < ngroups && key_cmp(r, &ent[groups[lo].start]) == 0) {
        return lo;
    }
    return (size_t)-1;
}

int canlog_compare(const char *ref_path, const char *cand_path, compare_stats_t *stats)
{
    if (!stats) {
        errno = EINVAL;
        return -1;
    }
    memset(stats, 0, sizeof(*stats));

    cand_entry_t *ent = NULL;
    size_t n = 0;
    if (load_candidate(cand_path, &ent, &n) != 0) {
        return -1;
    }
    stats->cand_frames = n;

    size_t ngroups = 0;
    group_t *groups = calloc(n ? n : 1, sizeof(*groups));
    if (!groups) {
        free(ent);
        return -1;
    }
    if (n > 0) {
        qsort(ent, n, sizeof(*ent), entry_cmp);
        for (size_t i = 0; i < n;) {
            size_t end = i + 1;
            while (end < n && entry_key_cmp(&ent[end], &ent[i]) == 0) {
                end++;
            }
            groups[ngroups++] = (group_t){.start = i, .end = end, .cursor = i};
            i = end;
        }
    }

    /* Stream the reference (snapshot at open), matching FIFO per key. */
    lsc_reader_t rd;
    if (lsc_reader_open(&rd, ref_path) != 0) {
        free(groups);
        free(ent);
        return -1;
    }

    bool have_last = false;
    uint32_t last_idx = 0;
    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        stats->ref_frames++;
        const size_t gi = find_group(groups, ngroups, ent, &rec);
        if (gi == (size_t)-1 || groups[gi].cursor == groups[gi].end) {
            stats->missing++;
            continue;
        }
        const uint32_t idx = ent[groups[gi].cursor++].idx;
        stats->matched++;
        if (have_last && idx < last_idx) {
            stats->order++;
        }
        last_idx = idx;
        have_last = true;
    }
    lsc_reader_close(&rd);

    if (rc >= 0) {
        for (size_t i = 0; i < ngroups; i++) {
            stats->extra += groups[i].end - groups[i].cursor;
        }
    }
    free(groups);
    free(ent);
    return rc < 0 ? -1 : 0;
}
