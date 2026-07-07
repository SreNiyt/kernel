// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012-2013 Samsung Electronics Co., Ltd.
 */

#include <linux/slab.h>
#include <asm/unaligned.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>

#include "exfat_raw.h"
#include "exfat_fs.h"

static int exfat_mirror_bh(struct super_block *sb,
                struct buffer_head *bh)
{
        struct buffer_head *c_bh;
        struct exfat_sb_info *sbi = EXFAT_SB(sb);
        sector_t sec = bh->b_blocknr;
        sector_t sec2;
        int err = 0;

        if (sbi->FAT2_start_sector != sbi->FAT1_start_sector) {
                sec2 = sec - sbi->FAT1_start_sector + sbi->FAT2_start_sector;
                c_bh = sb_getblk(sb, sec2);
                if (!c_bh)
                        return -ENOMEM;

                memcpy(c_bh->b_data, bh->b_data, sb->s_blocksize);
                exfat_update_bh(c_bh, sb->s_flags & SB_SYNCHRONOUS);
                brelse(c_bh);
        }

        return err;
}

static int exfat_end_bh(struct super_block *sb, struct buffer_head *bh)
{
        int err;

        exfat_update_bh(bh, sb->s_flags & SB_SYNCHRONOUS);
        err = exfat_mirror_bh(sb, bh);
        brelse(bh);

        return err;
}

static int __exfat_ent_get(struct super_block *sb, unsigned int loc,
		unsigned int *content)
{
	unsigned int off;
	sector_t sec;
	struct buffer_head *bh;

	sec = FAT_ENT_OFFSET_SECTOR(sb, loc);
	off = FAT_ENT_OFFSET_BYTE_IN_SECTOR(sb, loc);

	bh = sb_bread(sb, sec);
	if (!bh)
		return -EIO;

	*content = le32_to_cpu(*(__le32 *)(&bh->b_data[off]));

	/* remap reserved clusters to simplify code */
	if (*content > EXFAT_BAD_CLUSTER)
		*content = EXFAT_EOF_CLUSTER;

	brelse(bh);
	return 0;
}

static int __exfat_ent_set(struct super_block *sb,
        unsigned int loc, unsigned int content,
        struct buffer_head **cache)
{
        unsigned int off;
        sector_t sec;
        __le32 *fat_entry;
        struct buffer_head *bh = cache ? *cache : NULL;

        sec = FAT_ENT_OFFSET_SECTOR(sb, loc);
        off = FAT_ENT_OFFSET_BYTE_IN_SECTOR(sb, loc);

        if (!bh || bh->b_blocknr != sec || !buffer_uptodate(bh)) {
                if (bh)
                        exfat_end_bh(sb, bh);

                bh = sb_bread(sb, sec);

                if (cache)
                        *cache = bh;

                if (!bh)
                        return -EIO;
        }

        fat_entry = (__le32 *)&(bh->b_data[off]);
        *fat_entry = cpu_to_le32(content);

        if (!cache)
                exfat_end_bh(sb, bh);

        return 0;
}

int exfat_ent_set(struct super_block *sb,
        unsigned int loc,
        unsigned int content)
{
        return __exfat_ent_set(sb, loc, content, NULL);
}

static inline bool is_valid_cluster(struct exfat_sb_info *sbi,
		unsigned int clus)
{
	if (clus < EXFAT_FIRST_CLUSTER || sbi->num_clusters <= clus)
		return false;
	return true;
}

int exfat_ent_get(struct super_block *sb, unsigned int loc,
		unsigned int *content)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	int err;

	if (!is_valid_cluster(sbi, loc)) {
		exfat_fs_error(sb, "invalid access to FAT (entry 0x%08x)",
			loc);
		return -EIO;
	}

	err = __exfat_ent_get(sb, loc, content);
	if (err) {
		exfat_fs_error(sb,
			"failed to access to FAT (entry 0x%08x, err:%d)",
			loc, err);
		return err;
	}

	if (*content == EXFAT_FREE_CLUSTER) {
		exfat_fs_error(sb,
			"invalid access to FAT free cluster (entry 0x%08x)",
			loc);
		return -EIO;
	}

	if (*content == EXFAT_BAD_CLUSTER) {
		exfat_fs_error(sb,
			"invalid access to FAT bad cluster (entry 0x%08x)",
			loc);
		return -EIO;
	}

	if (*content != EXFAT_EOF_CLUSTER && !is_valid_cluster(sbi, *content)) {
		exfat_fs_error(sb,
			"invalid access to FAT (entry 0x%08x) bogus content (0x%08x)",
			loc, *content);
		return -EIO;
	}

	return 0;
}

static void exfat_blk_readahead(struct super_block *sb, sector_t sec,
                sector_t *ra, blkcnt_t *ra_cnt, sector_t end)
{
        struct blk_plug plug;

        if (sec < *ra)
                return;

        *ra += *ra_cnt;

        if (*ra >= end)
                return;

        *ra_cnt = min(end - *ra + 1, EXFAT_BLK_RA_SIZE(sb));
        if (*ra_cnt == 0) {
                *ra = end;
                return;
        }

        blk_start_plug(&plug);
        for (unsigned int i = 0; i < *ra_cnt; i++)
                sb_breadahead(sb, *ra + i);
        blk_finish_plug(&plug);
}

int exfat_chain_cont_cluster(struct super_block *sb, unsigned int chain,
                unsigned int len)
{
        struct buffer_head *bh = NULL;
        sector_t sec, end, ra;
        blkcnt_t ra_cnt;

        if (!len)
                return 0;

        ra_cnt = 0;
        ra = FAT_ENT_OFFSET_SECTOR(sb, chain);
        end = FAT_ENT_OFFSET_SECTOR(sb, chain + len - 1);

        while (len > 1) {
                sec = FAT_ENT_OFFSET_SECTOR(sb, chain);
                exfat_blk_readahead(sb, sec, &ra, &ra_cnt, end);

                if (__exfat_ent_set(sb, chain, chain + 1, &bh))
                        return -EIO;

                chain++;
                len--;
        }

        if (__exfat_ent_set(sb, chain, EXFAT_EOF_CLUSTER, &bh))
                return -EIO;

        exfat_end_bh(sb, bh);
        return 0;
}

int exfat_free_cluster(struct inode *inode, struct exfat_chain *p_chain)
{
	unsigned int num_clusters = 0;
	unsigned int clu;
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);

	/* invalid cluster number */
	if (p_chain->dir == EXFAT_FREE_CLUSTER ||
	    p_chain->dir == EXFAT_EOF_CLUSTER ||
	    p_chain->dir < EXFAT_FIRST_CLUSTER)
		return 0;

	/* no cluster to truncate */
	if (p_chain->size == 0)
		return 0;

	/* check cluster validation */
	if (!is_valid_cluster(sbi, p_chain->dir)) {
		exfat_err(sb, "invalid start cluster (%u)", p_chain->dir);
		return -EIO;
	}

	clu = p_chain->dir;

	if (p_chain->flags == ALLOC_NO_FAT_CHAIN) {
		do {
			exfat_clear_bitmap(inode, clu);
			clu++;

			num_clusters++;
		} while (num_clusters < p_chain->size);
	} else {
		do {
			exfat_clear_bitmap(inode, clu);

			if (exfat_get_next_cluster(sb, &clu))
				goto dec_used_clus;

			num_clusters++;
		} while (clu != EXFAT_EOF_CLUSTER);
	}

dec_used_clus:
	sbi->used_clusters -= num_clusters;
	return 0;
}

int exfat_find_last_cluster(struct super_block *sb, struct exfat_chain *p_chain,
		unsigned int *ret_clu)
{
	unsigned int clu, next;
	unsigned int count = 0;

	next = p_chain->dir;
	if (p_chain->flags == ALLOC_NO_FAT_CHAIN) {
		*ret_clu = next + p_chain->size - 1;
		return 0;
	}

	do {
		count++;
		clu = next;
		if (exfat_ent_get(sb, clu, &next))
			return -EIO;
	} while (next != EXFAT_EOF_CLUSTER);

	if (p_chain->size != count) {
		exfat_fs_error(sb,
			"bogus directory size (clus : ondisk(%d) != counted(%d))",
			p_chain->size, count);
		return -EIO;
	}

	*ret_clu = clu;
	return 0;
}

int exfat_zeroed_cluster(struct inode *dir, unsigned int clu)
{
	struct super_block *sb = dir->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct buffer_head *bh;
	sector_t blknr, last_blknr;
	int i;

	blknr = exfat_cluster_to_sector(sbi, clu);
	last_blknr = blknr + sbi->sect_per_clus;

	if (last_blknr > sbi->num_sectors && sbi->num_sectors > 0) {
		exfat_fs_error_ratelimit(sb,
			"%s: out of range(sect:%llu len:%u)",
			__func__, (unsigned long long)blknr,
			sbi->sect_per_clus);
		return -EIO;
	}

	/* Zeroing the unused blocks on this cluster */
	for (i = blknr; i < last_blknr; i++) {
		bh = sb_getblk(sb, i);
		if (!bh)
			return -ENOMEM;

		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		brelse(bh);
	}

	if (IS_DIRSYNC(dir))
		return sync_blockdev(sb->s_bdev);
	return 0;
}

int exfat_alloc_cluster(struct inode *inode, unsigned int num_alloc,
		struct exfat_chain *p_chain)
{
	int ret = -ENOSPC;
	unsigned int num_clusters = 0, total_cnt;
	unsigned int hint_clu, new_clu, last_clu = EXFAT_EOF_CLUSTER;
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);

	total_cnt = EXFAT_DATA_CLUSTER_COUNT(sbi);

	if (unlikely(total_cnt < sbi->used_clusters)) {
		exfat_fs_error_ratelimit(sb,
			"%s: invalid used clusters(t:%u,u:%u)\n",
			__func__, total_cnt, sbi->used_clusters);
		return -EIO;
	}

	if (num_alloc > total_cnt - sbi->used_clusters)
		return -ENOSPC;

	hint_clu = p_chain->dir;
	/* find new cluster */
	if (hint_clu == EXFAT_EOF_CLUSTER) {
		if (sbi->clu_srch_ptr < EXFAT_FIRST_CLUSTER) {
			exfat_err(sb, "sbi->clu_srch_ptr is invalid (%u)\n",
				  sbi->clu_srch_ptr);
			sbi->clu_srch_ptr = EXFAT_FIRST_CLUSTER;
		}

		hint_clu = exfat_find_free_bitmap(sb, sbi->clu_srch_ptr);
		if (hint_clu == EXFAT_EOF_CLUSTER)
			return -ENOSPC;
	}

	/* check cluster validation */
	if (!is_valid_cluster(sbi, hint_clu)) {
		exfat_err(sb, "hint_cluster is invalid (%u)",
			hint_clu);
		hint_clu = EXFAT_FIRST_CLUSTER;
		if (p_chain->flags == ALLOC_NO_FAT_CHAIN) {
			if (exfat_chain_cont_cluster(sb, p_chain->dir,
					num_clusters))
				return -EIO;
			p_chain->flags = ALLOC_FAT_CHAIN;
		}
	}

	p_chain->dir = EXFAT_EOF_CLUSTER;

	while ((new_clu = exfat_find_free_bitmap(sb, hint_clu)) !=
	       EXFAT_EOF_CLUSTER) {
		if (new_clu != hint_clu &&
		    p_chain->flags == ALLOC_NO_FAT_CHAIN) {
			if (exfat_chain_cont_cluster(sb, p_chain->dir,
					num_clusters)) {
				ret = -EIO;
				goto free_cluster;
			}
			p_chain->flags = ALLOC_FAT_CHAIN;
		}

		/* update allocation bitmap */
		if (exfat_set_bitmap(inode, new_clu)) {
			ret = -EIO;
			goto free_cluster;
		}

		num_clusters++;

		/* update FAT table */
		if (p_chain->flags == ALLOC_FAT_CHAIN) {
			if (exfat_ent_set(sb, new_clu, EXFAT_EOF_CLUSTER)) {
				ret = -EIO;
				goto free_cluster;
			}
		}

		if (p_chain->dir == EXFAT_EOF_CLUSTER) {
			p_chain->dir = new_clu;
		} else if (p_chain->flags == ALLOC_FAT_CHAIN) {
			if (exfat_ent_set(sb, last_clu, new_clu)) {
				ret = -EIO;
				goto free_cluster;
			}
		}
		last_clu = new_clu;

		if (--num_alloc == 0) {
			sbi->clu_srch_ptr = hint_clu;
			sbi->used_clusters += num_clusters;

			p_chain->size += num_clusters;
			return 0;
		}

		hint_clu = new_clu + 1;
		if (hint_clu >= sbi->num_clusters) {
			hint_clu = EXFAT_FIRST_CLUSTER;

			if (p_chain->flags == ALLOC_NO_FAT_CHAIN) {
				if (exfat_chain_cont_cluster(sb, p_chain->dir,
						num_clusters)) {
					ret = -EIO;
					goto free_cluster;
				}
				p_chain->flags = ALLOC_FAT_CHAIN;
			}
		}
	}
free_cluster:
	if (num_clusters)
		exfat_free_cluster(inode, p_chain);
	return ret;
}

int exfat_count_num_clusters(struct super_block *sb,
		struct exfat_chain *p_chain, unsigned int *ret_count)
{
	unsigned int i, count;
	unsigned int clu;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);

	if (!p_chain->dir || p_chain->dir == EXFAT_EOF_CLUSTER) {
		*ret_count = 0;
		return 0;
	}

	if (p_chain->flags == ALLOC_NO_FAT_CHAIN) {
		*ret_count = p_chain->size;
		return 0;
	}

	clu = p_chain->dir;
	count = 0;
	for (i = EXFAT_FIRST_CLUSTER; i < sbi->num_clusters; i++) {
		count++;
		if (exfat_ent_get(sb, clu, &clu))
			return -EIO;
		if (clu == EXFAT_EOF_CLUSTER)
			break;
	}

	*ret_count = count;
	return 0;
}
