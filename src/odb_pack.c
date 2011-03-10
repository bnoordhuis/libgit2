/*
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "common.h"
#include "git2/zlib.h"
#include "git2/repository.h"
#include "fileops.h"
#include "hash.h"
#include "odb.h"
#include "delta-apply.h"

#include "git2/odb_backend.h"

#define DEFAULT_WINDOW_SIZE \
	(sizeof(void*) >= 8 \
		?  1 * 1024 * 1024 * 1024 \
		: 32 * 1024 * 1024)

#define DEFAULT_MAPPED_LIMIT \
	((1024L * 1024L) * (sizeof(void*) >= 8 ? 8192 : 256))

#define PACK_SIGNATURE 0x5041434b	/* "PACK" */
#define PACK_VERSION 2
#define pack_version_ok(v) ((v) == htonl(2) || (v) == htonl(3))
struct pack_header {
	uint32_t hdr_signature;
	uint32_t hdr_version;
	uint32_t hdr_entries;
};

/*
 * The first four bytes of index formats later than version 1 should
 * start with this signature, as all older git binaries would find this
 * value illegal and abort reading the file.
 *
 * This is the case because the number of objects in a packfile
 * cannot exceed 1,431,660,000 as every object would need at least
 * 3 bytes of data and the overall packfile cannot exceed 4 GiB with
 * version 1 of the index file due to the offsets limited to 32 bits.
 * Clearly the signature exceeds this maximum.
 *
 * Very old git binaries will also compare the first 4 bytes to the
 * next 4 bytes in the index and abort with a "non-monotonic index"
 * error if the second 4 byte word is smaller than the first 4
 * byte word.  This would be true in the proposed future index
 * format as idx_signature would be greater than idx_version.
 */
#define PACK_IDX_SIGNATURE 0xff744f63	/* "\377tOc" */

struct pack_idx_header {
	uint32_t idx_signature;
	uint32_t idx_version;
};

struct pack_window {
	struct pack_window *next;
	git_map window_map;
	off_t offset;
	unsigned int last_used;
	unsigned int inuse_cnt;
};

struct packed_git {
	struct pack_window *windows;
	off_t pack_size;

	git_map index_map;

	uint32_t num_objects;
	uint32_t num_bad_objects;
	git_oid *bad_object_sha1; /* array of git_oid */

	int index_version;
	time_t mtime;
	int pack_fd;
	unsigned pack_local:1, pack_keep:1;
	git_oid sha1;

	/* something like ".git/objects/pack/xxxxx.pack" */
	char pack_name[GIT_FLEX_ARRAY]; /* more */
};

struct pack_entry {
	off_t offset;
	git_oid sha1;
	struct packed_git *p;
};

typedef struct {
	git_odb_backend parent;
	git_vector packs;
	struct packed_git *last_found;

	size_t window_size; /* needs default value */

	size_t mapped_limit; /* needs default value */
	size_t peak_mapped;
	size_t mapped;

	size_t used_ctr;

	unsigned int peak_open_windows;
	unsigned int open_windows;

	unsigned int mmap_calls;
} pack_backend;


static int in_window(struct pack_window *win, off_t offset);
static void scan_windows(
	struct packed_git *p,
	struct packed_git **lru_p,
	struct pack_window **lru_w,
	struct pack_window **lru_l);
static int unuse_one_window(
		pack_backend *backend,
		struct packed_git *current,
		git_file keep_fd);


static int sort_pack(const void *a_, const void *b_);
static void unuse_pack(struct pack_window **w_cursor);
static unsigned char *use_pack(
		pack_backend *backend,
		struct packed_git *p,
		struct pack_window **w_cursor,
		off_t offset,
		unsigned int *left);
static int check_pack_index(const char *path,  struct packed_git *p);
static int open_pack_index(struct packed_git *p);
static struct packed_git *alloc_packfile(int extra);
static int open_packfile(struct packed_git *p);
static int process_packfile(struct packed_git **pack_out, const char *path, int local);
static int load_packfile(void *_data, char *path);
static int load_packs(pack_backend *backend, const char *odb_path, int local);
static off_t nth_packed_object_offset(const struct packed_git *p, uint32_t n);
static int find_pack_entry_offset(
		off_t *offset_out,
		struct packed_git *p,
		const git_oid *oid);
static int find_pack_entry_in_pack(
		struct pack_entry *e,
		struct packed_git *p,
		const git_oid *oid);
static int find_pack_entry(
		struct pack_entry *e,
		pack_backend *backend,
		const git_oid *oid);
static unsigned long unpack_object_header_buffer(const unsigned char *buf,
		unsigned long len, git_otype *type, size_t *sizep);
static int unpack_object_header(
				git_rawobj *object,
				pack_backend *backend,
				struct packed_git *p,
				struct pack_window **w_curs,
				off_t *curpos);
static int unpack_entry(
		git_rawobj *obj,
		pack_backend *backend,
		struct packed_git *p,
		off_t obj_offset);


static int in_window(struct pack_window *win, off_t offset)
{
	/* We must promise at least 20 bytes (one hash) after the
	 * offset is available from this window, otherwise the offset
	 * is not actually in this window and a different window (which
	 * has that one hash excess) must be used.  This is to support
	 * the object header and delta base parsing routines below.
	 */
	off_t win_off = win->offset;
	return win_off <= offset
		&& (offset + 20) <= (off_t)(win_off + win->window_map.len);
}

static void scan_windows(
	struct packed_git *p,
	struct packed_git **lru_p,
	struct pack_window **lru_w,
	struct pack_window **lru_l)
{
	struct pack_window *w, *w_l;

	for (w_l = NULL, w = p->windows; w; w = w->next) {
		if (!w->inuse_cnt) {
			if (!*lru_w || w->last_used < (*lru_w)->last_used) {
				*lru_p = p;
				*lru_w = w;
				*lru_l = w_l;
			}
		}
		w_l = w;
	}
}

static int unuse_one_window(
		pack_backend *backend,
		struct packed_git *current,
		git_file keep_fd)
{
	struct packed_git *lru_p = NULL;
	struct pack_window *lru_w = NULL, *lru_l = NULL;
	size_t i;

	if (current)
		scan_windows(current, &lru_p, &lru_w, &lru_l);

	for (i = 0; i < backend->packs.length; ++i)
		scan_windows(git_vector_get(&backend->packs, i), &lru_p, &lru_w, &lru_l);

	if (lru_p) {
		backend->mapped -= lru_w->window_map.len;
		gitfo_free_map(&lru_w->window_map);

		if (lru_l)
			lru_l->next = lru_w->next;
		else {
			lru_p->windows = lru_w->next;
			if (!lru_p->windows && lru_p->pack_fd != keep_fd) {
				gitfo_close(lru_p->pack_fd);
				lru_p->pack_fd = -1;
			}
		}

		free(lru_w);
		backend->open_windows--;
		return GIT_SUCCESS;
	}

	return GIT_ERROR;
}




static int sort_pack(const void *a_, const void *b_)
{
	struct packed_git *a = *((struct packed_git **)a_);
	struct packed_git *b = *((struct packed_git **)b_);
	int st;

	/*
	 * Local packs tend to contain objects specific to our
	 * variant of the project than remote ones.  In addition,
	 * remote ones could be on a network mounted filesystem.
	 * Favor local ones for these reasons.
	 */
	st = a->pack_local - b->pack_local;
	if (st)
		return -st;

	/*
	 * Younger packs tend to contain more recent objects,
	 * and more recent objects tend to get accessed more
	 * often.
	 */
	if (a->mtime < b->mtime)
		return 1;
	else if (a->mtime == b->mtime)
		return 0;

	return -1;
}

static void unuse_pack(struct pack_window **w_cursor)
{
	struct pack_window *w = *w_cursor;
	if (w) {
		w->inuse_cnt--;
		*w_cursor = NULL;
	}
}

static unsigned char *use_pack(
		pack_backend *backend,
		struct packed_git *p,
		struct pack_window **w_cursor,
		off_t offset,
		unsigned int *left)
{
	struct pack_window *win = *w_cursor;

	if (p->pack_fd == -1 && open_packfile(p) < GIT_SUCCESS)
		return NULL;

	/* Since packfiles end in a hash of their content and it's
	 * pointless to ask for an offset into the middle of that
	 * hash, and the in_window function above wouldn't match
	 * don't allow an offset too close to the end of the file.
	 */
	if (offset > (p->pack_size - 20))
		return NULL;

	if (!win || !in_window(win, offset)) {

		if (win)
			win->inuse_cnt--;

		for (win = p->windows; win; win = win->next) {
			if (in_window(win, offset))
				break;
		}

		if (!win) {
			size_t window_align = backend->window_size / 2;
			size_t len;

			win = git__calloc(1, sizeof(*win));
			win->offset = (offset / window_align) * window_align;

			len = (size_t)(p->pack_size - win->offset);
			if (len > backend->window_size)
				len = backend->window_size;

			backend->mapped += len;

			while (backend->mapped_limit < backend->mapped &&
				unuse_one_window(backend, p, p->pack_fd) == GIT_SUCCESS) {}

			if (gitfo_map_ro(&win->window_map, p->pack_fd,
					win->offset, len) < GIT_SUCCESS)
				return NULL;

			backend->mmap_calls++;
			backend->open_windows++;

			if (backend->mapped > backend->peak_mapped)
				backend->peak_mapped = backend->mapped;

			if (backend->open_windows > backend->peak_open_windows)
				backend->peak_open_windows = backend->open_windows;

			win->next = p->windows;
			p->windows = win;
		}
	}

	if (win != *w_cursor) {
		win->last_used = backend->used_ctr++;
		win->inuse_cnt++;
		*w_cursor = win;
	}

	offset -= win->offset;
	assert(git__is_sizet(offset));

	if (left)
		*left = win->window_map.len - (size_t)offset;

	return (unsigned char *)win->window_map.data + offset;
}


static int check_pack_index(const char *path,  struct packed_git *p)
{
	struct pack_idx_header *hdr;
	uint32_t version, nr, i, *index;

	void *idx_map;
	size_t idx_size;

	struct stat st;

	/* TODO: properly open the file without access time */
	git_file fd = gitfo_open(path, O_RDONLY /*| O_NOATIME */);

	int error;

	if (fd < 0)
		return GIT_EOSERR;

	if (gitfo_fstat(fd, &st) < GIT_SUCCESS) {
		gitfo_close(fd);
		return GIT_EOSERR;
	}

	if (!git__is_sizet(st.st_size))
		return GIT_ENOMEM;

	idx_size = (size_t)st.st_size;

	if (idx_size < 4 * 256 + 20 + 20) {
		gitfo_close(fd);
		return GIT_EOBJCORRUPTED;
	}

	error = gitfo_map_ro(&p->index_map, fd, 0, idx_size);
	gitfo_close(fd);

	if (error < GIT_SUCCESS)
		return error;

	hdr = idx_map = p->index_map.data;

	if (hdr->idx_signature == htonl(PACK_IDX_SIGNATURE)) {
		version = ntohl(hdr->idx_version);

		if (version < 2 || version > 2) {
			gitfo_free_map(&p->index_map);
			return GIT_EOBJCORRUPTED; /* unsupported index version */
		}

	} else
		version = 1;

	nr = 0;
	index = idx_map;

	if (version > 1)
		index += 2;  /* skip index header */

	for (i = 0; i < 256; i++) {
		uint32_t n = ntohl(index[i]);
		if (n < nr) {
			gitfo_free_map(&p->index_map);
			return GIT_EOBJCORRUPTED; /* non-monotonic index */
		}
		nr = n;
	}

	if (version == 1) {
		/*
		 * Total size:
		 *  - 256 index entries 4 bytes each
		 *  - 24-byte entries * nr (20-byte sha1 + 4-byte offset)
		 *  - 20-byte SHA1 of the packfile
		 *  - 20-byte SHA1 file checksum
		 */
		if (idx_size != 4*256 + nr * 24 + 20 + 20) {
			gitfo_free_map(&p->index_map);
			return GIT_EOBJCORRUPTED;
		}
	} else if (version == 2) {
		/*
		 * Minimum size:
		 *  - 8 bytes of header
		 *  - 256 index entries 4 bytes each
		 *  - 20-byte sha1 entry * nr
		 *  - 4-byte crc entry * nr
		 *  - 4-byte offset entry * nr
		 *  - 20-byte SHA1 of the packfile
		 *  - 20-byte SHA1 file checksum
		 * And after the 4-byte offset table might be a
		 * variable sized table containing 8-byte entries
		 * for offsets larger than 2^31.
		 */
		unsigned long min_size = 8 + 4*256 + nr*(20 + 4 + 4) + 20 + 20;
		unsigned long max_size = min_size;

		if (nr)
			max_size += (nr - 1)*8;

		if (idx_size < min_size || idx_size > max_size) {
			gitfo_free_map(&p->index_map);
			return GIT_EOBJCORRUPTED;
		}

		/* Make sure that off_t is big enough to access the whole pack...
		 * Is this an issue in libgit2? It shouldn't. */
		if (idx_size != min_size && (sizeof(off_t) <= 4)) {
			gitfo_free_map(&p->index_map);
			return GIT_EOSERR;
		}
	}

	p->index_version = version;
	p->num_objects = nr;
	return GIT_SUCCESS;
}

static int open_pack_index(struct packed_git *p)
{
	char *idx_name;
	int error;

	if (p->index_map.data)
		return GIT_SUCCESS;

	idx_name = git__strdup(p->pack_name);
	strcpy(idx_name + strlen(idx_name) - STRLEN(".pack"), ".idx");

	error = check_pack_index(idx_name, p);
	free(idx_name);

	return error;
}

static struct packed_git *alloc_packfile(int extra)
{
	struct packed_git *p = git__malloc(sizeof(*p) + extra);
	memset(p, 0, sizeof(*p));
	p->pack_fd = -1;
	return p;
}

static int open_packfile(struct packed_git *p)
{
	struct stat st;
	struct pack_header hdr;
	git_oid sha1;
	unsigned char *idx_sha1;

	if (!p->index_map.data && open_pack_index(p) < GIT_SUCCESS)
		return GIT_ENOTFOUND;

	/* TODO: open with noatime */
	p->pack_fd = gitfo_open(p->pack_name, O_RDONLY);
	if (p->pack_fd < 0 || gitfo_fstat(p->pack_fd, &st) < GIT_SUCCESS)
		return GIT_EOSERR;

	/* If we created the struct before we had the pack we lack size. */
	if (!p->pack_size) {
		if (!S_ISREG(st.st_mode))
			goto cleanup;
		p->pack_size = (off_t)st.st_size;
	} else if (p->pack_size != st.st_size)
		goto cleanup;

#if 0
	/* We leave these file descriptors open with sliding mmap;
	 * there is no point keeping them open across exec(), though.
	 */
	fd_flag = fcntl(p->pack_fd, F_GETFD, 0);
	if (fd_flag < 0)
		return error("cannot determine file descriptor flags");

	fd_flag |= FD_CLOEXEC;
	if (fcntl(p->pack_fd, F_SETFD, fd_flag) == -1)
		return GIT_EOSERR;
#endif

	/* Verify we recognize this pack file format. */
	if (gitfo_read(p->pack_fd, &hdr, sizeof(hdr)) < GIT_SUCCESS)
		goto cleanup;

	if (hdr.hdr_signature != htonl(PACK_SIGNATURE))
		goto cleanup;

	if (!pack_version_ok(hdr.hdr_version))
		goto cleanup;

	/* Verify the pack matches its index. */
	if (p->num_objects != ntohl(hdr.hdr_entries))
		goto cleanup;

	if (gitfo_lseek(p->pack_fd, p->pack_size - GIT_OID_RAWSZ, SEEK_SET) == -1)
		goto cleanup;

	if (gitfo_read(p->pack_fd, sha1.id, GIT_OID_RAWSZ) < GIT_SUCCESS)
		goto cleanup;

	idx_sha1 = ((unsigned char *)p->index_map.data) + p->index_map.len - 40;

	if (git_oid_cmp(&sha1, (git_oid *)idx_sha1) != 0)
		goto cleanup;

	return GIT_SUCCESS;	

cleanup:
	gitfo_close(p->pack_fd);
	p->pack_fd = -1;
	return GIT_EPACKCORRUPTED;
}

static int process_packfile(struct packed_git **pack_out, const char *path, int local)
{
	struct stat st;
	struct packed_git *p;
	size_t path_len;

	*pack_out = NULL;
	path_len = strlen(path);
	p = alloc_packfile(path_len + 2);

	/*
	 * Make sure a corresponding .pack file exists and that
	 * the index looks sane.
	 */
	path_len -= STRLEN(".idx");
	if (path_len < 1) {
		free(p);
		return GIT_ENOTFOUND;
	}

	memcpy(p->pack_name, path, path_len);

	strcpy(p->pack_name + path_len, ".keep");
	if (gitfo_exists(p->pack_name) == GIT_SUCCESS)
		p->pack_keep = 1;

	strcpy(p->pack_name + path_len, ".pack");
	if (gitfo_stat(p->pack_name, &st) < GIT_SUCCESS || !S_ISREG(st.st_mode)) {
		free(p);
		return GIT_ENOTFOUND;
	}

	/* ok, it looks sane as far as we can check without
	 * actually mapping the pack file.
	 */
	p->pack_size = (off_t)st.st_size;
	p->pack_local = local;
	p->mtime = st.st_mtime;

	/* see if we can parse the sha1 oid in the packfile name */
	if (path_len < 40 ||
		git_oid_mkstr(&p->sha1, path + path_len - GIT_OID_HEXSZ) < GIT_SUCCESS)
		memset(&p->sha1, 0x0, GIT_OID_RAWSZ);

	*pack_out = p;
	return GIT_SUCCESS;
}

typedef struct {
	pack_backend *backend;
	int is_pack_local;
} pack_dirent_data;

static int load_packfile(void *_data, char *path)
{
	pack_dirent_data *data = (pack_dirent_data *)_data;
	struct packed_git *pack;
	int error;

	if (git__suffixcmp(path, ".idx") != 0)
		return GIT_SUCCESS; /* not an index */

	/* FIXME: git.git checks for duplicate packs.
	 * But that makes no fucking sense. Our dirent is not
	 * going to generate dupicate entries */

	error = process_packfile(&pack, path, data->is_pack_local);
	if (error < GIT_SUCCESS)
		return error;

	if (git_vector_insert(&data->backend->packs, pack) < GIT_SUCCESS) {
		free(pack);
		return GIT_ENOMEM;
	}

	return GIT_SUCCESS;
}

static int load_packs(pack_backend *backend, const char *odb_path, int local)
{
	int error;
	char path[GIT_PATH_MAX];
	pack_dirent_data data;

	data.backend = backend;
	data.is_pack_local = local;

	git__joinpath(path, odb_path, "pack");
	if (gitfo_isdir(path) < GIT_SUCCESS)
		return GIT_SUCCESS;

	error = gitfo_dirent(path, GIT_PATH_MAX, load_packfile, (void *)&data);
	if (error < GIT_SUCCESS)
		return error;

	git_vector_sort(&backend->packs);
	backend->last_found = git_vector_get(&backend->packs, 0);

	return GIT_SUCCESS;
}

static off_t nth_packed_object_offset(const struct packed_git *p, uint32_t n)
{
	const unsigned char *index = p->index_map.data;
	index += 4 * 256;
	if (p->index_version == 1) {
		return ntohl(*((uint32_t *)(index + 24 * n)));
	} else {
		uint32_t off;
		index += 8 + p->num_objects * (20 + 4);
		off = ntohl(*((uint32_t *)(index + 4 * n)));
		if (!(off & 0x80000000))
			return off;
		index += p->num_objects * 4 + (off & 0x7fffffff) * 8;
		return (((uint64_t)ntohl(*((uint32_t *)(index + 0)))) << 32) |
				   ntohl(*((uint32_t *)(index + 4)));
	}
}

static int find_pack_entry_offset(
		off_t *offset_out,
		struct packed_git *p,
		const git_oid *oid)
{
	const uint32_t *level1_ofs = p->index_map.data;
	const unsigned char *index = p->index_map.data;
	unsigned hi, lo, stride;

	*offset_out = 0;

	if (index == NULL) {
		int error;

		if ((error = open_pack_index(p)) < GIT_SUCCESS)
			return error;

		assert(p->index_map.data);

		index = p->index_map.data;
		level1_ofs = p->index_map.data;
	}

	if (p->index_version > 1) {
		level1_ofs += 2;
		index += 8;
	}

	index += 4 * 256;
	hi = ntohl(level1_ofs[(int)oid->id[0]]);
	lo = ((oid->id[0] == 0x0) ? 0 : ntohl(level1_ofs[(int)oid->id[0] - 1]));

	if (p->index_version > 1) {
		stride = 20;
	} else {
		stride = 24;
		index += 4;
	}

#ifdef INDEX_DEBUG_LOOKUP
	printf("%02x%02x%02x... lo %u hi %u nr %d\n",
		oid->id[0], oid->id[1], oid->id[2], lo, hi, p->num_objects);
#endif

#ifdef GIT2_INDEX_LOOKUP /* TODO: use the advanced lookup method from git.git */

	int pos = sha1_entry_pos(index, stride, 0, lo, hi, p->num_objects, oid);
	if (pos < 0)
		return GIT_ENOTFOUND;

	*offset_out = nth_packed_object_offset(p, pos);
	return GIT_SUCCESS;

#else /* use an old and boring binary search */

	do {
		unsigned mi = (lo + hi) / 2;
		int cmp = memcmp(index + mi * stride, oid->id, GIT_OID_RAWSZ);

		if (!cmp) {
			*offset_out = nth_packed_object_offset(p, mi);
			return GIT_SUCCESS;
		}

		if (cmp > 0)
			hi = mi;
		else
			lo = mi+1;

	} while (lo < hi);

	return GIT_ENOTFOUND;
#endif
}

static int find_pack_entry_in_pack(
		struct pack_entry *e,
		struct packed_git *p,
		const git_oid *oid)
{
	off_t offset;

	assert(p);

	if (p->num_bad_objects) {
		unsigned i;
		for (i = 0; i < p->num_bad_objects; i++)
			if (git_oid_cmp(oid, &p->bad_object_sha1[i]) == 0)
				return GIT_ERROR;
	}

	if (find_pack_entry_offset(&offset, p, oid) < GIT_SUCCESS)
		return GIT_ENOTFOUND;
	
	/* we found an entry in the index;
	 * make sure the packfile backing the index 
	 * still exists on disk */
	if (p->pack_fd == -1 && open_packfile(p) < GIT_SUCCESS)
		return GIT_EOSERR;

	e->offset = offset;
	e->p = p;

	printf("Found pack entry in pack '%s' [%d]\n", p->pack_name, (int)offset);
	git_oid_cpy(&e->sha1, oid);
	return GIT_SUCCESS;
}

static int find_pack_entry(struct pack_entry *e, pack_backend *backend, const git_oid *oid)
{
	size_t i;

	if (backend->last_found &&
		find_pack_entry_in_pack(e, backend->last_found, oid) == GIT_SUCCESS)
		return GIT_SUCCESS;

	for (i = 0; i < backend->packs.length; ++i) {
		struct packed_git *p;

		p = git_vector_get(&backend->packs, i);
		if (p == backend->last_found)
			continue;

		if (find_pack_entry_in_pack(e, p, oid) == GIT_SUCCESS) {
			backend->last_found = p;
			return GIT_SUCCESS;
		}
	}

	return GIT_ENOTFOUND;
}

static unsigned long unpack_object_header_buffer(const unsigned char *buf,
		unsigned long len, git_otype *type, size_t *sizep)
{
	unsigned shift;
	unsigned long size, c;
	unsigned long used = 0;

	c = buf[used++];
	*type = (c >> 4) & 7;
	size = c & 15;
	shift = 4;
	while (c & 0x80) {
		if (len <= used || bitsizeof(long) <= shift)
			return 0;

		c = buf[used++];
		size += (c & 0x7f) << shift;
		shift += 7;
	}

	*sizep = (size_t)size;
	return used;
}

static int unpack_object_header(
				git_rawobj *object,
				pack_backend *backend,
				struct packed_git *p,
				struct pack_window **w_curs,
				off_t *curpos)
{
	unsigned char *base;
	unsigned int left;
	unsigned long used;

	/* use_pack() assures us we have [base, base + 20) available
	 * as a range that we can look at at.  (Its actually the hash
	 * size that is assured.)  With our object header encoding
	 * the maximum deflated object size is 2^137, which is just
	 * insane, so we know won't exceed what we have been given.
	 */
	base = use_pack(backend, p, w_curs, *curpos, &left);
	if (base == NULL)
		return GIT_ENOMEM;

	used = unpack_object_header_buffer(base, left, &object->type, &object->len);

	if (used == 0)
		return GIT_EOBJCORRUPTED;

	*curpos += used;
	return GIT_SUCCESS;
}

static int unpack_compressed_entry(
		git_rawobj *obj,
		pack_backend *backend,
		struct packed_git *p,
		struct pack_window **w_curs,
		off_t curpos,
		unsigned long size)
{
	int st;
	z_stream stream;
	unsigned char *buffer, *in;

	buffer = git__malloc(size);

	memset(&stream, 0, sizeof(stream));
	stream.next_out = buffer;
	stream.avail_out = size + 1;

	st = inflateInit(&stream);
	if (st != Z_OK) {
		free(buffer);
		return GIT_EZLIB;
	}

	do {
		in = use_pack(backend, p, w_curs, curpos, &stream.avail_in);
		stream.next_in = in;
		st = inflate(&stream, Z_FINISH);

		if (!stream.avail_out)
			break; /* the payload is larger than it should be */

		curpos += stream.next_in - in;
	} while (st == Z_OK || st == Z_BUF_ERROR);

	inflateEnd(&stream);

	if ((st != Z_STREAM_END) || stream.total_out != size) {
		free(buffer);
		return GIT_EZLIB;
	}

	obj->len = size;
	obj->data = buffer;
	return GIT_SUCCESS;
}

static off_t get_delta_base(
		pack_backend *backend,
		struct packed_git *p,
		struct pack_window **w_curs,
		off_t *curpos,
		git_otype type,
		off_t delta_obj_offset)
{
	unsigned char *base_info = use_pack(backend, p, w_curs, *curpos, NULL);
	off_t base_offset;

	/* use_pack() assured us we have [base_info, base_info + 20)
	 * as a range that we can look at without walking off the
	 * end of the mapped window.  Its actually the hash size
	 * that is assured.  An OFS_DELTA longer than the hash size
	 * is stupid, as then a REF_DELTA would be smaller to store.
	 */
	if (type == GIT_OBJ_OFS_DELTA) {
		unsigned used = 0;
		unsigned char c = base_info[used++];
		base_offset = c & 127;
		while (c & 128) {
			base_offset += 1;
			if (!base_offset || MSB(off_t, base_offset, 7))
				return 0;  /* overflow */
			c = base_info[used++];
			base_offset = (base_offset << 7) + (c & 127);
		}
		base_offset = delta_obj_offset - base_offset;
		if (base_offset <= 0 || base_offset >= delta_obj_offset)
			return 0;  /* out of bound */
		*curpos += used;
	} else if (type == GIT_OBJ_REF_DELTA) {
		/* The base entry _must_ be in the same pack */
		if (find_pack_entry_offset(&base_offset, p, (git_oid *)base_info) < GIT_SUCCESS)
			return GIT_EPACKCORRUPTED;
		*curpos += 20;
	} else
		return 0;

	return base_offset;
}

static int unpack_delta_entry(
		git_rawobj *obj,
		pack_backend *backend,
		struct packed_git *p,
		struct pack_window **w_curs,
		off_t curpos,
		unsigned long delta_size,
		off_t obj_offset)
{
	off_t base_offset;
	git_rawobj base, delta;
	int error;

	base_offset = get_delta_base(backend, p, w_curs, &curpos, obj->type, obj_offset);
	if (base_offset == 0)
		return GIT_EOBJCORRUPTED;

	unuse_pack(w_curs);
	error = unpack_entry(&base, backend, p, base_offset);

	/* TODO: git.git tries to load the base from other packfiles
	 * or loose objects */
	if (error < GIT_SUCCESS)
		return error;

	error = unpack_compressed_entry(&delta, backend, p, w_curs, curpos, delta_size);
	if (error < GIT_SUCCESS) {
		git_rawobj_close(&base);
		return error;
	}

	obj->type = base.type;
	error = git__delta_apply(obj,
			base.data, base.len,
			delta.data, delta.len);

	git_rawobj_close(&base);
	git_rawobj_close(&delta);

	/* TODO: we might want to cache this shit. eventually */
	//add_delta_base_cache(p, base_offset, base, base_size, *type);
	return error;
}

static int unpack_entry(
		git_rawobj *obj,
		pack_backend *backend,
		struct packed_git *p,
		off_t obj_offset)
{
	struct pack_window *w_curs = NULL;
	off_t curpos = obj_offset;
	int error;

#if 0
	if (backend->check_crc && p->index_version > 1) {
		struct revindex_entry *revidx = find_pack_revindex(p, obj_offset);
		unsigned long len = revidx[1].offset - obj_offset;
		if (check_pack_crc(p, &w_curs, obj_offset, len, revidx->nr)) {
			const unsigned char *sha1 =
				nth_packed_object_sha1(p, revidx->nr);
			error("bad packed object CRC for %s",
			      sha1_to_hex(sha1));
			mark_bad_packed_object(p, sha1);
			unuse_pack(&w_curs);
			return NULL;
		}
	}
#endif

	obj->data = NULL;
	obj->len = 0;
	obj->type = GIT_OBJ_BAD;

	error = unpack_object_header(obj, backend, p, &w_curs, &curpos);
	if (error < GIT_SUCCESS)
		return error;

	switch (obj->type) {
	case GIT_OBJ_OFS_DELTA:
	case GIT_OBJ_REF_DELTA:
		error = unpack_delta_entry(
			obj, backend, p,
			&w_curs, curpos,
			obj->len, obj_offset);
		break;

	case GIT_OBJ_COMMIT:
	case GIT_OBJ_TREE:
	case GIT_OBJ_BLOB:
	case GIT_OBJ_TAG:
		error = unpack_compressed_entry(
			obj, backend, p,
			&w_curs, curpos,
			obj->len);
		break;

	default:
		error = GIT_EOBJCORRUPTED;
		break;
	}

	unuse_pack(&w_curs);
	return error;
}



/***********************************************************
 *
 * PACKED BACKEND PUBLIC API
 *
 * Implement the git_odb_backend API calls
 *
 ***********************************************************/

/*
int pack_backend__read_header(git_rawobj *obj, git_odb_backend *backend, const git_oid *oid)
{
	pack_location location;

	assert(obj && backend && oid);

	if (locate_packfile(&location, (pack_backend *)backend, oid) < 0)
		return GIT_ENOTFOUND;

	return read_header_packed(obj, &location);
}
*/

int pack_backend__read(git_rawobj *obj, git_odb_backend *backend, const git_oid *oid)
{
	struct pack_entry e;
	int error;

	if ((error = find_pack_entry(&e, (pack_backend *)backend, oid)) < GIT_SUCCESS)
		return error;

	return unpack_entry(obj, (pack_backend *)backend, e.p, e.offset);
}

int pack_backend__exists(git_odb_backend *backend, const git_oid *oid)
{
	struct pack_entry e;
	return find_pack_entry(&e, (pack_backend *)backend, oid) == GIT_SUCCESS;
}

void pack_backend__free(git_odb_backend *_backend)
{
	pack_backend *backend;

	assert(_backend);

	backend = (pack_backend *)_backend;

	/* TODO: free all packs; leaking! */

	free(backend);
}

int git_odb_backend_pack(git_odb_backend **backend_out, const char *objects_dir)
{
	int error;
	pack_backend *backend;

	backend = git__calloc(1, sizeof(pack_backend));
	if (backend == NULL)
		return GIT_ENOMEM;

	if (git_vector_init(&backend->packs, 8, sort_pack) < GIT_SUCCESS) {
		free(backend);
		return GIT_ENOMEM;
	}

	backend->window_size = DEFAULT_WINDOW_SIZE;
	backend->mapped_limit = DEFAULT_MAPPED_LIMIT;

	error = load_packs(backend, objects_dir, 1);
	if (error < GIT_SUCCESS) {
		pack_backend__free((git_odb_backend *)backend);
		return error;
	}

	backend->parent.read = &pack_backend__read;
	backend->parent.read_header = NULL;
	backend->parent.write = NULL;
	backend->parent.exists = &pack_backend__exists;
	backend->parent.free = &pack_backend__free;

	*backend_out = (git_odb_backend *)backend;
	return GIT_SUCCESS;
}
