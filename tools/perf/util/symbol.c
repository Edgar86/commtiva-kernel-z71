#include "util.h"
#include "../perf.h"
#include "string.h"
#include "symbol.h"
#include "thread.h"

#include "debug.h"

#include <asm/bug.h>
#include <libelf.h>
#include <gelf.h>
#include <elf.h>
#include <limits.h>
#include <sys/utsname.h>

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

enum dso_origin {
	DSO__ORIG_KERNEL = 0,
	DSO__ORIG_JAVA_JIT,
	DSO__ORIG_FEDORA,
	DSO__ORIG_UBUNTU,
	DSO__ORIG_BUILDID,
	DSO__ORIG_DSO,
	DSO__ORIG_KMODULE,
	DSO__ORIG_NOT_FOUND,
};

static void dsos__add(struct list_head *head, struct dso *dso);
static struct map *thread__find_map_by_name(struct thread *self, char *name);
static struct map *map__new2(u64 start, struct dso *dso, enum map_type type);
struct symbol *dso__find_symbol(struct dso *self, enum map_type type, u64 addr);
static int dso__load_kernel_sym(struct dso *self, struct map *map,
				struct thread *thread, symbol_filter_t filter);
unsigned int symbol__priv_size;
static int vmlinux_path__nr_entries;
static char **vmlinux_path;

static struct symbol_conf symbol_conf__defaults = {
	.use_modules	  = true,
	.try_vmlinux_path = true,
};

static struct thread kthread_mem;
struct thread *kthread = &kthread_mem;

bool dso__loaded(const struct dso *self, enum map_type type)
{
	return self->loaded & (1 << type);
}

static void dso__set_loaded(struct dso *self, enum map_type type)
{
	self->loaded |= (1 << type);
}

static void symbols__fixup_end(struct rb_root *self)
{
	struct rb_node *nd, *prevnd = rb_first(self);
	struct symbol *curr, *prev;

	if (prevnd == NULL)
		return;

	curr = rb_entry(prevnd, struct symbol, rb_node);

	for (nd = rb_next(prevnd); nd; nd = rb_next(nd)) {
		prev = curr;
		curr = rb_entry(nd, struct symbol, rb_node);

		if (prev->end == prev->start)
			prev->end = curr->start - 1;
	}

	/* Last entry */
	if (curr->end == curr->start)
		curr->end = roundup(curr->start, 4096);
}

static void __thread__fixup_maps_end(struct thread *self, enum map_type type)
{
	struct map *prev, *curr;
	struct rb_node *nd, *prevnd = rb_first(&self->maps[type]);

	if (prevnd == NULL)
		return;

	curr = rb_entry(prevnd, struct map, rb_node);

	for (nd = rb_next(prevnd); nd; nd = rb_next(nd)) {
		prev = curr;
		curr = rb_entry(nd, struct map, rb_node);
		prev->end = curr->start - 1;
	}

	/*
	 * We still haven't the actual symbols, so guess the
	 * last map final address.
	 */
	curr->end = ~0UL;
}

static void thread__fixup_maps_end(struct thread *self)
{
	int i;
	for (i = 0; i < MAP__NR_TYPES; ++i)
		__thread__fixup_maps_end(self, i);
}

static struct symbol *symbol__new(u64 start, u64 len, const char *name)
{
	size_t namelen = strlen(name) + 1;
	struct symbol *self = zalloc(symbol__priv_size +
				     sizeof(*self) + namelen);
	if (self == NULL)
		return NULL;

	if (symbol__priv_size)
		self = ((void *)self) + symbol__priv_size;

	self->start = start;
	self->end   = len ? start + len - 1 : start;

	pr_debug3("%s: %s %#Lx-%#Lx\n", __func__, name, start, self->end);

	memcpy(self->name, name, namelen);

	return self;
}

static void symbol__delete(struct symbol *self)
{
	free(((void *)self) - symbol__priv_size);
}

static size_t symbol__fprintf(struct symbol *self, FILE *fp)
{
	return fprintf(fp, " %llx-%llx %s\n",
		       self->start, self->end, self->name);
}

static void dso__set_long_name(struct dso *self, char *name)
{
	if (name == NULL)
		return;
	self->long_name = name;
	self->long_name_len = strlen(name);
}

static void dso__set_basename(struct dso *self)
{
	self->short_name = basename(self->long_name);
}

struct dso *dso__new(const char *name)
{
	struct dso *self = malloc(sizeof(*self) + strlen(name) + 1);

	if (self != NULL) {
		int i;
		strcpy(self->name, name);
		dso__set_long_name(self, self->name);
		self->short_name = self->name;
		for (i = 0; i < MAP__NR_TYPES; ++i)
			self->symbols[i] = RB_ROOT;
		self->find_symbol = dso__find_symbol;
		self->slen_calculated = 0;
		self->origin = DSO__ORIG_NOT_FOUND;
		self->loaded = 0;
		self->has_build_id = 0;
	}

	return self;
}

static void symbols__delete(struct rb_root *self)
{
	struct symbol *pos;
	struct rb_node *next = rb_first(self);

	while (next) {
		pos = rb_entry(next, struct symbol, rb_node);
		next = rb_next(&pos->rb_node);
		rb_erase(&pos->rb_node, self);
		symbol__delete(pos);
	}
}

void dso__delete(struct dso *self)
{
	int i;
	for (i = 0; i < MAP__NR_TYPES; ++i)
		symbols__delete(&self->symbols[i]);
	if (self->long_name != self->name)
		free(self->long_name);
	free(self);
}

void dso__set_build_id(struct dso *self, void *build_id)
{
	memcpy(self->build_id, build_id, sizeof(self->build_id));
	self->has_build_id = 1;
}

static void symbols__insert(struct rb_root *self, struct symbol *sym)
{
	struct rb_node **p = &self->rb_node;
	struct rb_node *parent = NULL;
	const u64 ip = sym->start;
	struct symbol *s;

	while (*p != NULL) {
		parent = *p;
		s = rb_entry(parent, struct symbol, rb_node);
		if (ip < s->start)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&sym->rb_node, parent, p);
	rb_insert_color(&sym->rb_node, self);
}

static struct symbol *symbols__find(struct rb_root *self, u64 ip)
{
	struct rb_node *n;

	if (self == NULL)
		return NULL;

	n = self->rb_node;

	while (n) {
		struct symbol *s = rb_entry(n, struct symbol, rb_node);

		if (ip < s->start)
			n = n->rb_left;
		else if (ip > s->end)
			n = n->rb_right;
		else
			return s;
	}

	return NULL;
}

struct symbol *dso__find_symbol(struct dso *self, enum map_type type, u64 addr)
{
	return symbols__find(&self->symbols[type], addr);
}

int build_id__sprintf(u8 *self, int len, char *bf)
{
	char *bid = bf;
	u8 *raw = self;
	int i;

	for (i = 0; i < len; ++i) {
		sprintf(bid, "%02x", *raw);
		++raw;
		bid += 2;
	}

	return raw - self;
}

size_t dso__fprintf_buildid(struct dso *self, FILE *fp)
{
	char sbuild_id[BUILD_ID_SIZE * 2 + 1];

	build_id__sprintf(self->build_id, sizeof(self->build_id), sbuild_id);
	return fprintf(fp, "%s", sbuild_id);
}

size_t dso__fprintf(struct dso *self, enum map_type type, FILE *fp)
{
	struct rb_node *nd;
	size_t ret = fprintf(fp, "dso: %s (", self->short_name);

	ret += dso__fprintf_buildid(self, fp);
	ret += fprintf(fp, ")\n");
	for (nd = rb_first(&self->symbols[type]); nd; nd = rb_next(nd)) {
		struct symbol *pos = rb_entry(nd, struct symbol, rb_node);
		ret += symbol__fprintf(pos, fp);
	}

	return ret;
}

/*
 * Loads the function entries in /proc/kallsyms into kernel_map->dso,
 * so that we can in the next step set the symbol ->end address and then
 * call kernel_maps__split_kallsyms.
 */
static int dso__load_all_kallsyms(struct dso *self, struct map *map)
{
	char *line = NULL;
	size_t n;
	struct rb_root *root = &self->symbols[map->type];
	FILE *file = fopen("/proc/kallsyms", "r");

	if (file == NULL)
		goto out_failure;

	while (!feof(file)) {
		u64 start;
		struct symbol *sym;
		int line_len, len;
		char symbol_type;
		char *symbol_name;

		line_len = getline(&line, &n, file);
		if (line_len < 0)
			break;

		if (!line)
			goto out_failure;

		line[--line_len] = '\0'; /* \n */

		len = hex2u64(line, &start);

		len++;
		if (len + 2 >= line_len)
			continue;

		symbol_type = toupper(line[len]);
		/*
		 * We're interested only in code ('T'ext)
		 */
		if (symbol_type != 'T' && symbol_type != 'W')
			continue;

		symbol_name = line + len + 2;
		/*
		 * Will fix up the end later, when we have all symbols sorted.
		 */
		sym = symbol__new(start, 0, symbol_name);

		if (sym == NULL)
			goto out_delete_line;
		/*
		 * We will pass the symbols to the filter later, in
		 * map__split_kallsyms, when we have split the maps per module
		 */
		symbols__insert(root, sym);
	}

	free(line);
	fclose(file);

	return 0;

out_delete_line:
	free(line);
out_failure:
	return -1;
}

/*
 * Split the symbols into maps, making sure there are no overlaps, i.e. the
 * kernel range is broken in several maps, named [kernel].N, as we don't have
 * the original ELF section names vmlinux have.
 */
static int dso__split_kallsyms(struct dso *self, struct map *map, struct thread *thread,
			       symbol_filter_t filter)
{
	struct map *curr_map = map;
	struct symbol *pos;
	int count = 0;
	struct rb_root *root = &self->symbols[map->type];
	struct rb_node *next = rb_first(root);
	int kernel_range = 0;

	while (next) {
		char *module;

		pos = rb_entry(next, struct symbol, rb_node);
		next = rb_next(&pos->rb_node);

		module = strchr(pos->name, '\t');
		if (module) {
			if (!thread->use_modules)
				goto discard_symbol;

			*module++ = '\0';

			if (strcmp(self->name, module)) {
				curr_map = thread__find_map_by_name(thread, module);
				if (curr_map == NULL) {
					pr_debug("/proc/{kallsyms,modules} "
					         "inconsistency!\n");
					return -1;
				}
			}
			/*
			 * So that we look just like we get from .ko files,
			 * i.e. not prelinked, relative to map->start.
			 */
			pos->start = curr_map->map_ip(curr_map, pos->start);
			pos->end   = curr_map->map_ip(curr_map, pos->end);
		} else if (curr_map != map) {
			char dso_name[PATH_MAX];
			struct dso *dso;

			snprintf(dso_name, sizeof(dso_name), "[kernel].%d",
				 kernel_range++);

			dso = dso__new(dso_name);
			if (dso == NULL)
				return -1;

			curr_map = map__new2(pos->start, dso, map->type);
			if (map == NULL) {
				dso__delete(dso);
				return -1;
			}

			curr_map->map_ip = curr_map->unmap_ip = identity__map_ip;
			__thread__insert_map(thread, curr_map);
			++kernel_range;
		}

		if (filter && filter(curr_map, pos)) {
discard_symbol:		rb_erase(&pos->rb_node, root);
			symbol__delete(pos);
		} else {
			if (curr_map != map) {
				rb_erase(&pos->rb_node, root);
				symbols__insert(&curr_map->dso->symbols[curr_map->type], pos);
			}
			count++;
		}
	}

	return count;
}


static int dso__load_kallsyms(struct dso *self, struct map *map,
			      struct thread *thread, symbol_filter_t filter)
{
	if (dso__load_all_kallsyms(self, map) < 0)
		return -1;

	symbols__fixup_end(&self->symbols[map->type]);
	self->origin = DSO__ORIG_KERNEL;

	return dso__split_kallsyms(self, map, thread, filter);
}

size_t kernel_maps__fprintf(FILE *fp)
{
	size_t printed = fprintf(fp, "Kernel maps:\n");
	printed += thread__fprintf_maps(kthread, fp);
	return printed + fprintf(fp, "END kernel maps\n");
}

static int dso__load_perf_map(struct dso *self, struct map *map,
			      symbol_filter_t filter)
{
	char *line = NULL;
	size_t n;
	FILE *file;
	int nr_syms = 0;

	file = fopen(self->long_name, "r");
	if (file == NULL)
		goto out_failure;

	while (!feof(file)) {
		u64 start, size;
		struct symbol *sym;
		int line_len, len;

		line_len = getline(&line, &n, file);
		if (line_len < 0)
			break;

		if (!line)
			goto out_failure;

		line[--line_len] = '\0'; /* \n */

		len = hex2u64(line, &start);

		len++;
		if (len + 2 >= line_len)
			continue;

		len += hex2u64(line + len, &size);

		len++;
		if (len + 2 >= line_len)
			continue;

		sym = symbol__new(start, size, line + len);

		if (sym == NULL)
			goto out_delete_line;

		if (filter && filter(map, sym))
			symbol__delete(sym);
		else {
			symbols__insert(&self->symbols[map->type], sym);
			nr_syms++;
		}
	}

	free(line);
	fclose(file);

	return nr_syms;

out_delete_line:
	free(line);
out_failure:
	return -1;
}

/**
 * elf_symtab__for_each_symbol - iterate thru all the symbols
 *
 * @self: struct elf_symtab instance to iterate
 * @idx: uint32_t idx
 * @sym: GElf_Sym iterator
 */
#define elf_symtab__for_each_symbol(syms, nr_syms, idx, sym) \
	for (idx = 0, gelf_getsym(syms, idx, &sym);\
	     idx < nr_syms; \
	     idx++, gelf_getsym(syms, idx, &sym))

static inline uint8_t elf_sym__type(const GElf_Sym *sym)
{
	return GELF_ST_TYPE(sym->st_info);
}

static inline int elf_sym__is_function(const GElf_Sym *sym)
{
	return elf_sym__type(sym) == STT_FUNC &&
	       sym->st_name != 0 &&
	       sym->st_shndx != SHN_UNDEF;
}

static inline int elf_sym__is_label(const GElf_Sym *sym)
{
	return elf_sym__type(sym) == STT_NOTYPE &&
		sym->st_name != 0 &&
		sym->st_shndx != SHN_UNDEF &&
		sym->st_shndx != SHN_ABS;
}

static inline const char *elf_sec__name(const GElf_Shdr *shdr,
					const Elf_Data *secstrs)
{
	return secstrs->d_buf + shdr->sh_name;
}

static inline int elf_sec__is_text(const GElf_Shdr *shdr,
					const Elf_Data *secstrs)
{
	return strstr(elf_sec__name(shdr, secstrs), "text") != NULL;
}

static inline const char *elf_sym__name(const GElf_Sym *sym,
					const Elf_Data *symstrs)
{
	return symstrs->d_buf + sym->st_name;
}

static Elf_Scn *elf_section_by_name(Elf *elf, GElf_Ehdr *ep,
				    GElf_Shdr *shp, const char *name,
				    size_t *idx)
{
	Elf_Scn *sec = NULL;
	size_t cnt = 1;

	while ((sec = elf_nextscn(elf, sec)) != NULL) {
		char *str;

		gelf_getshdr(sec, shp);
		str = elf_strptr(elf, ep->e_shstrndx, shp->sh_name);
		if (!strcmp(name, str)) {
			if (idx)
				*idx = cnt;
			break;
		}
		++cnt;
	}

	return sec;
}

#define elf_section__for_each_rel(reldata, pos, pos_mem, idx, nr_entries) \
	for (idx = 0, pos = gelf_getrel(reldata, 0, &pos_mem); \
	     idx < nr_entries; \
	     ++idx, pos = gelf_getrel(reldata, idx, &pos_mem))

#define elf_section__for_each_rela(reldata, pos, pos_mem, idx, nr_entries) \
	for (idx = 0, pos = gelf_getrela(reldata, 0, &pos_mem); \
	     idx < nr_entries; \
	     ++idx, pos = gelf_getrela(reldata, idx, &pos_mem))

/*
 * We need to check if we have a .dynsym, so that we can handle the
 * .plt, synthesizing its symbols, that aren't on the symtabs (be it
 * .dynsym or .symtab).
 * And always look at the original dso, not at debuginfo packages, that
 * have the PLT data stripped out (shdr_rel_plt.sh_type == SHT_NOBITS).
 */
static int dso__synthesize_plt_symbols(struct  dso *self, struct map *map,
				       symbol_filter_t filter)
{
	uint32_t nr_rel_entries, idx;
	GElf_Sym sym;
	u64 plt_offset;
	GElf_Shdr shdr_plt;
	struct symbol *f;
	GElf_Shdr shdr_rel_plt, shdr_dynsym;
	Elf_Data *reldata, *syms, *symstrs;
	Elf_Scn *scn_plt_rel, *scn_symstrs, *scn_dynsym;
	size_t dynsym_idx;
	GElf_Ehdr ehdr;
	char sympltname[1024];
	Elf *elf;
	int nr = 0, symidx, fd, err = 0;

	fd = open(self->long_name, O_RDONLY);
	if (fd < 0)
		goto out;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL)
		goto out_close;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		goto out_elf_end;

	scn_dynsym = elf_section_by_name(elf, &ehdr, &shdr_dynsym,
					 ".dynsym", &dynsym_idx);
	if (scn_dynsym == NULL)
		goto out_elf_end;

	scn_plt_rel = elf_section_by_name(elf, &ehdr, &shdr_rel_plt,
					  ".rela.plt", NULL);
	if (scn_plt_rel == NULL) {
		scn_plt_rel = elf_section_by_name(elf, &ehdr, &shdr_rel_plt,
						  ".rel.plt", NULL);
		if (scn_plt_rel == NULL)
			goto out_elf_end;
	}

	err = -1;

	if (shdr_rel_plt.sh_link != dynsym_idx)
		goto out_elf_end;

	if (elf_section_by_name(elf, &ehdr, &shdr_plt, ".plt", NULL) == NULL)
		goto out_elf_end;

	/*
	 * Fetch the relocation section to find the idxes to the GOT
	 * and the symbols in the .dynsym they refer to.
	 */
	reldata = elf_getdata(scn_plt_rel, NULL);
	if (reldata == NULL)
		goto out_elf_end;

	syms = elf_getdata(scn_dynsym, NULL);
	if (syms == NULL)
		goto out_elf_end;

	scn_symstrs = elf_getscn(elf, shdr_dynsym.sh_link);
	if (scn_symstrs == NULL)
		goto out_elf_end;

	symstrs = elf_getdata(scn_symstrs, NULL);
	if (symstrs == NULL)
		goto out_elf_end;

	nr_rel_entries = shdr_rel_plt.sh_size / shdr_rel_plt.sh_entsize;
	plt_offset = shdr_plt.sh_offset;

	if (shdr_rel_plt.sh_type == SHT_RELA) {
		GElf_Rela pos_mem, *pos;

		elf_section__for_each_rela(reldata, pos, pos_mem, idx,
					   nr_rel_entries) {
			symidx = GELF_R_SYM(pos->r_info);
			plt_offset += shdr_plt.sh_entsize;
			gelf_getsym(syms, symidx, &sym);
			snprintf(sympltname, sizeof(sympltname),
				 "%s@plt", elf_sym__name(&sym, symstrs));

			f = symbol__new(plt_offset, shdr_plt.sh_entsize,
					sympltname);
			if (!f)
				goto out_elf_end;

			if (filter && filter(map, f))
				symbol__delete(f);
			else {
				symbols__insert(&self->symbols[map->type], f);
				++nr;
			}
		}
	} else if (shdr_rel_plt.sh_type == SHT_REL) {
		GElf_Rel pos_mem, *pos;
		elf_section__for_each_rel(reldata, pos, pos_mem, idx,
					  nr_rel_entries) {
			symidx = GELF_R_SYM(pos->r_info);
			plt_offset += shdr_plt.sh_entsize;
			gelf_getsym(syms, symidx, &sym);
			snprintf(sympltname, sizeof(sympltname),
				 "%s@plt", elf_sym__name(&sym, symstrs));

			f = symbol__new(plt_offset, shdr_plt.sh_entsize,
					sympltname);
			if (!f)
				goto out_elf_end;

			if (filter && filter(map, f))
				symbol__delete(f);
			else {
				symbols__insert(&self->symbols[map->type], f);
				++nr;
			}
		}
	}

	err = 0;
out_elf_end:
	elf_end(elf);
out_close:
	close(fd);

	if (err == 0)
		return nr;
out:
	pr_warning("%s: problems reading %s PLT info.\n",
		   __func__, self->long_name);
	return 0;
}

static int dso__load_sym(struct dso *self, struct map *map,
			 struct thread *thread, const char *name, int fd,
			 symbol_filter_t filter, int kernel, int kmodule)
{
	struct map *curr_map = map;
	struct dso *curr_dso = self;
	size_t dso_name_len = strlen(self->short_name);
	Elf_Data *symstrs, *secstrs;
	uint32_t nr_syms;
	int err = -1;
	uint32_t idx;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	Elf_Data *syms;
	GElf_Sym sym;
	Elf_Scn *sec, *sec_strndx;
	Elf *elf;
	int nr = 0;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL) {
		pr_err("%s: cannot read %s ELF file.\n", __func__, name);
		goto out_close;
	}

	if (gelf_getehdr(elf, &ehdr) == NULL) {
		pr_err("%s: cannot get elf header.\n", __func__);
		goto out_elf_end;
	}

	sec = elf_section_by_name(elf, &ehdr, &shdr, ".symtab", NULL);
	if (sec == NULL) {
		sec = elf_section_by_name(elf, &ehdr, &shdr, ".dynsym", NULL);
		if (sec == NULL)
			goto out_elf_end;
	}

	syms = elf_getdata(sec, NULL);
	if (syms == NULL)
		goto out_elf_end;

	sec = elf_getscn(elf, shdr.sh_link);
	if (sec == NULL)
		goto out_elf_end;

	symstrs = elf_getdata(sec, NULL);
	if (symstrs == NULL)
		goto out_elf_end;

	sec_strndx = elf_getscn(elf, ehdr.e_shstrndx);
	if (sec_strndx == NULL)
		goto out_elf_end;

	secstrs = elf_getdata(sec_strndx, NULL);
	if (secstrs == NULL)
		goto out_elf_end;

	nr_syms = shdr.sh_size / shdr.sh_entsize;

	memset(&sym, 0, sizeof(sym));
	if (!kernel) {
		self->adjust_symbols = (ehdr.e_type == ET_EXEC ||
				elf_section_by_name(elf, &ehdr, &shdr,
						     ".gnu.prelink_undo",
						     NULL) != NULL);
	} else self->adjust_symbols = 0;

	elf_symtab__for_each_symbol(syms, nr_syms, idx, sym) {
		struct symbol *f;
		const char *elf_name;
		char *demangled = NULL;
		int is_label = elf_sym__is_label(&sym);
		const char *section_name;

		if (!is_label && !elf_sym__is_function(&sym))
			continue;

		sec = elf_getscn(elf, sym.st_shndx);
		if (!sec)
			goto out_elf_end;

		gelf_getshdr(sec, &shdr);

		if (is_label && !elf_sec__is_text(&shdr, secstrs))
			continue;

		elf_name = elf_sym__name(&sym, symstrs);
		section_name = elf_sec__name(&shdr, secstrs);

		if (kernel || kmodule) {
			char dso_name[PATH_MAX];

			if (strcmp(section_name,
				   curr_dso->short_name + dso_name_len) == 0)
				goto new_symbol;

			if (strcmp(section_name, ".text") == 0) {
				curr_map = map;
				curr_dso = self;
				goto new_symbol;
			}

			snprintf(dso_name, sizeof(dso_name),
				 "%s%s", self->short_name, section_name);

			curr_map = thread__find_map_by_name(thread, dso_name);
			if (curr_map == NULL) {
				u64 start = sym.st_value;

				if (kmodule)
					start += map->start + shdr.sh_offset;

				curr_dso = dso__new(dso_name);
				if (curr_dso == NULL)
					goto out_elf_end;
				curr_map = map__new2(start, curr_dso,
						     MAP__FUNCTION);
				if (curr_map == NULL) {
					dso__delete(curr_dso);
					goto out_elf_end;
				}
				curr_map->map_ip = identity__map_ip;
				curr_map->unmap_ip = identity__map_ip;
				curr_dso->origin = DSO__ORIG_KERNEL;
				__thread__insert_map(kthread, curr_map);
				dsos__add(&dsos__kernel, curr_dso);
			} else
				curr_dso = curr_map->dso;

			goto new_symbol;
		}

		if (curr_dso->adjust_symbols) {
			pr_debug2("adjusting symbol: st_value: %Lx sh_addr: "
				  "%Lx sh_offset: %Lx\n", (u64)sym.st_value,
				  (u64)shdr.sh_addr, (u64)shdr.sh_offset);
			sym.st_value -= shdr.sh_addr - shdr.sh_offset;
		}
		/*
		 * We need to figure out if the object was created from C++ sources
		 * DWARF DW_compile_unit has this, but we don't always have access
		 * to it...
		 */
		demangled = bfd_demangle(NULL, elf_name, DMGL_PARAMS | DMGL_ANSI);
		if (demangled != NULL)
			elf_name = demangled;
new_symbol:
		f = symbol__new(sym.st_value, sym.st_size, elf_name);
		free(demangled);
		if (!f)
			goto out_elf_end;

		if (filter && filter(curr_map, f))
			symbol__delete(f);
		else {
			symbols__insert(&curr_dso->symbols[curr_map->type], f);
			nr++;
		}
	}

	/*
	 * For misannotated, zeroed, ASM function sizes.
	 */
	if (nr > 0)
		symbols__fixup_end(&self->symbols[map->type]);
	err = nr;
out_elf_end:
	elf_end(elf);
out_close:
	return err;
}

static bool dso__build_id_equal(const struct dso *self, u8 *build_id)
{
	return memcmp(self->build_id, build_id, sizeof(self->build_id)) == 0;
}

static bool __dsos__read_build_ids(struct list_head *head)
{
	bool have_build_id = false;
	struct dso *pos;

	list_for_each_entry(pos, head, node)
		if (filename__read_build_id(pos->long_name, pos->build_id,
					    sizeof(pos->build_id)) > 0) {
			have_build_id	  = true;
			pos->has_build_id = true;
		}

	return have_build_id;
}

bool dsos__read_build_ids(void)
{
	return __dsos__read_build_ids(&dsos__kernel) ||
	       __dsos__read_build_ids(&dsos__user);
}

/*
 * Align offset to 4 bytes as needed for note name and descriptor data.
 */
#define NOTE_ALIGN(n) (((n) + 3) & -4U)

int filename__read_build_id(const char *filename, void *bf, size_t size)
{
	int fd, err = -1;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	Elf_Data *data;
	Elf_Scn *sec;
	Elf_Kind ek;
	void *ptr;
	Elf *elf;

	if (size < BUILD_ID_SIZE)
		goto out;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto out;

	elf = elf_begin(fd, PERF_ELF_C_READ_MMAP, NULL);
	if (elf == NULL) {
		pr_debug2("%s: cannot read %s ELF file.\n", __func__, filename);
		goto out_close;
	}

	ek = elf_kind(elf);
	if (ek != ELF_K_ELF)
		goto out_elf_end;

	if (gelf_getehdr(elf, &ehdr) == NULL) {
		pr_err("%s: cannot get elf header.\n", __func__);
		goto out_elf_end;
	}

	sec = elf_section_by_name(elf, &ehdr, &shdr,
				  ".note.gnu.build-id", NULL);
	if (sec == NULL) {
		sec = elf_section_by_name(elf, &ehdr, &shdr,
					  ".notes", NULL);
		if (sec == NULL)
			goto out_elf_end;
	}

	data = elf_getdata(sec, NULL);
	if (data == NULL)
		goto out_elf_end;

	ptr = data->d_buf;
	while (ptr < (data->d_buf + data->d_size)) {
		GElf_Nhdr *nhdr = ptr;
		int namesz = NOTE_ALIGN(nhdr->n_namesz),
		    descsz = NOTE_ALIGN(nhdr->n_descsz);
		const char *name;

		ptr += sizeof(*nhdr);
		name = ptr;
		ptr += namesz;
		if (nhdr->n_type == NT_GNU_BUILD_ID &&
		    nhdr->n_namesz == sizeof("GNU")) {
			if (memcmp(name, "GNU", sizeof("GNU")) == 0) {
				memcpy(bf, ptr, BUILD_ID_SIZE);
				err = BUILD_ID_SIZE;
				break;
			}
		}
		ptr += descsz;
	}
out_elf_end:
	elf_end(elf);
out_close:
	close(fd);
out:
	return err;
}

int sysfs__read_build_id(const char *filename, void *build_id, size_t size)
{
	int fd, err = -1;

	if (size < BUILD_ID_SIZE)
		goto out;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto out;

	while (1) {
		char bf[BUFSIZ];
		GElf_Nhdr nhdr;
		int namesz, descsz;

		if (read(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr))
			break;

		namesz = NOTE_ALIGN(nhdr.n_namesz);
		descsz = NOTE_ALIGN(nhdr.n_descsz);
		if (nhdr.n_type == NT_GNU_BUILD_ID &&
		    nhdr.n_namesz == sizeof("GNU")) {
			if (read(fd, bf, namesz) != namesz)
				break;
			if (memcmp(bf, "GNU", sizeof("GNU")) == 0) {
				if (read(fd, build_id,
				    BUILD_ID_SIZE) == BUILD_ID_SIZE) {
					err = 0;
					break;
				}
			} else if (read(fd, bf, descsz) != descsz)
				break;
		} else {
			int n = namesz + descsz;
			if (read(fd, bf, n) != n)
				break;
		}
	}
	close(fd);
out:
	return err;
}

char dso__symtab_origin(const struct dso *self)
{
	static const char origin[] = {
		[DSO__ORIG_KERNEL] =   'k',
		[DSO__ORIG_JAVA_JIT] = 'j',
		[DSO__ORIG_FEDORA] =   'f',
		[DSO__ORIG_UBUNTU] =   'u',
		[DSO__ORIG_BUILDID] =  'b',
		[DSO__ORIG_DSO] =      'd',
		[DSO__ORIG_KMODULE] =  'K',
	};

	if (self == NULL || self->origin == DSO__ORIG_NOT_FOUND)
		return '!';
	return origin[self->origin];
}

int dso__load(struct dso *self, struct map *map, symbol_filter_t filter)
{
	int size = PATH_MAX;
	char *name;
	u8 build_id[BUILD_ID_SIZE];
	int ret = -1;
	int fd;

	dso__set_loaded(self, map->type);

	if (self->kernel)
		return dso__load_kernel_sym(self, map, kthread, filter);

	name = malloc(size);
	if (!name)
		return -1;

	self->adjust_symbols = 0;

	if (strncmp(self->name, "/tmp/perf-", 10) == 0) {
		ret = dso__load_perf_map(self, map, filter);
		self->origin = ret > 0 ? DSO__ORIG_JAVA_JIT :
					 DSO__ORIG_NOT_FOUND;
		return ret;
	}

	self->origin = DSO__ORIG_FEDORA - 1;

more:
	do {
		self->origin++;
		switch (self->origin) {
		case DSO__ORIG_FEDORA:
			snprintf(name, size, "/usr/lib/debug%s.debug",
				 self->long_name);
			break;
		case DSO__ORIG_UBUNTU:
			snprintf(name, size, "/usr/lib/debug%s",
				 self->long_name);
			break;
		case DSO__ORIG_BUILDID:
			if (filename__read_build_id(self->long_name, build_id,
						    sizeof(build_id))) {
				char build_id_hex[BUILD_ID_SIZE * 2 + 1];

				build_id__sprintf(build_id, sizeof(build_id),
						  build_id_hex);
				snprintf(name, size,
					 "/usr/lib/debug/.build-id/%.2s/%s.debug",
					build_id_hex, build_id_hex + 2);
				if (self->has_build_id)
					goto compare_build_id;
				break;
			}
			self->origin++;
			/* Fall thru */
		case DSO__ORIG_DSO:
			snprintf(name, size, "%s", self->long_name);
			break;

		default:
			goto out;
		}

		if (self->has_build_id) {
			if (filename__read_build_id(name, build_id,
						    sizeof(build_id)) < 0)
				goto more;
compare_build_id:
			if (!dso__build_id_equal(self, build_id))
				goto more;
		}

		fd = open(name, O_RDONLY);
	} while (fd < 0);

	ret = dso__load_sym(self, map, NULL, name, fd, filter, 0, 0);
	close(fd);

	/*
	 * Some people seem to have debuginfo files _WITHOUT_ debug info!?!?
	 */
	if (!ret)
		goto more;

	if (ret > 0) {
		int nr_plt = dso__synthesize_plt_symbols(self, map, filter);
		if (nr_plt > 0)
			ret += nr_plt;
	}
out:
	free(name);
	if (ret < 0 && strstr(self->name, " (deleted)") != NULL)
		return 0;
	return ret;
}

static struct map *thread__find_map_by_name(struct thread *self, char *name)
{
	struct rb_node *nd;

	for (nd = rb_first(&self->maps[MAP__FUNCTION]); nd; nd = rb_next(nd)) {
		struct map *map = rb_entry(nd, struct map, rb_node);

		if (map->dso && strcmp(map->dso->name, name) == 0)
			return map;
	}

	return NULL;
}

static int dsos__set_modules_path_dir(char *dirname)
{
	struct dirent *dent;
	DIR *dir = opendir(dirname);

	if (!dir) {
		pr_debug("%s: cannot open %s dir\n", __func__, dirname);
		return -1;
	}

	while ((dent = readdir(dir)) != NULL) {
		char path[PATH_MAX];

		if (dent->d_type == DT_DIR) {
			if (!strcmp(dent->d_name, ".") ||
			    !strcmp(dent->d_name, ".."))
				continue;

			snprintf(path, sizeof(path), "%s/%s",
				 dirname, dent->d_name);
			if (dsos__set_modules_path_dir(path) < 0)
				goto failure;
		} else {
			char *dot = strrchr(dent->d_name, '.'),
			     dso_name[PATH_MAX];
			struct map *map;
			char *long_name;

			if (dot == NULL || strcmp(dot, ".ko"))
				continue;
			snprintf(dso_name, sizeof(dso_name), "[%.*s]",
				 (int)(dot - dent->d_name), dent->d_name);

			strxfrchar(dso_name, '-', '_');
			map = thread__find_map_by_name(kthread, dso_name);
			if (map == NULL)
				continue;

			snprintf(path, sizeof(path), "%s/%s",
				 dirname, dent->d_name);

			long_name = strdup(path);
			if (long_name == NULL)
				goto failure;
			dso__set_long_name(map->dso, long_name);
		}
	}

	return 0;
failure:
	closedir(dir);
	return -1;
}

static int dsos__set_modules_path(void)
{
	struct utsname uts;
	char modules_path[PATH_MAX];

	if (uname(&uts) < 0)
		return -1;

	snprintf(modules_path, sizeof(modules_path), "/lib/modules/%s/kernel",
		 uts.release);

	return dsos__set_modules_path_dir(modules_path);
}

/*
 * Constructor variant for modules (where we know from /proc/modules where
 * they are loaded) and for vmlinux, where only after we load all the
 * symbols we'll know where it starts and ends.
 */
static struct map *map__new2(u64 start, struct dso *dso, enum map_type type)
{
	struct map *self = malloc(sizeof(*self));

	if (self != NULL) {
		/*
		 * ->end will be filled after we load all the symbols
		 */
		map__init(self, type, start, 0, 0, dso);
	}

	return self;
}

static int thread__create_module_maps(struct thread *self)
{
	char *line = NULL;
	size_t n;
	FILE *file = fopen("/proc/modules", "r");
	struct map *map;

	if (file == NULL)
		return -1;

	while (!feof(file)) {
		char name[PATH_MAX];
		u64 start;
		struct dso *dso;
		char *sep;
		int line_len;

		line_len = getline(&line, &n, file);
		if (line_len < 0)
			break;

		if (!line)
			goto out_failure;

		line[--line_len] = '\0'; /* \n */

		sep = strrchr(line, 'x');
		if (sep == NULL)
			continue;

		hex2u64(sep + 1, &start);

		sep = strchr(line, ' ');
		if (sep == NULL)
			continue;

		*sep = '\0';

		snprintf(name, sizeof(name), "[%s]", line);
		dso = dso__new(name);

		if (dso == NULL)
			goto out_delete_line;

		map = map__new2(start, dso, MAP__FUNCTION);
		if (map == NULL) {
			dso__delete(dso);
			goto out_delete_line;
		}

		snprintf(name, sizeof(name),
			 "/sys/module/%s/notes/.note.gnu.build-id", line);
		if (sysfs__read_build_id(name, dso->build_id,
					 sizeof(dso->build_id)) == 0)
			dso->has_build_id = true;

		dso->origin = DSO__ORIG_KMODULE;
		__thread__insert_map(self, map);
		dsos__add(&dsos__kernel, dso);
	}

	free(line);
	fclose(file);

	return dsos__set_modules_path();

out_delete_line:
	free(line);
out_failure:
	return -1;
}

static int dso__load_vmlinux(struct dso *self, struct map *map, struct thread *thread,
			     const char *vmlinux, symbol_filter_t filter)
{
	int err = -1, fd;

	if (self->has_build_id) {
		u8 build_id[BUILD_ID_SIZE];

		if (filename__read_build_id(vmlinux, build_id,
					    sizeof(build_id)) < 0) {
			pr_debug("No build_id in %s, ignoring it\n", vmlinux);
			return -1;
		}
		if (!dso__build_id_equal(self, build_id)) {
			char expected_build_id[BUILD_ID_SIZE * 2 + 1],
			     vmlinux_build_id[BUILD_ID_SIZE * 2 + 1];

			build_id__sprintf(self->build_id,
					  sizeof(self->build_id),
					  expected_build_id);
			build_id__sprintf(build_id, sizeof(build_id),
					  vmlinux_build_id);
			pr_debug("build_id in %s is %s while expected is %s, "
				 "ignoring it\n", vmlinux, vmlinux_build_id,
				 expected_build_id);
			return -1;
		}
	}

	fd = open(vmlinux, O_RDONLY);
	if (fd < 0)
		return -1;

	dso__set_loaded(self, map->type);
	err = dso__load_sym(self, map, thread, self->long_name, fd, filter, 1, 0);
	close(fd);

	return err;
}

static int dso__load_kernel_sym(struct dso *self, struct map *map,
				struct thread *thread, symbol_filter_t filter)
{
	int err;
	bool is_kallsyms;

	if (vmlinux_path != NULL) {
		int i;
		pr_debug("Looking at the vmlinux_path (%d entries long)\n",
			 vmlinux_path__nr_entries);
		for (i = 0; i < vmlinux_path__nr_entries; ++i) {
			err = dso__load_vmlinux(self, map, thread,
						vmlinux_path[i], filter);
			if (err > 0) {
				pr_debug("Using %s for symbols\n",
					 vmlinux_path[i]);
				dso__set_long_name(self,
						   strdup(vmlinux_path[i]));
				goto out_fixup;
			}
		}
	}

	is_kallsyms = self->long_name[0] == '[';
	if (is_kallsyms)
		goto do_kallsyms;

	err = dso__load_vmlinux(self, map, thread, self->long_name, filter);
	if (err <= 0) {
		pr_info("The file %s cannot be used, "
			"trying to use /proc/kallsyms...", self->long_name);
do_kallsyms:
		err = dso__load_kallsyms(self, map, thread, filter);
		if (err > 0 && !is_kallsyms)
                        dso__set_long_name(self, strdup("[kernel.kallsyms]"));
	}

	if (err > 0) {
out_fixup:
		map__fixup_start(map);
		map__fixup_end(map);
	}

	return err;
}

LIST_HEAD(dsos__user);
LIST_HEAD(dsos__kernel);
struct dso *vdso;

static void dsos__add(struct list_head *head, struct dso *dso)
{
	list_add_tail(&dso->node, head);
}

static struct dso *dsos__find(struct list_head *head, const char *name)
{
	struct dso *pos;

	list_for_each_entry(pos, head, node)
		if (strcmp(pos->name, name) == 0)
			return pos;
	return NULL;
}

struct dso *dsos__findnew(const char *name)
{
	struct dso *dso = dsos__find(&dsos__user, name);

	if (!dso) {
		dso = dso__new(name);
		if (dso != NULL) {
			dsos__add(&dsos__user, dso);
			dso__set_basename(dso);
		}
	}

	return dso;
}

static void __dsos__fprintf(struct list_head *head, FILE *fp)
{
	struct dso *pos;

	list_for_each_entry(pos, head, node) {
		int i;
		for (i = 0; i < MAP__NR_TYPES; ++i)
			dso__fprintf(pos, i, fp);
	}
}

void dsos__fprintf(FILE *fp)
{
	__dsos__fprintf(&dsos__kernel, fp);
	__dsos__fprintf(&dsos__user, fp);
}

static size_t __dsos__fprintf_buildid(struct list_head *head, FILE *fp)
{
	struct dso *pos;
	size_t ret = 0;

	list_for_each_entry(pos, head, node) {
		ret += dso__fprintf_buildid(pos, fp);
		ret += fprintf(fp, " %s\n", pos->long_name);
	}
	return ret;
}

size_t dsos__fprintf_buildid(FILE *fp)
{
	return (__dsos__fprintf_buildid(&dsos__kernel, fp) +
		__dsos__fprintf_buildid(&dsos__user, fp));
}

static int thread__create_kernel_map(struct thread *self, const char *vmlinux)
{
	struct map *kmap;
	struct dso *kernel = dso__new(vmlinux ?: "[kernel.kallsyms]");

	if (kernel == NULL)
		return -1;

	kmap = map__new2(0, kernel, MAP__FUNCTION);
	if (kmap == NULL)
		goto out_delete_kernel_dso;

	kmap->map_ip	   = kmap->unmap_ip = identity__map_ip;
	kernel->short_name = "[kernel]";
	kernel->kernel	   = 1;

	vdso = dso__new("[vdso]");
	if (vdso == NULL)
		goto out_delete_kernel_map;
	dso__set_loaded(vdso, MAP__FUNCTION);

	if (sysfs__read_build_id("/sys/kernel/notes", kernel->build_id,
				 sizeof(kernel->build_id)) == 0)
		kernel->has_build_id = true;

	__thread__insert_map(self, kmap);
	dsos__add(&dsos__kernel, kernel);
	dsos__add(&dsos__user, vdso);

	return 0;

out_delete_kernel_map:
	map__delete(kmap);
out_delete_kernel_dso:
	dso__delete(kernel);
	return -1;
}

static void vmlinux_path__exit(void)
{
	while (--vmlinux_path__nr_entries >= 0) {
		free(vmlinux_path[vmlinux_path__nr_entries]);
		vmlinux_path[vmlinux_path__nr_entries] = NULL;
	}

	free(vmlinux_path);
	vmlinux_path = NULL;
}

static int vmlinux_path__init(void)
{
	struct utsname uts;
	char bf[PATH_MAX];

	if (uname(&uts) < 0)
		return -1;

	vmlinux_path = malloc(sizeof(char *) * 5);
	if (vmlinux_path == NULL)
		return -1;

	vmlinux_path[vmlinux_path__nr_entries] = strdup("vmlinux");
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		goto out_fail;
	++vmlinux_path__nr_entries;
	vmlinux_path[vmlinux_path__nr_entries] = strdup("/boot/vmlinux");
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		goto out_fail;
	++vmlinux_path__nr_entries;
	snprintf(bf, sizeof(bf), "/boot/vmlinux-%s", uts.release);
	vmlinux_path[vmlinux_path__nr_entries] = strdup(bf);
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		goto out_fail;
	++vmlinux_path__nr_entries;
	snprintf(bf, sizeof(bf), "/lib/modules/%s/build/vmlinux", uts.release);
	vmlinux_path[vmlinux_path__nr_entries] = strdup(bf);
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		goto out_fail;
	++vmlinux_path__nr_entries;
	snprintf(bf, sizeof(bf), "/usr/lib/debug/lib/modules/%s/vmlinux",
		 uts.release);
	vmlinux_path[vmlinux_path__nr_entries] = strdup(bf);
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		goto out_fail;
	++vmlinux_path__nr_entries;

	return 0;

out_fail:
	vmlinux_path__exit();
	return -1;
}

int symbol__init(struct symbol_conf *conf)
{
	const struct symbol_conf *pconf = conf ?: &symbol_conf__defaults;

	elf_version(EV_CURRENT);
	symbol__priv_size = pconf->priv_size;
	thread__init(kthread, 0);

	if (pconf->try_vmlinux_path && vmlinux_path__init() < 0)
		return -1;

	if (thread__create_kernel_map(kthread, pconf->vmlinux_name) < 0) {
		vmlinux_path__exit();
		return -1;
	}

	kthread->use_modules = pconf->use_modules;
	if (pconf->use_modules && thread__create_module_maps(kthread) < 0)
		pr_debug("Failed to load list of modules in use, "
			 "continuing...\n");
	/*
	 * Now that we have all the maps created, just set the ->end of them:
	 */
	thread__fixup_maps_end(kthread);
	return 0;
}