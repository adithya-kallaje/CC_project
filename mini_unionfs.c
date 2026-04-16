#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UFS ((struct mini_unionfs_state *) fuse_get_context()->private_data)

static void build_upper(const char *path, char *out) {
    snprintf(out, PATH_MAX, "%s%s", UFS->upper_dir, path);
}

static void build_lower(const char *path, char *out) {
    snprintf(out, PATH_MAX, "%s%s", UFS->lower_dir, path);
}

/* For /a/b/c.txt -> upper_dir/a/b/.wh.c.txt */
static void build_whiteout(const char *path, char *out) {
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    size_t dirlen     = slash ? (size_t)(slash - path) : 0;
    /* Build "<upper_dir><dirpart>/.wh.<base>" bounded by PATH_MAX. */
    int n = snprintf(out, PATH_MAX, "%s", UFS->upper_dir);
    if (n < 0 || n >= PATH_MAX) { out[0] = '\0'; return; }
    if (dirlen > 0 && (size_t)n + dirlen < PATH_MAX) {
        memcpy(out + n, path, dirlen);
        n += dirlen;
    }
    snprintf(out + n, PATH_MAX - n, "/.wh.%s", base);
}

static int exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

/* Walk each ancestor of `path` (not including the leaf) and check whether
 * a whiteout for that ancestor exists in the upper layer. If so, the entire
 * subtree below it is hidden. */
static int ancestor_whiteouted(const char *path) {
    if (!path || path[0] != '/' || path[1] == '\0') return 0;
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            char wh[PATH_MAX];
            build_whiteout(buf, wh);
            if (exists(wh)) return 1;
            *p = '/';
        }
    }
    return 0;
}

/* Resolve a path for reads.
 * Returns 0 on success and fills `out` with an absolute path on disk.
 * Returns -ENOENT if whiteouted or missing in both branches. */
static int resolve_read(const char *path, char *out) {
    if (ancestor_whiteouted(path)) return -ENOENT;
    char wh[PATH_MAX];
    build_whiteout(path, wh);
    if (exists(wh)) return -ENOENT;

    char up[PATH_MAX];
    build_upper(path, up);
    if (exists(up)) {
        memcpy(out, up, PATH_MAX);
        return 0;
    }
    char lo[PATH_MAX];
    build_lower(path, lo);
    if (exists(lo)) {
        memcpy(out, lo, PATH_MAX);
        return 0;
    }
    return -ENOENT;
}

/* mkdir -p on all ancestor dirs of `upper_dir + path`. Does NOT create
 * the leaf itself. */
static int ensure_upper_parent(const char *path) {
    char up[PATH_MAX];
    build_upper(path, up);
    char *slash = strrchr(up, '/');
    if (!slash || slash == up) return 0;
    *slash = '\0';

    /* iteratively mkdir each component of `up` */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", up);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -errno;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -errno;
    return 0;
}

/* Copy lower_dir+path -> upper_dir+path (Copy-on-Write). */
static int copy_up(const char *path) {
    char lo[PATH_MAX], up[PATH_MAX];
    build_lower(path, lo);
    build_upper(path, up);

    if (exists(up)) return 0;

    struct stat st;
    if (lstat(lo, &st) == -1) return -errno;

    int rc = ensure_upper_parent(path);
    if (rc < 0) return rc;

    int src = open(lo, O_RDONLY);
    if (src == -1) return -errno;
    int dst = open(up, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dst == -1) { int e = errno; close(src); return -e; }

    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst, buf + off, n - off);
            if (w == -1) { int e = errno; close(src); close(dst); return -e; }
            off += w;
        }
    }
    int e = (n == -1) ? errno : 0;
    close(src);
    close(dst);
    return e ? -e : 0;
}

/* ---------- FUSE operations ---------- */

static int ufs_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi) {
    (void) fi;
    char resolved[PATH_MAX];
    int rc = resolve_read(path, resolved);
    if (rc < 0) return rc;
    if (lstat(resolved, stbuf) == -1) return -errno;
    return 0;
}

/* Merge readdir from upper and lower; suppress .wh.* and anything a
 * whiteout shadows; upper entries take precedence automatically because
 * the hash-set insert rejects duplicates after we add upper first. */

struct name_node { char *name; struct name_node *next; };

static unsigned hash_name(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

#define NBUCK 128
static int set_has(struct name_node **tbl, const char *s) {
    struct name_node *n = tbl[hash_name(s) % NBUCK];
    for (; n; n = n->next) if (strcmp(n->name, s) == 0) return 1;
    return 0;
}
static void set_add(struct name_node **tbl, const char *s) {
    if (set_has(tbl, s)) return;
    struct name_node *n = malloc(sizeof(*n));
    n->name = strdup(s);
    n->next = tbl[hash_name(s) % NBUCK];
    tbl[hash_name(s) % NBUCK] = n;
}
static void set_free(struct name_node **tbl) {
    for (int i = 0; i < NBUCK; i++) {
        struct name_node *n = tbl[i];
        while (n) {
            struct name_node *next = n->next;
            free(n->name); free(n);
            n = next;
        }
        tbl[i] = NULL;
    }
}

static int ufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;

    if (ancestor_whiteouted(path)) return -ENOENT;

    struct name_node *seen[NBUCK] = {0};
    struct name_node *whiteouts[NBUCK] = {0};

    char up[PATH_MAX], lo[PATH_MAX];
    build_upper(path, up);
    build_lower(path, lo);

    /* First pass on upper: collect whiteouts so we can hide shadowed names. */
    DIR *d = opendir(up);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, ".wh.", 4) == 0)
                set_add(whiteouts, de->d_name + 4);
        }
        closedir(d);
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    set_add(seen, ".");
    set_add(seen, "..");

    /* Upper entries (real files, skipping whiteout markers themselves). */
    d = opendir(up);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            if (set_has(seen, de->d_name)) continue;
            set_add(seen, de->d_name);
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(d);
    }

    /* Lower entries, suppressing those shadowed by upper or whiteouts. */
    d = opendir(lo);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (set_has(seen, de->d_name)) continue;
            if (set_has(whiteouts, de->d_name)) continue;
            set_add(seen, de->d_name);
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(d);
    }

    set_free(seen);
    set_free(whiteouts);
    return 0;
}

static int ufs_open(const char *path, struct fuse_file_info *fi) {
    if (ancestor_whiteouted(path)) return -ENOENT;
    char wh[PATH_MAX];
    build_whiteout(path, wh);
    if (exists(wh)) return -ENOENT;

    char up[PATH_MAX], lo[PATH_MAX];
    build_upper(path, up);
    build_lower(path, lo);

    int writing = (fi->flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) != 0
                  || (fi->flags & O_ACCMODE) != O_RDONLY;

    const char *target;
    if (exists(up)) {
        target = up;
    } else if (exists(lo)) {
        if (writing) {
            int rc = copy_up(path);
            if (rc < 0) return rc;
            target = up;
        } else {
            target = lo;
        }
    } else {
        return -ENOENT;
    }

    int fd = open(target, fi->flags);
    if (fd == -1) return -errno;
    fi->fh = fd;
    return 0;
}

static int ufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int rc = ensure_upper_parent(path);
    if (rc < 0) return rc;

    char wh[PATH_MAX];
    build_whiteout(path, wh);
    unlink(wh); /* clear any stale whiteout so recreation is visible */

    char up[PATH_MAX];
    build_upper(path, up);
    int fd = open(up, fi->flags | O_CREAT, mode);
    if (fd == -1) return -errno;
    fi->fh = fd;
    return 0;
}

static int ufs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void) path;
    ssize_t n = pread(fi->fh, buf, size, offset);
    if (n == -1) return -errno;
    return (int) n;
}

static int ufs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    (void) path;
    ssize_t n = pwrite(fi->fh, buf, size, offset);
    if (n == -1) return -errno;
    return (int) n;
}

static int ufs_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    if (fi->fh) close(fi->fh);
    return 0;
}

static int ufs_unlink(const char *path) {
    char up[PATH_MAX], lo[PATH_MAX], wh[PATH_MAX];
    build_upper(path, up);
    build_lower(path, lo);
    build_whiteout(path, wh);

    int had_upper = exists(up);
    int had_lower = exists(lo);

    if (!had_upper && !had_lower) return -ENOENT;

    if (had_upper) {
        if (unlink(up) == -1) return -errno;
    }
    if (had_lower) {
        int rc = ensure_upper_parent(path);
        if (rc < 0) return rc;
        int fd = open(wh, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) return -errno;
        close(fd);
    }
    return 0;
}

static int ufs_mkdir(const char *path, mode_t mode) {
    int rc = ensure_upper_parent(path);
    if (rc < 0) return rc;

    char wh[PATH_MAX];
    build_whiteout(path, wh);
    unlink(wh);

    char up[PATH_MAX];
    build_upper(path, up);
    if (mkdir(up, mode) == -1) return -errno;
    return 0;
}

static int ufs_rmdir(const char *path) {
    char up[PATH_MAX], lo[PATH_MAX], wh[PATH_MAX];
    build_upper(path, up);
    build_lower(path, lo);
    build_whiteout(path, wh);

    int had_upper = exists(up);
    int had_lower = exists(lo);

    if (!had_upper && !had_lower) return -ENOENT;

    /* Verify merged view is empty: walk both dirs and bail on any
     * non-hidden entry that isn't already whited out. */
    struct name_node *whs[NBUCK] = {0};
    if (had_upper) {
        DIR *d = opendir(up);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (strncmp(de->d_name, ".wh.", 4) == 0)
                    set_add(whs, de->d_name + 4);
            }
            closedir(d);
        }
    }

    int nonempty = 0;
    if (had_upper) {
        DIR *d = opendir(up);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
                nonempty = 1; break;
            }
            closedir(d);
        }
    }
    if (!nonempty && had_lower) {
        DIR *d = opendir(lo);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                if (set_has(whs, de->d_name)) continue;
                nonempty = 1; break;
            }
            closedir(d);
        }
    }
    set_free(whs);
    if (nonempty) return -ENOTEMPTY;

    if (had_upper) {
        if (rmdir(up) == -1) return -errno;
    }
    if (had_lower) {
        int rc = ensure_upper_parent(path);
        if (rc < 0) return rc;
        int fd = open(wh, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) return -errno;
        close(fd);
    }
    return 0;
}

static const struct fuse_operations unionfs_oper = {
    .getattr = ufs_getattr,
    .readdir = ufs_readdir,
    .open    = ufs_open,
    .create  = ufs_create,
    .read    = ufs_read,
    .write   = ufs_write,
    .release = ufs_release,
    .unlink  = ufs_unlink,
    .mkdir   = ufs_mkdir,
    .rmdir   = ufs_rmdir,
};

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <lower_dir> <upper_dir> <mount_point> [FUSE options]\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) { usage(argv[0]); return 1; }

    struct mini_unionfs_state state;
    state.lower_dir = realpath(argv[1], NULL);
    state.upper_dir = realpath(argv[2], NULL);
    if (!state.lower_dir || !state.upper_dir) {
        fprintf(stderr, "Error resolving lower/upper dir: %s\n",
                strerror(errno));
        return 1;
    }

    /* Rebuild argv for fuse_main: drop the two branch dirs. */
    int new_argc = argc - 2;
    char **new_argv = calloc(new_argc + 1, sizeof(char *));
    new_argv[0] = argv[0];
    new_argv[1] = argv[3];                 /* mount point */
    for (int i = 4, j = 2; i < argc; i++, j++) new_argv[j] = argv[i];

    int rc = fuse_main(new_argc, new_argv, &unionfs_oper, &state);
    free(new_argv);
    free(state.lower_dir);
    free(state.upper_dir);
    return rc;
}
