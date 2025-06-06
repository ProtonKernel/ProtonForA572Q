
/* Copyright (c) 2017,2020,2021 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt) "Minidump: " fmt

#include <linux/init.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <soc/qcom/minidump.h>
#include "minidump_private.h"

#define MAX_NUM_ENTRIES         (CONFIG_MINIDUMP_MAX_ENTRIES + 1)
#define MAX_STRTBL_SIZE		(MAX_NUM_ENTRIES * MAX_REGION_NAME_LENGTH)

/**
 * md_table : Local Minidump toc holder
 * @num_regions : Number of regions requested
 * @md_ss_toc  : HLOS toc pointer
 * @md_gbl_toc : Global toc pointer
 * @md_regions : HLOS regions base pointer
 * @entry : array of HLOS regions requested
 */
struct md_table {
	u32			revision;
	u32                     num_regions;
	struct md_ss_toc	*md_ss_toc;
	struct md_global_toc	*md_gbl_toc;
	struct md_ss_region	*md_regions;
	struct md_region        entry[MAX_NUM_ENTRIES];
};

/**
 * md_elfhdr: Minidump table elf header
 * @ehdr: elf main header
 * @shdr: Section header
 * @phdr: Program header
 * @elf_offset: section offset in elf
 * @strtable_idx: string table current index position
 */
struct md_elfhdr {
	struct elfhdr		*ehdr;
	struct elf_shdr		*shdr;
	struct elf_phdr		*phdr;
	u64			elf_offset;
	u64			strtable_idx;
};

/* Protect elfheader and smem table from deferred calls contention */
static DEFINE_SPINLOCK(mdt_lock);
static struct md_table		minidump_table;
static struct md_elfhdr		minidump_elfheader;

/* Number of pending entries to be added in ToC regions */
static unsigned int pendings;

static inline char *elf_lookup_string(struct elfhdr *hdr, int offset)
{
	char *strtab = elf_str_table(hdr);

	if ((strtab == NULL) || (minidump_elfheader.strtable_idx < offset))
		return NULL;
	return strtab + offset;
}

static inline unsigned int set_section_name(const char *name)
{
	char *strtab = elf_str_table(minidump_elfheader.ehdr);
	int idx = minidump_elfheader.strtable_idx;
	int ret = 0;

	if ((strtab == NULL) || (name == NULL))
		return 0;

	ret = idx;
	idx += strlcpy((strtab + idx), name, MAX_REGION_NAME_LENGTH);
	minidump_elfheader.strtable_idx = idx + 1;

	return ret;
}

static inline bool md_check_name(const char *name)
{
	struct md_region *mde = minidump_table.entry;
	int i, regno = minidump_table.num_regions;

	for (i = 0; i < regno; i++, mde++)
		if (!strcmp(mde->name, name))
			return true;
	return false;
}

/* Return next seq no, if name already exists in the table */
static inline int md_get_seq_num(const char *name)
{
	struct md_ss_region *mde = minidump_table.md_regions;
	int i, regno = minidump_table.md_ss_toc->ss_region_count;
	int seqno = 0;

	for (i = 0; i < (regno - 1); i++, mde++) {
		if (!strcmp(mde->name, name)) {
			if (mde->seq_num >= seqno)
				seqno = mde->seq_num + 1;
		}
	}
	return seqno;
}

/* Update Mini dump table in SMEM */
static void md_update_ss_toc(const struct md_region *entry)
{
	struct md_ss_region *mdr;
	struct elfhdr *hdr = minidump_elfheader.ehdr;
	struct elf_shdr *shdr = elf_section(hdr, hdr->e_shnum++);
	struct elf_phdr *phdr = elf_program(hdr, hdr->e_phnum++);
	int reg_cnt = minidump_table.md_ss_toc->ss_region_count++;

	mdr = &minidump_table.md_regions[reg_cnt];

	strlcpy(mdr->name, entry->name, sizeof(mdr->name));
	mdr->region_base_address = entry->phys_addr;
	mdr->region_size = entry->size;
	mdr->seq_num = md_get_seq_num(entry->name);

	/* Update elf header */
	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_name = set_section_name(mdr->name);
	shdr->sh_addr = (elf_addr_t)entry->virt_addr;
	shdr->sh_size = mdr->region_size;
	shdr->sh_flags = SHF_WRITE;
	shdr->sh_offset = minidump_elfheader.elf_offset;
	shdr->sh_entsize = 0;

	phdr->p_type = PT_LOAD;
	phdr->p_offset = minidump_elfheader.elf_offset;
	phdr->p_vaddr = entry->virt_addr;
	phdr->p_paddr = entry->phys_addr;
	phdr->p_filesz = phdr->p_memsz =  mdr->region_size;
	phdr->p_flags = PF_R | PF_W;

	minidump_elfheader.elf_offset += shdr->sh_size;
	mdr->md_valid = MD_REGION_VALID;
}

bool msm_minidump_enabled(void)
{
	bool ret = false;

	spin_lock(&mdt_lock);
	if (minidump_table.md_ss_toc &&
		(minidump_table.md_ss_toc->md_ss_enable_status ==
		 MD_SS_ENABLED))
		ret = true;
	spin_unlock(&mdt_lock);
	return ret;
}
EXPORT_SYMBOL(msm_minidump_enabled);

int msm_minidump_add_region(const struct md_region *entry)
{
	u32 entries;
	struct md_region *mdr;
	int ret = 0;

	if (!entry)
		return -EINVAL;

	if ((strlen(entry->name) > MAX_NAME_LENGTH) ||
		md_check_name(entry->name) || !entry->virt_addr) {
		pr_err("Invalid entry details\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(entry->size, 4)) {
		pr_err("size should be 4 byte aligned\n");
		return -EINVAL;
	}

	spin_lock(&mdt_lock);
	entries = minidump_table.num_regions;
	if (entries >= MAX_NUM_ENTRIES) {
		pr_err("Maximum entries reached.\n");
		spin_unlock(&mdt_lock);
		return -ENOMEM;
	}

	mdr = &minidump_table.entry[entries];
	strlcpy(mdr->name, entry->name, sizeof(mdr->name));
	mdr->virt_addr = entry->virt_addr;
	mdr->phys_addr = entry->phys_addr;
	mdr->size = entry->size;
	mdr->id = entry->id;

	minidump_table.num_regions = entries + 1;

	if (minidump_table.md_ss_toc &&
		(minidump_table.md_ss_toc->md_ss_enable_status ==
		MD_SS_ENABLED))
		md_update_ss_toc(entry);
	else
		pendings++;

	spin_unlock(&mdt_lock);

	return ret;
}
EXPORT_SYMBOL(msm_minidump_add_region);

static int msm_minidump_add_header(void)
{
	struct md_ss_region *mdreg = &minidump_table.md_regions[0];
	struct elfhdr *ehdr;
	struct elf_shdr *shdr;
	struct elf_phdr *phdr;
	unsigned int strtbl_off, elfh_size, phdr_off;
	char *banner;
	size_t linux_banner_len = strlen(linux_banner);

	/* Header buffer contains:
	 * elf header, MAX_NUM_ENTRIES+4 of section and program elf headers,
	 * string table section and linux banner.
	 */
	elfh_size = sizeof(*ehdr) + MAX_STRTBL_SIZE + (strlen(linux_banner) +
		1) + ((sizeof(*shdr) + sizeof(*phdr)) * (MAX_NUM_ENTRIES + 4));
	elfh_size = ALIGN(elfh_size, 4);

	minidump_elfheader.ehdr = kzalloc(elfh_size, GFP_KERNEL);
	if (!minidump_elfheader.ehdr)
		return -ENOMEM;

	strlcpy(mdreg->name, "KELF_HEADER", sizeof(mdreg->name));
	mdreg->region_base_address = virt_to_phys(minidump_elfheader.ehdr);
	mdreg->region_size = elfh_size;

	ehdr = minidump_elfheader.ehdr;
	/* Assign section/program headers offset */
	minidump_elfheader.shdr = shdr = (struct elf_shdr *)(ehdr + 1);
	minidump_elfheader.phdr = phdr =
				 (struct elf_phdr *)(shdr + MAX_NUM_ENTRIES);
	phdr_off = sizeof(*ehdr) + (sizeof(*shdr) * MAX_NUM_ENTRIES);

	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELF_CLASS;
	ehdr->e_ident[EI_DATA] = ELF_DATA;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELF_OSABI;
	ehdr->e_type = ET_CORE;
	ehdr->e_machine  = ELF_ARCH;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phoff = phdr_off;
	ehdr->e_phentsize = sizeof(*phdr);
	ehdr->e_shoff = sizeof(*ehdr);
	ehdr->e_shentsize = sizeof(*shdr);
	ehdr->e_shstrndx = 1;

	minidump_elfheader.elf_offset = elfh_size;

	/*
	 * First section header should be NULL,
	 * 2nd section is string table.
	 */
	minidump_elfheader.strtable_idx = 1;
	strtbl_off = sizeof(*ehdr) +
			((sizeof(*phdr) + sizeof(*shdr)) * MAX_NUM_ENTRIES);
	shdr++;
	shdr->sh_type = SHT_STRTAB;
	shdr->sh_offset = (elf_addr_t)strtbl_off;
	shdr->sh_size = MAX_STRTBL_SIZE;
	shdr->sh_entsize = 0;
	shdr->sh_flags = 0;
	shdr->sh_name = set_section_name("STR_TBL");
	shdr++;

	/* 3rd section is for minidump_table VA, used by parsers */
	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_entsize = 0;
	shdr->sh_flags = 0;
	shdr->sh_addr = (elf_addr_t)&minidump_table;
	shdr->sh_name = set_section_name("minidump_table");
	shdr++;

	/* 4th section is linux banner */
	banner = (char *)ehdr + strtbl_off + MAX_STRTBL_SIZE;
	strlcpy(banner, linux_banner, linux_banner_len + 1);

	shdr->sh_type = SHT_PROGBITS;
	shdr->sh_offset = (elf_addr_t)(strtbl_off + MAX_STRTBL_SIZE);
	shdr->sh_size = strlen(linux_banner) + 1;
	shdr->sh_addr = (elf_addr_t)linux_banner;
	shdr->sh_entsize = 0;
	shdr->sh_flags = SHF_WRITE;
	shdr->sh_name = set_section_name("linux_banner");

	phdr->p_type = PT_LOAD;
	phdr->p_offset = (elf_addr_t)(strtbl_off + MAX_STRTBL_SIZE);
	phdr->p_vaddr = (elf_addr_t)linux_banner;
	phdr->p_paddr = virt_to_phys(linux_banner);
	phdr->p_filesz = phdr->p_memsz = strlen(linux_banner) + 1;
	phdr->p_flags = PF_R | PF_W;

	/* Update headers count*/
	ehdr->e_phnum = 1;
	ehdr->e_shnum = 4;

	mdreg->md_valid = MD_REGION_VALID;
	return 0;
}

static int __init msm_minidump_init(void)
{
	unsigned int i;
	size_t size;
	struct md_region *mdr;
	struct md_global_toc *md_global_toc;
	struct md_ss_toc *md_ss_toc;

	/* Get Minidump table */
	md_global_toc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SBL_MINIDUMP_SMEM_ID,
				      &size);
	if (IS_ERR_OR_NULL(md_global_toc)) {
		pr_err("SMEM is not initialized.\n");
		return -ENODEV;
	}

	/*Check global minidump support initialization */
	if (!md_global_toc->md_toc_init) {
		pr_err("System Minidump TOC not initialized\n");
		return -ENODEV;
	}

	minidump_table.md_gbl_toc = md_global_toc;
	minidump_table.revision = md_global_toc->md_revision;
	md_ss_toc = &md_global_toc->md_ss_toc[MD_SS_HLOS_ID];

	md_ss_toc->encryption_status = MD_SS_ENCR_NONE;
	md_ss_toc->encryption_required = MD_SS_ENCR_REQ;

	minidump_table.md_ss_toc = md_ss_toc;
	minidump_table.md_regions = kzalloc((MAX_NUM_ENTRIES *
				sizeof(struct md_ss_region)), GFP_KERNEL);
	if (!minidump_table.md_regions)
		return -ENOMEM;

	md_ss_toc->md_ss_smem_regions_baseptr =
				virt_to_phys(minidump_table.md_regions);

	/* First entry would be ELF header */
	md_ss_toc->ss_region_count = 1;
	msm_minidump_add_header();

	/* Add pending entries to HLOS TOC */
	spin_lock(&mdt_lock);
	md_ss_toc->md_ss_toc_init = 1;
	md_ss_toc->md_ss_enable_status = MD_SS_ENABLED;
	for (i = 0; i < pendings; i++) {
		mdr = &minidump_table.entry[i];
		md_update_ss_toc(mdr);
	}

	pendings = 0;
	spin_unlock(&mdt_lock);

	pr_info("Enabled with max number of regions %d\n",
		CONFIG_MINIDUMP_MAX_ENTRIES);

	return 0;
}
subsys_initcall(msm_minidump_init)
