#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <search.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CMP(a, b) (((a) > (b)) - ((a) < (b)))

typedef struct File File;

struct File {
	File *next;
	char *name;
	int fd;
	dev_t dev;
	ino_t ino;
	char *buf;
	off_t bufsiz;
};

typedef struct {
	off_t size;
	File *head;
	int n;
} Node;

static void *root;
static const size_t blksize = 8 * 1024; /* TODO: call stat(2) and use st_blksize */

static ssize_t readn(int fd, void *av, size_t n) {
	char *a;
	size_t t;
	ssize_t m;

	a = av;
	t = 0;
	while(t < n) {
		m = read(fd, a + t, n - t);
		if(m == -1) {
			if(errno == EINTR)
				continue;
			else
				return -1;
		}
		if(m == 0)
			break;
		t += m;
	}
	return t;
}

static void *emalloc(size_t n) {
	void *p = malloc(n);
	if(p == NULL)
		err(1, "malloc");
	return p;
}

static void filefree(File *p) {
	if(!p)
		return;
	free(p->name);
	free(p->buf);
	if(p->fd != -1)
		close(p->fd);
	free(p);
}

static int filedevinocmp(File *a, File *b) {
	int res = CMP(a->dev, b->dev);
	return res ? res : CMP(a->ino, b->ino);
}

static int filesetsizecmp(const void *a, const void *b) {
	return CMP(((Node *)a)->size, ((Node *)b)->size);
}

static int filesetdatacmp(const void *a, const void *b) {
	const Node *na, *nb;

	na = a;
	nb = b;
	return memcmp(na->head->buf, nb->head->buf, na->head->bufsiz);
}

static void insert(File *file, Node *v) {
	File **p = &v->head;
	while(*p != NULL && filedevinocmp(file, *p) > 0)
		p = &(*p)->next;
	if(*p != NULL && filedevinocmp(file, *p) == 0)
		errx(1, "same file");
	file->next = *p;
	*p = file;
	v->n++;
}

static void scan(char *dir) {
	char *path;
	struct stat st;
	DIR *dirp;
	struct dirent *dp;
	File *file;
	Node *set, **v;

	dirp = opendir(dir);
	if(dirp == NULL) {
		warn("opendir: %s", dir);
		return;
	}

	int dfd = dirfd(dirp);
	while((dp = readdir(dirp)) != NULL) {
		if(dp->d_name[0] == '.' && (dp->d_name[1] == '\0' || (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))
			continue;

		if(fstatat(dfd, dp->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1)
			err(1, "fstatat");

		if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
			continue;

		if(asprintf(&path, "%s/%s", dir, dp->d_name) == -1)
			err(1, "asprintf");

		if(S_ISDIR(st.st_mode)) {
			scan(path);
			free(path);
		} else if(S_ISREG(st.st_mode)) {
			file = emalloc(sizeof *file);
			file->name = path;
			file->buf = NULL;
			file->fd = -1;
			file->dev = st.st_dev;
			file->ino = st.st_ino;
			file->next = NULL;

			set = emalloc(sizeof *set);
			set->size = st.st_size;
			set->head = file;
			set->n = 1;

			v = tsearch(set, &root, filesetsizecmp);
			if(*v == NULL)
				errx(1, "tsearch");
			if(*v != set) {
				free(set);
				insert(file, *v);
			}
		}
	}

	closedir(dirp);
}

static void compareblock(Node *);

static void newaction(const void *nodep, const VISIT which, const int depth) {
	if(which == postorder || which == leaf) {
		Node *set = *(Node **)nodep;
		if(set->n > 1)
			compareblock(set);
		else
			filefree(set->head);
	}
}

static void compareblock(Node *set) {
	void *newroot;
	Node *n, **v;
	File *p, *q;
	ssize_t r;

	if(set->size == 0) {
		static bool firstset = true;
		if(!firstset)
			putchar('\n');
		else
			firstset = false;
		while((p = set->head) != NULL) {
			set->head = p->next;
			puts(p->name);
			filefree(p);
		}
		return;
	}

	for(p = set->head; p; p = p->next) {
		p->bufsiz = set->size > blksize ? blksize : set->size;
		r = readn(p->fd, p->buf, p->bufsiz);
		if(r == -1)
			err(1, "read");
		if(r < p->bufsiz)
			errx(1, "unexpected end of file: %s", p->name);
	}

	newroot = NULL;
	for(p = set->head; p; p = q) {
		q = p->next;
		p->next = NULL;

		n = emalloc(sizeof *n);
		n->size = set->size - p->bufsiz;
		n->head = p;
		n->n = 1;

		v = tsearch(n, &newroot, filesetdatacmp);
		if(*v == NULL)
			errx(1, "tsearch");
		if(*v != n) {
			free(n);
			insert(p, *v);
		}
	}
	twalk(newroot, newaction);
}

static void compare(Node *set) {
	File **p;

	/* Open all files. */
	for(p = &set->head; *p; p = &(*p)->next) {
		(*p)->fd = open((*p)->name, O_RDONLY);
		if((*p)->fd == -1) {
			warn("open: %s", (*p)->name);
			free((*p)->name);
			*p = (*p)->next;
			continue;
		}
		(*p)->buf = emalloc(blksize);
	}

	compareblock(set);
}

static void action(const void *nodep, const VISIT which, const int depth) {
	if(which == postorder || which == leaf) {
		Node *set = *(Node **)nodep;
		if(set->n > 1)
			compare(set);
		else
			filefree(set->head);
	}
}

int main(int argc, char *argv[]) {
	if(argc < 2) {
		fputs("usage: dups directory [directory..]\n", stderr);
		return 1;
	}

	while(*++argv)
		scan(*argv);

	twalk(root, action);
	return 0;
}
