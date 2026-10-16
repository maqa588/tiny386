/*
 * isofs.c - streaming ISO9660 view over a host folder (virtual CD-ROM).
 *
 * The volume metadata (volume descriptors, path tables, directory records) is
 * generated once into a small RAM buffer; file contents are read straight
 * from the host/SD files on demand, so RAM usage stays at a few tens of KB
 * regardless of the folder size.
 *
 * ISO9660 Level 1 only (uppercase 8.3 identifiers), which MSCDEX / Win9x
 * CDFS and Linux iso9660 all understand natively.
 */

#include "isofs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ISO_SECTOR	2048
#define ISO_MAX_FILES	512
#define ISO_MAX_DIRS	128
#define ISO_MAX_DEPTH	6
#define ISO_PATH_MAX	512
#define ISO_ID_MAX	16

typedef struct IsoNode {
	struct IsoNode *next;
	int is_dir;
	char id[ISO_ID_MAX];		/* ISO level-1 identifier (no ";1") */
	char *host_path;		/* files only */
	uint32_t size;			/* files: byte size; dirs: extent length */
	uint32_t sectors;		/* allocated 2048-byte sectors */
	uint32_t lba;			/* extent start LBA */
	int path_index;			/* dirs: BFS index */
	uint32_t pt_parent;		/* dirs: 1-based parent path index */
	struct IsoNode *parent;		/* dirs */
	struct IsoNode *children;	/* dirs: child list */
} IsoNode;

typedef struct {
	const char *const *forbidden;
	int file_count;
	int dir_count;
} IsoScan;

struct ISOFolder {
	IsoNode *root;
	IsoNode *dirs[ISO_MAX_DIRS];
	int dir_count;
	int file_count;

	uint32_t pt_size;
	uint32_t pt_sectors;
	uint32_t pt_lba_l;
	uint32_t pt_lba_m;
	uint32_t root_lba;
	uint32_t meta_lba;		/* first file-data LBA (= metadata end) */
	uint32_t total_lba;

	uint8_t *meta;			/* bytes [0, meta_lba * ISO_SECTOR) */

	IsoNode *files[ISO_MAX_FILES];	/* ordered by ascending LBA */

	FILE *cache_fp;
	char cache_path[ISO_PATH_MAX];
};

/* ---- little/big/both-endian field writers ---- */

static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (v >> 8) & 0xff; p[1] = v & 0xff; }
static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_both16(uint8_t *p, uint16_t v) { put_le16(p, v); put_be16(p + 2, v); }
static void put_both32(uint8_t *p, uint32_t v) { put_le32(p, v); put_be32(p + 4, v); }

/* ---- ISO level-1 name handling ---- */

static char up_char(char c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32;
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		return c;
	return '_';
}

static void make_id(const char *name, int is_dir, char *out)
{
	if (is_dir) {
		int o = 0;
		for (const char *s = name; *s && o < 8; s++)
			out[o++] = up_char(*s);
		if (o == 0)
			out[o++] = '_';
		out[o] = 0;
		return;
	}

	const char *dot = strrchr(name, '.');
	const char *ext = NULL;
	int blen, elen = 0;
	if (dot && dot != name && dot[1]) {
		blen = (int)(dot - name);
		ext = dot + 1;
		elen = (int)strlen(ext);
	} else {
		blen = (int)strlen(name);
	}
	if (blen > 8)
		blen = 8;
	if (elen > 3)
		elen = 3;

	int o = 0;
	for (int i = 0; i < blen; i++)
		out[o++] = up_char(name[i]);
	if (o == 0)
		out[o++] = '_';
	if (elen) {
		out[o++] = '.';
		for (int i = 0; i < elen; i++)
			out[o++] = up_char(ext[i]);
	}
	out[o] = 0;
}

static int id_used(IsoNode *list, const char *id, IsoNode *skip)
{
	for (IsoNode *c = list; c; c = c->next) {
		if (c == skip)
			continue;
		if (strcmp(c->id, id) == 0)
			return 1;
	}
	return 0;
}

static void unique_id(IsoNode *list, IsoNode *n)
{
	if (!id_used(list, n->id, n))
		return;

	char base[16] = {0}, ext[8] = {0};
	char *dot = strchr(n->id, '.');
	if (dot) {
		int bl = (int)(dot - n->id);
		if (bl > 15)
			bl = 15;
		memcpy(base, n->id, bl);
		strncpy(ext, dot, sizeof(ext) - 1);
	} else {
		strncpy(base, n->id, sizeof(base) - 1);
	}

	for (int i = 1; i < 1000; i++) {
		char num[8];
		snprintf(num, sizeof(num), "%d", i);
		int nd = (int)strlen(num);
		int keep = 8 - nd;
		if (keep < 0)
			keep = 0;

		char cand[24];
		int o = 0;
		for (int k = 0; k < keep && base[k]; k++)
			cand[o++] = base[k];
		for (int k = 0; k < nd; k++)
			cand[o++] = num[k];
		if (ext[0]) {
			int el = (int)strlen(ext);
			if (el > 4)
				el = 4;
			for (int k = 0; k < el; k++)
				cand[o++] = ext[k];
		}
		cand[o] = 0;
		strncpy(n->id, cand, ISO_ID_MAX - 1);
		n->id[ISO_ID_MAX - 1] = 0;
		if (!id_used(list, n->id, n))
			return;
	}
}

/* ---- filtering ---- */

static int ext_filtered(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return 0;
	return !strcasecmp(dot, ".iso") || !strcasecmp(dot, ".img") ||
	       !strcasecmp(dot, ".vhd");
}

static int is_forbidden(const char *full, IsoScan *sc)
{
	if (!sc->forbidden)
		return 0;
	const char *base = strrchr(full, '/');
	base = base ? base + 1 : full;
	for (int i = 0; sc->forbidden[i]; i++) {
		const char *f = sc->forbidden[i];
		if (strcmp(f, full) == 0)
			return 1;
		const char *fb = strrchr(f, '/');
		fb = fb ? fb + 1 : f;
		if (strcmp(fb, base) == 0)
			return 1;
	}
	return 0;
}

/* ---- directory scanning ---- */

typedef struct {
	char *name;
	int is_dir;
	int64_t size;
} ScanEnt;

static int cmp_ent(const void *a, const void *b)
{
	return strcasecmp(((const ScanEnt *)a)->name, ((const ScanEnt *)b)->name);
}

static IsoNode *scan_dir(const char *host_path, IsoNode *dir, int depth, IsoScan *sc)
{
	DIR *d = opendir(host_path);
	if (!d)
		return NULL;

	ScanEnt *ents = malloc(sizeof(ScanEnt) * 256);
	int n = 0;

	struct dirent *ent;
	while (n < 256 && (ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (ext_filtered(ent->d_name))
			continue;

		char full[ISO_PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", host_path, ent->d_name);
		if (is_forbidden(full, sc))
			continue;

		struct stat st;
		if (stat(full, &st) != 0)
			continue;
		int isd = S_ISDIR(st.st_mode);
		if (!isd && !S_ISREG(st.st_mode))
			continue;

		ents[n].name = strdup(ent->d_name);
		ents[n].is_dir = isd;
		ents[n].size = isd ? 0 : (int64_t)st.st_size;
		n++;
	}
	closedir(d);

	if (n > 1)
		qsort(ents, n, sizeof(ScanEnt), cmp_ent);

	IsoNode *head = NULL, *tail = NULL;
	for (int i = 0; i < n; i++) {
		if (ents[i].is_dir) {
			if (sc->dir_count >= ISO_MAX_DIRS || depth >= ISO_MAX_DEPTH) {
				free(ents[i].name);
				continue;
			}
		} else {
			if (sc->file_count >= ISO_MAX_FILES) {
				free(ents[i].name);
				continue;
			}
		}

		IsoNode *node = calloc(1, sizeof(IsoNode));
		node->is_dir = ents[i].is_dir;
		node->parent = dir;
		make_id(ents[i].name, ents[i].is_dir, node->id);

		char full[ISO_PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", host_path, ents[i].name);

		if (node->is_dir) {
			sc->dir_count++;
			node->children = scan_dir(full, node, depth + 1, sc);
		} else {
			sc->file_count++;
			node->size = (uint32_t)ents[i].size;
			node->host_path = strdup(full);
		}
		free(ents[i].name);

		if (!head) {
			head = node;
		} else {
			tail->next = node;
		}
		tail = node;
	}

	free(ents);

	/* resolve level-1 identifier collisions within this directory */
	for (IsoNode *c = head; c; c = c->next)
		unique_id(head, c);

	return head;
}

/* ---- layout ---- */

static uint32_t dir_used_bytes(IsoNode *dir)
{
	uint32_t off = 34 + 34; /* "." + ".." */
	for (IsoNode *c = dir->children; c; c = c->next) {
		int idlen = c->is_dir ? (int)strlen(c->id) : (int)strlen(c->id) + 2;
		uint32_t rl = 33 + idlen + ((idlen & 1) ? 0 : 1);
		if ((off % ISO_SECTOR) + rl > ISO_SECTOR)
			off += ISO_SECTOR - (off % ISO_SECTOR);
		off += rl;
	}
	return off;
}

static int layout(ISOFolder *fs)
{
	IsoNode *queue[ISO_MAX_DIRS];
	int qh = 0, qt = 0;

	fs->dir_count = 0;
	fs->root->path_index = 0;
	fs->root->pt_parent = 1;
	queue[qt++] = fs->root;
	while (qh < qt) {
		IsoNode *d = queue[qh++];
		d->path_index = fs->dir_count;
		fs->dirs[fs->dir_count++] = d;
		uint32_t used = dir_used_bytes(d);
		d->sectors = (used + ISO_SECTOR - 1) / ISO_SECTOR;
		if (!d->sectors)
			d->sectors = 1;
		d->size = d->sectors * ISO_SECTOR;
		for (IsoNode *c = d->children; c; c = c->next) {
			if (c->is_dir) {
				c->pt_parent = d->path_index + 1;
				queue[qt++] = c;
			}
		}
	}

	fs->pt_size = 0;
	for (int i = 0; i < fs->dir_count; i++) {
		int idlen = i ? (int)strlen(fs->dirs[i]->id) : 1;
		fs->pt_size += 8 + idlen + (idlen & 1);
	}
	fs->pt_sectors = (fs->pt_size + ISO_SECTOR - 1) / ISO_SECTOR;
	if (!fs->pt_sectors)
		fs->pt_sectors = 1;

	fs->pt_lba_l = 18;
	fs->pt_lba_m = 18 + fs->pt_sectors;

	uint32_t lba = 18 + 2 * fs->pt_sectors;
	fs->root_lba = lba;
	for (int i = 0; i < fs->dir_count; i++) {
		fs->dirs[i]->lba = lba;
		lba += fs->dirs[i]->sectors;
	}

	fs->meta_lba = lba;
	fs->file_count = 0;
	qh = qt = 0;
	queue[qt++] = fs->root;
	while (qh < qt) {
		IsoNode *d = queue[qh++];
		for (IsoNode *c = d->children; c; c = c->next) {
			if (c->is_dir) {
				queue[qt++] = c;
				continue;
			}
			c->sectors = (c->size + ISO_SECTOR - 1) / ISO_SECTOR;
			c->lba = lba;
			lba += c->sectors;
			if (fs->file_count < ISO_MAX_FILES)
				fs->files[fs->file_count++] = c;
		}
	}
	fs->total_lba = lba;

	fs->meta = calloc((size_t)fs->meta_lba * ISO_SECTOR, 1);
	return fs->meta ? 0 : -1;
}

/* ---- metadata emission ---- */

static uint32_t write_dir_record(uint8_t *p, uint32_t extent_lba, uint32_t data_len,
				 int is_dir, const uint8_t *id, int idlen)
{
	memset(p, 0, 33 + idlen + 1);
	p[0] = 33 + idlen + ((idlen & 1) ? 0 : 1);
	p[1] = 0;
	put_both32(p + 2, extent_lba);
	put_both32(p + 10, data_len);
	p[18] = 120; /* years since 1900 (2020) */
	p[19] = 1;
	p[20] = 1;
	/* p[21..24] hour/min/sec/gmt = 0 */
	p[25] = is_dir ? 0x02 : 0x00;
	put_both16(p + 28, 1);
	p[32] = (uint8_t)idlen;
	memcpy(p + 33, id, idlen);
	return p[0];
}

static void emit_descriptors(ISOFolder *fs, const char *volname)
{
	static const uint8_t dot_id[1] = { 0x00 };

	uint8_t *p = fs->meta + 16 * ISO_SECTOR;
	memset(p, 0, ISO_SECTOR);
	p[0] = 1;
	memcpy(p + 1, "CD001", 5);
	p[6] = 1;
	memset(p + 8, ' ', 32); /* system identifier */
	char v[32];
	memset(v, ' ', sizeof(v));
	int vi = 0;
	for (const char *s = volname; *s && vi < 32; s++)
		v[vi++] = up_char(*s);
	memcpy(p + 40, v, 32); /* volume identifier */
	put_both32(p + 80, fs->total_lba);
	put_both16(p + 120, 1); /* volume set size */
	put_both16(p + 124, 1); /* volume sequence number */
	put_both16(p + 128, ISO_SECTOR);
	put_both32(p + 132, fs->pt_size);
	put_le32(p + 140, fs->pt_lba_l);
	put_le32(p + 144, 0);
	put_be32(p + 148, fs->pt_lba_m);
	put_be32(p + 152, 0);
	write_dir_record(p + 156, fs->root_lba, fs->root->size, 1, dot_id, 1);
	p[881] = 1; /* file structure version */

	/* volume descriptor set terminator */
	uint8_t *t = fs->meta + 17 * ISO_SECTOR;
	memset(t, 0, ISO_SECTOR);
	t[0] = 0xff;
	memcpy(t + 1, "CD001", 5);
	t[6] = 1;
}

static void emit_path_tables(ISOFolder *fs)
{
	for (int big = 0; big < 2; big++) {
		uint8_t *base = fs->meta +
			(uint64_t)(big ? fs->pt_lba_m : fs->pt_lba_l) * ISO_SECTOR;
		uint32_t off = 0;
		for (int i = 0; i < fs->dir_count; i++) {
			IsoNode *d = fs->dirs[i];
			uint8_t *p = base + off;
			uint8_t zero = 0;
			int idlen = i ? (int)strlen(d->id) : 1;
			const uint8_t *idp = i ? (const uint8_t *)d->id : &zero;
			p[0] = (uint8_t)idlen;
			p[1] = 0;
			if (big) {
				put_be32(p + 2, d->lba);
				put_be16(p + 6, (uint16_t)d->pt_parent);
			} else {
				put_le32(p + 2, d->lba);
				put_le16(p + 6, (uint16_t)d->pt_parent);
			}
			memcpy(p + 8, idp, idlen);
			off += 8 + idlen + (idlen & 1);
		}
	}
}

static void emit_dirs(ISOFolder *fs)
{
	static const uint8_t dot_id[1] = { 0x00 };
	static const uint8_t dotdot_id[1] = { 0x01 };

	for (int i = 0; i < fs->dir_count; i++) {
		IsoNode *d = fs->dirs[i];
		uint8_t *base = fs->meta + (uint64_t)d->lba * ISO_SECTOR;
		uint32_t off = 0;

		off += write_dir_record(base + off, d->lba, d->size, 1, dot_id, 1);
		uint32_t plba = d->parent ? d->parent->lba : d->lba;
		uint32_t psize = d->parent ? d->parent->size : d->size;
		off += write_dir_record(base + off, plba, psize, 1, dotdot_id, 1);

		for (IsoNode *c = d->children; c; c = c->next) {
			char idbuf[ISO_ID_MAX + 2];
			const uint8_t *idp;
			int idlen;
			if (c->is_dir) {
				idlen = (int)strlen(c->id);
				idp = (const uint8_t *)c->id;
			} else {
				snprintf(idbuf, sizeof(idbuf), "%s;1", c->id);
				idlen = (int)strlen(idbuf);
				idp = (const uint8_t *)idbuf;
			}
			uint32_t rl = 33 + idlen + ((idlen & 1) ? 0 : 1);
			if ((off % ISO_SECTOR) + rl > ISO_SECTOR)
				off += ISO_SECTOR - (off % ISO_SECTOR);
			off += write_dir_record(base + off, c->lba, c->size, c->is_dir, idp, idlen);
		}
	}
}

/* ---- on-demand file reading ---- */

static IsoNode *find_file(ISOFolder *fs, uint64_t off)
{
	int lo = 0, hi = fs->file_count - 1, res = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint64_t s = (uint64_t)fs->files[mid]->lba * ISO_SECTOR;
		if (s <= off) {
			res = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if (res < 0)
		return NULL;
	IsoNode *f = fs->files[res];
	uint64_t s = (uint64_t)f->lba * ISO_SECTOR;
	if (off < s + (uint64_t)f->sectors * ISO_SECTOR)
		return f;
	return NULL;
}

static int host_read(ISOFolder *fs, const char *path, uint64_t off, void *buf, uint32_t n)
{
	if (!fs->cache_fp || strcmp(fs->cache_path, path) != 0) {
		if (fs->cache_fp)
			fclose(fs->cache_fp);
		fs->cache_fp = NULL;
		fs->cache_path[0] = 0;
		FILE *f = fopen(path, "rb");
		if (!f)
			return -1;
		fs->cache_fp = f;
		strncpy(fs->cache_path, path, sizeof(fs->cache_path) - 1);
		fs->cache_path[sizeof(fs->cache_path) - 1] = 0;
	}
	if (fseeko(fs->cache_fp, (off_t)off, SEEK_SET) != 0)
		return -1;
	if (fread(buf, 1, n, fs->cache_fp) != n)
		return -1;
	return 0;
}

/* ---- public API ---- */

static void free_node(IsoNode *n)
{
	while (n) {
		IsoNode *nx = n->next;
		if (n->children)
			free_node(n->children);
		free(n->host_path);
		free(n);
		n = nx;
	}
}

ISOFolder *isofs_open(const char *folder, const char *const *forbidden)
{
	if (!folder || !folder[0])
		return NULL;

	struct stat st;
	if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode))
		return NULL;

	ISOFolder *fs = calloc(1, sizeof(ISOFolder));
	if (!fs)
		return NULL;

	IsoScan sc;
	memset(&sc, 0, sizeof(sc));
	sc.forbidden = forbidden;

	IsoNode *root = calloc(1, sizeof(IsoNode));
	if (!root) {
		free(fs);
		return NULL;
	}
	root->is_dir = 1;
	root->children = scan_dir(folder, root, 0, &sc);
	fs->root = root;

	if (!root->children) { /* empty folder: nothing to mount */
		free_node(root);
		free(fs);
		return NULL;
	}

	if (layout(fs) != 0) {
		free_node(root);
		free(fs);
		return NULL;
	}

	const char *bn = strrchr(folder, '/');
	bn = bn ? bn + 1 : folder;
	emit_descriptors(fs, bn);
	emit_path_tables(fs);
	emit_dirs(fs);

	return fs;
}

void isofs_close(ISOFolder *fs)
{
	if (!fs)
		return;
	if (fs->cache_fp)
		fclose(fs->cache_fp);
	free_node(fs->root);
	free(fs->meta);
	free(fs);
}

uint64_t isofs_size(ISOFolder *fs)
{
	if (!fs)
		return 0;
	return (uint64_t)fs->total_lba * ISO_SECTOR;
}

int isofs_file_count(ISOFolder *fs)
{
	return fs ? fs->file_count : 0;
}

int isofs_read(ISOFolder *fs, uint64_t offset, void *buf, uint32_t len)
{
	if (!fs || !fs->meta)
		return -1;

	uint64_t meta_bytes = (uint64_t)fs->meta_lba * ISO_SECTOR;
	uint64_t total = (uint64_t)fs->total_lba * ISO_SECTOR;
	uint8_t *out = buf;

	while (len) {
		uint32_t n;
		if (offset < meta_bytes) {
			uint64_t avail = meta_bytes - offset;
			n = avail < len ? (uint32_t)avail : len;
			memcpy(out, fs->meta + offset, n);
		} else if (offset < total) {
			IsoNode *f = find_file(fs, offset);
			if (!f) {
				n = len;
				memset(out, 0, n);
			} else {
				uint64_t fsb = (uint64_t)f->lba * ISO_SECTOR;
				uint64_t end = fsb + f->size;
				if (offset < end) {
					uint64_t avail = end - offset;
					n = avail < len ? (uint32_t)avail : len;
					if (host_read(fs, f->host_path, offset - fsb, out, n) != 0)
						return -1;
				} else {
					n = len;
					memset(out, 0, n);
				}
			}
		} else {
			n = len;
			memset(out, 0, n);
		}
		out += n;
		offset += n;
		len -= n;
	}
	return 0;
}
