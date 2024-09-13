// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f3fs/recovery.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <asm/unaligned.h>
#include <linux/fs.h>
#include "f3fs_fs.h"
#include <linux/sched/mm.h>
#include "f3fs.h"
#include "node.h"
#include "segment.h"

/*
 * Roll forward recovery scenarios.
 *
 * [Term] F: fsync_mark, D: dentry_mark
 *
 * 1. inode(x) | CP | inode(x) | dnode(F)
 * -> Update the latest inode(x).
 *
 * 2. inode(x) | CP | inode(F) | dnode(F)
 * -> No problem.
 *
 * 3. inode(x) | CP | dnode(F) | inode(x)
 * -> Recover to the latest dnode(F), and drop the last inode(x)
 *
 * 4. inode(x) | CP | dnode(F) | inode(F)
 * -> No problem.
 *
 * 5. CP | inode(x) | dnode(F)
 * -> The inode(DF) was missing. Should drop this dnode(F).
 *
 * 6. CP | inode(DF) | dnode(F)
 * -> No problem.
 *
 * 7. CP | dnode(F) | inode(DF)
 * -> If f3fs_iget fails, then goto next to find inode(DF).
 *
 * 8. CP | dnode(F) | inode(x)
 * -> If f3fs_iget fails, then goto next to find inode(DF).
 *    But it will fail due to no inode(DF).
 */

static struct kmem_cache *fsync_entry_slab;

#if IS_ENABLED(CONFIG_UNICODE)
extern struct kmem_cache *f3fs_cf_name_slab;
#endif

bool f3fs_space_for_roll_forward(struct f3fs_sb_info *sbi)
{
	s64 nalloc = percpu_counter_sum_positive(&sbi->alloc_valid_block_count);

	if (sbi->last_valid_block_count + nalloc > sbi->user_block_count)
		return false;
	if (NM_I(sbi)->max_rf_node_blocks &&
		percpu_counter_sum_positive(&sbi->rf_node_block_count) >=
						NM_I(sbi)->max_rf_node_blocks)
		return false;
	return true;
}

static struct fsync_inode_entry *get_fsync_inode(struct list_head *head,
								nid_t ino)
{
	struct fsync_inode_entry *entry;

	list_for_each_entry(entry, head, list)
		if (entry->inode->i_ino == ino)
			return entry;

	return NULL;
}

static struct fsync_inode_entry *add_fsync_inode(struct f3fs_sb_info *sbi,
			struct list_head *head, nid_t ino, bool quota_inode)
{
	struct inode *inode;
	struct fsync_inode_entry *entry;
	int err;

	inode = f3fs_iget_retry(sbi->sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	err = f3fs_dquot_initialize(inode);
	if (err)
		goto err_out;

	if (quota_inode) {
		err = dquot_alloc_inode(inode);
		if (err)
			goto err_out;
	}

	entry = f3fs_kmem_cache_alloc(fsync_entry_slab,
					GFP_F3FS_ZERO, true, NULL);
	entry->inode = inode;
	list_add_tail(&entry->list, head);

	return entry;
err_out:
	iput(inode);
	return ERR_PTR(err);
}

static void del_fsync_inode(struct fsync_inode_entry *entry, int drop)
{
	if (drop) {
		/* inode should not be recovered, drop it */
		f3fs_inode_synced(entry->inode);
	}
	iput(entry->inode);
	list_del(&entry->list);
	kmem_cache_free(fsync_entry_slab, entry);
}

static int init_recovered_filename(const struct inode *dir,
				   struct f3fs_inode *raw_inode,
				   struct f3fs_filename *fname,
				   struct qstr *usr_fname)
{
	int err;

	memset(fname, 0, sizeof(*fname));
	fname->disk_name.len = le32_to_cpu(raw_inode->i_namelen);
	fname->disk_name.name = raw_inode->i_name;

	if (WARN_ON(fname->disk_name.len > F3FS_NAME_LEN))
		return -ENAMETOOLONG;

	if (!IS_ENCRYPTED(dir)) {
		usr_fname->name = fname->disk_name.name;
		usr_fname->len = fname->disk_name.len;
		fname->usr_fname = usr_fname;
	}

	/* Compute the hash of the filename */
	if (IS_ENCRYPTED(dir) && IS_CASEFOLDED(dir)) {
		/*
		 * In this case the hash isn't computable without the key, so it
		 * was saved on-disk.
		 */
		if (fname->disk_name.len + sizeof(f3fs_hash_t) > F3FS_NAME_LEN)
			return -EINVAL;
		fname->hash = get_unaligned((f3fs_hash_t *)
				&raw_inode->i_name[fname->disk_name.len]);
	} else if (IS_CASEFOLDED(dir)) {
		err = f3fs_init_casefolded_name(dir, fname);
		if (err)
			return err;
		f3fs_hash_filename(dir, fname);
#if IS_ENABLED(CONFIG_UNICODE)
		/* Case-sensitive match is fine for recovery */
		kmem_cache_free(f3fs_cf_name_slab, fname->cf_name.name);
		fname->cf_name.name = NULL;
#endif
	} else {
		f3fs_hash_filename(dir, fname);
	}
	return 0;
}

static int recover_dentry(struct inode *inode, struct page *ipage,
						struct list_head *dir_list)
{
	struct f3fs_inode *raw_inode = F3FS_INODE(ipage);
	nid_t pino = le32_to_cpu(raw_inode->i_pino);
	struct f3fs_dir_entry *de;
	struct f3fs_filename fname;
	struct qstr usr_fname;
	struct page *page;
	struct inode *dir, *einode;
	struct fsync_inode_entry *entry;
	int err = 0;
	char *name;

	entry = get_fsync_inode(dir_list, pino);
	if (!entry) {
		entry = add_fsync_inode(F3FS_I_SB(inode), dir_list,
							pino, false);
		if (IS_ERR(entry)) {
			dir = ERR_CAST(entry);
			err = PTR_ERR(entry);
			goto out;
		}
	}

	dir = entry->inode;
	err = init_recovered_filename(dir, raw_inode, &fname, &usr_fname);
	if (err)
		goto out;
retry:
	de = __f3fs_find_entry(dir, &fname, &page);
	if (de && inode->i_ino == le32_to_cpu(de->ino))
		goto out_put;

	if (de) {
		einode = f3fs_iget_retry(inode->i_sb, le32_to_cpu(de->ino));
		if (IS_ERR(einode)) {
			WARN_ON(1);
			err = PTR_ERR(einode);
			if (err == -ENOENT)
				err = -EEXIST;
			goto out_put;
		}

		err = f3fs_dquot_initialize(einode);
		if (err) {
			iput(einode);
			goto out_put;
		}

		err = f3fs_acquire_orphan_inode(F3FS_I_SB(inode));
		if (err) {
			iput(einode);
			goto out_put;
		}
		f3fs_delete_entry(de, page, dir, einode);
		iput(einode);
		goto retry;
	} else if (IS_ERR(page)) {
		err = PTR_ERR(page);
	} else {
		err = f3fs_add_dentry(dir, &fname, inode,
					inode->i_ino, inode->i_mode);
	}
	if (err == -ENOMEM)
		goto retry;
	goto out;

out_put:
	f3fs_put_page(page, 0);
out:
	if (file_enc_name(inode))
		name = "<encrypted>";
	else
		name = raw_inode->i_name;
	f3fs_notice(F3FS_I_SB(inode), "%s: ino = %x, name = %s, dir = %lx, err = %d",
		    __func__, ino_of_node(ipage), name,
		    IS_ERR(dir) ? 0 : dir->i_ino, err);
	return err;
}

static int recover_quota_data(struct inode *inode, struct page *page)
{
	struct f3fs_inode *raw = F3FS_INODE(page);
	struct iattr attr;
	uid_t i_uid = le32_to_cpu(raw->i_uid);
	gid_t i_gid = le32_to_cpu(raw->i_gid);
	int err;

	memset(&attr, 0, sizeof(attr));

	attr.ia_vfsuid = VFSUIDT_INIT(make_kuid(inode->i_sb->s_user_ns, i_uid));
	attr.ia_vfsgid = VFSGIDT_INIT(make_kgid(inode->i_sb->s_user_ns, i_gid));

	if (!vfsuid_eq(attr.ia_vfsuid, i_uid_into_vfsuid(&init_user_ns, inode)))
		attr.ia_valid |= ATTR_UID;
	if (!vfsgid_eq(attr.ia_vfsgid, i_gid_into_vfsgid(&init_user_ns, inode)))
		attr.ia_valid |= ATTR_GID;

	if (!attr.ia_valid)
		return 0;

	err = dquot_transfer(&init_user_ns, inode, &attr);
	if (err)
		set_sbi_flag(F3FS_I_SB(inode), SBI_QUOTA_NEED_REPAIR);
	return err;
}

static void recover_inline_flags(struct inode *inode, struct f3fs_inode *ri)
{
	if (ri->i_inline & F3FS_PIN_FILE)
		set_inode_flag(inode, FI_PIN_FILE);
	else
		clear_inode_flag(inode, FI_PIN_FILE);
	if (ri->i_inline & F3FS_DATA_EXIST)
		set_inode_flag(inode, FI_DATA_EXIST);
	else
		clear_inode_flag(inode, FI_DATA_EXIST);
}

static int recover_inode(struct inode *inode, struct page *page)
{
	struct f3fs_inode *raw = F3FS_INODE(page);
	char *name;
	int err;

	inode->i_mode = le16_to_cpu(raw->i_mode);

	err = recover_quota_data(inode, page);
	if (err)
		return err;

	i_uid_write(inode, le32_to_cpu(raw->i_uid));
	i_gid_write(inode, le32_to_cpu(raw->i_gid));

	if (raw->i_inline & F3FS_EXTRA_ATTR) {
		if (f3fs_sb_has_project_quota(F3FS_I_SB(inode)) &&
			F3FS_FITS_IN_INODE(raw, le16_to_cpu(raw->i_extra_isize),
								i_projid)) {
			projid_t i_projid;
			kprojid_t kprojid;

			i_projid = (projid_t)le32_to_cpu(raw->i_projid);
			kprojid = make_kprojid(&init_user_ns, i_projid);

			if (!projid_eq(kprojid, F3FS_I(inode)->i_projid)) {
				err = f3fs_transfer_project_quota(inode,
								kprojid);
				if (err)
					return err;
				F3FS_I(inode)->i_projid = kprojid;
			}
		}
	}

	f3fs_i_size_write(inode, le64_to_cpu(raw->i_size));
	inode->i_atime.tv_sec = le64_to_cpu(raw->i_atime);
	inode->i_ctime.tv_sec = le64_to_cpu(raw->i_ctime);
	inode->i_mtime.tv_sec = le64_to_cpu(raw->i_mtime);
	inode->i_atime.tv_nsec = le32_to_cpu(raw->i_atime_nsec);
	inode->i_ctime.tv_nsec = le32_to_cpu(raw->i_ctime_nsec);
	inode->i_mtime.tv_nsec = le32_to_cpu(raw->i_mtime_nsec);

	F3FS_I(inode)->i_advise = raw->i_advise;
	F3FS_I(inode)->i_flags = le32_to_cpu(raw->i_flags);
	f3fs_set_inode_flags(inode);
	F3FS_I(inode)->i_gc_failures[GC_FAILURE_PIN] =
				le16_to_cpu(raw->i_gc_failures);

	recover_inline_flags(inode, raw);

	f3fs_mark_inode_dirty_sync(inode, true);

	if (file_enc_name(inode))
		name = "<encrypted>";
	else
		name = F3FS_INODE(page)->i_name;

	f3fs_notice(F3FS_I_SB(inode), "recover_inode: ino = %x, name = %s, inline = %x",
		    ino_of_node(page), name, raw->i_inline);
	return 0;
}

static unsigned int adjust_por_ra_blocks(struct f3fs_sb_info *sbi,
				unsigned int ra_blocks, unsigned int blkaddr,
				unsigned int next_blkaddr)
{
	if (blkaddr + 1 == next_blkaddr)
		ra_blocks = min_t(unsigned int, RECOVERY_MAX_RA_BLOCKS,
							ra_blocks * 2);
	else if (next_blkaddr % sbi->blocks_per_seg)
		ra_blocks = max_t(unsigned int, RECOVERY_MIN_RA_BLOCKS,
							ra_blocks / 2);
	return ra_blocks;
}

static int find_fsync_dnodes(struct f3fs_sb_info *sbi, struct list_head *head,
				bool check_only)
{
	struct curseg_info *curseg;
	struct page *page = NULL;
	block_t blkaddr;
	unsigned int loop_cnt = 0;
	unsigned int ra_blocks = RECOVERY_MAX_RA_BLOCKS;
	unsigned int free_blocks = MAIN_SEGS(sbi) * sbi->blocks_per_seg -
						valid_user_blocks(sbi);
	int err = 0;

	/* get node pages in the current segment */
	curseg = CURSEG_I(sbi, CURSEG_WARM_NODE);
	blkaddr = NEXT_FREE_BLKADDR(sbi, curseg);

	while (1) {
		struct fsync_inode_entry *entry;

		if (!f3fs_is_valid_blkaddr(sbi, blkaddr, META_POR))
			return 0;

		page = f3fs_get_tmp_page(sbi, blkaddr);
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			break;
		}

		if (!is_recoverable_dnode(page)) {
			f3fs_put_page(page, 1);
			break;
		}

		if (!is_fsync_dnode(page))
			goto next;

		entry = get_fsync_inode(head, ino_of_node(page));
		if (!entry) {
			bool quota_inode = false;

			if (!check_only &&
					IS_INODE(page) && is_dent_dnode(page)) {
				err = f3fs_recover_inode_page(sbi, page);
				if (err) {
					f3fs_put_page(page, 1);
					break;
				}
				quota_inode = true;
			}

			/*
			 * CP | dnode(F) | inode(DF)
			 * For this case, we should not give up now.
			 */
			entry = add_fsync_inode(sbi, head, ino_of_node(page),
								quota_inode);
			if (IS_ERR(entry)) {
				err = PTR_ERR(entry);
				if (err == -ENOENT) {
					err = 0;
					goto next;
				}
				f3fs_put_page(page, 1);
				break;
			}
		}
		entry->blkaddr = blkaddr;

		if (IS_INODE(page) && is_dent_dnode(page))
			entry->last_dentry = blkaddr;
next:
		/* sanity check in order to detect looped node chain */
		if (++loop_cnt >= free_blocks ||
			blkaddr == next_blkaddr_of_node(page)) {
			f3fs_notice(sbi, "%s: detect looped node chain, blkaddr:%u, next:%u",
				    __func__, blkaddr,
				    next_blkaddr_of_node(page));
			f3fs_put_page(page, 1);
			err = -EINVAL;
			break;
		}

		ra_blocks = adjust_por_ra_blocks(sbi, ra_blocks, blkaddr,
						next_blkaddr_of_node(page));

		/* check next segment */
		blkaddr = next_blkaddr_of_node(page);
		f3fs_put_page(page, 1);

		f3fs_ra_meta_pages_cond(sbi, blkaddr, ra_blocks);
	}
	return err;
}

static void destroy_fsync_dnodes(struct list_head *head, int drop)
{
	struct fsync_inode_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, head, list)
		del_fsync_inode(entry, drop);
}

static int check_index_in_prev_nodes(struct f3fs_sb_info *sbi,
			block_t blkaddr, struct dnode_of_data *dn)
{
	struct seg_entry *sentry;
	unsigned int segno = GET_SEGNO(sbi, blkaddr);
	unsigned short blkoff = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);
	struct f3fs_summary_block *sum_node;
	struct f3fs_summary sum;
	struct page *sum_page, *node_page;
	struct dnode_of_data tdn = *dn;
	nid_t ino, nid;
	struct inode *inode;
	unsigned int offset;
	block_t bidx;
	int i;

	sentry = get_seg_entry(sbi, segno);
	if (!f3fs_test_bit(blkoff, sentry->cur_valid_map))
		return 0;

	/* Get the previous summary */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		struct curseg_info *curseg = CURSEG_I(sbi, i);

		if (curseg->segno == segno) {
			sum = curseg->sum_blk->entries[blkoff];
			goto got_it;
		}
	}

	sum_page = f3fs_get_sum_page(sbi, segno);
	if (IS_ERR(sum_page))
		return PTR_ERR(sum_page);
	sum_node = (struct f3fs_summary_block *)page_address(sum_page);
	sum = sum_node->entries[blkoff];
	f3fs_put_page(sum_page, 1);
got_it:
	/* Use the locked dnode page and inode */
	nid = le32_to_cpu(sum.nid);
	if (dn->inode->i_ino == nid) {
		tdn.nid = nid;
		if (!dn->inode_page_locked)
			lock_page(dn->inode_page);
		tdn.node_page = dn->inode_page;
		tdn.ofs_in_node = le16_to_cpu(sum.ofs_in_node);
		goto truncate_out;
	} else if (dn->nid == nid) {
		tdn.ofs_in_node = le16_to_cpu(sum.ofs_in_node);
		goto truncate_out;
	}

	/* Get the node page */
	node_page = f3fs_get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return PTR_ERR(node_page);

	offset = ofs_of_node(node_page);
	ino = ino_of_node(node_page);
	f3fs_put_page(node_page, 1);

	if (ino != dn->inode->i_ino) {
		int ret;

		/* Deallocate previous index in the node page */
		inode = f3fs_iget_retry(sbi->sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);

		ret = f3fs_dquot_initialize(inode);
		if (ret) {
			iput(inode);
			return ret;
		}
	} else {
		inode = dn->inode;
	}

	bidx = f3fs_start_bidx_of_node(offset, inode) +
				le16_to_cpu(sum.ofs_in_node);

	/*
	 * if inode page is locked, unlock temporarily, but its reference
	 * count keeps alive.
	 */
	if (ino == dn->inode->i_ino && dn->inode_page_locked)
		unlock_page(dn->inode_page);

	set_new_dnode(&tdn, inode, NULL, NULL, 0);
	if (f3fs_get_dnode_of_data(&tdn, bidx, LOOKUP_NODE))
		goto out;

	if (tdn.data_blkaddr == blkaddr)
		f3fs_truncate_data_blocks_range(&tdn, 1);

	f3fs_put_dnode(&tdn);
out:
	if (ino != dn->inode->i_ino)
		iput(inode);
	else if (dn->inode_page_locked)
		lock_page(dn->inode_page);
	return 0;

truncate_out:
	if (f3fs_data_blkaddr(&tdn) == blkaddr)
		f3fs_truncate_data_blocks_range(&tdn, 1);
	if (dn->inode->i_ino == nid && !dn->inode_page_locked)
		unlock_page(dn->inode_page);
	return 0;
}

static int do_recover_data(struct f3fs_sb_info *sbi, struct inode *inode,
					struct page *page)
{
	struct dnode_of_data dn;
	struct node_info ni;
	unsigned int start, end;
	int err = 0, recovered = 0;

	/* step 1: recover xattr */
	if (IS_INODE(page)) {
		err = f3fs_recover_inline_xattr(inode, page);
		if (err)
			goto out;
	} else if (f3fs_has_xattr_block(ofs_of_node(page))) {
		err = f3fs_recover_xattr_data(inode, page);
		if (!err)
			recovered++;
		goto out;
	}

	/* step 2: recover inline data */
	err = f3fs_recover_inline_data(inode, page);
	if (err) {
		if (err == 1)
			err = 0;
		goto out;
	}

	/* step 3: recover data indices */
	start = f3fs_start_bidx_of_node(ofs_of_node(page), inode);
	end = start + ADDRS_PER_PAGE(page, inode);

	set_new_dnode(&dn, inode, NULL, NULL, 0);
retry_dn:
	err = f3fs_get_dnode_of_data(&dn, start, ALLOC_NODE);
	if (err) {
		if (err == -ENOMEM) {
			memalloc_retry_wait(GFP_NOFS);
			goto retry_dn;
		}
		goto out;
	}

	f3fs_wait_on_page_writeback(dn.node_page, NODE, true, true);

	err = f3fs_get_node_info(sbi, dn.nid, &ni, false);
	if (err)
		goto err;

	f3fs_bug_on(sbi, ni.ino != ino_of_node(page));

	if (ofs_of_node(dn.node_page) != ofs_of_node(page)) {
		f3fs_warn(sbi, "Inconsistent ofs_of_node, ino:%lu, ofs:%u, %u",
			  inode->i_ino, ofs_of_node(dn.node_page),
			  ofs_of_node(page));
		err = -EFSCORRUPTED;
		goto err;
	}

	for (; start < end; start++, dn.ofs_in_node++) {
		block_t src, dest;

		src = f3fs_data_blkaddr(&dn);
		dest = data_blkaddr(dn.inode, page, dn.ofs_in_node);

		if (__is_valid_data_blkaddr(src) &&
			!f3fs_is_valid_blkaddr(sbi, src, META_POR)) {
			err = -EFSCORRUPTED;
			goto err;
		}

		if (__is_valid_data_blkaddr(dest) &&
			!f3fs_is_valid_blkaddr(sbi, dest, META_POR)) {
			err = -EFSCORRUPTED;
			goto err;
		}

		/* skip recovering if dest is the same as src */
		if (src == dest)
			continue;

		/* dest is invalid, just invalidate src block */
		if (dest == NULL_ADDR) {
			f3fs_truncate_data_blocks_range(&dn, 1);
			continue;
		}

		if (!file_keep_isize(inode) &&
			(i_size_read(inode) <= ((loff_t)start << PAGE_SHIFT)))
			f3fs_i_size_write(inode,
				(loff_t)(start + 1) << PAGE_SHIFT);

		/*
		 * dest is reserved block, invalidate src block
		 * and then reserve one new block in dnode page.
		 */
		if (dest == NEW_ADDR) {
			f3fs_truncate_data_blocks_range(&dn, 1);
			f3fs_reserve_new_block(&dn);
			continue;
		}

		/* dest is valid block, try to recover from src to dest */
		if (f3fs_is_valid_blkaddr(sbi, dest, META_POR)) {

			if (src == NULL_ADDR) {
				err = f3fs_reserve_new_block(&dn);
				while (err &&
				       IS_ENABLED(CONFIG_F3FS_FAULT_INJECTION))
					err = f3fs_reserve_new_block(&dn);
				/* We should not get -ENOSPC */
				f3fs_bug_on(sbi, err);
				if (err)
					goto err;
			}
retry_prev:
			/* Check the previous node page having this index */
			err = check_index_in_prev_nodes(sbi, dest, &dn);
			if (err) {
				if (err == -ENOMEM) {
					memalloc_retry_wait(GFP_NOFS);
					goto retry_prev;
				}
				goto err;
			}

			/* write dummy data page */
			f3fs_replace_block(sbi, &dn, src, dest,
						ni.version, false, false);
			recovered++;
		}
	}

	copy_node_footer(dn.node_page, page);
	fill_node_footer(dn.node_page, dn.nid, ni.ino,
					ofs_of_node(page), false);
	set_page_dirty(dn.node_page);
err:
	f3fs_put_dnode(&dn);
out:
	f3fs_notice(sbi, "recover_data: ino = %lx (i_size: %s) recovered = %d, err = %d",
		    inode->i_ino, file_keep_isize(inode) ? "keep" : "recover",
		    recovered, err);
	return err;
}

static int recover_data(struct f3fs_sb_info *sbi, struct list_head *inode_list,
		struct list_head *tmp_inode_list, struct list_head *dir_list)
{
	struct curseg_info *curseg;
	struct page *page = NULL;
	int err = 0;
	block_t blkaddr;
	unsigned int ra_blocks = RECOVERY_MAX_RA_BLOCKS;

	/* get node pages in the current segment */
	curseg = CURSEG_I(sbi, CURSEG_WARM_NODE);
	blkaddr = NEXT_FREE_BLKADDR(sbi, curseg);

	while (1) {
		struct fsync_inode_entry *entry;

		if (!f3fs_is_valid_blkaddr(sbi, blkaddr, META_POR))
			break;

		page = f3fs_get_tmp_page(sbi, blkaddr);
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			break;
		}

		if (!is_recoverable_dnode(page)) {
			f3fs_put_page(page, 1);
			break;
		}

		entry = get_fsync_inode(inode_list, ino_of_node(page));
		if (!entry)
			goto next;
		/*
		 * inode(x) | CP | inode(x) | dnode(F)
		 * In this case, we can lose the latest inode(x).
		 * So, call recover_inode for the inode update.
		 */
		if (IS_INODE(page)) {
			err = recover_inode(entry->inode, page);
			if (err) {
				f3fs_put_page(page, 1);
				break;
			}
		}
		if (entry->last_dentry == blkaddr) {
			err = recover_dentry(entry->inode, page, dir_list);
			if (err) {
				f3fs_put_page(page, 1);
				break;
			}
		}
		err = do_recover_data(sbi, entry->inode, page);
		if (err) {
			f3fs_put_page(page, 1);
			break;
		}

		if (entry->blkaddr == blkaddr)
			list_move_tail(&entry->list, tmp_inode_list);
next:
		ra_blocks = adjust_por_ra_blocks(sbi, ra_blocks, blkaddr,
						next_blkaddr_of_node(page));

		/* check next segment */
		blkaddr = next_blkaddr_of_node(page);
		f3fs_put_page(page, 1);

		f3fs_ra_meta_pages_cond(sbi, blkaddr, ra_blocks);
	}
	if (!err)
		f3fs_allocate_new_segments(sbi);
	return err;
}

int f3fs_recover_fsync_data(struct f3fs_sb_info *sbi, bool check_only)
{
	struct list_head inode_list, tmp_inode_list;
	struct list_head dir_list;
	int err;
	int ret = 0;
	unsigned long s_flags = sbi->sb->s_flags;
	bool need_writecp = false;
	bool fix_curseg_write_pointer = false;
#ifdef CONFIG_QUOTA
	int quota_enabled;
#endif

	if (s_flags & SB_RDONLY) {
		f3fs_info(sbi, "recover fsync data on readonly fs");
		sbi->sb->s_flags &= ~SB_RDONLY;
	}

#ifdef CONFIG_QUOTA
	/* Turn on quotas so that they are updated correctly */
	quota_enabled = f3fs_enable_quota_files(sbi, s_flags & SB_RDONLY);
#endif

	INIT_LIST_HEAD(&inode_list);
	INIT_LIST_HEAD(&tmp_inode_list);
	INIT_LIST_HEAD(&dir_list);

	/* prevent checkpoint */
	f3fs_down_write(&sbi->cp_global_sem);

	/* step #1: find fsynced inode numbers */
	err = find_fsync_dnodes(sbi, &inode_list, check_only);
	if (err || list_empty(&inode_list))
		goto skip;

	if (check_only) {
		ret = 1;
		goto skip;
	}

	need_writecp = true;

	/* step #2: recover data */
	err = recover_data(sbi, &inode_list, &tmp_inode_list, &dir_list);
	if (!err)
		f3fs_bug_on(sbi, !list_empty(&inode_list));
	else
		f3fs_bug_on(sbi, sbi->sb->s_flags & SB_ACTIVE);
skip:
	fix_curseg_write_pointer = !check_only || list_empty(&inode_list);

	destroy_fsync_dnodes(&inode_list, err);
	destroy_fsync_dnodes(&tmp_inode_list, err);

	/* truncate meta pages to be used by the recovery */
	truncate_inode_pages_range(META_MAPPING(sbi),
			(loff_t)MAIN_BLKADDR(sbi) << PAGE_SHIFT, -1);

	if (err) {
		truncate_inode_pages_final(NODE_MAPPING(sbi));
		truncate_inode_pages_final(META_MAPPING(sbi));
	}

	/*
	 * If fsync data succeeds or there is no fsync data to recover,
	 * and the f3fs is not read only, check and fix zoned block devices'
	 * write pointer consistency.
	 */
	if (!err && fix_curseg_write_pointer && !f3fs_readonly(sbi->sb) &&
			f3fs_sb_has_blkzoned(sbi)) {
		err = f3fs_fix_curseg_write_pointer(sbi);
		ret = err;
	}

	if (!err)
		clear_sbi_flag(sbi, SBI_POR_DOING);

	f3fs_up_write(&sbi->cp_global_sem);

	/* let's drop all the directory inodes for clean checkpoint */
	destroy_fsync_dnodes(&dir_list, err);

	if (need_writecp) {
		set_sbi_flag(sbi, SBI_IS_RECOVERED);

		if (!err) {
			struct cp_control cpc = {
				.reason = CP_RECOVERY,
			};
			err = f3fs_write_checkpoint(sbi, &cpc);
		}
	}

#ifdef CONFIG_QUOTA
	/* Turn quotas off */
	if (quota_enabled)
		f3fs_quota_off_umount(sbi->sb);
#endif
	sbi->sb->s_flags = s_flags; /* Restore SB_RDONLY status */

	return ret ? ret : err;
}

int __init f3fs_create_recovery_cache(void)
{
	fsync_entry_slab = f3fs_kmem_cache_create("f3fs_fsync_inode_entry",
					sizeof(struct fsync_inode_entry));
	if (!fsync_entry_slab)
		return -ENOMEM;
	return 0;
}

void f3fs_destroy_recovery_cache(void)
{
	kmem_cache_destroy(fsync_entry_slab);
}
