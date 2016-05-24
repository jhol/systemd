/***
  This file is part of systemd.

  Copyright 2010-2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* When we include libgen.h because we need dirname() we immediately
 * undefine basename() since libgen.h defines it as a macro to the
 * POSIX version which is really broken. We prefer GNU basename(). */
#include <libgen.h>
#undef basename

#include "alloc-util.h"
#include "extract-word.h"
#include "fs-util.h"
#include "log.h"
#include "macro.h"
#include "missing.h"
#include "path-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"

bool path_is_absolute(const char *p) {
        return p[0] == '/';
}

bool is_path(const char *p) {
        return !!strchr(p, '/');
}

int path_split_and_make_absolute(const char *p, char ***ret) {
        char **l;
        int r;

        assert(p);
        assert(ret);

        l = strv_split(p, ":");
        if (!l)
                return -ENOMEM;

        r = path_strv_make_absolute_cwd(l);
        if (r < 0) {
                strv_free(l);
                return r;
        }

        *ret = l;
        return r;
}

char *path_make_absolute(const char *p, const char *prefix) {
        assert(p);

        /* Makes every item in the list an absolute path by prepending
         * the prefix, if specified and necessary */

        if (path_is_absolute(p) || !prefix)
                return strdup(p);

        return strjoin(prefix, "/", p, NULL);
}

int path_make_absolute_cwd(const char *p, char **ret) {
        char *c;

        assert(p);
        assert(ret);

        /* Similar to path_make_absolute(), but prefixes with the
         * current working directory. */

        if (path_is_absolute(p))
                c = strdup(p);
        else {
                _cleanup_free_ char *cwd = NULL;

                cwd = get_current_dir_name();
                if (!cwd)
                        return negative_errno();

                c = strjoin(cwd, "/", p, NULL);
        }
        if (!c)
                return -ENOMEM;

        *ret = c;
        return 0;
}

int path_make_relative(const char *from_dir, const char *to_path, char **_r) {
        char *r, *p;
        unsigned n_parents;

        assert(from_dir);
        assert(to_path);
        assert(_r);

        /* Strips the common part, and adds ".." elements as necessary. */

        if (!path_is_absolute(from_dir))
                return -EINVAL;

        if (!path_is_absolute(to_path))
                return -EINVAL;

        /* Skip the common part. */
        for (;;) {
                size_t a;
                size_t b;

                from_dir += strspn(from_dir, "/");
                to_path += strspn(to_path, "/");

                if (!*from_dir) {
                        if (!*to_path)
                                /* from_dir equals to_path. */
                                r = strdup(".");
                        else
                                /* from_dir is a parent directory of to_path. */
                                r = strdup(to_path);

                        if (!r)
                                return -ENOMEM;

                        path_kill_slashes(r);

                        *_r = r;
                        return 0;
                }

                if (!*to_path)
                        break;

                a = strcspn(from_dir, "/");
                b = strcspn(to_path, "/");

                if (a != b)
                        break;

                if (memcmp(from_dir, to_path, a) != 0)
                        break;

                from_dir += a;
                to_path += b;
        }

        /* If we're here, then "from_dir" has one or more elements that need to
         * be replaced with "..". */

        /* Count the number of necessary ".." elements. */
        for (n_parents = 0;;) {
                from_dir += strspn(from_dir, "/");

                if (!*from_dir)
                        break;

                from_dir += strcspn(from_dir, "/");
                n_parents++;
        }

        r = malloc(n_parents * 3 + strlen(to_path) + 1);
        if (!r)
                return -ENOMEM;

        for (p = r; n_parents > 0; n_parents--, p += 3)
                memcpy(p, "../", 3);

        strcpy(p, to_path);
        path_kill_slashes(r);

        *_r = r;
        return 0;
}

int path_normalize(const char *name, char **ret) {
        char *r, *dest, *apex;
        const char *start, *end, *r_limit;
        long path_max;
        size_t seg_len, append_len;

        assert(name);
        assert(ret);

        path_max = pathconf(name, _PC_PATH_MAX);
        if (path_max <= 0)
                path_max = 1024;

        r = dest = malloc(path_max);
        if (!r)
                return -ENOMEM;
        r_limit = r + path_max;

        dest = r;
        start = end = name;
        if (path_is_absolute(name)) {
                *dest++ = '/';
        } else {
                for (; *start; start = end) {
                        /* Skip sequence of multiple path-separators. */
                        while (*start == '/')
                                ++start;

                        /* Find end of path component. */
                        for (end = start; *end && *end != '/'; ++end);

                        if (end - start == 2 && start[0] == '.' && start[1] == '.') {
                                /* If the segment was ../, add it to the result path */
                                if (dest + 3 >= r_limit) {
                                        const size_t new_size = r_limit - r + path_max;
                                        char *const new_r = (char*)realloc(r, new_size);
                                        if (!new_r) {
                                                free(r);
                                                return -ENOMEM;
                                        }
                                        dest += new_r - r;
                                        r = new_r;
                                        r_limit = r + new_size;
                                }

                                *dest++ = '.', *dest++ = '.', *dest++ = '/';
                        } else if (end - start != 1 || start[0] != '.') {
                                /* If the segment was not ./ or ../, we have reached the apex */
                                break;
                        }
                }
        }

        *dest = '\0';
        apex = dest;

        for (; *start; start = end) {
                /* Skip sequence of multiple path-separators. */
                while (*start == '/')
                        ++start;

                /* Find end of path component. */
                for (end = start; *end && *end != '/'; ++end);

                seg_len = end - start;
                if (seg_len == 0)
                        break;
                else if (seg_len == 1 && start[0] == '.')
                        continue;
                else if (seg_len == 2 && start[0] == '.' && start[1] == '.') {
                        /* Back up to previous component, ignore if at apex already. */
                        if (dest > apex) {
                                dest--;
                                for (; dest > apex && dest[-1] != '/'; dest--);
                                continue;
                        }
                }

                append_len = seg_len + 1;
                if (dest + append_len >= r_limit) {
                        const size_t new_size = r_limit - r +
                                ((append_len > (size_t)path_max) ? append_len : (size_t)path_max);
                        char *const new_r = (char*)realloc(r, new_size);
                        if (!new_r) {
                                free(r);
                                return -ENOMEM;
                        }
                        apex += new_r - r;
                        dest += new_r - r;
                        r = new_r;
                        r_limit = r + new_size;
                }

                dest = mempcpy(dest, start, seg_len);
                *dest++ = '/';
                *dest = '\0';

                /* If we're adding an extra ../ to the prefix, move the apex along. */
                if (seg_len == 2 && start[0] == '.' && start[1] == '.')
                        apex = dest;
        }

        if (dest > r + 1 && dest[-1] == '/')
                --dest;
        *dest = '\0';

        *ret = r;
        return 0;
}

int path_strv_make_absolute_cwd(char **l) {
        char **s;
        int r;

        /* Goes through every item in the string list and makes it
         * absolute. This works in place and won't rollback any
         * changes on failure. */

        STRV_FOREACH(s, l) {
                char *t;

                r = path_make_absolute_cwd(*s, &t);
                if (r < 0)
                        return r;

                free(*s);
                *s = t;
        }

        return 0;
}

char **path_strv_resolve(char **l, const char *prefix) {
        char **s;
        unsigned k = 0;
        bool enomem = false;

        if (strv_isempty(l))
                return l;

        /* Goes through every item in the string list and canonicalize
         * the path. This works in place and won't rollback any
         * changes on failure. */

        STRV_FOREACH(s, l) {
                char *t, *u;
                _cleanup_free_ char *orig = NULL;

                if (!path_is_absolute(*s)) {
                        free(*s);
                        continue;
                }

                if (prefix) {
                        orig = *s;
                        t = strappend(prefix, orig);
                        if (!t) {
                                enomem = true;
                                continue;
                        }
                } else
                        t = *s;

                errno = 0;
                u = canonicalize_file_name(t);
                if (!u) {
                        if (errno == ENOENT) {
                                if (prefix) {
                                        u = orig;
                                        orig = NULL;
                                        free(t);
                                } else
                                        u = t;
                        } else {
                                free(t);
                                if (errno == ENOMEM || errno == 0)
                                        enomem = true;

                                continue;
                        }
                } else if (prefix) {
                        char *x;

                        free(t);
                        x = path_startswith(u, prefix);
                        if (x) {
                                /* restore the slash if it was lost */
                                if (!startswith(x, "/"))
                                        *(--x) = '/';

                                t = strdup(x);
                                free(u);
                                if (!t) {
                                        enomem = true;
                                        continue;
                                }
                                u = t;
                        } else {
                                /* canonicalized path goes outside of
                                 * prefix, keep the original path instead */
                                free(u);
                                u = orig;
                                orig = NULL;
                        }
                } else
                        free(t);

                l[k++] = u;
        }

        l[k] = NULL;

        if (enomem)
                return NULL;

        return l;
}

char **path_strv_resolve_uniq(char **l, const char *prefix) {

        if (strv_isempty(l))
                return l;

        if (!path_strv_resolve(l, prefix))
                return NULL;

        return strv_uniq(l);
}

char *path_kill_slashes(char *path) {
        char *f, *t;
        bool slash = false;

        /* Removes redundant inner and trailing slashes. Modifies the
         * passed string in-place.
         *
         * ///foo///bar/ becomes /foo/bar
         */

        for (f = path, t = path; *f; f++) {

                if (*f == '/') {
                        slash = true;
                        continue;
                }

                if (slash) {
                        slash = false;
                        *(t++) = '/';
                }

                *(t++) = *f;
        }

        /* Special rule, if we are talking of the root directory, a
        trailing slash is good */

        if (t == path && slash)
                *(t++) = '/';

        *t = 0;
        return path;
}

char* path_startswith(const char *path, const char *prefix) {
        assert(path);
        assert(prefix);

        if ((path[0] == '/') != (prefix[0] == '/'))
                return NULL;

        for (;;) {
                size_t a, b;

                path += strspn(path, "/");
                prefix += strspn(prefix, "/");

                if (*prefix == 0)
                        return (char*) path;

                if (*path == 0)
                        return NULL;

                a = strcspn(path, "/");
                b = strcspn(prefix, "/");

                if (a != b)
                        return NULL;

                if (memcmp(path, prefix, a) != 0)
                        return NULL;

                path += a;
                prefix += b;
        }
}

int path_compare(const char *a, const char *b) {
        int d;

        assert(a);
        assert(b);

        /* A relative path and an abolute path must not compare as equal.
         * Which one is sorted before the other does not really matter.
         * Here a relative path is ordered before an absolute path. */
        d = (a[0] == '/') - (b[0] == '/');
        if (d != 0)
                return d;

        for (;;) {
                size_t j, k;

                a += strspn(a, "/");
                b += strspn(b, "/");

                if (*a == 0 && *b == 0)
                        return 0;

                /* Order prefixes first: "/foo" before "/foo/bar" */
                if (*a == 0)
                        return -1;
                if (*b == 0)
                        return 1;

                j = strcspn(a, "/");
                k = strcspn(b, "/");

                /* Alphabetical sort: "/foo/aaa" before "/foo/b" */
                d = memcmp(a, b, MIN(j, k));
                if (d != 0)
                        return (d > 0) - (d < 0); /* sign of d */

                /* Sort "/foo/a" before "/foo/aaa" */
                d = (j > k) - (j < k);  /* sign of (j - k) */
                if (d != 0)
                        return d;

                a += j;
                b += k;
        }
}

bool path_equal(const char *a, const char *b) {
        return path_compare(a, b) == 0;
}

bool path_equal_or_files_same(const char *a, const char *b) {
        return path_equal(a, b) || files_same(a, b) > 0;
}

char* path_join(const char *root, const char *path, const char *rest) {
        assert(path);

        if (!isempty(root))
                return strjoin(root, endswith(root, "/") ? "" : "/",
                               path[0] == '/' ? path+1 : path,
                               rest ? (endswith(path, "/") ? "" : "/") : NULL,
                               rest && rest[0] == '/' ? rest+1 : rest,
                               NULL);
        else
                return strjoin(path,
                               rest ? (endswith(path, "/") ? "" : "/") : NULL,
                               rest && rest[0] == '/' ? rest+1 : rest,
                               NULL);
}

int find_binary(const char *name, char **ret) {
        int last_error, r;
        const char *p;

        assert(name);

        if (is_path(name)) {
                if (access(name, X_OK) < 0)
                        return -errno;

                if (ret) {
                        r = path_make_absolute_cwd(name, ret);
                        if (r < 0)
                                return r;
                }

                return 0;
        }

        /**
         * Plain getenv, not secure_getenv, because we want
         * to actually allow the user to pick the binary.
         */
        p = getenv("PATH");
        if (!p)
                p = DEFAULT_PATH;

        last_error = -ENOENT;

        for (;;) {
                _cleanup_free_ char *j = NULL, *element = NULL;

                r = extract_first_word(&p, &element, ":", EXTRACT_RELAX|EXTRACT_DONT_COALESCE_SEPARATORS);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (!path_is_absolute(element))
                        continue;

                j = strjoin(element, "/", name, NULL);
                if (!j)
                        return -ENOMEM;

                if (access(j, X_OK) >= 0) {
                        /* Found it! */

                        if (ret) {
                                *ret = path_kill_slashes(j);
                                j = NULL;
                        }

                        return 0;
                }

                last_error = -errno;
        }

        return last_error;
}

bool paths_check_timestamp(const char* const* paths, usec_t *timestamp, bool update) {
        bool changed = false;
        const char* const* i;

        assert(timestamp);

        if (paths == NULL)
                return false;

        STRV_FOREACH(i, paths) {
                struct stat stats;
                usec_t u;

                if (stat(*i, &stats) < 0)
                        continue;

                u = timespec_load(&stats.st_mtim);

                /* first check */
                if (*timestamp >= u)
                        continue;

                log_debug("timestamp of '%s' changed", *i);

                /* update timestamp */
                if (update) {
                        *timestamp = u;
                        changed = true;
                } else
                        return true;
        }

        return changed;
}

static int binary_is_good(const char *binary) {
        _cleanup_free_ char *p = NULL, *d = NULL;
        int r;

        r = find_binary(binary, &p);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        /* An fsck that is linked to /bin/true is a non-existent
         * fsck */

        r = readlink_malloc(p, &d);
        if (r == -EINVAL) /* not a symlink */
                return 1;
        if (r < 0)
                return r;

        return !PATH_IN_SET(d, "true"
                               "/bin/true",
                               "/usr/bin/true",
                               "/dev/null");
}

int fsck_exists(const char *fstype) {
        const char *checker;

        assert(fstype);

        if (streq(fstype, "auto"))
                return -EINVAL;

        checker = strjoina("fsck.", fstype);
        return binary_is_good(checker);
}

int mkfs_exists(const char *fstype) {
        const char *mkfs;

        assert(fstype);

        if (streq(fstype, "auto"))
                return -EINVAL;

        mkfs = strjoina("mkfs.", fstype);
        return binary_is_good(mkfs);
}

char *prefix_root(const char *root, const char *path) {
        char *n, *p;
        size_t l;

        /* If root is passed, prefixes path with it. Otherwise returns
         * it as is. */

        assert(path);

        /* First, drop duplicate prefixing slashes from the path */
        while (path[0] == '/' && path[1] == '/')
                path++;

        if (isempty(root) || path_equal(root, "/"))
                return strdup(path);

        l = strlen(root) + 1 + strlen(path) + 1;

        n = new(char, l);
        if (!n)
                return NULL;

        p = stpcpy(n, root);

        while (p > n && p[-1] == '/')
                p--;

        if (path[0] != '/')
                *(p++) = '/';

        strcpy(p, path);
        return n;
}

int parse_path_argument_and_warn(const char *path, bool suppress_root, char **arg) {
        char *p;
        int r;

        /*
         * This function is intended to be used in command line
         * parsers, to handle paths that are passed in. It makes the
         * path absolute, and reduces it to NULL if omitted or
         * root (the latter optionally).
         *
         * NOTE THAT THIS WILL FREE THE PREVIOUS ARGUMENT POINTER ON
         * SUCCESS! Hence, do not pass in uninitialized pointers.
         */

        if (isempty(path)) {
                *arg = mfree(*arg);
                return 0;
        }

        r = path_make_absolute_cwd(path, &p);
        if (r < 0)
                return log_error_errno(r, "Failed to parse path \"%s\" and make it absolute: %m", path);

        path_kill_slashes(p);
        if (suppress_root && path_equal(p, "/"))
                p = mfree(p);

        free(*arg);
        *arg = p;
        return 0;
}

char* dirname_malloc(const char *path) {
        char *d, *dir, *dir2;

        assert(path);

        d = strdup(path);
        if (!d)
                return NULL;

        dir = dirname(d);
        assert(dir);

        if (dir == d)
                return d;

        dir2 = strdup(dir);
        free(d);

        return dir2;
}

bool filename_is_valid(const char *p) {
        const char *e;

        if (isempty(p))
                return false;

        if (streq(p, "."))
                return false;

        if (streq(p, ".."))
                return false;

        e = strchrnul(p, '/');
        if (*e != 0)
                return false;

        if (e - p > FILENAME_MAX)
                return false;

        return true;
}

bool path_is_safe(const char *p) {

        if (isempty(p))
                return false;

        if (streq(p, "..") || startswith(p, "../") || endswith(p, "/..") || strstr(p, "/../"))
                return false;

        if (strlen(p)+1 > PATH_MAX)
                return false;

        /* The following two checks are not really dangerous, but hey, they still are confusing */
        if (streq(p, ".") || startswith(p, "./") || endswith(p, "/.") || strstr(p, "/./"))
                return false;

        if (strstr(p, "//"))
                return false;

        return true;
}

char *file_in_same_dir(const char *path, const char *filename) {
        char *e, *ret;
        size_t k;

        assert(path);
        assert(filename);

        /* This removes the last component of path and appends
         * filename, unless the latter is absolute anyway or the
         * former isn't */

        if (path_is_absolute(filename))
                return strdup(filename);

        e = strrchr(path, '/');
        if (!e)
                return strdup(filename);

        k = strlen(filename);
        ret = new(char, (e + 1 - path) + k + 1);
        if (!ret)
                return NULL;

        memcpy(mempcpy(ret, path, e + 1 - path), filename, k + 1);
        return ret;
}

bool hidden_or_backup_file(const char *filename) {
        const char *p;

        assert(filename);

        if (filename[0] == '.' ||
            streq(filename, "lost+found") ||
            streq(filename, "aquota.user") ||
            streq(filename, "aquota.group") ||
            endswith(filename, "~"))
                return true;

        p = strrchr(filename, '.');
        if (!p)
                return false;

        /* Please, let's not add more entries to the list below. If external projects think it's a good idea to come up
         * with always new suffixes and that everybody else should just adjust to that, then it really should be on
         * them. Hence, in future, let's not add any more entries. Instead, let's ask those packages to instead adopt
         * one of the generic suffixes/prefixes for hidden files or backups, possibly augmented with an additional
         * string. Specifically: there's now:
         *
         *    The generic suffixes "~" and ".bak" for backup files
         *    The generic prefix "." for hidden files
         *
         * Thus, if a new package manager "foopkg" wants its own set of ".foopkg-new", ".foopkg-old", ".foopkg-dist"
         * or so registered, let's refuse that and ask them to use ".foopkg.new", ".foopkg.old" or ".foopkg~" instead.
         */

        return STR_IN_SET(p + 1,
                          "rpmnew",
                          "rpmsave",
                          "rpmorig",
                          "dpkg-old",
                          "dpkg-new",
                          "dpkg-tmp",
                          "dpkg-dist",
                          "dpkg-bak",
                          "dpkg-backup",
                          "dpkg-remove",
                          "ucf-new",
                          "ucf-old",
                          "ucf-dist",
                          "swp",
                          "bak",
                          "old",
                          "new");
}

bool is_device_path(const char *path) {

        /* Returns true on paths that refer to a device, either in
         * sysfs or in /dev */

        return
                path_startswith(path, "/dev/") ||
                path_startswith(path, "/sys/");
}
