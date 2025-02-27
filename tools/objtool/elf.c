// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * elf.c - ELF access library
 *
 * Adapted from kpatch (https://github.com/dynup/kpatch):
 * Copyright (C) 2013-2015 Josh Poimboeuf <jpoimboe@redhat.com>
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/interval_tree_generic.h>
#include <objtool/builtin.h>

#include <objtool/elf.h>
#include <objtool/warn.h>

#define MAX_NAME_LEN 128

static int first_nonlocal_sym;
static int nr_add_syms;
/* Now for .orc_unwind and .orc_unwind_ip */
#define MAX_NUM_ORC_ADD_SYMS 2
static struct symbol *add_syms[MAX_NUM_ORC_ADD_SYMS];

static inline u32 str_hash(const char *str)
{
	return jhash(str, strlen(str), 0);
}

#define __elf_table(name)	(elf->name##_hash)
#define __elf_bits(name)	(elf->name##_bits)

#define elf_hash_add(name, node, key) \
	hlist_add_head(node, &__elf_table(name)[hash_min(key, __elf_bits(name))])

#define elf_hash_for_each_possible(name, obj, member, key) \
	hlist_for_each_entry(obj, &__elf_table(name)[hash_min(key, __elf_bits(name))], member)

#define elf_alloc_hash(name, size) \
({ \
	__elf_bits(name) = max(10, ilog2(size)); \
	__elf_table(name) = mmap(NULL, sizeof(struct hlist_head) << __elf_bits(name), \
				 PROT_READ|PROT_WRITE, \
				 MAP_PRIVATE|MAP_ANON, -1, 0); \
	if (__elf_table(name) == (void *)-1L) { \
		WARN("mmap fail " #name); \
		__elf_table(name) = NULL; \
	} \
	__elf_table(name); \
})

static inline unsigned long __sym_start(struct symbol *s)
{
	return s->offset;
}

static inline unsigned long __sym_last(struct symbol *s)
{
	return s->offset + s->len - 1;
}

INTERVAL_TREE_DEFINE(struct symbol, node, unsigned long, __subtree_last,
		     __sym_start, __sym_last, static, __sym)

#define __sym_for_each(_iter, _tree, _start, _end)			\
	for (_iter = __sym_iter_first((_tree), (_start), (_end));	\
	     _iter; _iter = __sym_iter_next(_iter, (_start), (_end)))

struct symbol_hole {
	unsigned long key;
	const struct symbol *sym;
};

/*
 * Find !section symbol where @offset is after it.
 */
static int symbol_hole_by_offset(const void *key, const struct rb_node *node)
{
	const struct symbol *s = rb_entry(node, struct symbol, node);
	struct symbol_hole *sh = (void *)key;

	if (sh->key < s->offset)
		return -1;

	if (sh->key >= s->offset + s->len) {
		if (s->type != STT_SECTION)
			sh->sym = s;
		return 1;
	}

	return 0;
}

struct section *find_section_by_name(const struct elf *elf, const char *name)
{
	struct section *sec;

	elf_hash_for_each_possible(section_name, sec, name_hash, str_hash(name)) {
		if (!strcmp(sec->name, name))
			return sec;
	}

	return NULL;
}

static struct section *find_section_by_index(struct elf *elf,
					     unsigned int idx)
{
	struct section *sec;

	elf_hash_for_each_possible(section, sec, hash, idx) {
		if (sec->idx == idx)
			return sec;
	}

	return NULL;
}

static struct symbol *find_symbol_by_index(struct elf *elf, unsigned int idx)
{
	struct symbol *sym;

	elf_hash_for_each_possible(symbol, sym, hash, idx) {
		if (sym->idx == idx)
			return sym;
	}

	return NULL;
}

struct symbol *find_symbol_by_offset(struct section *sec, unsigned long offset)
{
	struct rb_root_cached *tree = (struct rb_root_cached *)&sec->symbol_tree;
	struct symbol *iter;

	__sym_for_each(iter, tree, offset, offset) {
		if (iter->offset == offset && iter->type != STT_SECTION)
			return iter;
	}

	return NULL;
}

struct symbol *find_func_by_offset(struct section *sec, unsigned long offset)
{
	struct rb_root_cached *tree = (struct rb_root_cached *)&sec->symbol_tree;
	struct symbol *iter;

	__sym_for_each(iter, tree, offset, offset) {
		if (iter->offset == offset && iter->type == STT_FUNC)
			return iter;
	}

	return NULL;
}

struct symbol *find_symbol_containing(const struct section *sec, unsigned long offset)
{
	struct rb_root_cached *tree = (struct rb_root_cached *)&sec->symbol_tree;
	struct symbol *iter;

	__sym_for_each(iter, tree, offset, offset) {
		if (iter->type != STT_SECTION)
			return iter;
	}

	return NULL;
}

/*
 * Returns size of hole starting at @offset.
 */
int find_symbol_hole_containing(const struct section *sec, unsigned long offset)
{
	struct symbol_hole hole = {
		.key = offset,
		.sym = NULL,
	};
	struct rb_node *n;
	struct symbol *s;

	/*
	 * Find the rightmost symbol for which @offset is after it.
	 */
	n = rb_find(&hole, &sec->symbol_tree.rb_root, symbol_hole_by_offset);

	/* found a symbol that contains @offset */
	if (n)
		return 0; /* not a hole */

	/* didn't find a symbol for which @offset is after it */
	if (!hole.sym)
		return 0; /* not a hole */

	/* @offset >= sym->offset + sym->len, find symbol after it */
	n = rb_next(&hole.sym->node);
	if (!n)
		return -1; /* until end of address space */

	/* hole until start of next symbol */
	s = rb_entry(n, struct symbol, node);
	return s->offset - offset;
}

struct symbol *find_func_containing(struct section *sec, unsigned long offset)
{
	struct rb_root_cached *tree = (struct rb_root_cached *)&sec->symbol_tree;
	struct symbol *iter;

	__sym_for_each(iter, tree, offset, offset) {
		if (iter->type == STT_FUNC)
			return iter;
	}

	return NULL;
}

struct symbol *find_symbol_by_name(const struct elf *elf, const char *name)
{
	struct symbol *sym;

	elf_hash_for_each_possible(symbol_name, sym, name_hash, str_hash(name)) {
		if (!strcmp(sym->name, name))
			return sym;
	}

	return NULL;
}

struct reloc *find_reloc_by_dest_range(const struct elf *elf, struct section *sec,
				     unsigned long offset, unsigned int len)
{
	struct reloc *reloc, *r = NULL;
	unsigned long o;

	if (!sec->reloc)
		return NULL;

	sec = sec->reloc;

	for_offset_range(o, offset, offset + len) {
		elf_hash_for_each_possible(reloc, reloc, hash,
					   sec_offset_hash(sec, o)) {
			if (reloc->sec != sec)
				continue;

			if (reloc->offset >= offset && reloc->offset < offset + len) {
				if (!r || reloc->offset < r->offset)
					r = reloc;
			}
		}
		if (r)
			return r;
	}

	return NULL;
}

struct reloc *find_reloc_by_dest(const struct elf *elf, struct section *sec, unsigned long offset)
{
	return find_reloc_by_dest_range(elf, sec, offset, 1);
}

static int read_sections(struct elf *elf)
{
	Elf_Scn *s = NULL;
	struct section *sec;
	size_t shstrndx, sections_nr;
	int i;

	if (elf_getshdrnum(elf->elf, &sections_nr)) {
		WARN_ELF("elf_getshdrnum");
		return -1;
	}

	if (elf_getshdrstrndx(elf->elf, &shstrndx)) {
		WARN_ELF("elf_getshdrstrndx");
		return -1;
	}

	if (!elf_alloc_hash(section, sections_nr) ||
	    !elf_alloc_hash(section_name, sections_nr))
		return -1;

	elf->section_data = calloc(sections_nr, sizeof(*sec));
	if (!elf->section_data) {
		perror("calloc");
		return -1;
	}
	for (i = 0; i < sections_nr; i++) {
		sec = &elf->section_data[i];

		INIT_LIST_HEAD(&sec->symbol_list);
		INIT_LIST_HEAD(&sec->reloc_list);

		s = elf_getscn(elf->elf, i);
		if (!s) {
			WARN_ELF("elf_getscn");
			return -1;
		}

		sec->idx = elf_ndxscn(s);

		if (!gelf_getshdr(s, &sec->sh)) {
			WARN_ELF("gelf_getshdr");
			return -1;
		}

		sec->name = elf_strptr(elf->elf, shstrndx, sec->sh.sh_name);
		if (!sec->name) {
			WARN_ELF("elf_strptr");
			return -1;
		}

		if (sec->sh.sh_size != 0) {
			sec->data = elf_getdata(s, NULL);
			if (!sec->data) {
				WARN_ELF("elf_getdata");
				return -1;
			}
			if (sec->data->d_off != 0 ||
			    sec->data->d_size != sec->sh.sh_size) {
				WARN("unexpected data attributes for %s",
				     sec->name);
				return -1;
			}
		}

		if (sec->sh.sh_flags & SHF_EXECINSTR)
			elf->text_size += sec->sh.sh_size;

		list_add_tail(&sec->list, &elf->sections);
		elf_hash_add(section, &sec->hash, sec->idx);
		elf_hash_add(section_name, &sec->name_hash, str_hash(sec->name));
	}

	if (opts.stats) {
		printf("nr_sections: %lu\n", (unsigned long)sections_nr);
		printf("section_bits: %d\n", elf->section_bits);
	}

	/* sanity check, one more call to elf_nextscn() should return NULL */
	if (elf_nextscn(elf->elf, s)) {
		WARN("section entry mismatch");
		return -1;
	}

	return 0;
}

static void elf_add_symbol(struct elf *elf, struct symbol *sym)
{
	struct list_head *entry;
	struct rb_node *pnode;
	struct symbol *iter;

	INIT_LIST_HEAD(&sym->reloc_list);
	INIT_LIST_HEAD(&sym->pv_target);
	sym->alias = sym;

	sym->type = GELF_ST_TYPE(sym->sym.st_info);
	sym->bind = GELF_ST_BIND(sym->sym.st_info);

	if (sym->type == STT_FILE)
		elf->num_files++;

	sym->offset = sym->sym.st_value;
	sym->len = sym->sym.st_size;

	__sym_for_each(iter, &sym->sec->symbol_tree, sym->offset, sym->offset) {
		if (iter->offset == sym->offset && iter->type == sym->type)
			iter->alias = sym;
	}

	__sym_insert(sym, &sym->sec->symbol_tree);
	pnode = rb_prev(&sym->node);
	if (pnode)
		entry = &rb_entry(pnode, struct symbol, node)->list;
	else
		entry = &sym->sec->symbol_list;
	list_add(&sym->list, entry);
	elf_hash_add(symbol, &sym->hash, sym->idx);
	elf_hash_add(symbol_name, &sym->name_hash, str_hash(sym->name));

	/*
	 * Don't store empty STT_NOTYPE symbols in the rbtree.  They
	 * can exist within a function, confusing the sorting.
	 */
	if (!sym->len)
		__sym_remove(sym, &sym->sec->symbol_tree);
}

static int read_symbols(struct elf *elf)
{
	struct section *symtab, *symtab_shndx, *sec;
	struct symbol *sym, *pfunc;
	int symbols_nr, i;
	char *coldstr;
	Elf_Data *shndx_data = NULL;
	Elf32_Word shndx;

	symtab = find_section_by_name(elf, ".symtab");
	if (symtab) {
		symtab_shndx = find_section_by_name(elf, ".symtab_shndx");
		if (symtab_shndx)
			shndx_data = symtab_shndx->data;

		first_nonlocal_sym = symtab->sh.sh_info;
		symbols_nr = symtab->sh.sh_size / symtab->sh.sh_entsize;
	} else {
		/*
		 * A missing symbol table is actually possible if it's an empty
		 * .o file. This can happen for thunk_64.o. Make sure to at
		 * least allocate the symbol hash tables so we can do symbol
		 * lookups without crashing.
		 */
		symbols_nr = 0;
	}

	if (!elf_alloc_hash(symbol, symbols_nr) ||
	    !elf_alloc_hash(symbol_name, symbols_nr))
		return -1;

	elf->symbol_data = calloc(symbols_nr, sizeof(*sym));
	if (!elf->symbol_data) {
		perror("calloc");
		return -1;
	}
	for (i = 0; i < symbols_nr; i++) {
		sym = &elf->symbol_data[i];

		sym->idx = i;

		if (!gelf_getsymshndx(symtab->data, shndx_data, i, &sym->sym,
				      &shndx)) {
			WARN_ELF("gelf_getsymshndx");
			goto err;
		}

		sym->name = elf_strptr(elf->elf, symtab->sh.sh_link,
				       sym->sym.st_name);
		if (!sym->name) {
			WARN_ELF("elf_strptr");
			goto err;
		}

		if ((sym->sym.st_shndx > SHN_UNDEF &&
		     sym->sym.st_shndx < SHN_LORESERVE) ||
		    (shndx_data && sym->sym.st_shndx == SHN_XINDEX)) {
			if (sym->sym.st_shndx != SHN_XINDEX)
				shndx = sym->sym.st_shndx;

			sym->sec = find_section_by_index(elf, shndx);
			if (!sym->sec) {
				WARN("couldn't find section for symbol %s",
				     sym->name);
				goto err;
			}
			if (GELF_ST_TYPE(sym->sym.st_info) == STT_SECTION) {
				sym->name = sym->sec->name;
				sym->sec->sym = sym;
			}
		} else
			sym->sec = find_section_by_index(elf, 0);

		elf_add_symbol(elf, sym);
	}

	if (opts.stats) {
		printf("nr_symbols: %lu\n", (unsigned long)symbols_nr);
		printf("symbol_bits: %d\n", elf->symbol_bits);
	}

	/* Create parent/child links for any cold subfunctions */
	list_for_each_entry(sec, &elf->sections, list) {
		sec_for_each_sym(sec, sym) {
			char pname[MAX_NAME_LEN + 1];
			size_t pnamelen;
			if (sym->type != STT_FUNC)
				continue;

			if (sym->pfunc == NULL)
				sym->pfunc = sym;

			if (sym->cfunc == NULL)
				sym->cfunc = sym;

			coldstr = strstr(sym->name, ".cold");
			if (!coldstr)
				continue;

			pnamelen = coldstr - sym->name;
			if (pnamelen > MAX_NAME_LEN) {
				WARN("%s(): parent function name exceeds maximum length of %d characters",
				     sym->name, MAX_NAME_LEN);
				return -1;
			}

			strncpy(pname, sym->name, pnamelen);
			pname[pnamelen] = '\0';
			pfunc = find_symbol_by_name(elf, pname);

			if (!pfunc) {
				WARN("%s(): can't find parent function",
				     sym->name);
				return -1;
			}

			sym->pfunc = pfunc;
			pfunc->cfunc = sym;

			/*
			 * Unfortunately, -fnoreorder-functions puts the child
			 * inside the parent.  Remove the overlap so we can
			 * have sane assumptions.
			 *
			 * Note that pfunc->len now no longer matches
			 * pfunc->sym.st_size.
			 */
			if (sym->sec == pfunc->sec &&
			    sym->offset >= pfunc->offset &&
			    sym->offset + sym->len == pfunc->offset + pfunc->len) {
				pfunc->len -= sym->len;
			}
		}
	}

	return 0;

err:
	free(sym);
	return -1;
}

static struct section *elf_create_reloc_section(struct elf *elf,
						struct section *base,
						int reltype);

struct reloc *elf_add_reloc(struct elf *elf, struct section *sec, unsigned long offset,
			    unsigned int type, struct symbol *sym, s64 addend, struct reloc *prev)
{
	struct reloc *reloc;

	if (!sec->reloc && !elf_create_reloc_section(elf, sec, SHT_RELA))
		return NULL;

	reloc = malloc(sizeof(*reloc));
	if (!reloc) {
		perror("malloc");
		return NULL;
	}
	memset(reloc, 0, sizeof(*reloc));

	reloc->sec = sec->reloc;
	reloc->offset = offset;
	reloc->type = type;

	if (!sym && !strncmp(sec->sym->name, ".orc_unwind_ip", 14))
		reloc->sym = sec->sym;
	else
		reloc->sym = sym;

	reloc->addend = addend;

	if (prev) {
		prev->next = reloc;
	} else {
		list_add_tail(&reloc->sym_reloc_entry, &sym->reloc_list);
		list_add_tail(&reloc->list, &sec->reloc->reloc_list);
		elf_hash_add(reloc, &reloc->hash, reloc_hash(reloc));
	}

	sec->reloc->sh.sh_size += sec->reloc->sh.sh_entsize;
	sec->reloc->changed = true;

	return reloc;
}

/*
 * Ensure that any reloc section containing references to @sym is marked
 * changed such that it will get re-generated in elf_rebuild_reloc_sections()
 * with the new symbol index.
 */
static void elf_dirty_reloc_sym(struct elf *elf, struct symbol *sym)
{
	struct reloc *reloc;

	list_for_each_entry(reloc, &sym->reloc_list, sym_reloc_entry)
		reloc->sec->changed = true;
}

/*
 * The libelf API is terrible; gelf_update_sym*() takes a data block relative
 * index value, *NOT* the symbol index. As such, iterate the data blocks and
 * adjust index until it fits.
 *
 * If no data block is found, allow adding a new data block provided the index
 * is only one past the end.
 */
static int elf_update_symbol(struct elf *elf, struct section *symtab,
			     struct section *symtab_shndx, struct symbol *sym)
{
	Elf32_Word shndx = sym->sec ? sym->sec->idx : SHN_UNDEF;
	Elf_Data *symtab_data = NULL, *shndx_data = NULL;
	Elf64_Xword entsize = symtab->sh.sh_entsize;
	int max_idx, idx = sym->idx;
	Elf_Scn *s, *t = NULL;
	bool is_special_shndx = sym->sym.st_shndx >= SHN_LORESERVE &&
				sym->sym.st_shndx != SHN_XINDEX;

	if (is_special_shndx)
		shndx = sym->sym.st_shndx;

	s = elf_getscn(elf->elf, symtab->idx);
	if (!s) {
		WARN_ELF("elf_getscn");
		return -1;
	}

	if (symtab_shndx) {
		t = elf_getscn(elf->elf, symtab_shndx->idx);
		if (!t) {
			WARN_ELF("elf_getscn");
			return -1;
		}
	}

	for (;;) {
		/* get next data descriptor for the relevant sections */
		symtab_data = elf_getdata(s, symtab_data);
		if (t)
			shndx_data = elf_getdata(t, shndx_data);

		/* end-of-list */
		if (!symtab_data) {
			/*
			 * Over-allocate to avoid O(n^2) symbol creation
			 * behaviour.  The down side is that libelf doesn't
			 * like this; see elf_truncate_section() for the fixup.
			 */
			int num = max(1U, sym->idx/3);
			void *buf;

			if (idx) {
				/* we don't do holes in symbol tables */
				WARN("index out of range");
				return -1;
			}

			/* if @idx == 0, it's the next contiguous entry, create it */
			symtab_data = elf_newdata(s);
			if (t)
				shndx_data = elf_newdata(t);

			buf = calloc(num, entsize);
			if (!buf) {
				WARN("malloc");
				return -1;
			}

			symtab_data->d_buf = buf;
			symtab_data->d_size = num * entsize;
			symtab_data->d_align = 1;
			symtab_data->d_type = ELF_T_SYM;

			symtab->changed = true;
			symtab->truncate = true;

			if (t) {
				buf = calloc(num, sizeof(Elf32_Word));
				if (!buf) {
					WARN("malloc");
					return -1;
				}

				shndx_data->d_buf = buf;
				shndx_data->d_size = num * sizeof(Elf32_Word);
				shndx_data->d_align = sizeof(Elf32_Word);
				shndx_data->d_type = ELF_T_WORD;

				symtab_shndx->changed = true;
				symtab_shndx->truncate = true;
			}

			break;
		}

		/* empty blocks should not happen */
		if (!symtab_data->d_size) {
			WARN("zero size data");
			return -1;
		}

		/* is this the right block? */
		max_idx = symtab_data->d_size / entsize;
		if (idx < max_idx)
			break;

		/* adjust index and try again */
		idx -= max_idx;
	}

	/* something went side-ways */
	if (idx < 0) {
		WARN("negative index");
		return -1;
	}

	/* setup extended section index magic and write the symbol */
	if ((shndx >= SHN_UNDEF && shndx < SHN_LORESERVE) || is_special_shndx) {
		sym->sym.st_shndx = shndx;
		if (!shndx_data)
			shndx = 0;
	} else {
		sym->sym.st_shndx = SHN_XINDEX;
		if (!shndx_data) {
			WARN("no .symtab_shndx");
			return -1;
		}
	}

	if (!gelf_update_symshndx(symtab_data, shndx_data, idx, &sym->sym, shndx)) {
		WARN_ELF("gelf_update_symshndx");
		return -1;
	}

	return 0;
}

static struct symbol *
__elf_create_symbol(struct elf *elf, struct symbol *sym)
{
	struct section *symtab, *symtab_shndx;
	Elf32_Word first_non_local, new_idx;
	struct symbol *old;

	symtab = find_section_by_name(elf, ".symtab");
	if (symtab) {
		symtab_shndx = find_section_by_name(elf, ".symtab_shndx");
	} else {
		WARN("no .symtab");
		return NULL;
	}

	new_idx = symtab->sh.sh_size / symtab->sh.sh_entsize;

	if (GELF_ST_BIND(sym->sym.st_info) != STB_LOCAL)
		goto non_local;

	/*
	 * Move the first global symbol, as per sh_info, into a new, higher
	 * symbol index. This fees up a spot for a new local symbol.
	 */
	first_non_local = symtab->sh.sh_info;
	old = find_symbol_by_index(elf, first_non_local);
	if (old) {
		old->idx = new_idx;

		hlist_del(&old->hash);
		elf_hash_add(symbol, &old->hash, old->idx);

		elf_dirty_reloc_sym(elf, old);

		if (elf_update_symbol(elf, symtab, symtab_shndx, old)) {
			WARN("elf_update_symbol move");
			return NULL;
		}

		new_idx = first_non_local;
	}

	/*
	 * Either way, we will add a LOCAL symbol.
	 */
	symtab->sh.sh_info += 1;

non_local:
	sym->idx = new_idx;
	if (elf_update_symbol(elf, symtab, symtab_shndx, sym)) {
		WARN("elf_update_symbol");
		return NULL;
	}

	symtab->sh.sh_size += symtab->sh.sh_entsize;
	symtab->changed = true;

	if (symtab_shndx) {
		symtab_shndx->sh.sh_size += sizeof(Elf32_Word);
		symtab_shndx->changed = true;
	}

	return sym;
}

static struct symbol *
elf_create_section_symbol(struct elf *elf, struct section *sec)
{
	struct symbol *sym = calloc(1, sizeof(*sym));

	if (!sym) {
		perror("malloc");
		return NULL;
	}

	sym->name = sec->name;
	sym->sec = sec;

	// st_name 0
	sym->sym.st_info = GELF_ST_INFO(STB_LOCAL, STT_SECTION);
	// st_other 0
	// st_value 0
	// st_size 0

	sym = __elf_create_symbol(elf, sym);
	if (sym)
		elf_add_symbol(elf, sym);

	return sym;
}

static int elf_add_string(struct elf *elf, struct section *strtab, char *str);

struct symbol *
elf_create_prefix_symbol(struct elf *elf, struct symbol *orig, long size)
{
	struct symbol *sym = calloc(1, sizeof(*sym));
	size_t namelen = strlen(orig->name) + sizeof("__pfx_");
	char *name = malloc(namelen);

	if (!sym || !name) {
		perror("malloc");
		return NULL;
	}

	snprintf(name, namelen, "__pfx_%s", orig->name);

	sym->name = name;
	sym->sec = orig->sec;

	sym->sym.st_name = elf_add_string(elf, NULL, name);
	sym->sym.st_info = orig->sym.st_info;
	sym->sym.st_value = orig->sym.st_value - size;
	sym->sym.st_size = size;

	sym = __elf_create_symbol(elf, sym);
	if (sym)
		elf_add_symbol(elf, sym);

	return sym;
}

struct reloc *elf_add_reloc_to_insn(struct elf *elf, struct section *sec,
				    unsigned long offset, unsigned int type,
				    struct section *insn_sec, unsigned long insn_off,
				    struct reloc *prev)
{
	struct symbol *sym = insn_sec->sym;
	int addend = insn_off;

	if (prev) {
		sym = NULL;
	} else if (!sym) {
		/*
		 * Due to how weak functions work, we must use section based
		 * relocations. Symbol based relocations would result in the
		 * weak and non-weak function annotations being overlaid on the
		 * non-weak function after linking.
		 */
		sym = elf_create_section_symbol(elf, insn_sec);
		if (!sym)
			return NULL;

		insn_sec->sym = sym;
	}

	return elf_add_reloc(elf, sec, offset, type, sym, addend, prev);
}

static int read_rel_reloc(struct section *sec, int i, struct reloc *reloc, unsigned int *symndx)
{
	if (!gelf_getrel(sec->data, i, &reloc->rel)) {
		WARN_ELF("gelf_getrel");
		return -1;
	}
	reloc->type = GELF_R_TYPE(reloc->rel.r_info);
	reloc->addend = 0;
	reloc->offset = reloc->rel.r_offset;
	*symndx = GELF_R_SYM(reloc->rel.r_info);
	return 0;
}

static int read_rela_reloc(struct section *sec, int i, struct reloc *reloc, unsigned int *symndx)
{
	if (!gelf_getrela(sec->data, i, &reloc->rela)) {
		WARN_ELF("gelf_getrela");
		return -1;
	}
	reloc->type = GELF_R_TYPE(reloc->rela.r_info);
	reloc->addend = reloc->rela.r_addend;
	reloc->offset = reloc->rela.r_offset;
	*symndx = GELF_R_SYM(reloc->rela.r_info);
	return 0;
}

static int read_relocs(struct elf *elf)
{
	unsigned long nr_reloc, max_reloc = 0, tot_reloc = 0;
	struct section *sec;
	struct reloc *reloc, *last_reloc;
	unsigned int symndx;
	struct symbol *sym;
	int i;

	if (!elf_alloc_hash(reloc, elf->text_size / 16))
		return -1;

	list_for_each_entry(sec, &elf->sections, list) {
		if ((sec->sh.sh_type != SHT_RELA) &&
		    (sec->sh.sh_type != SHT_REL))
			continue;

		sec->base = find_section_by_index(elf, sec->sh.sh_info);
		if (!sec->base) {
			WARN("can't find base section for reloc section %s",
			     sec->name);
			return -1;
		}

		sec->base->reloc = sec;
		last_reloc = NULL;

		nr_reloc = 0;
		sec->reloc_data = calloc(sec->sh.sh_size / sec->sh.sh_entsize, sizeof(*reloc));
		if (!sec->reloc_data) {
			perror("calloc");
			return -1;
		}
		for (i = 0; i < sec->sh.sh_size / sec->sh.sh_entsize; i++) {
			reloc = &sec->reloc_data[i];
			switch (sec->sh.sh_type) {
			case SHT_REL:
				if (read_rel_reloc(sec, i, reloc, &symndx))
					return -1;
				break;
			case SHT_RELA:
				if (read_rela_reloc(sec, i, reloc, &symndx))
					return -1;
				break;
			default: return -1;
			}

			reloc->sec = sec;
			reloc->idx = i;
			reloc->sym = sym = find_symbol_by_index(elf, symndx);
			if (!reloc->sym) {
				WARN("can't find reloc entry symbol %d for %s",
				     symndx, sec->name);
				return -1;
			}

			if (last_reloc && reloc->offset == last_reloc->offset) {
				last_reloc->next = reloc;
				last_reloc = reloc;
				continue;
			}

			last_reloc = reloc;

			list_add_tail(&reloc->sym_reloc_entry, &sym->reloc_list);
			list_add_tail(&reloc->list, &sec->reloc_list);
			elf_hash_add(reloc, &reloc->hash, reloc_hash(reloc));

			nr_reloc++;
		}
		max_reloc = max(max_reloc, nr_reloc);
		tot_reloc += nr_reloc;
	}

	if (opts.stats) {
		printf("max_reloc: %lu\n", max_reloc);
		printf("tot_reloc: %lu\n", tot_reloc);
		printf("reloc_bits: %d\n", elf->reloc_bits);
	}

	return 0;
}

struct elf *elf_open_read(const char *name, int flags)
{
	struct elf *elf;
	Elf_Cmd cmd;

	elf_version(EV_CURRENT);

	elf = malloc(sizeof(*elf));
	if (!elf) {
		perror("malloc");
		return NULL;
	}
	memset(elf, 0, offsetof(struct elf, sections));

	INIT_LIST_HEAD(&elf->sections);

	elf->fd = open(name, flags);
	if (elf->fd == -1) {
		fprintf(stderr, "objtool: Can't open '%s': %s\n",
			name, strerror(errno));
		goto err;
	}

	if ((flags & O_ACCMODE) == O_RDONLY)
		cmd = ELF_C_READ_MMAP;
	else if ((flags & O_ACCMODE) == O_RDWR)
		cmd = ELF_C_RDWR;
	else /* O_WRONLY */
		cmd = ELF_C_WRITE;

	elf->elf = elf_begin(elf->fd, cmd, NULL);
	if (!elf->elf) {
		WARN_ELF("elf_begin");
		goto err;
	}

	if (!gelf_getehdr(elf->elf, &elf->ehdr)) {
		WARN_ELF("gelf_getehdr");
		goto err;
	}

	if (read_sections(elf))
		goto err;

	if (read_symbols(elf))
		goto err;

	if (read_relocs(elf))
		goto err;

	return elf;

err:
	elf_close(elf);
	return NULL;
}

static int elf_add_string(struct elf *elf, struct section *strtab, char *str)
{
	Elf_Data *data;
	Elf_Scn *s;
	int len;

	if (!strtab)
		strtab = find_section_by_name(elf, ".strtab");
	if (!strtab) {
		WARN("can't find .strtab section");
		return -1;
	}

	s = elf_getscn(elf->elf, strtab->idx);
	if (!s) {
		WARN_ELF("elf_getscn");
		return -1;
	}

	data = elf_newdata(s);
	if (!data) {
		WARN_ELF("elf_newdata");
		return -1;
	}

	data->d_buf = str;
	data->d_size = strlen(str) + 1;
	data->d_align = 1;

	len = strtab->sh.sh_size;
	strtab->sh.sh_size += data->d_size;
	strtab->changed = true;

	return len;
}

struct section *elf_create_section(struct elf *elf, const char *name,
				   unsigned int sh_flags, size_t entsize, int nr)
{
	struct section *sec, *shstrtab;
	struct section *symtab;
	struct symbol *sym;
	size_t size = entsize * nr;
	Elf_Scn *s;

	sec = malloc(sizeof(*sec));
	if (!sec) {
		perror("malloc");
		return NULL;
	}
	memset(sec, 0, sizeof(*sec));

	INIT_LIST_HEAD(&sec->symbol_list);
	INIT_LIST_HEAD(&sec->reloc_list);

	s = elf_newscn(elf->elf);
	if (!s) {
		WARN_ELF("elf_newscn");
		return NULL;
	}

	sec->name = strdup(name);
	if (!sec->name) {
		perror("strdup");
		return NULL;
	}

	sec->idx = elf_ndxscn(s);
	sec->changed = true;

	sec->data = elf_newdata(s);
	if (!sec->data) {
		WARN_ELF("elf_newdata");
		return NULL;
	}

	sec->data->d_size = size;
	sec->data->d_align = 1;

	if (size) {
		sec->data->d_buf = malloc(size);
		if (!sec->data->d_buf) {
			perror("malloc");
			return NULL;
		}
		memset(sec->data->d_buf, 0, size);
	}

	if (!gelf_getshdr(s, &sec->sh)) {
		WARN_ELF("gelf_getshdr");
		return NULL;
	}

	sec->sh.sh_size = size;
	sec->sh.sh_entsize = entsize;
	sec->sh.sh_type = SHT_PROGBITS;
	sec->sh.sh_addralign = 1;
	sec->sh.sh_flags = SHF_ALLOC | sh_flags;

	/*
	 * Prepare add section symtab information to .symtab.
	 * Only extra ".orc_unwind" and ".orc_unwind_ip" symbols are added.
	 * Do not modify .symtab until we really want to write elf.
	 */
	if (strncmp(sec->name, ".orc_unwind", 11))
		goto skip;

	if (nr_add_syms >= MAX_NUM_ORC_ADD_SYMS) {
		WARN("can't create symtab info");
		return NULL;
	}

	symtab = find_section_by_name(elf, ".symtab");
	if (!symtab) {
		WARN("can't find .symtab section");
		return NULL;
	}
	sym = malloc(sizeof(*sym));
	if (!sym) {
		perror("malloc");
		return NULL;
	}
	memset(sym, 0, sizeof(*sym));
	add_syms[nr_add_syms] = sym;
	sym->idx = first_nonlocal_sym + nr_add_syms;
	nr_add_syms++;
	sym->name = sec->name;
	sym->sym.st_info = GELF_ST_INFO(STB_LOCAL, STT_SECTION);
	sym->sym.st_shndx = sec->idx;
	sec->sym = sym;
	symtab->sh.sh_size += symtab->sh.sh_entsize;
	symtab->sh.sh_info++;
	symtab->changed = true;

skip:
	/* Add section name to .shstrtab (or .strtab for Clang) */
	shstrtab = find_section_by_name(elf, ".shstrtab");
	if (!shstrtab)
		shstrtab = find_section_by_name(elf, ".strtab");
	if (!shstrtab) {
		WARN("can't find .shstrtab or .strtab section");
		return NULL;
	}
	sec->sh.sh_name = elf_add_string(elf, shstrtab, sec->name);
	if (sec->sh.sh_name == -1)
		return NULL;

	list_add_tail(&sec->list, &elf->sections);
	elf_hash_add(section, &sec->hash, sec->idx);
	elf_hash_add(section_name, &sec->name_hash, str_hash(sec->name));

	elf->changed = true;

	return sec;
}

static struct section *elf_create_rel_reloc_section(struct elf *elf, struct section *base)
{
	char *relocname;
	struct section *sec;

	relocname = malloc(strlen(base->name) + strlen(".rel") + 1);
	if (!relocname) {
		perror("malloc");
		return NULL;
	}
	strcpy(relocname, ".rel");
	strcat(relocname, base->name);

	sec = elf_create_section(elf, relocname, 0, sizeof(GElf_Rel), 0);
	free(relocname);
	if (!sec)
		return NULL;

	base->reloc = sec;
	sec->base = base;

	sec->sh.sh_type = SHT_REL;
	sec->sh.sh_addralign = 8;
	sec->sh.sh_link = find_section_by_name(elf, ".symtab")->idx;
	sec->sh.sh_info = base->idx;
	sec->sh.sh_flags = SHF_INFO_LINK;

	return sec;
}

static struct section *elf_create_rela_reloc_section(struct elf *elf, struct section *base)
{
	char *relocname;
	struct section *sec;
	int addrsize = elf_class_addrsize(elf);

	relocname = malloc(strlen(base->name) + strlen(".rela") + 1);
	if (!relocname) {
		perror("malloc");
		return NULL;
	}
	strcpy(relocname, ".rela");
	strcat(relocname, base->name);

	if (addrsize == sizeof(u32))
		sec = elf_create_section(elf, relocname, 0, sizeof(Elf32_Rela), 0);
	else
		sec = elf_create_section(elf, relocname, 0, sizeof(GElf_Rela), 0);
	free(relocname);
	if (!sec)
		return NULL;

	base->reloc = sec;
	sec->base = base;

	sec->sh.sh_type = SHT_RELA;
	sec->sh.sh_addralign = addrsize;
	sec->sh.sh_link = find_section_by_name(elf, ".symtab")->idx;
	sec->sh.sh_info = base->idx;
	sec->sh.sh_flags = SHF_INFO_LINK;

	return sec;
}

static struct section *elf_create_reloc_section(struct elf *elf,
					 struct section *base,
					 int reltype)
{
	switch (reltype) {
	case SHT_REL:  return elf_create_rel_reloc_section(elf, base);
	case SHT_RELA: return elf_create_rela_reloc_section(elf, base);
	default:       return NULL;
	}
}

static int elf_rebuild_rel_reloc_section(struct section *sec)
{
	struct reloc *reloc;
	int idx = 0;
	void *buf;

	/* Allocate a buffer for relocations */
	buf = malloc(sec->sh.sh_size);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	sec->data->d_buf = buf;
	sec->data->d_size = sec->sh.sh_size;
	sec->data->d_type = ELF_T_REL;

	idx = 0;
	list_for_each_entry(reloc, &sec->reloc_list, list) {
		reloc->rel.r_offset = reloc->offset;
		reloc->rel.r_info = GELF_R_INFO(reloc->sym->idx, reloc->type);
		if (!gelf_update_rel(sec->data, idx, &reloc->rel)) {
			WARN_ELF("gelf_update_rel");
			return -1;
		}
		idx++;
	}

	return 0;
}

static int elf_rebuild_rela_reloc_section(struct section *sec)
{
	struct reloc *reloc, *p;
	int idx = 0;
	void *buf;

	/* Allocate a buffer for relocations with addends */
	buf = malloc(sec->sh.sh_size);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	sec->data->d_buf = buf;
	sec->data->d_size = sec->sh.sh_size;
	sec->data->d_type = ELF_T_RELA;

	idx = 0;
	list_for_each_entry(reloc, &sec->reloc_list, list) {
		for (p = reloc; p; p = p->next) {
			p->rela.r_offset = p->offset;
			p->rela.r_addend = p->addend;
			p->rela.r_info = GELF_R_INFO(p->sym ? p->sym->idx : 0, p->type);
			if (!gelf_update_rela(sec->data, idx, &p->rela)) {
				WARN_ELF("gelf_update_rela");
				return -1;
			}
			idx++;
		}
	}

	return 0;
}

static int elf_rebuild_reloc_section(struct elf *elf, struct section *sec)
{
	switch (sec->sh.sh_type) {
	case SHT_REL:  return elf_rebuild_rel_reloc_section(sec);
	case SHT_RELA: return elf_rebuild_rela_reloc_section(sec);
	default:       return -1;
	}
}

int elf_write_insn(struct elf *elf, struct section *sec,
		   unsigned long offset, unsigned int len,
		   const char *insn)
{
	Elf_Data *data = sec->data;

	if (data->d_type != ELF_T_BYTE || data->d_off) {
		WARN("write to unexpected data for section: %s", sec->name);
		return -1;
	}

	memcpy(data->d_buf + offset, insn, len);
	elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);

	elf->changed = true;

	return 0;
}

int elf_write_reloc(struct elf *elf, struct reloc *reloc)
{
	struct section *sec = reloc->sec;

	if (sec->sh.sh_type == SHT_REL) {
		reloc->rel.r_info = GELF_R_INFO(reloc->sym->idx, reloc->type);
		reloc->rel.r_offset = reloc->offset;

		if (!gelf_update_rel(sec->data, reloc->idx, &reloc->rel)) {
			WARN_ELF("gelf_update_rel");
			return -1;
		}
	} else {
		reloc->rela.r_info = GELF_R_INFO(reloc->sym->idx, reloc->type);
		reloc->rela.r_addend = reloc->addend;
		reloc->rela.r_offset = reloc->offset;

		if (!gelf_update_rela(sec->data, reloc->idx, &reloc->rela)) {
			WARN_ELF("gelf_update_rela");
			return -1;
		}
	}

	elf->changed = true;

	return 0;
}

/*
 * When Elf_Scn::sh_size is smaller than the combined Elf_Data::d_size
 * do you:
 *
 *   A) adhere to the section header and truncate the data, or
 *   B) ignore the section header and write out all the data you've got?
 *
 * Yes, libelf sucks and we need to manually truncate if we over-allocate data.
 */
static int elf_truncate_section(struct elf *elf, struct section *sec)
{
	u64 size = sec->sh.sh_size;
	bool truncated = false;
	Elf_Data *data = NULL;
	Elf_Scn *s;

	s = elf_getscn(elf->elf, sec->idx);
	if (!s) {
		WARN_ELF("elf_getscn");
		return -1;
	}

	for (;;) {
		/* get next data descriptor for the relevant section */
		data = elf_getdata(s, data);

		if (!data) {
			if (size) {
				WARN("end of section data but non-zero size left\n");
				return -1;
			}
			return 0;
		}

		if (truncated) {
			/* when we remove symbols */
			WARN("truncated; but more data\n");
			return -1;
		}

		if (!data->d_size) {
			WARN("zero size data");
			return -1;
		}

		if (data->d_size > size) {
			truncated = true;
			data->d_size = size;
		}

		size -= data->d_size;
	}
}


static int elf_adjust_nonlocal_symbol(struct elf *elf)
{
	struct section *sec, *symtab;
	Elf_Scn *s;
	Elf_Data *data;
	GElf_Sym *sym;
	char *buf;
	int i, nr_symbols, special = 0;

	/* Adjust symtab first */
	symtab = find_section_by_name(elf, ".symtab");
	if (!symtab) {
		WARN("can't find .symtab section");
		return -1;
	}

	if (!symtab->changed)
		return 0;

	nr_symbols = symtab->sh.sh_size / symtab->sh.sh_entsize - nr_add_syms;
	sym = symtab->data->d_buf;
	assert(nr_symbols * sizeof(*sym) == symtab->data->d_size);

	/* There may be not enough nonlocal symbol. */
	if (nr_symbols < first_nonlocal_sym + nr_add_syms)
		special = first_nonlocal_sym + nr_add_syms - nr_symbols;

	buf = malloc(nr_add_syms * sizeof(*sym));
	if (!buf) {
		perror("malloc");
		return -1;
	}

	memcpy(buf + special * sizeof(*sym),
	       &sym[nr_symbols - nr_add_syms + special],
	       (nr_add_syms - special) * sizeof(*sym));
	for (i = 0; i < special; i++)
		memcpy(buf + i * sizeof(*sym),
		       &add_syms[nr_add_syms - special + i]->sym,
		       sizeof(*sym));

	s = elf_getscn(elf->elf, symtab->idx);
	if (!s) {
		WARN_ELF("elf_getscn");
		return -1;
	}
	data = elf_newdata(s);
	if (!data) {
		WARN_ELF("elf_newdata");
		return -1;
	}
	data->d_buf = buf;
	data->d_size = nr_add_syms * sizeof(*sym);
	data->d_align = 8;

	for (i = nr_symbols - nr_add_syms - 1; !special && i >= first_nonlocal_sym; i--)
		memcpy(&sym[i + nr_add_syms], &sym[i], sizeof(*sym));

	for (i = 0; i < nr_add_syms - special; i++)
		memcpy(&sym[first_nonlocal_sym + i], &add_syms[i]->sym, sizeof(*sym));

	/* Then adjust ".rela" sections */
	list_for_each_entry(sec, &elf->sections, list) {
		GElf_Rela *rela;
		struct reloc *reloc,*p;
		int nr_relas, type, symndx;

		if (sec->sh.sh_type != SHT_RELA)
			continue;

		/* No need to adjust .rela.orc_unwind_ip */
		if (!strcmp(sec->name, ".rela.orc_unwind_ip"))
			continue;

		/* Update the symbol index data in the r_info member in the GElf_Rela structure */
		nr_relas = sec->sh.sh_size / sec->sh.sh_entsize;
		rela = (GElf_Rela *)sec->data->d_buf;
		assert(sec->data->d_size == nr_relas * sizeof(*rela));
		for (i = 0; i < nr_relas; i++) {
			type = GELF_R_TYPE(rela[i].r_info);
			symndx = GELF_R_SYM(rela[i].r_info);
			if (symndx < first_nonlocal_sym)
				continue;
			symndx += nr_add_syms;
			rela[i].r_info = GELF_R_INFO(symndx, type);
			sec->changed = true;
		}

		/* Update the idx member data in the symbol structure */
		list_for_each_entry(reloc, &sec->reloc_list, list) {
			for (p = reloc; p; p = p->next) {
				if (!p->sym)
					continue;
				if (p->sym->idx < first_nonlocal_sym)
					continue;
				sec->changed = true;
				if (p->sym->changed)
					continue;
				p->sym->idx += nr_add_syms;
				p->sym->changed = true;
			}
		}
	}

	return 0;
}

int elf_write(struct elf *elf)
{
	struct section *sec;
	Elf_Scn *s;

	if (opts.dryrun)
		return 0;

	if (elf_adjust_nonlocal_symbol(elf))
		return -1;

	/* Update changed relocation sections and section headers: */
	list_for_each_entry(sec, &elf->sections, list) {
		if (sec->truncate)
			elf_truncate_section(elf, sec);

		if (sec->changed) {
			s = elf_getscn(elf->elf, sec->idx);
			if (!s) {
				WARN_ELF("elf_getscn");
				return -1;
			}
			if (!gelf_update_shdr(s, &sec->sh)) {
				WARN_ELF("gelf_update_shdr");
				return -1;
			}

			if (sec->base &&
			    elf_rebuild_reloc_section(elf, sec)) {
				WARN("elf_rebuild_reloc_section");
				return -1;
			}

			sec->changed = false;
			elf->changed = true;
		}
	}

	/* Make sure the new section header entries get updated properly. */
	elf_flagelf(elf->elf, ELF_C_SET, ELF_F_DIRTY);

	/* Write all changes to the file. */
	if (elf_update(elf->elf, ELF_C_WRITE) < 0) {
		WARN_ELF("elf_update");
		return -1;
	}

	elf->changed = false;

	return 0;
}

void elf_close(struct elf *elf)
{
	struct section *sec, *tmpsec;
	struct symbol *sym, *tmpsym;
	struct reloc *reloc, *tmpreloc;

	if (elf->elf)
		elf_end(elf->elf);

	if (elf->fd > 0)
		close(elf->fd);

	list_for_each_entry_safe(sec, tmpsec, &elf->sections, list) {
		list_for_each_entry_safe(sym, tmpsym, &sec->symbol_list, list) {
			list_del(&sym->list);
			hash_del(&sym->hash);
		}
		list_for_each_entry_safe(reloc, tmpreloc, &sec->reloc_list, list) {
			list_del(&reloc->list);
			hash_del(&reloc->hash);
		}
		list_del(&sec->list);
		free(sec->reloc_data);
	}

	free(elf->symbol_data);
	free(elf->section_data);
	free(elf);
}
