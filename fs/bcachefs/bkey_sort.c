// SPDX-License-Identifier: GPL-2.0
#include "bcachefs.h"
#include "bkey_buf.h"
#include "bkey_cmp.h"
#include "bkey_sort.h"
#include "bset.h"
#include "extents.h"

typedef int (*sort_cmp_fn)(const struct btree *,
			   const struct bkey_packed *,
			   const struct bkey_packed *);

static inline bool sort_iter_end(struct sort_iter *iter)
{
	return !iter->used;
}

static inline void sort_iter_sift(struct sort_iter *iter, unsigned from,
				  sort_cmp_fn cmp)
{
	unsigned i;

	for (i = from;
	     i + 1 < iter->used &&
	     cmp(iter->b, iter->data[i].k, iter->data[i + 1].k) > 0;
	     i++)
		swap(iter->data[i], iter->data[i + 1]);
}

static inline void sort_iter_sort(struct sort_iter *iter, sort_cmp_fn cmp)
{
	unsigned i = iter->used;

	while (i--)
		sort_iter_sift(iter, i, cmp);
}

static inline struct bkey_packed *sort_iter_peek(struct sort_iter *iter)
{
	return !sort_iter_end(iter) ? iter->data->k : NULL;
}

static inline void sort_iter_advance(struct sort_iter *iter, sort_cmp_fn cmp)
{
	struct sort_iter_set *i = iter->data;

	BUG_ON(!iter->used);

	i->k = bkey_p_next(i->k);

	BUG_ON(i->k > i->end);

	if (i->k == i->end)
		array_remove_item(iter->data, iter->used, 0);
	else
		sort_iter_sift(iter, 0, cmp);
}

static inline struct bkey_packed *sort_iter_next(struct sort_iter *iter,
						 sort_cmp_fn cmp)
{
	struct bkey_packed *ret = sort_iter_peek(iter);

	if (ret)
		sort_iter_advance(iter, cmp);

	return ret;
}

/*
 * If keys compare equal, compare by pointer order:
 */
static inline int key_sort_fix_overlapping_cmp(const struct btree *b,
					       const struct bkey_packed *l,
					       const struct bkey_packed *r)
{
	return bch2_bkey_cmp_packed(b, l, r) ?:
		cmp_int((unsigned long) l, (unsigned long) r);
}

static inline bool should_drop_next_key(struct sort_iter *iter)
{
	/*
	 * key_sort_cmp() ensures that when keys compare equal the older key
	 * comes first; so if l->k compares equal to r->k then l->k is older
	 * and should be dropped.
	 */
	return iter->used >= 2 &&
		!bch2_bkey_cmp_packed(iter->b,
				 iter->data[0].k,
				 iter->data[1].k);
}

struct btree_nr_keys
bch2_key_sort_fix_overlapping(struct bch_fs *c, struct bset *dst,
			      struct sort_iter *iter)
{
	struct bkey_packed *out = dst->start;
	struct bkey_packed *k;
	struct btree_nr_keys nr;

	memset(&nr, 0, sizeof(nr));

	sort_iter_sort(iter, key_sort_fix_overlapping_cmp);

	while ((k = sort_iter_peek(iter))) {
		if (!bkey_deleted(k) &&
		    !should_drop_next_key(iter)) {
			bkey_p_copy(out, k);
			btree_keys_account_key_add(&nr, 0, out);
			out = bkey_p_next(out);
		}

		sort_iter_advance(iter, key_sort_fix_overlapping_cmp);
	}

	dst->u64s = cpu_to_le16((u64 *) out - dst->_data);
	return nr;
}

/* Sort + repack in a new format: */
struct btree_nr_keys
bch2_sort_repack(struct bset *dst, struct btree *src,
		 struct btree_node_iter *src_iter,
		 struct bkey_format *out_f,
		 bool filter_whiteouts)
{
	struct bkey_format *in_f = &src->format;
	struct bkey_packed *in, *out = vstruct_last(dst);
	struct btree_nr_keys nr;
	bool transform = memcmp(out_f, &src->format, sizeof(*out_f));

	memset(&nr, 0, sizeof(nr));

	while ((in = bch2_btree_node_iter_next_all(src_iter, src))) {
		if (filter_whiteouts && bkey_deleted(in))
			continue;

		if (!transform)
			bkey_p_copy(out, in);
		else if (bch2_bkey_transform(out_f, out, bkey_packed(in)
					     ? in_f : &bch2_bkey_format_current, in))
			out->format = KEY_FORMAT_LOCAL_BTREE;
		else
			bch2_bkey_unpack(src, (void *) out, in);

		out->needs_whiteout = false;

		btree_keys_account_key_add(&nr, 0, out);
		out = bkey_p_next(out);
	}

	dst->u64s = cpu_to_le16((u64 *) out - dst->_data);
	return nr;
}

static inline int keep_unwritten_whiteouts_cmp(const struct btree *b,
				const struct bkey_packed *l,
				const struct bkey_packed *r)
{
	return bch2_bkey_cmp_packed_inlined(b, l, r) ?:
		(int) bkey_deleted(r) - (int) bkey_deleted(l) ?:
		(long) l - (long) r;
}

#include "btree_update_interior.h"

/*
 * For sorting in the btree node write path: whiteouts not in the unwritten
 * whiteouts area are dropped, whiteouts in the unwritten whiteouts area are
 * dropped if overwritten by real keys:
 */
unsigned bch2_sort_keys_keep_unwritten_whiteouts(struct bkey_packed *dst, struct sort_iter *iter)
{
	struct bkey_packed *in, *next, *out = dst;

	sort_iter_sort(iter, keep_unwritten_whiteouts_cmp);

	while ((in = sort_iter_next(iter, keep_unwritten_whiteouts_cmp))) {
		if (bkey_deleted(in) && in < unwritten_whiteouts_start(iter->b))
			continue;

		if ((next = sort_iter_peek(iter)) &&
		    !bch2_bkey_cmp_packed_inlined(iter->b, in, next))
			continue;

		bkey_p_copy(out, in);
		out = bkey_p_next(out);
	}

	return (u64 *) out - (u64 *) dst;
}

/*
 * Main sort routine for compacting a btree node in memory: we always drop
 * whiteouts because any whiteouts that need to be written are in the unwritten
 * whiteouts area:
 */
unsigned bch2_sort_keys(struct bkey_packed *dst, struct sort_iter *iter)
{
	struct bkey_packed *in, *out = dst;

	sort_iter_sort(iter, bch2_bkey_cmp_packed_inlined);

	while ((in = sort_iter_next(iter, bch2_bkey_cmp_packed_inlined))) {
		if (bkey_deleted(in))
			continue;

		bkey_p_copy(out, in);
		out = bkey_p_next(out);
	}

	return (u64 *) out - (u64 *) dst;
}
