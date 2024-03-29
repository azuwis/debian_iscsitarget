/*
 * Volume manager
 * (C) 2004 - 2005 FUJITA Tomonori <tomof@acm.org>
 * This code is licenced under the GPL.
 */

#include <linux/types.h>
#include <linux/parser.h>
#include <linux/blkdev.h>
#include <scsi/scsi.h>

#include "iscsi.h"
#include "iscsi_dbg.h"
#include "iotype.h"

struct iet_volume *volume_lookup(struct iscsi_target *target, u32 lun)
{
	struct iet_volume *volume;

	list_for_each_entry(volume, &target->volumes, list) {
		if (volume->lun == lun)
			return volume;
	}
	return NULL;
}

enum {
	opt_type,
	opt_iomode,
	opt_scsiid,
	opt_scsisn,
	opt_blk_size,
	opt_err,
};

static match_table_t tokens = {
	{opt_type, "type=%s"},
	{opt_iomode, "iomode=%s"},
	{opt_scsiid, "scsiid=%s"},
	{opt_scsisn, "scsisn=%s"},
	{opt_blk_size, "blocksize=%u"},
	{opt_err, NULL},
};

static int set_scsiid(struct iet_volume *volume, const char *id)
{
	size_t len;

	if ((len = strlen(id)) > SCSI_ID_LEN) {
		eprintk("SCSI ID too long, %zd provided, %u max\n", len,
								SCSI_ID_LEN);
		return -EINVAL;
	}

	memcpy(volume->scsi_id, id, len);

	return 0;
}

static int set_scsisn(struct iet_volume *volume, const char *sn)
{
	size_t len;
	int i;

	if ((len = strlen(sn)) > SCSI_SN_LEN) {
		eprintk("SCSI SN too long, %zd provided, %u max\n", len,
								SCSI_SN_LEN);
		return -EINVAL;
	}

	for (i = 0; i < len; i++) {
		if (!isascii(*(sn + i)) || !isprint(*(sn + i))) {
			eprintk("invalid characters in SCSI SN, %s\n",
				"only printable ascii characters allowed!");
			return -EINVAL;
		}
	}

	memcpy(volume->scsi_sn, sn, len);

	return 0;
}

/* Generate a MD5 hash of the target IQN and LUN number */
static void gen_scsiid(struct iet_volume *volume)
{
	struct hash_desc hash;

	hash.tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	hash.flags = 0;

	if (!IS_ERR(hash.tfm)) {
		struct scatterlist sg[2];
		unsigned int nbytes = 0;

		sg_init_table(sg, 2);

		sg_set_buf(&sg[0], volume->target->name,
					strlen(volume->target->name));
		nbytes += strlen(volume->target->name);

		sg_set_buf(&sg[1], &volume->lun, sizeof(volume->lun));
		nbytes += sizeof(volume->lun);

		crypto_hash_init(&hash);
		crypto_hash_update(&hash, sg, nbytes);
		crypto_hash_final(&hash, volume->scsi_id);

		crypto_free_hash(hash.tfm);
	} else {
		/* If no MD5 available set ID to TID and LUN */
		memcpy(volume->scsi_id, &volume->target->tid,
						sizeof(volume->target->tid));
		memcpy(volume->scsi_id + sizeof(volume->target->tid),
					&volume->lun, sizeof(volume->lun));
	}
}

static int parse_volume_params(struct iet_volume *volume, char *params)
{
	int err = 0;
	unsigned blk_sz;
	substring_t args[MAX_OPT_ARGS];
	char *p, *argp = NULL, *bp, *buf = (char *) get_zeroed_page(GFP_USER);

	if (!buf)
		return -ENOMEM;

	bp = buf;

	strncpy(buf, params, PAGE_CACHE_SIZE);

	while ((p = strsep(&buf, ",")) != NULL) {
		int token;

		if (!*p)
			continue;
		iet_strtolower(p);
		token = match_token(p, tokens, args);
		switch (token) {
		case opt_type:
			argp = match_strdup(&args[0]);
			if (!argp) {
				err = -ENOMEM;
				break;
			}
			if (!(volume->iotype = get_iotype(argp)))
				err = -ENOENT;
			kfree(argp);
			break;
		case opt_iomode:
			argp = match_strdup(&args[0]);
			if (!argp) {
				err = -ENOMEM;
				break;
			}
			if (!strcmp(argp, "ro"))
				SetLUReadonly(volume);
			else if (!strcmp(argp, "wb"))
				SetLUWCache(volume);
			else if (strcmp(argp, "wt"))
				err = -EINVAL;
			kfree(argp);
			break;
		case opt_scsiid:
			argp = match_strdup(&args[0]);
			if (!argp) {
				err = -ENOMEM;
				break;
			}
			err = set_scsiid(volume, argp);
			kfree(argp);
			break;
		case opt_scsisn:
			argp = match_strdup(&args[0]);
			if (!argp) {
				err = -ENOMEM;
				break;
			}
			err = set_scsisn(volume, argp);
			kfree(argp);
			break;
		case opt_blk_size:
			argp = match_strdup(&args[0]);
			if (!argp) {
				err = -ENOMEM;
				break;
			}
			blk_sz = simple_strtoull(argp, NULL, 10);
			if (is_power_of_2(blk_sz) &&
			    512 <= blk_sz && blk_sz <= IET_MAX_BLOCK_SIZE)
				volume->blk_shift = blksize_bits(blk_sz);
			else {
				eprintk("invalid BlockSize=%u\n", blk_sz);
				err = -EINVAL;
			}
			kfree(argp);
			break;
		default:
			break;
		}
	}

	if (!err && !volume->iotype && !(volume->iotype = get_iotype("fileio"))) {
		eprintk("%s\n", "Cannot find fileio");
		err = -EINVAL;
	}

	free_page((unsigned long) bp);

	return err;
}

static void
volume_reservation_exit(struct iet_volume *volume)
{
	struct list_head *l, *n;
	struct registration *tmp;
	struct reservation *reservation = &volume->reservation;

	list_for_each_safe(l, n, &reservation->registration_list) {
		tmp = list_entry(l, struct registration, r_list);
		list_del(l);
		kfree(tmp);
	}
}

int volume_add(struct iscsi_target *target, struct volume_info *info)
{
	int ret;
	struct iet_volume *volume;
	char *args;

	volume = volume_lookup(target, info->lun);
	if (volume)
		return -EEXIST;

	if (info->lun > 0x3fff)
		return -EINVAL;

	volume = kzalloc(sizeof(*volume), GFP_KERNEL);
	if (!volume)
		return -ENOMEM;

	volume->target = target;
	volume->lun = info->lun;
	INIT_LIST_HEAD(&volume->reservation.registration_list);

	args = kzalloc(info->args_len + 1, GFP_KERNEL);
	if (!args) {
		ret = -ENOMEM;
		goto free_volume;
	}

	ret = copy_from_user(args, (void *)(unsigned long)info->args_ptr,
			     info->args_len);
	if (ret) {
		ret = -EFAULT;
		goto free_args;
	}

	ret = parse_volume_params(volume, args);
	if (ret < 0)
		goto free_args;

	ret = volume->iotype->attach(volume, args);
	if (ret < 0)
		goto free_args;

	if (!volume->scsi_id[0])
		gen_scsiid(volume);

	if (!volume->scsi_sn[0]) {
		int i;

		for (i = 0; i < SCSI_ID_LEN; i++)
			snprintf(volume->scsi_sn + (i * 2), 3, "%02x",
							volume->scsi_id[i]);
	}

	INIT_LIST_HEAD(&volume->queue.wait_list);
	spin_lock_init(&volume->queue.queue_lock);
	spin_lock_init(&volume->reserve_lock);

	volume->l_state = IDEV_RUNNING;
	atomic_set(&volume->l_count, 0);

	list_add_tail(&volume->list, &target->volumes);
	atomic_inc(&target->nr_volumes);

	kfree(args);

	return 0;
free_args:
	kfree(args);
free_volume:
	put_iotype(volume->iotype);
	kfree(volume);

	return ret;
}

void iscsi_volume_destroy(struct iet_volume *volume)
{
	assert(volume->l_state == IDEV_DEL);
	assert(!atomic_read(&volume->l_count));

	volume->iotype->detach(volume);
	put_iotype(volume->iotype);
	list_del(&volume->list);
	volume_reservation_exit(volume);
	kfree(volume);
}

int iscsi_volume_del(struct iscsi_target *target, struct volume_info *info)
{
	struct iet_volume *volume;

	eprintk("%x %x\n", target->tid, info->lun);
	if (!(volume = volume_lookup(target, info->lun)))
		return -ENOENT;

	volume->l_state = IDEV_DEL;
	atomic_dec(&target->nr_volumes);
	if (!atomic_read(&volume->l_count))
		iscsi_volume_destroy(volume);

	return 0;
}

struct iet_volume *volume_get(struct iscsi_target *target, u32 lun)
{
	struct iet_volume *volume;

	if ((volume = volume_lookup(target, lun))) {
		if (volume->l_state == IDEV_RUNNING)
			atomic_inc(&volume->l_count);
		else
			volume = NULL;
	}
	return volume;
}

void volume_put(struct iet_volume *volume)
{
	if (atomic_dec_and_test(&volume->l_count) && volume->l_state == IDEV_DEL)
		iscsi_volume_destroy(volume);
}

int volume_reserve(struct iet_volume *volume, u64 sid)
{
	int err = 0;
	struct reservation *reservation;

	if (!volume)
		return -ENOENT;

	reservation = &volume->reservation;
	spin_lock(&volume->reserve_lock);
	if (pr_is_reserved(reservation) && reservation->sid != sid)
		err = -EBUSY;
	else {
		reservation->reservation_type = RESERVATION_TYPE_RESERVE;
		reservation->sid = sid;
	}

	spin_unlock(&volume->reserve_lock);
	return err;
}

int is_volume_reserved(struct iet_volume *volume, u64 sid, u8 *scb)
{
	int err = 0;
	struct reservation *reservation;
	bool registered = false;
	bool write_excl = false;
	bool excl_access = false;
	bool write_excl_ro = false;
	bool excl_access_ro = false;

	if (!volume)
		return -ENOENT;

	reservation = &volume->reservation;

	spin_lock(&volume->reserve_lock);
	if (!pr_is_reserved(reservation) || reservation->sid == sid) {
		spin_unlock(&volume->reserve_lock);
		return 0;
	}

	if (reservation->reservation_type == RESERVATION_TYPE_RESERVE) {
		switch (scb[0]) {
		case INQUIRY:
		case RELEASE:
		case REPORT_LUNS:
		case REQUEST_SENSE:
		case READ_CAPACITY:
			/* allowed commands when reserved */
			break;
		case SERVICE_ACTION_IN:
			if ((scb[1] & 0x1F) == 0x10)
				break;
			/* fall through */
		default:
			err = -EBUSY;
			break;
		}
		spin_unlock(&volume->reserve_lock);
		return err;
	}

	registered = pr_initiator_has_registered(reservation, sid);

	switch (reservation->persistent_type) {
	case PR_TYPE_WRITE_EXCLUSIVE:
		write_excl = true;
		break;
	case PR_TYPE_EXCLUSIVE_ACCESS:
		excl_access = true;
		break;
	case PR_TYPE_WRITE_EXCLUSIVE_REGISTRANTS_ONLY:
	case PR_TYPE_WRITE_EXCLUSIVE_ALL_REGISTRANTS:
		write_excl_ro = true;
		break;
	case PR_TYPE_EXCLUSIVE_ACCESS_REGISTRANTS_ONLY:
	case PR_TYPE_EXCLUSIVE_ACCESS_ALL_REGISTRANTS:
		excl_access_ro = true;
		break;
	default:
		break;
	}

	switch (scb[0]) {
	case INQUIRY:
	case TEST_UNIT_READY:
	case PERSISTENT_RESERVE_IN:
	case PERSISTENT_RESERVE_OUT:
	case REPORT_LUNS:
	case REQUEST_SENSE:
	case READ_CAPACITY:
	case START_STOP:
		break;
	case MODE_SENSE:
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case WRITE_SAME_16:
	case WRITE_VERIFY:
	case SYNCHRONIZE_CACHE:
		if (write_excl || excl_access)
			err = -EBUSY;
		if ((write_excl_ro || excl_access_ro) && !registered)
			err = -EBUSY;
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
	case VERIFY:
	case VERIFY_16:
		if (excl_access)
			err = -EBUSY;
		if (excl_access_ro && !registered)
			err = -EBUSY;
		break;
	case SERVICE_ACTION_IN:
		if ((scb[1] & 0x1F) == 0x10)
			break;
		/* fall through */
	case RELEASE:
	case RESERVE:
	default:
		err = -EBUSY;
		break;
	}
	spin_unlock(&volume->reserve_lock);
	return err;
}

int volume_release(struct iet_volume *volume, u64 sid, int force)
{
	int err = 0;
	struct reservation *reservation;

	if (!volume)
		return -ENOENT;

	reservation = &volume->reservation;
	spin_lock(&volume->reserve_lock);

	if (reservation->reservation_type == RESERVATION_TYPE_RESERVE &&
	    (force || reservation->sid == sid)) {
		reservation->reservation_type = RESERVATION_TYPE_NONE;
		reservation->sid = 0;
	} else {
		err = -EBUSY;
	}

	spin_unlock(&volume->reserve_lock);
	return err;
}

static void iet_volume_info_show(struct seq_file *seq, struct iscsi_target *target)
{
	struct iet_volume *volume;

	list_for_each_entry(volume, &target->volumes, list) {
		seq_printf(seq, "\tlun:%u state:%x iotype:%s",
			   volume->lun, volume->l_state, volume->iotype->name);
		if (LUReadonly(volume))
			seq_printf(seq, " iomode:ro");
		else if (LUWCache(volume))
			seq_printf(seq, " iomode:wb");
		else
			seq_printf(seq, " iomode:wt");

		seq_printf(seq, " blocks:%llu blocksize:%u",
			volume->blk_cnt, 1 << volume->blk_shift);
		if (volume->iotype->show)
			volume->iotype->show(volume, seq);
		else
			seq_printf(seq, "\n");
	}
}

static int iet_volume_seq_open(struct inode *inode, struct file *file)
{
	int res;
	res = seq_open(file, &iet_seq_op);
	if (!res)
		((struct seq_file *)file->private_data)->private =
			iet_volume_info_show;
	return res;
}

struct file_operations volume_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= iet_volume_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};
