/* kb_blob_store.c: content-addressed blob store. See kb_blob_store.h. */
#include "kb_blob_store.h"

#include "config.h"
#include "kb_doc_hash.h" /* kb_doc_content_hash (sha256 hex) */
#include "kb_paths.h"    /* kb_default_config_dir */
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *kb_blob_store_root(char *out, size_t cap)
{
   if (!out || cap == 0)
      return NULL;
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.kb_pdf_blob_dir[0])
      snprintf(out, cap, "%s", cfg.kb_pdf_blob_dir);
   else
      snprintf(out, cap, "%s/kb-blobs", kb_default_config_dir());
   return out;
}

/* Build root/<ab>/<sha> into `out`. Returns 0/-1 (bad sha). */
static int blob_path(const char *root, const char *sha, char *out, size_t cap)
{
   if (!sha || strlen(sha) != KB_DOC_HASH_HEX_LEN)
      return -1;
   for (const char *p = sha; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return -1; /* only lowercase hex — never a path-traversal sequence */
   snprintf(out, cap, "%s/%c%c/%s", root, sha[0], sha[1], sha);
   return 0;
}

/* mkdir -p for a single level (root, then root/ab). 0600/0700 like the PKI store. */
static int ensure_dir(const char *path)
{
   if (mkdir(path, 0700) == 0)
      return 0;
   if (errno == EEXIST)
   {
      /* It already exists — verify it is a REAL directory, not a symlink (which a pre-created/
       * misconfigured kb_pdf_blob_dir could use to redirect blobs out of the store). */
      struct stat st;
      if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
         return 0;
      return -1;
   }
   return -1;
}

/* fsync a directory so a prior rename()'s entry is durable (the blob-durable-before-row
 * guarantee needs the directory entry, not just the file content, on disk). Best-effort. */
static void fsync_dir(const char *path)
{
   int dfd = open(path, O_RDONLY | O_DIRECTORY);
   if (dfd >= 0)
   {
      fsync(dfd);
      close(dfd);
   }
}

int kb_blob_store_put(const void *bytes, size_t n, char *sha_out, size_t sha_cap)
{
   if ((!bytes && n > 0) || !sha_out || sha_cap <= KB_DOC_HASH_HEX_LEN)
      return -1;
   char sha[KB_DOC_HASH_HEX_LEN + 1];
   kb_doc_content_hash((const char *)(bytes ? bytes : ""), (int)n, sha);
   snprintf(sha_out, sha_cap, "%s", sha);

   char root[4096];
   if (!kb_blob_store_root(root, sizeof(root)))
      return -1;
   if (ensure_dir(root) != 0)
   {
      LOG_WARN("kb_blob", "cannot create blob root %s (%s)", root, strerror(errno));
      return -1;
   }
   char shard[4128];
   snprintf(shard, sizeof(shard), "%s/%c%c", root, sha[0], sha[1]);
   if (ensure_dir(shard) != 0)
      return -1;

   char final[4256];
   if (blob_path(root, sha, final, sizeof(final)) != 0)
      return -1;
   if (access(final, F_OK) == 0)
      return 0; /* dedup: byte-identical blob already present */

   /* Write to a unique temp in the same dir, fsync, then atomic rename — so a concurrent
    * reader/reconciler never sees a partial blob and a crash leaves only a temp orphan. */
   char tmp[4300];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", final, (int)getpid());
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return -1;
   size_t off = 0;
   const char *p = (const char *)bytes;
   int ok = 1;
   while (off < n)
   {
      ssize_t w = write(fd, p + off, n - off);
      if (w < 0)
      {
         if (errno == EINTR)
            continue;
         ok = 0;
         break;
      }
      off += (size_t)w;
   }
   if (ok && fsync(fd) != 0)
      ok = 0;
   close(fd);
   if (!ok)
   {
      unlink(tmp);
      return -1;
   }
   if (rename(tmp, final) != 0)
   {
      /* A racing put may have created `final` first — that's the same content, so success. */
      if (access(final, F_OK) == 0)
      {
         unlink(tmp);
         return 0;
      }
      unlink(tmp);
      return -1;
   }
   /* Make the new directory ENTRY durable too, so the blob-durable-before-row guarantee holds
    * across a crash right after this returns (otherwise the row could outlive a lost entry). */
   fsync_dir(shard);
   return 0;
}

/* File size of a blob in bytes, or -1 if absent/error. Lets a caller reject an oversized blob
 * (open_asset's inline cap) WITHOUT first reading the whole thing into memory. */
long long kb_blob_store_size(const char *sha)
{
   char root[4096], path[4256];
   if (!kb_blob_store_root(root, sizeof(root)) || blob_path(root, sha, path, sizeof(path)) != 0)
      return -1;
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   return (long long)st.st_size;
}

int kb_blob_store_exists(const char *sha)
{
   char root[4096], path[4256];
   if (!kb_blob_store_root(root, sizeof(root)) || blob_path(root, sha, path, sizeof(path)) != 0)
      return -1;
   return access(path, F_OK) == 0 ? 1 : 0;
}

int kb_blob_store_read(const char *sha, void **out, size_t *n_out)
{
   if (out)
      *out = NULL;
   if (n_out)
      *n_out = 0;
   if (!out || !n_out)
      return -1;
   char root[4096], path[4256];
   if (!kb_blob_store_root(root, sizeof(root)) || blob_path(root, sha, path, sizeof(path)) != 0)
      return -1;
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || st.st_size < 0)
   {
      close(fd);
      return -1;
   }
   size_t sz = (size_t)st.st_size;
   char *buf = malloc(sz ? sz : 1);
   if (!buf)
   {
      close(fd);
      return -1;
   }
   size_t off = 0;
   int ok = 1;
   while (off < sz)
   {
      ssize_t r = read(fd, buf + off, sz - off);
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         ok = 0;
         break;
      }
      if (r == 0)
         break;
      off += (size_t)r;
   }
   close(fd);
   if (!ok || off != sz)
   {
      free(buf);
      return -1;
   }
   *out = buf;
   *n_out = sz;
   return 0;
}

int kb_blob_store_unlink(const char *sha)
{
   char root[4096], path[4256];
   if (!kb_blob_store_root(root, sizeof(root)) || blob_path(root, sha, path, sizeof(path)) != 0)
      return -1;
   if (unlink(path) == 0 || errno == ENOENT)
      return 0;
   return -1;
}

long long kb_blob_store_foreach(kb_blob_visit_fn fn, void *ctx)
{
   if (!fn)
      return -1;
   char root[4096];
   if (!kb_blob_store_root(root, sizeof(root)))
      return -1;
   DIR *rd = opendir(root);
   if (!rd)
      return errno == ENOENT ? 0 : -1; /* no store yet → nothing to visit */
   long long visited = 0;
   struct dirent *shard;
   int stop = 0;
   while (!stop && (shard = readdir(rd)) != NULL)
   {
      if (shard->d_name[0] == '.' || strlen(shard->d_name) != 2)
         continue;
      char shardpath[4128];
      snprintf(shardpath, sizeof(shardpath), "%s/%s", root, shard->d_name);
      DIR *sd = opendir(shardpath);
      if (!sd)
         continue;
      struct dirent *e;
      while ((e = readdir(sd)) != NULL)
      {
         if (strlen(e->d_name) != KB_DOC_HASH_HEX_LEN)
            continue; /* skip .tmp.* and any non-blob entry */
         char fp[4256];
         snprintf(fp, sizeof(fp), "%s/%s", shardpath, e->d_name);
         struct stat st;
         if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
            continue; /* only real blob files — never count/reclaim a symlink or device */
         visited++;
         if (fn(e->d_name, (long long)st.st_size, (long long)st.st_mtime, ctx) != 0)
         {
            stop = 1;
            break;
         }
      }
      closedir(sd);
   }
   closedir(rd);
   return visited;
}
