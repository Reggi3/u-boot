/*
 * S3C64XX/S5PC100 OneNAND driver at U-Boot
 *
 * Copyright (C) 2008-2009 Samsung Electronics
 * Kyungmin Park <kyungmin.park@samsung.com>
 *
 * Implementation:
 *	Emulate the pseudo BufferRAM
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <malloc.h>
#include <linux/compat.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/onenand.h>
#include <linux/mtd/samsung_onenand.h>

#include <asm/io.h>
#include <asm/errno.h>

#ifdef ONENAND_DEBUG
#define DPRINTK(format, args...)					\
do {									\
	printf("%s[%d]: " format "\n", __func__, __LINE__, ##args);	\
} while (0)
#else
#define DPRINTK(...)			do { } while (0)
#endif

#define ONENAND_ERASE_STATUS		0x00
#define ONENAND_MULTI_ERASE_SET		0x01
#define ONENAND_ERASE_START		0x03
#define ONENAND_UNLOCK_START		0x08
#define ONENAND_UNLOCK_END		0x09
#define ONENAND_LOCK_START		0x0A
#define ONENAND_LOCK_END		0x0B
#define ONENAND_LOCK_TIGHT_START	0x0C
#define ONENAND_LOCK_TIGHT_END		0x0D
#define ONENAND_UNLOCK_ALL		0x0E
#define ONENAND_OTP_ACCESS		0x12
#define ONENAND_SPARE_ACCESS_ONLY	0x13
#define ONENAND_MAIN_ACCESS_ONLY	0x14
#define ONENAND_ERASE_VERIFY		0x15
#define ONENAND_MAIN_SPARE_ACCESS	0x16
#define ONENAND_PIPELINE_READ		0x4000

#if defined(CONFIG_S3C64XX)
#define MAP_00				(0x0 << 24)
#define MAP_01				(0x1 << 24)
#define MAP_10				(0x2 << 24)
#define MAP_11				(0x3 << 24)
#elif defined(CONFIG_S5P)
#define MAP_00				(0x0 << 26)
#define MAP_01				(0x1 << 26)
#define MAP_10				(0x2 << 26)
#define MAP_11				(0x3 << 26)
#endif

/* read/write of XIP buffer */
#define CMD_MAP_00(mem_addr)		(MAP_00 | ((mem_addr) << 1))
/* read/write to the memory device */
#define CMD_MAP_01(mem_addr)		(MAP_01 | (mem_addr))
/* control special functions of the memory device */
#define CMD_MAP_10(mem_addr)		(MAP_10 | (mem_addr))
/* direct interface(direct access) with the memory device */
#define CMD_MAP_11(mem_addr)		(MAP_11 | ((mem_addr) << 2))

struct s3c_onenand {
	struct mtd_info	*mtd;
	void __iomem	*base;
	void __iomem	*ahb_addr;
	int		bootram_command;
	void __iomem	*page_buf;
	void __iomem	*oob_buf;
	unsigned int	(*mem_addr)(int fba, int fpa, int fsa);
	struct samsung_onenand *reg;
};

static struct s3c_onenand *onenand;

static int s3c_read_cmd(unsigned int cmd)
{
	return readl(onenand->ahb_addr + cmd);
}

static void s3c_write_cmd(int value, unsigned int cmd)
{
	writel(value, onenand->ahb_addr + cmd);
}

/*
 * MEM_ADDR
 *
 * fba: flash block address
 * fpa: flash page address
 * fsa: flash sector address
 *
 * return the buffer address on the memory device
 * It will be combined with CMD_MAP_XX
 */
#if defined(CONFIG_S3C64XX)
static unsigned int s3c_mem_addr(int fba, int fpa, int fsa)
{
	return (fba << 12) | (fpa << 6) | (fsa << 4);
}
#elif defined(CONFIG_S5P)
static unsigned int s3c_mem_addr(int fba, int fpa, int fsa)
{
	return (fba << 13) | (fpa << 7) | (fsa << 5);
}
#endif

static void s3c_onenand_reset(void)
{
	unsigned long timeout = 0x10000;
	int stat;

	writel(ONENAND_MEM_RESET_COLD, &onenand->reg->mem_reset);
	while (timeout--) {
		stat = readl(&onenand->reg->int_err_stat);
		if (stat & RST_CMP)
			break;
	}
	stat = readl(&onenand->reg->int_err_stat);
	writel(stat, &onenand->reg->int_err_ack);

	/* Clear interrupt */
	writel(0x0, &onenand->reg->int_err_ack);
	/* Clear the ECC status */
	writel(0x0, &onenand->reg->ecc_err_stat);
}

static unsigned short s3c_onenand_readw(void __iomem *addr)
{
	struct onenand_chip *this = onenand->mtd->priv;
	int reg = addr - this->base;
	int word_addr = reg >> 1;
	int value;

	/* It's used for probing time */
	switch (reg) {
	case ONENAND_REG_MANUFACTURER_ID:
		return readl(&onenand->reg->manufact_id);
	case ONENAND_REG_DEVICE_ID:
		return readl(&onenand->reg->device_id);
	case ONENAND_REG_VERSION_ID:
		return readl(&onenand->reg->flash_ver_id);
	case ONENAND_REG_DATA_BUFFER_SIZE:
		return readl(&onenand->reg->data_buf_size);
	case ONENAND_REG_TECHNOLOGY:
		return readl(&onenand->reg->tech);
	case ONENAND_REG_SYS_CFG1:
		return readl(&onenand->reg->mem_cfg);

	/* Used at unlock all status */
	case ONENAND_REG_CTRL_STATUS:
		return 0;

	case ONENAND_REG_WP_STATUS:
		return ONENAND_WP_US;

	default:
		break;
	}

	/* BootRAM access control */
	if (reg < ONENAND_DATARAM && onenand->bootram_command) {
		if (word_addr == 0)
			return readl(&onenand->reg->manufact_id);
		if (word_addr == 1)
			return readl(&onenand->reg->device_id);
		if (word_addr == 2)
			return readl(&onenand->reg->flash_ver_id);
	}

	value = s3c_read_cmd(CMD_MAP_11(word_addr)) & 0xffff;
	printk(KERN_INFO "s3c_onenand_readw:  Illegal access"
		" at reg 0x%x, value 0x%x\n", word_addr, value);
	return value;
}

static void s3c_onenand_writew(unsigned short value, void __iomem *addr)
{
	struct onenand_chip *this = onenand->mtd->priv;
	int reg = addr - this->base;
	int word_addr = reg >> 1;

	/* It's used for probing time */
	switch (reg) {
	case ONENAND_REG_SYS_CFG1:
		/*
		 * This line was originally here,
		 * but it seems to hang the controller.
		 *
		 * writel(value, &onenand->reg->mem_cfg);
		 */
		return;

	case ONENAND_REG_START_ADDRESS1:
	case ONENAND_REG_START_ADDRESS2:
		return;

	/* Lock/lock-tight/unlock/unlock_all */
	case ONENAND_REG_START_BLOCK_ADDRESS:
		return;

	default:
		break;
	}

	/* BootRAM access control */
	if (reg < ONENAND_DATARAM) {
		if (value == ONENAND_CMD_READID) {
			onenand->bootram_command = 1;
			return;
		}
		if (value == ONENAND_CMD_RESET) {
			writel(ONENAND_MEM_RESET_COLD,
					&onenand->reg->mem_reset);
			onenand->bootram_command = 0;
			return;
		}
	}

	printk(KERN_INFO "s3c_onenand_writew: Illegal access"
		" at reg 0x%x, value 0x%x\n", word_addr, value);

	s3c_write_cmd(value, CMD_MAP_11(word_addr));
}

static inline unsigned long msecs_to_ticks(unsigned long msecs)
{
	return (msecs * get_tbclk()) / 1000;
}

static int s3c_onenand_wait(struct mtd_info *mtd, int state)
{
	unsigned int flags = INT_ACT;
	unsigned int stat, ecc;
	unsigned long timeout;
	unsigned long start;

	switch (state) {
	case FL_READING:
		flags |= BLK_RW_CMP | LOAD_CMP;
		break;
	case FL_WRITING:
		flags |= BLK_RW_CMP | PGM_CMP;
		break;
	case FL_ERASING:
		flags |= BLK_RW_CMP | ERS_CMP;
		break;
	case FL_LOCKING:
		flags |= BLK_RW_CMP;
		break;
	default:
		break;
	}

	/* The 20 msec is enough */
	start = get_timer(0);
	timeout = msecs_to_ticks(20);
	while (get_timer(start) < timeout) {
		stat = readl(&onenand->reg->int_err_stat);
		if (stat & flags)
			break;
	}

	/* To get correct interrupt status in timeout case */
	stat = readl(&onenand->reg->int_err_stat);
	writel(stat, &onenand->reg->int_err_ack);

	/*
	 * In the Spec. it checks the controller status first
	 * However if you get the correct information in case of
	 * power off recovery (POR) test, it should read ECC status first
	 */
	if (stat & LOAD_CMP) {
		ecc = readl(&onenand->reg->ecc_err_stat);
		if (ecc & ONENAND_ECC_4BIT_UNCORRECTABLE) {
			printk(KERN_INFO "%s: ECC error = 0x%04x\n",
					__func__, ecc);
			mtd->ecc_stats.failed++;
			return -EBADMSG;
		}
	}

	if (stat & (LOCKED_BLK | ERS_FAIL | PGM_FAIL | LD_FAIL_ECC_ERR)) {
		printk(KERN_INFO "%s: controller error = 0x%04x\n",
				__func__, stat);
		if (stat & LOCKED_BLK)
			printk(KERN_INFO "%s: it's locked error = 0x%04x\n",
					__func__, stat);

		return -EIO;
	}

	return 0;
}

#ifdef CONFIG_SAMSUNG_ONENAND_BURST_READ
static inline void s3c_onenand_read(struct s3c_onenand *onenand,
				unsigned int addr, int count, unsigned int *buf)
{
	__asm__ (
		"1:\n"
		"\tldmia %1, {r0-r7}\n"
		"\tstmia %2!, {r0-r7}\n"
		"\tsubs %0, #1\n"
		"\tbne 1b\n"
		:
		: "r"(count / 8),
			"r"(onenand->ahb_addr + addr), "r"(buf)
		: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
	);
}
#else
static inline void s3c_onenand_read(struct s3c_onenand *onenand,
				unsigned int addr, int count, unsigned int *buf)
{
	int i;
	for (i = 0; i < count; i++)
		*buf++ = s3c_read_cmd(addr);
}
#endif

#ifdef CONFIG_SAMSUNG_ONENAND_BURST_WRITE
static inline void s3c_onenand_write(struct s3c_onenand *onenand,
				unsigned int addr, int count, unsigned int *buf)
{
	__asm__ (
		"1:\n"
		"\tldmia %1!, {r0-r7}\n"
		"\tstmia %2, {r0-r7}\n"
		"\tsubs %0, #1\n"
		"\tbne 1b\n"
		:
		: "r"(count / 8),
			"r"(buf), "r"(onenand->ahb_addr + addr)
		: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
	);
}

static inline void s3c_onenand_dummy_write(struct s3c_onenand *onenand,
						unsigned int addr, int count)
{
	__asm__ (
		"\tmvn r0, #0\n"
		"\tmvn r1, #0\n"
		"\tmvn r2, #0\n"
		"\tmvn r3, #0\n"
		"\tmvn r4, #0\n"
		"\tmvn r5, #0\n"
		"\tmvn r6, #0\n"
		"\tmvn r7, #0\n"
		"1:\n"
		"\tstmia %1, {r0-r7}\n"
		"\tsubs %0, #1\n"
		"\tbne 1b\n"
		:
		: "r"(count / 8), "r"(onenand->ahb_addr + addr)
		: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
	);
}
#else
static inline void s3c_onenand_write(struct s3c_onenand *onenand,
				unsigned int addr, int count, unsigned int *buf)
{
	int i;
	for (i = 0; i < count; i++)
		s3c_write_cmd(*buf++, addr);
}

static inline void s3c_onenand_dummy_write(struct s3c_onenand *onenand,
						unsigned int addr, int count)
{
	int i;
	for (i = 0; i < count; i++)
		s3c_write_cmd(0xffffffff, addr);
}
#endif

static int s3c_onenand_command(struct mtd_info *mtd, int cmd,
		loff_t addr, size_t len)
{
	struct onenand_chip *this = mtd->priv;
	unsigned int *m, *s;
	int fba, fpa, fsa = 0;
	unsigned int mem_addr, cmd_map_01, cmd_map_10;
	int i, mcount, scount;
	int index;

	fba = (int) (addr >> this->erase_shift);
	fpa = (int) (addr >> this->page_shift);
	fpa &= this->page_mask;

	mem_addr = onenand->mem_addr(fba, fpa, fsa);
	cmd_map_01 = CMD_MAP_01(mem_addr);
	cmd_map_10 = CMD_MAP_10(mem_addr);

	switch (cmd) {
	case ONENAND_CMD_READ:
	case ONENAND_CMD_READOOB:
	case ONENAND_CMD_BUFFERRAM:
		ONENAND_SET_NEXT_BUFFERRAM(this);
	default:
		break;
	}

	index = ONENAND_CURRENT_BUFFERRAM(this);

	/*
	 * Emulate Two BufferRAMs and access with 4 bytes pointer
	 */
	m = (unsigned int *) onenand->page_buf;
	s = (unsigned int *) onenand->oob_buf;

	if (index) {
		m += (this->writesize >> 2);
		s += (mtd->oobsize >> 2);
	}

	mcount = mtd->writesize >> 2;
	scount = mtd->oobsize >> 2;

	switch (cmd) {
	case ONENAND_CMD_READ:
		/* Main */
		s3c_onenand_read(onenand, cmd_map_01, mcount, m);
		return 0;

	case ONENAND_CMD_READOOB:
		writel(TSRF, &onenand->reg->trans_spare);

		/* Main */
		s3c_onenand_read(onenand, cmd_map_01, mcount, m);
		/* Spare */
		s3c_onenand_read(onenand, cmd_map_01, scount, s);

		writel(0, &onenand->reg->trans_spare);
		return 0;

	case ONENAND_CMD_PROG:
		/* Main */
		s3c_onenand_write(onenand, cmd_map_01, mcount, m);
		return 0;

	case ONENAND_CMD_PROGOOB:
		writel(TSRF, &onenand->reg->trans_spare);

		/* Main - dummy write */
		s3c_onenand_dummy_write(onenand, cmd_map_01, mcount);
		/* Spare */
		s3c_onenand_write(onenand, cmd_map_01, scount, s);

		writel(0, &onenand->reg->trans_spare);
		return 0;

	case ONENAND_CMD_UNLOCK_ALL:
		s3c_write_cmd(ONENAND_UNLOCK_ALL, CMD_MAP_10(mem_addr));
		return 0;

	case ONENAND_CMD_ERASE:
		s3c_write_cmd(ONENAND_ERASE_START, CMD_MAP_10(mem_addr));
		return 0;

	case ONENAND_CMD_MULTIBLOCK_ERASE:
		s3c_write_cmd(ONENAND_MULTI_ERASE_SET, CMD_MAP_10(mem_addr));
		return 0;

	case ONENAND_CMD_ERASE_VERIFY:
		s3c_write_cmd(ONENAND_ERASE_VERIFY, CMD_MAP_10(mem_addr));
		return 0;

	default:
		break;
	}

	return 0;
}

static unsigned char *s3c_get_bufferram(struct mtd_info *mtd, int area)
{
	struct onenand_chip *this = mtd->priv;
	int index = ONENAND_CURRENT_BUFFERRAM(this);
	unsigned char *p;

	if (area == ONENAND_DATARAM) {
		p = (unsigned char *) onenand->page_buf;
		if (index == 1)
			p += this->writesize;
	} else {
		p = (unsigned char *) onenand->oob_buf;
		if (index == 1)
			p += mtd->oobsize;
	}

	return p;
}

static int onenand_read_bufferram(struct mtd_info *mtd, int area,
				  unsigned char *buffer, int offset,
				  size_t count)
{
	unsigned char *p;

	p = s3c_get_bufferram(mtd, area);
	memcpy(buffer, p + offset, count);
	return 0;
}

static int onenand_write_bufferram(struct mtd_info *mtd, int area,
				   const unsigned char *buffer, int offset,
				   size_t count)
{
	unsigned char *p;

	p = s3c_get_bufferram(mtd, area);
	memcpy(p + offset, buffer, count);
	return 0;
}

static int s3c_onenand_bbt_wait(struct mtd_info *mtd, int state)
{
	struct samsung_onenand *reg = (struct samsung_onenand *)onenand->base;
	unsigned int flags = INT_ACT | LOAD_CMP;
	unsigned int stat;
	unsigned long timeout;
	unsigned long start;

	/* The 20 msec is enough */
	start = get_timer(0);
	timeout = msecs_to_ticks(20);
	while (get_timer(start) < timeout) {
		stat = readl(&reg->int_err_stat);
		if (stat & flags)
			break;
	}
	/* To get correct interrupt status in timeout case */
	stat = readl(&onenand->reg->int_err_stat);
	writel(stat, &onenand->reg->int_err_ack);

	if (stat & LD_FAIL_ECC_ERR) {
		s3c_onenand_reset();
		return ONENAND_BBT_READ_ERROR;
	}

	if (stat & LOAD_CMP) {
		int ecc = readl(&onenand->reg->ecc_err_stat);
		if (ecc & ONENAND_ECC_4BIT_UNCORRECTABLE) {
			s3c_onenand_reset();
			return ONENAND_BBT_READ_ERROR;
		}
	}

	return 0;
}

static void s3c_onenand_check_lock_status(struct mtd_info *mtd)
{
	struct onenand_chip *this = mtd->priv;
	unsigned int block, end;

	end = this->chipsize >> this->erase_shift;

	for (block = 0; block < end; block++) {
		s3c_read_cmd(CMD_MAP_01(onenand->mem_addr(block, 0, 0)));

		if (readl(&onenand->reg->int_err_stat) & LOCKED_BLK) {
			printf("block %d is write-protected!\n", block);
			writel(LOCKED_BLK, &onenand->reg->int_err_ack);
		}
	}
}

static void s3c_onenand_do_lock_cmd(struct mtd_info *mtd, loff_t ofs,
		size_t len, int cmd)
{
	struct onenand_chip *this = mtd->priv;
	int start, end, start_mem_addr, end_mem_addr;

	start = ofs >> this->erase_shift;
	start_mem_addr = onenand->mem_addr(start, 0, 0);
	end = start + (len >> this->erase_shift) - 1;
	end_mem_addr = onenand->mem_addr(end, 0, 0);

	if (cmd == ONENAND_CMD_LOCK) {
		s3c_write_cmd(ONENAND_LOCK_START, CMD_MAP_10(start_mem_addr));
		s3c_write_cmd(ONENAND_LOCK_END, CMD_MAP_10(end_mem_addr));
	} else {
		s3c_write_cmd(ONENAND_UNLOCK_START, CMD_MAP_10(start_mem_addr));
		s3c_write_cmd(ONENAND_UNLOCK_END, CMD_MAP_10(end_mem_addr));
	}

	this->wait(mtd, FL_LOCKING);
}

static void s3c_onenand_unlock_all(struct mtd_info *mtd)
{
	struct onenand_chip *this = mtd->priv;
	loff_t ofs = 0;
	size_t len = this->chipsize;

	/* FIXME workaround */
	this->subpagesize = mtd->writesize;
	mtd->subpage_sft = 0;

	if (this->options & ONENAND_HAS_UNLOCK_ALL) {
		/* Write unlock command */
		this->command(mtd, ONENAND_CMD_UNLOCK_ALL, 0, 0);

		/* No need to check return value */
		this->wait(mtd, FL_LOCKING);

		/* Workaround for all block unlock in DDP */
		if (!ONENAND_IS_DDP(this)) {
			s3c_onenand_check_lock_status(mtd);
			return;
		}

		/* All blocks on another chip */
		ofs = this->chipsize >> 1;
		len = this->chipsize >> 1;
	}

	s3c_onenand_do_lock_cmd(mtd, ofs, len, ONENAND_CMD_UNLOCK);
	s3c_onenand_check_lock_status(mtd);
}


int s5pc110_chip_probe(struct mtd_info *mtd)
{
	return 0;
}

int s5pc210_chip_probe(struct mtd_info *mtd)
{
	return 0;
}
void s3c_onenand_init(struct mtd_info *mtd)
{
	struct onenand_chip *this = mtd->priv;
	u32 size = (4 << 10);	/* 4 KiB */

	onenand = malloc(sizeof(struct s3c_onenand));
	if (!onenand)
		return;

	onenand->page_buf = malloc(size * sizeof(char));
	if (!onenand->page_buf)
		return;
	memset(onenand->page_buf, 0xff, size);

	onenand->oob_buf = malloc(128 * sizeof(char));
	if (!onenand->oob_buf)
		return;
	memset(onenand->oob_buf, 0xff, 128);

	onenand->mtd = mtd;

#if defined(CONFIG_S3C64XX)
	onenand->base = (void *)0x70100000;
	onenand->ahb_addr = (void *)0x20000000;
#elif defined(CONFIG_S5P)
	onenand->base = (void *)0xE7100000;
	onenand->ahb_addr = (void *)0xB0000000;
#endif
	onenand->mem_addr = s3c_mem_addr;
	onenand->reg = (struct samsung_onenand *)onenand->base;

	this->read_word = s3c_onenand_readw;
	this->write_word = s3c_onenand_writew;

	this->wait = s3c_onenand_wait;
	this->bbt_wait = s3c_onenand_bbt_wait;
	this->unlock_all = s3c_onenand_unlock_all;
	this->command = s3c_onenand_command;

	this->read_bufferram = onenand_read_bufferram;
	this->write_bufferram = onenand_write_bufferram;
}
