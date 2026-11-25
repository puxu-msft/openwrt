// SPDX-License-Identifier: GPL-2.0-only
/*
 * Block Mapping & Bad Block Tables for EcoNet / Airoha EN75xx NAND.
 *
 * These SoCs use two tables, BBT and BMT. The BBT is factory bad blocks and
 * the BMT is blocks that wore out over time. When a block is added to the BBT,
 * everything following that block is shifted up by one, so adding a block to
 * the BBT after the fact is a no-go.
 *
 * Blocks added to the BBT reduce the user-servicable area directly, while
 * blocks added to the BMT use a pool of reserve blocks that is above the user
 * area. The BBT and BMT tables themselves are also stored at opposite ends of
 * the reserve area.
 *
 * While the BBT can't be changed, it can be reconstructed. To do this, first
 * the BMT must be reconstructed by scanning blocks in the reserve area to
 * identify those whose OOB data contains a back-reference to the block they
 * are mapped to. Once the BMT has been reconstructed, then we can scan all
 * remaining blocks to identify any remaining which are marked bad, and
 * consider them as (probably!) factory bad blocks.
 *
 * Reconstructing the BBT is not very safe because any confusion between a
 * factory bad block and a worn out block will result in wrong offsets and in
 * effect, data loss. Furthermore, bad blocks do not politely identify
 * themselves, generally they error out when we try to read them. Still, we
 * make the best effort we can by tagging worn blocks with 0x55 rather than
 * 0x00 which is used to tag factory blocks. Vendor firmware does not do this,
 * so if the bootloader or vendor OS marks a block bad, it will be
 * indistinguishable from a factory bad block.
 *
 * The layout looks a little bit like this:
 *
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |D D D D F D D D D D D D W D D D D D D D D F D D|B M M 0 0 0 0 0 T|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     User Area                 |   Reserve Area  |
 *
 * - D: Data block
 * - F: Factory bad block
 * - W: Worn block
 * - B: BBT block
 * - M: Mapped block (replacing a worn block)
 * - 0: Free block
 * - T: BMT table block
 *
 * In this example, there are 22 *usable* blocks in the user area (we include
 * the worn blocks but exclude the factory bad blocks) so this disk will
 * report 2816 KiB.
 *
 * The bottom of the reserve area is decided by counting down from the
 * end until we have REQUIRED_GOOD_BLOCKS non-bad blocks. So when blocks
 * wear out in the reserve area, the bottom is moved down, stealing
 * blocks from the end of the user area and making the disk "shrink".
 * A side-effect of this is if the user puts a valid BBT at the end of
 * their space, there's a chance it might get picked up a THE BBT.
 *
 * Mercifully, blocks in the reserve area are never added to the BBT or
 * BMT, so we don't have any mapping to do in the reserve area, we just
 * have to check every block to see if it's bad before using it.
 *
 * 数池大小时坏块只看 page0 的 spare[0]、不看读状态，与原厂内核和 tcboot 相同；spare[1] 非 FF 或读失败的块计入池大小但不分配。池内的块磨坏时本驱动按原厂在 spare[1] 标记，池底不动；上面说的池底下移只发生在 spare[0] 非 FF 的块上。
 *
 * Configuration:
 *
 * This driver is configured by device tree properties, the following
 * properties are available:
 *
 * - econet,bmt;
 *   This boolean property enables the BMT, if it is not present on the
 *   MTD device, the BMT will not be enabled.
 *
 * - econet,enable-remap;
 *   This boolean property enables remapping of observed worn blocks. It can
 *   be placed either on the device or on a partition within a fixed partitions
 *   table. By default, this module will handle existing mappings but will not
 *   update them. If remapping is enabled, a block may be remapped even if it
 *   is only being read, and remapping carries a risk of lost data, so you
 *   should avoid enabling this on critical partitions like the bootloader, and
 *   you should also avoid enabling this on partitions like UBI which have
 *   their own remapping algorithms.
 *
 * - econet,manual-remap; 分区节点上的布尔属性：允许经 debugfs 的 mtk-bmt/en75_remap 显式重映射本分区里的块，不受 econet,enable-remap 约束；I/O 路径（ECC 读、写、擦失败）照旧只看 econet,enable-remap。只认 partitions 下的分区节点，写在 NAND 节点上不生效，免得一条 echo 就能改到 bootloader 所在的块。撤映射（mtk-bmt 的 mark_good、repair）一律拒绝，见 pub_unmap_block。
 *
 * - econet,assert-reserve-size = <u32>;
 *   This allows you to assert that the computed reserve size matches
 *   the bootloader. It is typical for bootloaders to log a message
 *   such as "bmt pool size: 163" on startup. The computed reserve size MUST
 *   match the bootloader, otherwise it will be looking for the BBT in the
 *   be wrong place. If a block in the reserve space wears out, the reserve
 *   size will be increased to account for it, so this property is not
 *   appropriate for production use. But when porting to a new board, it will
 *   ensure that this module exits early if its calculation does not match the
 *   platform.
 *
 * - econet,can-write-factory-bbt;
 *   This boolean property enables updating / rebuilding of the factory BBT.
 *   ANY CHANGE TO THE BBT IMPLIES DATA LOSS. If you enable this in conjunction
 *   with econet,factory-badblocks then it will set the BBT to the configured
 *   bad blocks. If you enable it alone, it will use the bootloader's logic:
 *   Search for a BBT, if one cannot be found then scan the disk for faulty
 *   blocks which have not been mapped as "worn blocks", mark them all as bad,
 *   and create a new BBT with their indexes. This applies to the whole disk
 *   and makes no effort to recover data, it might decide the bootloader is
 *   bad, you were warned.
 *
 * - econet,factory-badblocks = <u32 array>;
 *   This property allows you to specify the factory bad blocks.
 *   If econet,can-write-factory-bbt is unset, this is an assertion which will
 *   cause early exit if the observed BBT does not match the specified bad
 *   blocks. If econet,can-write-factory-bbt is set, this will overwrite the
 *   BBT with the specified bad blocks.
 *
 * - econet,bbt-table-size = <u32>;
 *   Number of entries in the BBT table. Different vendor firmware versions
 *   use different sizes (250 or 1000). The checksum is calculated over the
 *   entire table, so this must match the firmware that created the BBT. If
 *   not specified, defaults to 1000.
 *
 * debugfs（mtk-bmt 目录下，本驱动另加的两个文件）：
 *
 * - en75_remap（只写）：写入 MTD 内按擦除块对齐的字节偏移，把该用户块显式重映射到保留池：取池底往上第一个可用块、擦除、整块搬过去（映射标记随每页数据一次写入）、新 BMT 写进另一个块并读回核对，通过后才切换映射，再把原块擦掉、打上磨损标记（原块已是备用块时按原厂擦掉后在池内标坏）。写入成功表示映射已落盘；失败时写入返回错误码、BMT 与映射都不变：-EINVAL 偏移不对齐或越界，-EPERM 不在 econet,manual-remap 的分区里，-ENOSPC 池里没有可用块，-EIO 搬运或写表失败。
 * - en75_state（只读）：池与表的状态、重映射允许的范围（[起, 止)）、最近一次显式重映射的结果，每行“键 值”，按键名取值、不以行序为接口，供修复工具前后对照。last_retire 说明最后一步做了没有：done 已退役原块；skipped <块> 映射已生效，但旧 BMT 所在的那一块擦不掉、原块没有退役（不要重试）；- 还没走到这一步。
 *
 * Copyright (C) 2025 Caleb James DeLisle <cjd@cjdns.fr>
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <asm/barrier.h>
#include "mtk_bmt.h"

#define MAX_BMT_SIZE				256

/* Maximum BBT table size (structure allocation limit). */
#define MAX_BBT_SIZE				1000

/* Default BBT table size if not specified in DTS. */
#define DEFAULT_BBT_TABLE_SIZE			1000

/* Vendor firmware calls this POOL_GOOD_BLOCK_PERCENT */
#define REQUIRED_GOOD_BLOCKS(total_blocks)	((total_blocks) * 8 / 100)

/* This is ours but it is on-disk so we need consensus with ourselves. */
#define BLOCK_WORN_MARK				0x55

/* How hard to try, these must be at least one. */
#define ERASE_TRIES				3
/*
 * Number of blocks to try to write updated tables to before failing.
 *
 * 每块只写一次：同一页再编程会把新算的 ECC 校验叠进旧校验（见 w_retire_user_block），写失败就换块。
 */
#define UPDATE_TABLE_TRIES			3

#define INITIAL_READ_TRIES			3

/* Max number of FBBs that can be expressed in the devicetree. */
#define MAX_FACTORY_BAD_BLOCKS_OF		32

const char *log_pfx			= "en75_bmt";
const char *name_can_write_factory_bbt	= "econet,can-write-factory-bbt";
const char *name_factory_badblocks	= "econet,factory-badblocks";
const char *name_enable_remap		= "econet,enable-remap";
const char *name_assert_reserve_size	= "econet,assert-reserve-size";
const char *name_bbt_table_size	= "econet,bbt-table-size";
const char *name_manual_remap		= "econet,manual-remap";

/* To promote readability, most functions must have their inputs passed in. */
#define bmtd dont_directly_reference_mtk_bmtd

/*
 * On disk
 */

struct bmt_table_header {
	/* "BMT" */
	char signature[3];
	/* 1 */
	u8 version;
	/* Unused */
	u8 bad_count;
	/* Number of mappings in table */
	u8 size;
	/* bmt_checksum() */
	u8 checksum;
	/* Unused */
	u8 reserved[13];
} bmt_table_header;
static_assert(sizeof(struct bmt_table_header) == 20);

struct bmt_entry {
	/* The bad block which is being mapped */
	u16	from;
	/* The block in the reserve area that is being used */
	u16	to;
};

struct bmt_table {
	struct bmt_table_header	header;
	struct bmt_entry	table[MAX_BMT_SIZE];
};

struct bbt_table_header {
	/* "RAWB" */
	char signature[4];
	/* bbt_checksum() (only 16 bits used) */
	u32 checksum;
	/* 1 */
	u8 version;
	/* Number of bad blocks in table */
	u8 size;
	/* Unused (ffff) */
	u8 reserved[2];
};

static_assert(sizeof(struct bbt_table_header) == 12);

struct bbt_table {
	struct bbt_table_header	header;
	/* This must be stored in ascending numerical order */
	u16			table[MAX_BBT_SIZE];
};

static_assert(sizeof(struct bbt_table) == 2012);

/*
 * In memory
 */

/* Use this to differentiate between on-disk indexes and integers */
struct block_index {
	u16 index;
};

static_assert(sizeof(struct block_index) == 2);

enum block_status {
	BS_INVALID,
	BS_AVAILABLE,
	BS_BAD,
	BS_MAPPED,
	BS_BMT,
	BS_BBT,
	BS_NEED_ERASE,
};

/* 池块状态的补充说明，只供 en75_state 显示 */
enum block_note {
	BN_NONE,
	/* BS_BAD：page0 spare[0] 不是 FF，不计入池大小 */
	BN_SPARE0,
	/* BS_BAD：spare[0] 是 FF、spare[1] 不是，原厂给池内坏块打的标记，计入池大小 */
	BN_SPARE1,
	/* BS_BAD：page0 读失败，计不计入池大小只看读到的 spare[0] */
	BN_READ,
	/* BS_MAPPED：page0 带映射标记（spare[2..3] = from） */
	BN_MARKER,
	/* BS_MAPPED：没有映射标记，只凭 BMT 表项认出（标记被分区重写擦掉了） */
	BN_BMT_ONLY,
};

struct block_info {
	struct block_index index;
	enum block_status status : 8;
	enum block_note note : 8;
};

static_assert(sizeof(struct block_info) == 4);

struct block_range {
	u16 begin;
	u16 end;
	/* 分区名（DT label），整个用户区时为 NULL；只供显示 */
	const char *label;
};

/* 一次显式重映射（debugfs en75_remap）的结果，en75_state 里显示 */
struct manual_remap_result {
	/* 第几次显式重映射，0 表示还没有过 */
	int seq;
	int ret;
	int user_block;
	/* BBT 平移后的物理块，即 BMT 表项的 from */
	int from;
	/* 重映射前数据所在的块：from 本身或旧备用块 */
	int old;
	/* 新备用块，没分到时为 -1 */
	int to;
	/* 新 BMT 所在块，没写成时为 -1 */
	int bmt_block;
	/* 搬运时读到不可纠错的页数（照搬，与原厂一样不看读状态） */
	int uncorrectable_pages;
	/* 映射切换之后有没有退役原块：旧表擦不掉时不退役，stale_bmt 记旧表所在块 */
	enum { RETIRE_NONE, RETIRE_DONE, RETIRE_SKIPPED } retire;
	int stale_bmt;
};

/*
 * Context and params
 */

struct en75_bmt_m {
	struct bmt_desc *mtk;

	/* In-memory copy of the BBT */
	struct bbt_table bbt;

	/* BBT table size, from DTS or default */
	u16 bbt_table_size;

	/* In-memory copy of the BMT */
	struct bmt_table bmt;

	/* Array of blocks in reserve area, size = reserve_block_count(ctx) */
	struct block_info *rblocks;

	/* BBT is on the first usable block after this. */
	u16 reserve_area_begin;

	/*
	 * Only allow remapping of blocks within the following ranges.
	 * Expressed as user block index, not mapped through BBT.
	 */
	u16 can_remap_range_count;
	struct block_range *can_remap_ranges;

	/* Incremented each time the BBT is changed, reset when written */
	s8 bbt_dirty;

	/* Incremented each time the BMT is changed, reset when written */
	s8 bmt_dirty;

	/* Unless set, fail any attempt to write the BBT. */
	s8 can_write_factory_bbt;

	/*
	 * 允许经 debugfs 显式重映射的用户块范围，来自分区节点的 econet,manual-remap，按用户块号、不经 BBT 平移。
	 */
	u16 manual_remap_range_count;
	struct block_range *manual_remap_ranges;

	/*
	 * 串行化改写 BMT 与保留池状态的入口：I/O 路径的自动重映射、debugfs 的显式重映射、读 en75_state。查表（get_mapping_block）不持锁，表项按 bmt_publish_entry() 的次序发布。
	 */
	struct mutex lock;

	struct manual_remap_result last_remap;
};

/*
 * In-memory functions (do not read or write)
 */

static u16 bbt_checksum(const struct bbt_table *bbt, u16 table_size)
{
	const u8 *data = (u8 *)bbt->table;
	u16 checksum = bbt->header.version + bbt->header.size;

	for (int i = 0; i < table_size * sizeof(bbt->table[0]); i++)
		checksum += data[i];

	return checksum;
}

static u8 bmt_checksum(const struct bmt_table *bmt, int check_entries)
{
	int length;
	const u8 *data;
	u8 checksum;

	WARN_ON_ONCE(check_entries > MAX_BMT_SIZE);
	length = min(check_entries, MAX_BMT_SIZE) * sizeof(bmt->table[0]);
	data = (u8 *)&bmt->table;
	checksum = bmt->header.version + bmt->header.size;
	for (int i = 0; i < length; i++)
		checksum += data[i];

	return checksum;
}

static int reserve_block_count(const struct en75_bmt_m *ctx)
{
	return ctx->mtk->total_blks - ctx->reserve_area_begin;
}

/* return a block_info or error pointer */
static struct block_info *find_available_block(const struct en75_bmt_m *ctx, bool start_from_end)
{
	int limit = reserve_block_count(ctx);
	int i = 0;
	int d = 1;

	/*
	 * rblocks is populated by scanning in reverse,
	 * so lowest block numbers are at the end.
	 */
	if (!start_from_end) {
		i = limit - 1;
		d = -1;
	}
	for (; i < limit && i >= 0; i += d) {
		if (ctx->rblocks[i].status == BS_AVAILABLE)
			return &ctx->rblocks[i];
	}
	return ERR_PTR(-ENOSPC);
}

static int compare_bbt(const void *a, const void *b)
{
	return *(u16 *)a - *(u16 *)b;
}

static void sort_bbt(struct bbt_table *bbt)
{
	sort(bbt->table, bbt->header.size, sizeof(bbt->table[0]),
	     compare_bbt, NULL);
}

/*
 * When there's a factory bad block, we shift everything above it up by one.
 * We sort the BBT so that we can do this in one pass. The vendor firmware
 * also requires an ascending ordered BBT.
 */
static int get_mapping_block_bbt(const struct en75_bmt_m *ctx, int block)
{
	int size = ctx->bbt.header.size;

	for (int i = 0; i < size; i++)
		if (ctx->bbt.table[i] <= block)
			block++;

	if (block >= ctx->reserve_area_begin || block < 0)
		return -EINVAL;

	return block;
}

static int get_mapping_block(const struct en75_bmt_m *ctx, int block)
{
	int size;

	block = get_mapping_block_bbt(ctx, block);

	if (block < 0)
		return block;

	/* 与 bmt_publish_entry() 配对：先读计数，再读表项 */
	size = READ_ONCE(ctx->bmt.header.size);
	smp_rmb();
	for (int i = size - 1; i >= 0; i--)
		if (ctx->bmt.table[i].from == block)
			return READ_ONCE(ctx->bmt.table[i].to);

	return block;
}

/* 物理块（BBT 平移后）对应的用户块：减去它下面的出厂坏块数 */
static int get_user_block(const struct en75_bmt_m *ctx, int block)
{
	int user = block;

	for (int i = 0; i < ctx->bbt.header.size; i++)
		if (ctx->bbt.table[i] < block)
			user--;
	return user;
}

/* from 的 BMT 表项下标，没有时返回 -1；与查表一样最后一项胜 */
static int bmt_find_from(const struct en75_bmt_m *ctx, u16 from)
{
	for (int i = ctx->bmt.header.size - 1; i >= 0; i--)
		if (ctx->bmt.table[i].from == from)
			return i;
	return -1;
}

/*
 * 改内存里的 BMT 表项。查表的 I/O 路径不持锁：新表项先写好再加计数，原位改 to 用一次 16 位写。调用方持 ctx->lock。
 */
static void bmt_publish_entry(struct en75_bmt_m *ctx, int idx, u16 from, u16 to)
{
	if (idx < ctx->bmt.header.size) {
		WRITE_ONCE(ctx->bmt.table[idx].to, to);
		return;
	}
	ctx->bmt.table[idx] = (struct bmt_entry){ .from = from, .to = to };
	smp_wmb();
	WRITE_ONCE(ctx->bmt.header.size, idx + 1);
}

static struct block_info *rblock_by_index(const struct en75_bmt_m *ctx, int block)
{
	int rblocks = reserve_block_count(ctx);

	for (int i = 0; i < rblocks; i++)
		if (ctx->rblocks[i].index.index == block)
			return &ctx->rblocks[i];
	return NULL;
}

static bool block_in_ranges(const struct block_range *ranges, int count,
			    int block, const char **label)
{
	for (int i = 0; i < count; i++) {
		if (block >= ranges[i].begin && block < ranges[i].end) {
			if (label)
				*label = ranges[i].label;
			return true;
		}
	}
	return false;
}

static bool block_is_erased(u8 *data, u32 datalen, u8 *oob, u32 ooblen)
{
	for (int i = 0; i < datalen; i++)
		if (data[i] != 0xff)
			return false;
	for (int i = 0; i < ooblen; i++)
		if (oob[i] != 0xff)
			return false;
	return true;
}

static bool block_index_is_sane(const struct en75_bmt_m *ctx,
				const struct block_index bi,
				bool user_block)
{
	if (bi.index >= ctx->mtk->total_blks)
		return false;
	if (user_block && bi.index >= ctx->reserve_area_begin)
		return false;
	if (!user_block && bi.index < ctx->reserve_area_begin)
		return false;
	return true;
}

/*
 * Write
 */

static int w_write(const struct en75_bmt_m *ctx,
		   const struct block_index bi,
		   bool user_area,
		   const char *name,
		   size_t len,
		   u8 *buf,
		   bool oob)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB,
		.ooboffs = 0,
		.ooblen = (oob) ? len : 0,
		.oobbuf = (oob) ? buf : NULL,
		.len = (!oob) ? len : 0,
		.datbuf = (!oob) ? buf : NULL,
	};
	int ret;

	if (WARN_ON(!block_index_is_sane(ctx, bi, user_area)))
		return -EINVAL;

	ret = ctx->mtk->_write_oob(
		ctx->mtk->mtd,
		((loff_t)bi.index) << ctx->mtk->blk_shift,
		&ops);
	if (ret)
		pr_warn("%s: error writing %s at %d\n",
			log_pfx, name, bi.index);
	return ret;
}

/*
 * Erase & Mark bad
 */

static int w_mark_bad(
	const struct en75_bmt_m *ctx,
	const struct block_index bi,
	bool user_area)
{
	u8 fdm[4] = {BLOCK_WORN_MARK, 0xff, 0xff, 0xff};

	/*
	 * Don't erase first because it's damaged so erase is likely to fail.
	 * In we should only be clearing bits so erase should be unnecessary.
	 */
	return w_write(ctx, bi, user_area, "BAD_BLK",
		       sizeof(fdm), fdm, true);
}

/*
 * 给保留池里的块标坏，按原厂：先擦，再在 page0 的 spare[1] 写 0x00（原厂 en7512_nand_mark_badblock(offset, BMT_BADBLOCK_GENERATE_LATER)，vmlinux 0x802d6eac；GPL spi_nand_flash.c:3947-3988）。spare[0] 保持 FF，池大小因此不变（原厂与我们都只按 spare[0] 数池大小），spare[1] 非 FF 让这块不再被分配。
 */
static int w_mark_bad_reserve(const struct en75_bmt_m *ctx, struct block_info *bif)
{
	u8 fdm[4] = {0xff, 0x00, 0xff, 0xff};

	bbt_nand_erase(bif->index.index);
	bif->status = BS_BAD;
	bif->note = BN_SPARE1;
	return w_write(ctx, bif->index, false, "POOL_BAD", sizeof(fdm), fdm, true);
}

/*
 * 退役一个用户块：按原厂先擦（en7512_nand_mark_badblock 先 spi_nand_erase_block，GPL spi_nand_flash.c:3977），再在 page0 的 spare[0] 写磨损标记 0x55（与出厂坏块的 0x00 区分，见文件头）。spare[0..3] 落在片上 ECC 保护的 metadata 里，在已编程的 page0 上以 ECC 开启再编程，片上 ECC 会把新算的校验叠进已有校验位，读出来多半不可纠（推断，GS2107 docs/35 N25、N26），所以不在原数据上直接补标记。调用方须在新 BMT 落盘、数据已在备用块上之后才调用。
 */
static int w_retire_user_block(const struct en75_bmt_m *ctx, u16 block)
{
	u8 fdm[4] = {BLOCK_WORN_MARK, 0xff, 0xff, 0xff};

	bbt_nand_erase(block);
	return w_write(ctx, (struct block_index) { .index = block }, true,
		       "WORN", sizeof(fdm), fdm, true);
}

static int w_erase_one(
	const struct en75_bmt_m *ctx,
	const struct block_index bi,
	bool user_area)
{
	int ret = 0;

	if (WARN_ON(!block_index_is_sane(ctx, bi, user_area)))
		return -EINVAL;

	for (int i = 0; i < ERASE_TRIES; i++) {
		ret = bbt_nand_erase(bi.index);
		if (!ret)
			break;
	}

	return ret;
}

static void w_erase_pending(const struct en75_bmt_m *ctx)
{
	int rblocks;

	rblocks = reserve_block_count(ctx);
	for (int i = 0; i < rblocks; i++) {
		const struct block_info bif = ctx->rblocks[i];
		int ret;

		if (bif.status != BS_NEED_ERASE)
			continue;

		ret = w_erase_one(ctx, bif.index, false);
		if (ret) {
			if (WARN_ON_ONCE(ret == -EINVAL)) {
				/* Just set status bad so we ignore it */
				ctx->rblocks[i].status = BS_BAD;
			} else {
				pr_info("%s: failed to erase block %d, marking bad\n",
					log_pfx, bif.index.index);
				w_mark_bad_reserve(ctx, &ctx->rblocks[i]);
			}
		} else {
			ctx->rblocks[i].status = BS_AVAILABLE;
		}
	}
}

static void mark_for_erasure(struct en75_bmt_m *ctx, enum block_status with_status)
{
	int rblocks = reserve_block_count(ctx);

	for (int i = 0; i < rblocks; i++) {
		if (ctx->rblocks[i].status == with_status)
			ctx->rblocks[i].status = BS_NEED_ERASE;
	}
}

/*
 * Update tables
 */

/*
 * Try on UPDATE_TABLE_TRIES blocks then give up
 * Return a block_info or pointer error.
 */
static struct block_info *w_update_table(
	const struct en75_bmt_m *ctx,
	bool start_from_end,
	const char *name,
	size_t len,
	u8 *buf)
{
	for (int i = 0; i < UPDATE_TABLE_TRIES; i++) {
		struct block_info *bif =
			find_available_block(ctx, start_from_end);
		int ret;

		if (IS_ERR(bif)) {
			/* 池里没有空块时才先擦待擦块，平时它们要等新表落盘之后再擦 */
			w_erase_pending(ctx);
			bif = find_available_block(ctx, start_from_end);
		}
		if (IS_ERR(bif)) {
			pr_err("%s: no space to store %s\n", log_pfx, name);
			return bif;
		}
		/*
		 * 先擦：原厂写表的块都是擦过的（write_bmt_to_flash 原地擦了再写，新块取自 find_available_block 擦过的池，GPL bmt.c:561-598、735-745），可用块在这里只按 page0 的数据与 spare[0..3] 判过擦除态，已编程的页不再编程（见 w_retire_user_block）。
		 */
		ret = w_erase_one(ctx, bif->index, false);
		if (!ret)
			ret = w_write(
				ctx, bif->index, false, name,
				len, buf, false);
		if (ret) {
			bif->status = BS_NEED_ERASE;
			continue;
		}
		return bif;
	}
	return ERR_PTR(-EIO);
}

/*
 * 把 bmt 写进保留池里自末端往下第一个可用块并读回逐字节核对，核对不过的块记为待擦、换下一块。成功后旧表所在块记为待擦（由 w_erase_pending 擦掉），新块记为 BS_BMT。写出的格式与原厂一致：[size, 池大小) 的表项清 0，校验和只算 size 项——原厂按池大小项算和（vmlinux fill_nand_bmt_buffer 0x802da674），多出的表项都是 0，结果相同。
 */
static struct block_info *w_write_bmt(struct en75_bmt_m *ctx, struct bmt_table *bmt)
{
	int clear_to = min(reserve_block_count(ctx), MAX_BMT_SIZE);

	for (int i = bmt->header.size; i < clear_to; i++)
		bmt->table[i] = (struct bmt_entry){ 0 };
	bmt->header.checksum = bmt_checksum(bmt, bmt->header.size);

	for (int i = 0; i < UPDATE_TABLE_TRIES; i++) {
		struct block_info *bif;
		u8 fdm[4];
		int ret;

		bif = w_update_table(ctx, true, "BMT", sizeof(*bmt), (u8 *)bmt);
		if (IS_ERR(bif))
			return bif;

		ret = bbt_nand_read(blk_pg(bif->index.index), ctx->mtk->data_buf,
				    ctx->mtk->pg_size, fdm, sizeof(fdm));
		if (!ret && !memcmp(ctx->mtk->data_buf, bmt, sizeof(*bmt))) {
			mark_for_erasure(ctx, BS_BMT);
			bif->status = BS_BMT;
			return bif;
		}
		pr_warn("%s: BMT read back from block %d does not match (%d)\n",
			log_pfx, bif->index.index, ret);
		bif->status = BS_NEED_ERASE;
	}
	return ERR_PTR(-EIO);
}

/*
 * 先写表、后擦：待擦的块里有旧表和被换下的备用块，新表落盘之前闪存上那张表还指着它们，先擦的话中间掉电就丢数据。
 */
static int w_sync_tables(struct en75_bmt_m *ctx)
{
	if (ctx->bmt_dirty) {
		int dirty = ctx->bmt_dirty;
		struct block_info *new_bmt_block;

		new_bmt_block = w_write_bmt(ctx, &ctx->bmt);

		/*
		 * If we can't write the BMT, we won't try to write the BBT.
		 * Without the BMT it is impossible to safely reconstruct.
		 */
		if (IS_ERR(new_bmt_block)) {
			pr_err("%s: error writing BMT to block\n", log_pfx);
			return PTR_ERR(new_bmt_block);
		}
		pr_info("%s: BMT written to block %d\n",
			log_pfx, new_bmt_block->index.index);

		ctx->bmt_dirty -= dirty;
		WARN_ON(ctx->bmt_dirty);
	}

	if (ctx->bbt_dirty && !ctx->can_write_factory_bbt) {
		WARN_ONCE("%s: BUG: BBT requires update but %s is not set\n",
			  log_pfx, name_can_write_factory_bbt);
	} else if (ctx->bbt_dirty) {
		int dirty = ctx->bbt_dirty;
		struct block_info *new_bbt_block;
		size_t bbt_write_size = sizeof(ctx->bbt.header) +
					ctx->bbt_table_size * sizeof(ctx->bbt.table[0]);

		for (int i = ctx->bbt.header.size; i < ctx->bbt_table_size; i++)
			ctx->bbt.table[i] = 0;
		ctx->bbt.header.checksum = bbt_checksum(&ctx->bbt, ctx->bbt_table_size);
		new_bbt_block = w_update_table(
			ctx,
			false,
			"BBT",
			bbt_write_size,
			(u8 *)&ctx->bbt);

		if (IS_ERR(new_bbt_block)) {
			pr_err("%s: error writing BBT to block\n", log_pfx);
			return PTR_ERR(new_bbt_block);
		}
		pr_info("%s: BBT written to block %d\n",
			log_pfx, new_bbt_block->index.index);
		mark_for_erasure(ctx, BS_BBT);
		new_bbt_block->status = BS_BBT;

		ctx->bbt_dirty -= dirty;
		WARN_ON(ctx->bbt_dirty);
	}

	w_erase_pending(ctx);
	return 0;
}

/*
 * Remap
 */

/*
 * 把 src 块的 [0, copy_len) 字节搬到已擦除的保留池块 dst，映射标记（spare[2..3] = from，主机序）随每页数据一次写入：原厂 update_bmt 内联的 migrate_from_bad 每页先读、set_bad_index_to_oob 填索引、再整页写一次（vmlinux 0x802db3e0 读、0x802db400 填、0x802db42c 写；GPL bmt.c:675-711），page0 只编程一次（spare[0..3] 在片上 ECC 保护区内，同一页编程两次读出来多半不可纠，见 w_retire_user_block）。spare[0..1] 写 FF：新块是好块。读到不可纠错的页照样搬并计数（原厂在 0x802db3e0 之后不判读状态）；page0 以外数据与 spare[0..3] 全 FF 的页不写，留着擦除态。返回 0；读源块出传输错误时返回负码、*dst_failed 为假；写 dst 失败时返回负码、*dst_failed 为真。
 */
static int w_copy_block(const struct en75_bmt_m *ctx, u16 dst, u16 src, u16 from,
			int copy_len, int *uncorrectable, bool *dst_failed)
{
	struct bmt_desc *mtk = ctx->mtk;
	int pages = mtk->blk_size >> mtk->pg_shift;

	*dst_failed = false;
	for (int p = 0; p < pages; p++) {
		loff_t off = (loff_t)p << mtk->pg_shift;
		u8 oob[4];
		struct mtd_oob_ops rd = {
			.mode = MTD_OPS_PLACE_OOB,
			.datbuf = mtk->data_buf,
			.len = mtk->pg_size,
			.oobbuf = oob,
			.ooblen = sizeof(oob),
		};
		struct mtd_oob_ops wr = rd;
		int ret;

		if (off >= copy_len)
			break;

		ret = mtk->_read_oob(mtk->mtd, ((loff_t)src << mtk->blk_shift) + off, &rd);
		if (mtd_is_eccerr(ret)) {
			(*uncorrectable)++;
		} else if (ret < 0) {
			pr_err("%s: copy %d->%d: reading page %d failed: %d\n",
			       log_pfx, src, dst, p, ret);
			return ret;
		}

		if (p && block_is_erased(mtk->data_buf, mtk->pg_size, oob, sizeof(oob)))
			continue;

		oob[0] = 0xff;
		oob[1] = 0xff;
		/* vendor firmware uses host order */
		memcpy(&oob[2], &from, sizeof(from));
		ret = mtk->_write_oob(mtk->mtd, ((loff_t)dst << mtk->blk_shift) + off, &wr);
		if (ret < 0) {
			pr_err("%s: copy %d->%d: writing page %d failed: %d\n",
			       log_pfx, src, dst, p, ret);
			*dst_failed = true;
			return ret;
		}
	}
	return 0;
}

/*
 * 取一个备用块：池底往上第一个可用块（原厂 update_bmt 调 find_available_block(false)，vmlinux 0x802db1c4、0x802db324），先擦（原厂第一次分配前把池里未映射的块全擦一遍，GPL bmt.c:561-598），再从 src 搬 [0, copy_len)。copy_len 为 0 时只擦不写：调用方接着要擦或写这块，先写映射标记只会被擦掉或让 page0 再编程一次。擦或写失败的块按原厂在池内标坏、换下一块，最多 UPDATE_TABLE_TRIES 块，都不行返回 -EIO。读源块出传输错误时返回该错误码并置 *src_failed。返回的块仍是 BS_AVAILABLE，调用方在 BMT 落盘或改好之后改成 BS_MAPPED。
 */
static struct block_info *w_alloc_spare(struct en75_bmt_m *ctx, u16 src, u16 from,
					int copy_len, int *uncorrectable,
					bool *src_failed)
{
	*src_failed = false;
	for (int i = 0; i < UPDATE_TABLE_TRIES; i++) {
		struct block_info *bif = find_available_block(ctx, false);
		bool dst_failed = true;
		int bad_pages = 0;
		int ret;

		if (IS_ERR(bif))
			return bif;

		ret = w_erase_one(ctx, bif->index, false);
		if (!ret && copy_len)
			ret = w_copy_block(ctx, bif->index.index, src, from,
					   copy_len, &bad_pages, &dst_failed);
		if (!ret) {
			*uncorrectable = bad_pages;
			return bif;
		}
		if (!dst_failed) {
			/* 源块读不出来，换备用块也没用 */
			bif->status = BS_NEED_ERASE;
			*src_failed = true;
			return ERR_PTR(ret);
		}
		pr_warn("%s: spare block %d failed (%d), marking bad\n",
			log_pfx, bif->index.index, ret);
		w_mark_bad_reserve(ctx, bif);
	}
	return ERR_PTR(-EIO);
}

static int w_remap_block(struct en75_bmt_m *ctx,
			 u16 block,
			 struct block_info *maybe_mapped_block,
			 int copy_len)
{
	struct block_info *new_block;
	u16 copy_from = block;
	int uncorrectable = 0;
	bool copied = copy_len;
	bool src_failed;
	int bmt_index;

	if (ctx->bmt.header.size == 0xff) {
		pr_err("%s: BMT full, cannot add more mappings\n", log_pfx);
		return -ENOSPC;
	}

	if (maybe_mapped_block)
		copy_from = maybe_mapped_block->index.index;

	new_block = w_alloc_spare(ctx, copy_from, block, copy_len, &uncorrectable,
				  &src_failed);
	/*
	 * 只有源块读不出来时才退回只擦不搬；备用块擦写失败就报错、不建映射，否则会把用户块映射到空块上。
	 */
	if (IS_ERR(new_block) && src_failed) {
		/*
		 * We can either return an error or continue.
		 * If the user is reading, not returning an error means
		 * they're going to suddenly switch to reading a another
		 * (empty) block and won't know it.
		 *
		 * If the user is erasing, returning an error means we're
		 * not doing our job. In any case, we ideally should be
		 * refusing to read newly remapped blocks until the user
		 * has issued an erase command, but we're using the mtk_bmt
		 * framework which does not support that, so we're going to
		 * have to continue.
		 *
		 * Good luck, user.
		 */
		pr_err("%s: remap copy from %d failed LIKELY DATA LOSS!\n",
		       log_pfx, copy_from);
		new_block = w_alloc_spare(ctx, copy_from, block, 0, &uncorrectable,
					  &src_failed);
		copied = false;
	}
	if (IS_ERR(new_block)) {
		pr_err("%s: cannot remap block %d: %ld\n",
		       log_pfx, block, PTR_ERR(new_block));
		return PTR_ERR(new_block);
	}

	bmt_index = bmt_find_from(ctx, block);
	if (bmt_index < 0)
		bmt_index = ctx->bmt.header.size;
	bmt_publish_entry(ctx, bmt_index, block, new_block->index.index);
	ctx->bmt_dirty++;

	new_block->status = BS_MAPPED;
	new_block->note = copied ? BN_MARKER : BN_BMT_ONLY;

	/*
	 * Directly mark the user block bad (if possible), the mapped block we can
	 * try to reclaim, mark it need erase and see if the eraser decides it's bad.
	 */
	w_mark_bad(ctx, (struct block_index) { .index = block }, true);
	if (maybe_mapped_block)
		maybe_mapped_block->status = BS_NEED_ERASE;

	return 0;
}

enum unmap_erase {
	UE_NO_ERASE,
	UE_ATTEMPT_ERASE,
	UE_REQUIRE_ERASE,
};

/*
 * If no mapping in BMT, -EINVAL
 * Attempt erase of block if requested
 * Remove mapping from BMT
 * Set mapped block to needs erase
 */
static int w_unmap_block(struct en75_bmt_m *ctx,
			 u16 block,
			 enum unmap_erase erase)
{
	int bmt_size = ctx->bmt.header.size;
	struct block_info *mapped_block = NULL;
	int bmt_index = 0;

	for (; bmt_index < bmt_size; bmt_index++) {
		int rblocks;

		rblocks = reserve_block_count(ctx);
		if (ctx->bmt.table[bmt_index].from != block)
			continue;

		for (int j = 0; j < rblocks; j++) {
			if (ctx->rblocks[j].index.index == ctx->bmt.table[bmt_index].to) {
				mapped_block = &ctx->rblocks[j];
				break;
			}
		}
		break;
	}
	if (!mapped_block) {
		pr_err("%s: block %d not mapped\n", log_pfx, block);
		return -EINVAL;
	}
	WARN_ON_ONCE(mapped_block->status != BS_MAPPED);
	if (erase > UE_NO_ERASE) {
		int ret;

		ret = w_erase_one(ctx, mapped_block->index, false);
		if (ret) {
			if (erase == UE_REQUIRE_ERASE) {
				pr_err("%s: unmap block %d: erase failed\n",
				       log_pfx, block);
				return ret;
			}
			pr_warn("%s: unmap block %d: erase failed\n",
				log_pfx, block);
		}
	}

	ctx->bmt.table[bmt_index] = ctx->bmt.table[--bmt_size];
	ctx->bmt.table[bmt_size] = (struct bmt_entry) { 0 };
	ctx->bmt.header.size = bmt_size;
	ctx->bmt_dirty++;

	mapped_block->status = BS_NEED_ERASE;
	return 0;
}

/*
 * Init functions
 */

enum block_is_bad {
	/* Good block */
	BB_GOOD,
	/* Probably marked bad at the factory (or by vendor firmware!) */
	BB_FACTORY_BAD,
	/* Marked bad by us */
	BB_WORN,
	/* We don't know */
	BB_UNKNOWN_BAD,
};

static enum block_is_bad fdm_is_bad(u8 fdm[static 4])
{
	if (fdm[0] == 0xff && fdm[1] == 0xff)
		return BB_GOOD;
	if (fdm[0] == BLOCK_WORN_MARK)
		return BB_WORN;
	if (fdm[0] == 0x00 || fdm[1] == 0x00)
		return BB_FACTORY_BAD;
	return BB_UNKNOWN_BAD;
}

static bool fdm_is_mapped(u8 fdm[static 4])
{
	if (fdm[0] != 0xff || fdm[1] != 0xff)
		return false;
	return fdm[2] != 0xff || fdm[3] != 0xff;
}

static void r_reconstruct_bmt(struct en75_bmt_m *ctx)
{
	int reserve_area_begin = ctx->reserve_area_begin;

	memset(&ctx->bmt, 0xff, sizeof(ctx->bmt.header));
	/*
	 * The table must be zero because the vendor firmware checksums
	 * over a number of entries equal to the number of blocks in the
	 * reserve area (note that when a block in the reserve area fails,
	 * this number will increase on next boot!). But we and the vendor
	 * firmware both store the entire table, and zeroed entries do not
	 * affect the checksum.
	 */
	memset(&ctx->bmt, 0x00, sizeof(ctx->bmt.table));
	memcpy(&ctx->bmt.header.signature, "BMT", 3);
	ctx->bmt.header.version = 1;
	ctx->bmt.header.size = 0;
	ctx->bmt_dirty++;

	for (int i = ctx->mtk->total_blks - 1; i >= reserve_area_begin; i--) {
		unsigned short mapped_block;
		u8 fdm[4];
		int ret;

		ret = bbt_nand_read(blk_pg(i),
				    ctx->mtk->data_buf, ctx->mtk->pg_size,
				    fdm, sizeof(fdm));
		if (ret < 0 || fdm_is_bad(fdm))
			continue;

		/* Vendor firmware uses host order. */
		memcpy(&mapped_block, &fdm[2], 2);
		if (mapped_block >= reserve_area_begin)
			continue;
		pr_info("%s: Found mapping %d->%d\n", log_pfx, mapped_block, i);
		ctx->bmt.table[ctx->bmt.header.size++] = (struct bmt_entry) {
			.from = mapped_block,
			.to = i
		};
	}
}

static int r_reconstruct_bbt(struct bbt_table *bbt_out, const struct en75_bmt_m *ctx)
{
	int reserve_area_begin = ctx->reserve_area_begin;
	int bmt_size = ctx->bmt.header.size;

	/* Need the BMT to exist in order to reconstruct the BBT. */
	if (WARN_ON_ONCE(!ctx->bmt.header.version))
		return -EINVAL;

	memset(bbt_out, 0xff, sizeof(bbt_out->header));
	/* Vendor firmware checksums the entire table, no matter how much is used. */
	memset(bbt_out->table, 0x00, ctx->bbt_table_size * sizeof(bbt_out->table[0]));
	memcpy(bbt_out->header.signature, "RAWB", 4);
	bbt_out->header.version = 1;
	bbt_out->header.size = 0;

	for (int i = 0; i < reserve_area_begin; i++) {
		bool is_mapped = false;
		int ret;
		u8 fdm[4];

		for (int j = 0; j < bmt_size; j++)
			if (ctx->bmt.table[j].from == i) {
				is_mapped = true;
				break;
			}

		if (is_mapped)
			continue;

		ret = bbt_nand_read(blk_pg(i),
				    ctx->mtk->data_buf, ctx->mtk->pg_size,
				    fdm, sizeof(fdm));
		if (!ret) {
			enum block_is_bad status = fdm_is_bad(fdm);

			if (status == BB_GOOD || status == BB_WORN)
				continue;
		}

		pr_info("%s: Found factory bad block %d\n", log_pfx, i);
		bbt_out->table[bbt_out->header.size++] = (u16)i;
	}
	sort_bbt(bbt_out);
	return 0;
}

static int try_parse_bbt(struct bbt_table *out, u8 *buf, int len, u16 table_size)
{
	static struct bbt_table workspace;
	size_t bbt_size = sizeof(workspace.header) + table_size * sizeof(workspace.table[0]);

	if (len < bbt_size)
		return -EINVAL;

	memcpy(&workspace, buf, bbt_size);

	if (strncmp(workspace.header.signature, "RAWB", 4))
		return -EINVAL;

	if (workspace.header.checksum != bbt_checksum(&workspace, table_size))
		return -EINVAL;

	sort_bbt(&workspace);

	memcpy(out, &workspace, bbt_size);
	return 0;
}

static int try_parse_bmt(struct bmt_table *out, u8 *buf, int len)
{
	static struct bmt_table workspace;

	if (len < sizeof(*out))
		return -EINVAL;

	memcpy(&workspace, buf, sizeof(workspace));

	if (strncmp(workspace.header.signature, "BMT", 3))
		return -EINVAL;

	/*
	 * The vendor firmware checksums over rblocks entries, but zero
	 * values do not affect the checksum so this works.
	 * We don't know rblocks while we're scanning and in any case
	 * it's a moving target, if a block fails in the reserve area,
	 * rblocks will increase by one. So we use the size from the
	 * header and if the vendor firmware left some trash in the
	 * buffer after the last entry, we're going to have an invalid
	 * checksum.
	 */
	if (workspace.header.checksum !=
		bmt_checksum(&workspace, workspace.header.size))
		return -EINVAL;

	memcpy(out, &workspace, sizeof(workspace));
	return 0;
}

/* The BMT is the authority for which reserve blocks are in use. The OOB
 * "mapped" marker is vendor-compatible metadata, but it can be lost or
 * unreadable; relying on it alone lets find_available_block() hand out a
 * spare that is already mapped to another user block, and the next copy
 * overwrites that block's data.
 */
static bool bmt_has_mapping_to(const struct en75_bmt_m *ctx, u16 block)
{
	if (!ctx->bmt.header.version)
		return false;
	for (int i = 0; i < ctx->bmt.header.size; i++)
		if (ctx->bmt.table[i].to == block)
			return true;
	return false;
}

static int r_scan_reserve(struct en75_bmt_m *ctx)
{
	u16 total_blks = ctx->mtk->total_blks;
	int cursor = total_blks - 1;
	int good_blocks = 0;
	int rblock = 0;
	int rblocks_available = 0;
	u8 fdm[4];

	for (; cursor > 0; cursor--) {
		int ret;
		u8 *data_buf = ctx->mtk->data_buf;
		u32 pg_size = ctx->mtk->pg_size;

		if (rblock >= rblocks_available) {
			rblocks_available += cursor;
			ctx->rblocks = krealloc(
				ctx->rblocks,
				rblocks_available * sizeof(*ctx->rblocks),
				GFP_KERNEL);
			if (!ctx->rblocks)
				return -ENOMEM;
		}

		for (int i = 0; i < INITIAL_READ_TRIES; i++) {
			/* 传输失败时 fdm 可能没被填：按擦除态起步，不沿用上一块的值 */
			memset(fdm, 0xff, sizeof(fdm));
			ret = bbt_nand_read(blk_pg(cursor),
					    data_buf,
					   pg_size,
					    fdm, sizeof(fdm));
			if (!ret)
				break;
		}
		struct block_info bif = {
			.index = { .index = cursor },
			.status = BS_INVALID
		};

		if (ret) {
			pr_info("%s: reserve block %d unreadable (%d), spare[0]=%02x\n",
				log_pfx, cursor, ret, fdm[0]);
			bif.status = BS_BAD;
			bif.note = BN_READ;
		} else if (fdm_is_bad(fdm)) {
			pr_info("%s: skipping bad block %d in reserve area\n", log_pfx, cursor);
			bif.status = BS_BAD;
			bif.note = fdm[0] != 0xff ? BN_SPARE0 : BN_SPARE1;
		} else if (fdm_is_mapped(fdm)) {
			pr_debug("%s: found mapped block %d\n", log_pfx, cursor);
			bif.status = BS_MAPPED;
			bif.note = BN_MARKER;
		} else if (!try_parse_bbt(&ctx->bbt, data_buf, pg_size, ctx->bbt_table_size)) {
			pr_info("%s: found BBT in block %d\n", log_pfx, cursor);
			bif.status = BS_BBT;
		} else if (!ctx->bmt.header.version &&
			   !try_parse_bmt(&ctx->bmt, data_buf, pg_size)) {
			pr_info("%s: found BMT in block %d\n", log_pfx, cursor);
			bif.status = BS_BMT;
		} else if (bmt_has_mapping_to(ctx, cursor)) {
			pr_debug("%s: found BMT-mapped block %d\n", log_pfx, cursor);
			bif.status = BS_MAPPED;
			bif.note = BN_BMT_ONLY;
		} else if (block_is_erased(data_buf, pg_size, fdm, sizeof(fdm))) {
			pr_debug("%s: found available block %d\n", log_pfx, cursor);
			bif.status = BS_AVAILABLE;
		} else {
			pr_debug("%s: found block needing erase %d\n", log_pfx, cursor);
			bif.status = BS_NEED_ERASE;
		}

		ctx->rblocks[rblock++] = bif;
		/*
		 * 池大小按原厂口径数：只看 page0 的 spare[0]，不管读状态。原厂内核 calc_bmt_pool_size 在 0x802d3c10、tcboot 在 0x87fd50f0 以 BAD_BLOCK_RAW 调坏块检查，检查函数（0x802d3a34、0x87fd4f24）不看页读的返回值、只比 spare[0]。-EBADMSG 时 spinand 照常把片内缓存读出的字节填进 fdm，与原厂看到的是同一份；传输错误时 fdm 是上面填的 FF。spare[1] 非 FF 或读失败的块照样计入池大小，但仍是 BS_BAD、不分配，与原厂 find_available_block 同口径（GPL bmt.c:625）。
		 */
		good_blocks += (fdm[0] == 0xff);

		if (good_blocks >= REQUIRED_GOOD_BLOCKS(total_blks))
			break;
	}
	if (!cursor) {
		pr_err("%s: not enough valid blocks found, need %d got %d\n",
		       log_pfx, REQUIRED_GOOD_BLOCKS(total_blks), good_blocks);
		return -ENOSPC;
	}
	ctx->reserve_area_begin = cursor;
	return 0;
}

/*
 * BMT 从闪存读到时，page0 带映射标记、但 BMT 不指向它的池块是孤儿：重映射写好新表、擦掉旧表之前掉电留下的搬运副本。记为待擦，pub_init 的 w_sync_tables 在末尾擦掉，所以只有出现孤儿时才写闪存。留着的话，BMT 一旦读不到，tcboot 按标记重建（reconstruct_bmt，GPL bmt.c:820-872）会让它以同一个 from 认领用户块，而里面是掉电那一刻的旧副本；写路径补标记之后，真正的映射块也带同一个 from，两块冲突。原厂第一次分配前同样会擦掉 BMT 里没有的池块（find_available_block，GPL bmt.c:561-598）。
 */
static void r_mark_orphan_spares(struct en75_bmt_m *ctx)
{
	int rblocks = reserve_block_count(ctx);

	for (int i = 0; i < rblocks; i++) {
		struct block_info *b = &ctx->rblocks[i];

		if (b->status != BS_MAPPED || b->note != BN_MARKER ||
		    bmt_has_mapping_to(ctx, b->index.index))
			continue;
		pr_info("%s: spare block %d has a mapping marker but is not in the BMT, erasing\n",
			log_pfx, b->index.index);
		b->status = BS_NEED_ERASE;
		b->note = BN_NONE;
	}
}

static int w_factory_badblocks(struct en75_bmt_m *ctx, const u32 *blocks, int count)
{
	if (WARN_ON_ONCE(!ctx->bbt.header.version))
		return -EINVAL;
	if (WARN_ON_ONCE(!ctx->bmt.header.version))
		return -EINVAL;

	for (int i = 0; i < count; i++) {
		if (blocks[i] >= ctx->reserve_area_begin) {
			pr_err("%s: factory bad block %d not in user area\n",
			       log_pfx, blocks[i]);
			return -EINVAL;
		}
	}

	if (count > ctx->bbt_table_size) {
		pr_err("%s: Can't set %d factory bad blocks, limit is %d\n",
		       log_pfx, count, ctx->bbt_table_size);
		return -ENOSPC;
	}

	/* If ctx->can_write_factory_bbt is not set, this is just an assertion. */
	if (!ctx->can_write_factory_bbt) {
		if (ctx->bbt.header.size != count) {
			pr_err("%s: factory bad block count mismatch %d != %d\n",
			       log_pfx, ctx->bbt.header.size, count);
			return -EIO;
		}
		for (int i = 0; i < count; i++)
			for (int j = 0; j < ctx->bbt.header.size; j++) {
				if (ctx->bbt.table[j] == blocks[i])
					break;
				if (j == ctx->bbt.header.size - 1) {
					pr_err("%s: factory bad block %d not in BBT\n",
					       log_pfx, blocks[i]);
					return -EIO;
				}
			}
		return 0;
	}

	/*
	 * Clear the BBT, and un-bad the blocks that will not be added back.
	 * We have to clear it because if we're adding a block, we can't
	 * properly check if it's mapped in the BMT unless it's removed from
	 * the BBT.
	 */
	for (int i = ctx->bbt.header.size - 1; i >= 0; i--) {
		int j = 0;

		for (; j < count; j++)
			if (ctx->bbt.table[i] == blocks[j])
				break;

		ctx->bbt.header.size--;
		ctx->bbt.table[i] = ctx->bbt.table[ctx->bbt.header.size];
		ctx->bbt.table[ctx->bbt.header.size] = 0;

		/* It's going to be added back so let it stay bad. */
		if (j < count)
			continue;

		/*
		 * Try to erase the bad marker, if it's impossible to erase
		 * then the BMT is going to catch it and map it out later.
		 * If you're running w_factory_badblocks(), then you're
		 * changing offsets so data loss is already guaranteed.
		 */
		w_erase_one(ctx,
			    (struct block_index) { .index = ctx->bbt.table[i] },
			    true);
	}

	/* Unmap any block that is mapped in the BMT */
	for (int i = 0; i < count; i++) {
		int mapped_block;
		int ret;

		mapped_block = get_mapping_block(ctx, blocks[i]);
		if (mapped_block < ctx->reserve_area_begin) {
			pr_info("%s: factory bad block %d not mapped, no unmap needed\n",
				log_pfx, blocks[i]);
			continue; /* not mapped */
		}

		pr_info("%s: unmapping block %d to set as factory bad block\n",
			log_pfx, blocks[i]);
		ret = w_unmap_block(ctx, blocks[i], UE_NO_ERASE);
		if (ret) {
			pr_warn("%s: failed unmapping %d to set as factory bad block\n",
				log_pfx, blocks[i]);
		}
	}

	/* Add everything in the list to the BBT */
	for (int i = 0; i < count; i++) {
		int j = 0;

		for (; j < ctx->bbt.header.size; j++)
			if (ctx->bbt.table[j] == blocks[i])
				break;

		if (j < ctx->bbt.header.size)
			continue;

		ctx->bbt.table[ctx->bbt.header.size++] = blocks[i];

		w_mark_bad(ctx, (struct block_index) { .index = blocks[i] }, true);
	}

	sort_bbt(&ctx->bbt);
	ctx->bbt_dirty++;

	return 0;
}

static int add_block_range(struct en75_bmt_m *ctx, struct block_range **ranges,
			   u16 *count, u16 begin_block, u16 size_blocks,
			   const char *label)
{
	struct block_range *grown;

	if (((int)begin_block) + size_blocks > ctx->reserve_area_begin) {
		pr_err("%s: remap range %d->%d exceeds user area\n",
		       log_pfx, begin_block, size_blocks);
		return -EINVAL;
	}
	grown = krealloc(*ranges, (*count + 1) * sizeof(**ranges), GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	*ranges = grown;
	(*ranges)[(*count)++] = (struct block_range) {
		.begin = begin_block,
		.end = begin_block + size_blocks,
		.label = label,
	};
	return 0;
}

/* partitions 下带 prop 的分区所占的块加进 ranges；起点或长度不按块对齐的分区跳过 */
static void of_add_partition_ranges(struct en75_bmt_m *ctx, struct device_node *np,
				    const char *prop, struct block_range **ranges,
				    u16 *count)
{
	struct device_node *parts_np;

	parts_np = of_get_child_by_name(np, "partitions");
	for_each_child_of_node_scoped(parts_np, part_np) {
		u32 start;
		u32 size;
		const __be32 *reg;
		int ret;

		if (!of_property_read_bool(part_np, prop))
			continue;

		reg = of_get_property(part_np, "reg", NULL);
		if (!reg) {
			pr_warn("%s: can't %s on %pOF, no reg property\n",
				log_pfx, prop, part_np);
			continue;
		}

		start = be32_to_cpup(reg);
		if (start & ((1 << ctx->mtk->blk_shift) - 1)) {
			pr_warn("%s: can't %s on %pOF start not aligned\n",
				log_pfx, prop, part_np);
			continue;
		}
		size = be32_to_cpup(&reg[1]);
		if (size & ((1 << ctx->mtk->blk_shift) - 1)) {
			pr_warn("%s: can't %s on %pOF size not aligned\n",
				log_pfx, prop, part_np);
			continue;
		}
		ret = add_block_range(ctx, ranges, count,
				      start >> ctx->mtk->blk_shift,
				      size >> ctx->mtk->blk_shift,
				      of_get_property(part_np, "label", NULL));
		if (ret)
			pr_warn("%s: failed %s on %pOF: %d\n",
				log_pfx, prop, part_np, ret);
		else
			pr_info("%s: %s set for %pOF\n",
				log_pfx, prop, part_np);
	}
	of_node_put(parts_np);
}

static int w_init(struct en75_bmt_m *ctx, struct device_node *np)
{
	u32 factory_badblocks[MAX_FACTORY_BAD_BLOCKS_OF];
	int factory_badblocks_count = -1;
	int assert_reserve_size = -1;
	bool bmt_on_flash;
	u32 bbt_table_size;
	int ret;

	if (of_property_read_u32(np, name_bbt_table_size, &bbt_table_size))
		bbt_table_size = DEFAULT_BBT_TABLE_SIZE;

	if (bbt_table_size > MAX_BBT_SIZE) {
		pr_err("%s: %s=%d exceeds MAX_BBT_SIZE=%d\n",
		       log_pfx, name_bbt_table_size, bbt_table_size, MAX_BBT_SIZE);
		return -EINVAL;
	}
	ctx->bbt_table_size = bbt_table_size;

	ret = r_scan_reserve(ctx);
	if (ret)
		return ret;
	bmt_on_flash = ctx->bmt.header.version;

	if (!of_property_read_u32(np, name_assert_reserve_size, &assert_reserve_size)) {
		if (assert_reserve_size != reserve_block_count(ctx)) {
			pr_err("%s: reserve area size mismatch %d != %d\n",
			       log_pfx, assert_reserve_size, reserve_block_count(ctx));
			return -EINVAL;
		}
	}

	if (of_property_read_bool(np, name_can_write_factory_bbt))
		ctx->can_write_factory_bbt = 1;

	ret = of_property_read_variable_u32_array(np, name_factory_badblocks,
						  factory_badblocks,
						  0, MAX_FACTORY_BAD_BLOCKS_OF);
	if (ret >= 0)
		factory_badblocks_count = ret;

	if (of_property_read_bool(np, name_enable_remap))
		add_block_range(ctx, &ctx->can_remap_ranges,
				&ctx->can_remap_range_count,
				0, ctx->reserve_area_begin, NULL);
	else
		of_add_partition_ranges(ctx, np, name_enable_remap,
					&ctx->can_remap_ranges,
					&ctx->can_remap_range_count);
	of_add_partition_ranges(ctx, np, name_manual_remap,
				&ctx->manual_remap_ranges,
				&ctx->manual_remap_range_count);

	if (ctx->bbt.header.version) {
		if (!ctx->bmt.header.version) {
			pr_info("%s: BBT found, BMT missing or corrupted\n", log_pfx);
			r_reconstruct_bmt(ctx);
		} else {
			pr_info("%s: BBT & BMT found\n", log_pfx);
		}
	} else if (!ctx->can_write_factory_bbt) {
		pr_err("%s: BBT not found and %s is unset, giving up\n",
		       log_pfx, name_can_write_factory_bbt);
		return -EIO;
	} else if (factory_badblocks_count > -1) {
		pr_info("%s: BBT not found, reconstructing from %s\n",
			log_pfx, name_factory_badblocks);
		if (!ctx->bmt.header.version)
			r_reconstruct_bmt(ctx);
	} else if (ctx->bmt.header.version) {
		pr_info("%s: BBT not found, BMT found, attempting reconstruction\n",
			log_pfx);
		ret = r_reconstruct_bbt(&ctx->bbt, ctx);
		if (ret)
			return ret;
	} else {
		pr_warn("%s: No BBT or BMT found, attempting reconstruction, LIKELY DATA LOSS!\n",
			log_pfx);
		r_reconstruct_bmt(ctx);
		ret = r_reconstruct_bbt(&ctx->bbt, ctx);
		if (ret)
			return ret;
	}

	if (factory_badblocks_count > -1) {
		int ret = w_factory_badblocks(ctx,
					      factory_badblocks,
					      factory_badblocks_count);
		if (ret)
			return ret;
	}

	if (bmt_on_flash)
		r_mark_orphan_spares(ctx);

	pr_info("%s: blocks: total: %d, user: %d, factory_bad: %d, worn: %d reserve: %d\n",
		log_pfx,
		ctx->mtk->total_blks,
		ctx->reserve_area_begin - ctx->bbt.header.size,
		ctx->bbt.header.size,
		ctx->bmt.header.size,
		reserve_block_count(ctx)
	);

	for (int i = 0; i < ctx->bbt.header.size; i++)
		pr_info(" - BBT factory bad block: %d\n", ctx->bbt.table[i]);
	for (int i = 0; i < ctx->bmt.header.size; i++)
		pr_info(" - BMT mapped worn block: %d->%d\n",
			ctx->bmt.table[i].from, ctx->bmt.table[i].to);

	ctx->mtk->mtd->size =
		(ctx->reserve_area_begin - ctx->bbt.header.size) << ctx->mtk->blk_shift;
	pr_info("%s: %u MiB usable space", log_pfx, (u32)ctx->mtk->mtd->size >> 20);

	return 0;
}

/*
 * 显式重映射一个用户块（debugfs en75_remap），调用方持 ctx->lock 且已核过范围。次序按原厂 update_bmt（vmlinux 0x802db174）：先备好新块（搬数据，映射标记随页写入），再写 BMT，最后给坏块打标记。与原厂不同的是新 BMT 写进另一个块、读回核对通过才切换内存里的映射；失败时 BMT 与映射都不变。
 */
static int w_manual_remap(struct en75_bmt_m *ctx, int user_block,
			  struct manual_remap_result *res)
{
	struct block_info *old_spare = NULL;
	struct block_info *old_bmt = NULL;
	struct block_info *new_block;
	struct block_info *bmt_block;
	struct bmt_table *cand;
	bool src_failed;
	int from, cur, idx;
	u8 checksum;

	from = get_mapping_block_bbt(ctx, user_block);
	if (from < 0)
		return -EINVAL;
	cur = get_mapping_block(ctx, user_block);
	res->from = from;
	res->old = cur;

	/*
	 * 旧备用块可能已是 BS_BAD（开机扫描时它的 page0 读不出来），这正是要退役它的情形，照样搬、照样标坏。
	 */
	if (cur != from) {
		old_spare = rblock_by_index(ctx, cur);
		if (WARN_ON_ONCE(!old_spare))
			return -EIO;
	}

	idx = bmt_find_from(ctx, from);
	if (idx < 0 && ctx->bmt.header.size == 0xff) {
		pr_err("%s: BMT full, cannot add more mappings\n", log_pfx);
		return -ENOSPC;
	}

	cand = kmalloc(sizeof(*cand), GFP_KERNEL);
	if (!cand)
		return -ENOMEM;

	new_block = w_alloc_spare(ctx, cur, from, ctx->mtk->blk_size,
				  &res->uncorrectable_pages, &src_failed);
	if (IS_ERR(new_block)) {
		kfree(cand);
		w_erase_pending(ctx);
		return PTR_ERR(new_block);
	}
	res->to = new_block->index.index;

	for (int i = 0; i < reserve_block_count(ctx); i++)
		if (ctx->rblocks[i].status == BS_BMT)
			old_bmt = &ctx->rblocks[i];

	memcpy(cand, &ctx->bmt, sizeof(*cand));
	if (idx < 0) {
		idx = cand->header.size++;
		cand->table[idx].from = from;
	}
	cand->table[idx].to = new_block->index.index;

	bmt_block = w_write_bmt(ctx, cand);
	checksum = cand->header.checksum;
	kfree(cand);
	if (IS_ERR(bmt_block)) {
		new_block->status = BS_NEED_ERASE;
		w_erase_pending(ctx);
		res->to = -1;
		return PTR_ERR(bmt_block);
	}
	res->bmt_block = bmt_block->index.index;

	bmt_publish_entry(ctx, idx, from, new_block->index.index);
	ctx->bmt.header.checksum = checksum;
	new_block->status = BS_MAPPED;
	new_block->note = BN_MARKER;

	/*
	 * 先擦旧表、再退役原块：新表可能比旧表低（自末端往下取空块），两表并存时开机取较高的旧表，旧表指着的原块在旧表擦掉之前必须原样留着。旧表擦不掉（标了坏也不保证读不出）就不退役，原块只是不带标记，映射以新表为准。
	 */
	w_erase_pending(ctx);
	if (old_bmt && old_bmt->status != BS_AVAILABLE) {
		pr_warn("%s: old BMT block %d not erased, leaving block %d unmarked\n",
			log_pfx, old_bmt->index.index, cur);
		res->retire = RETIRE_SKIPPED;
		res->stale_bmt = old_bmt->index.index;
		return 0;
	}
	res->retire = RETIRE_DONE;

	if (old_spare)
		/* 原厂：映射块变坏时在池内标坏、不回收（update_bmt 0x802db570，a1=1） */
		w_mark_bad_reserve(ctx, old_spare);
	else
		/* 原厂：update_bmt 0x802db2a8，a1=0 */
		w_retire_user_block(ctx, from);
	return 0;
}

/*
 * Public functions (only these have direct access to the context)
 */

static struct en75_bmt_m en75_bmt_m;

static int pub_init(struct device_node *np)
{
	int ret;

	ret = w_init(&en75_bmt_m, np);
	if (!ret)
		w_sync_tables(&en75_bmt_m);
	return ret;
}

/*
 * If we return true, mtk_bmt will retry the operation and if it continues
 * to fail, it will call us back 9 more times. If we return false, the user
 * gets an error immediately.
 *
 * mtk_bmt might be calling us because:
 * 1. user tried to read and it failed
 * 2. user tried to read and there was a "concerning" amount of bit errors
 *    in this case, the user does not get an error if we return false.
 * 3. user tried to write or erase and it failed
 *
 */
static bool pub_remap_block(
	u16 user_block,
	u16 mapped_block_idx,
	int copy_len)
{
	int block;
	struct block_info *maybe_mapped_block = NULL;
	int ret;

	if (!block_in_ranges(en75_bmt_m.can_remap_ranges,
			     en75_bmt_m.can_remap_range_count, user_block, NULL))
		return false;

	block = get_mapping_block_bbt(&en75_bmt_m, user_block);
	if (block < 0 || user_block >= en75_bmt_m.reserve_area_begin) {
		pr_info("%s: remap: block %d out of range\n",
			log_pfx, user_block);
		return false;
	}

	mutex_lock(&en75_bmt_m.lock);
	/* 并发的另一次 I/O 已经把它换走了：让 mtk_bmt 按新映射重试 */
	if (get_mapping_block(&en75_bmt_m, user_block) != mapped_block_idx) {
		mutex_unlock(&en75_bmt_m.lock);
		return true;
	}

	if (mapped_block_idx != block) {
		maybe_mapped_block = rblock_by_index(&en75_bmt_m, mapped_block_idx);
		if (WARN_ON_ONCE(!maybe_mapped_block)) {
			mutex_unlock(&en75_bmt_m.lock);
			return false;
		}
	}

	ret = w_remap_block(&en75_bmt_m, block, maybe_mapped_block, copy_len);
	w_sync_tables(&en75_bmt_m);
	mutex_unlock(&en75_bmt_m.lock);

	/*
	 * 只有映射换好了才让 mtk_bmt 重试；其余失败直接报给调用方，免得外层每次重试都再烧掉几个池块（mtk_bmt.c 的读、写、擦都最多重试 10 次）。
	 */
	return ret == 0;
}

/*
 * 只从 debugfs 的 mark_good、repair 进来，一律拒绝。原厂从不删 BMT 表项；repair（mtk_bmt.c mtk_bmt_debug_repair）撤映射之后从旧备用块拷回原块，而 en75 撤映射会把旧备用块擦掉（记待擦、写表后擦），拷回的是空块，en75 又分不出调用方是 mark_good 还是 repair。退役块用 en75_remap。
 */
static void pub_unmap_block(u16 user_block)
{
	pr_err("%s: unmap of block %d refused: en75_bmt does not unmap, retire blocks with en75_remap\n",
	       log_pfx, user_block);
}

static int pub_debug(void *data, u64 val)
{
	return 0;
}

/* debugfs en75_remap：val 是 MTD 内按擦除块对齐的字节偏移，与 mark_bad 等同一口径 */
static int pub_debugfs_remap(void *data, u64 val)
{
	struct manual_remap_result res = { .to = -1, .bmt_block = -1, .stale_bmt = -1 };
	u32 blk_mask = en75_bmt_m.mtk->blk_size - 1;
	u64 user_blocks = en75_bmt_m.mtk->mtd->size >> en75_bmt_m.mtk->blk_shift;
	const char *label = NULL;
	int ret;

	if (val & blk_mask) {
		pr_err("%s: en75_remap: 0x%llx is not an erase-block aligned byte offset\n",
		       log_pfx, val);
		return -EINVAL;
	}
	val >>= en75_bmt_m.mtk->blk_shift;
	if (val >= user_blocks) {
		pr_err("%s: en75_remap: block %llu beyond user area (%llu blocks)\n",
		       log_pfx, val, user_blocks);
		return -EINVAL;
	}
	if (!block_in_ranges(en75_bmt_m.manual_remap_ranges,
			     en75_bmt_m.manual_remap_range_count, val, &label)) {
		pr_err("%s: en75_remap: block %llu is not in an %s partition, refused\n",
		       log_pfx, val, name_manual_remap);
		return -EPERM;
	}
	res.user_block = val;

	mutex_lock(&en75_bmt_m.lock);
	ret = w_manual_remap(&en75_bmt_m, val, &res);
	res.ret = ret;
	res.seq = en75_bmt_m.last_remap.seq + 1;
	en75_bmt_m.last_remap = res;
	mutex_unlock(&en75_bmt_m.lock);

	if (ret)
		pr_err("%s: en75_remap: block %d (%s) failed: %d, BMT unchanged\n",
		       log_pfx, res.user_block, label ?: "?", ret);
	else
		pr_info("%s: en75_remap: block %d (%s) %d -> %d, BMT written to block %d, %d uncorrectable pages copied\n",
			log_pfx, res.user_block, label ?: "?", res.old, res.to,
			res.bmt_block, res.uncorrectable_pages);
	return ret;
}

static const char *const block_note_names[] = {
	[BN_NONE] = "-",
	[BN_SPARE0] = "spare0",
	[BN_SPARE1] = "spare1",
	[BN_READ] = "read",
	[BN_MARKER] = "marker",
	[BN_BMT_ONLY] = "bmt_only",
};

/*
 * debugfs en75_state：每行“键 值…”，修复工具按键名取值、不依赖行的次序；manual_remap/auto_remap 的范围是 [起, 止)，止不含在内。
 */
static int en75_state_show(struct seq_file *s, void *unused)
{
	struct en75_bmt_m *ctx = &en75_bmt_m;
	const struct manual_remap_result *r = &ctx->last_remap;
	struct block_info *next;
	int rblocks;
	int available = 0;

	mutex_lock(&ctx->lock);
	rblocks = reserve_block_count(ctx);
	seq_printf(s, "total_blocks %d\n", ctx->mtk->total_blks);
	seq_printf(s, "required_good %d\n", REQUIRED_GOOD_BLOCKS(ctx->mtk->total_blks));
	seq_printf(s, "reserve_begin %d\n", ctx->reserve_area_begin);
	seq_printf(s, "reserve_blocks %d\n", rblocks);
	seq_printf(s, "user_blocks %d\n", ctx->reserve_area_begin - ctx->bbt.header.size);
	for (int i = 0; i < ctx->bbt.header.size; i++)
		seq_printf(s, "bbt %d\n", ctx->bbt.table[i]);
	seq_printf(s, "bmt_entries %d\n", ctx->bmt.header.size);
	for (int i = 0; i < ctx->bmt.header.size; i++)
		seq_printf(s, "map %d %d %d\n",
			   get_user_block(ctx, ctx->bmt.table[i].from),
			   ctx->bmt.table[i].from, ctx->bmt.table[i].to);
	for (int i = 0; i < rblocks; i++) {
		const struct block_info *b = &ctx->rblocks[i];

		switch (b->status) {
		case BS_BBT:
			seq_printf(s, "bbt_block %d\n", b->index.index);
			break;
		case BS_BMT:
			seq_printf(s, "bmt_block %d\n", b->index.index);
			break;
		case BS_BAD:
			seq_printf(s, "pool_bad %d %s\n", b->index.index,
				   block_note_names[b->note]);
			break;
		case BS_MAPPED:
			seq_printf(s, "pool_mapped %d %s\n", b->index.index,
				   block_note_names[b->note]);
			break;
		case BS_AVAILABLE:
			available++;
			break;
		default:
			break;
		}
	}
	seq_printf(s, "pool_available %d\n", available);
	next = find_available_block(ctx, false);
	seq_printf(s, "next_spare %d\n", IS_ERR(next) ? -1 : next->index.index);
	next = find_available_block(ctx, true);
	seq_printf(s, "next_bmt_block %d\n", IS_ERR(next) ? -1 : next->index.index);
	for (int i = 0; i < ctx->manual_remap_range_count; i++)
		seq_printf(s, "manual_remap %d %d %s\n", ctx->manual_remap_ranges[i].begin,
			   ctx->manual_remap_ranges[i].end,
			   ctx->manual_remap_ranges[i].label ?: "-");
	for (int i = 0; i < ctx->can_remap_range_count; i++)
		seq_printf(s, "auto_remap %d %d %s\n", ctx->can_remap_ranges[i].begin,
			   ctx->can_remap_ranges[i].end,
			   ctx->can_remap_ranges[i].label ?: "-");
	seq_printf(s, "last_remap %d %d %d %d %d %d %d %d\n", r->seq, r->ret,
		   r->user_block, r->from, r->old, r->to, r->bmt_block,
		   r->uncorrectable_pages);
	if (r->retire == RETIRE_SKIPPED)
		seq_printf(s, "last_retire %d skipped %d\n", r->seq, r->stale_bmt);
	else
		seq_printf(s, "last_retire %d %s\n", r->seq,
			   r->retire == RETIRE_DONE ? "done" : "-");
	mutex_unlock(&ctx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en75_state);

DEFINE_DEBUGFS_ATTRIBUTE(fops_en75_remap, NULL, pub_debugfs_remap, "%llu\n");

static void pub_add_debugfs(struct dentry *dir)
{
	debugfs_create_file_unsafe("en75_remap", S_IWUSR, dir, NULL, &fops_en75_remap);
	debugfs_create_file("en75_state", S_IRUSR, dir, NULL, &en75_state_fops);
}

static int pub_get_mapping_block(int user_block)
{
	return get_mapping_block(&en75_bmt_m, user_block);
}

#undef bmtd
static struct en75_bmt_m en75_bmt_m = {
	.mtk = &bmtd,
	.lock = __MUTEX_INITIALIZER(en75_bmt_m.lock),
};

const struct mtk_bmt_ops en75_bmt_ops = {
	.init = pub_init,
	.remap_block = pub_remap_block,
	.unmap_block = pub_unmap_block,
	.get_mapping_block = pub_get_mapping_block,
	.debug = pub_debug,
	.add_debugfs = pub_add_debugfs,
};
