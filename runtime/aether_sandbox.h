// aether_sandbox.h: runtime containment checks
//
// Each thread has its own stack of grant levels, kept here in the runtime
// (#2955): an enforce block, a host bridge's run_sandboxed call or a
// sandbox_push() adds one, and leaving it takes it off. Stdlib functions
// (fs, net, os, udp) call aether_sandbox_check() before a privileged
// operation: with no level in force on the calling thread everything is
// allowed; with levels in force, the operation must match a grant of every
// one of them (nesting narrows, never widens).
//
// Generated code reaches all of this through the functions below, never by
// naming the per-thread state (a Windows DLL cannot export thread-locals),
// so every library sharing one runtime image (the shared runtime, or, on
// ELF, a library linked without -Bsymbolic, which `ae` no longer makes since
// #3481) shares each thread's levels.
//
// When AETHER_HAS_SANDBOX is not defined, aether_sandbox_check is a
// no-op macro that always returns 1, zero overhead.

#ifndef AETHER_SANDBOX_H
#define AETHER_SANDBOX_H

#include "aether_sandbox_match.h"   /* aether_net_resource, the shared matcher */

#include <stddef.h>

// What aether_sandbox_stat reports (#3751).
#define AETHER_SANDBOX_KIND_FILE    1
#define AETHER_SANDBOX_KIND_DIR     2
#define AETHER_SANDBOX_KIND_SYMLINK 3
#define AETHER_SANDBOX_KIND_OTHER   4
typedef struct {
    int kind;
    unsigned mode;      // st_mode on POSIX; S_IFDIR or S_IFREG and the write bits on Windows
    long long size;
    long long mtime;    // seconds since the epoch
} AetherSandboxStat;

// What aether_sandbox_stage_begin holds for commit or abort (#3751).
typedef struct {
    long long anchor;   // the directory: a descriptor, or a HANDLE on Windows
    long long held;     // Windows: the temporary's own handle, for the rename
    char name[1024];    // `path`'s last name
    char tmp[1100];     // the temporary's
} AetherSandboxStaged;

#ifdef AETHER_HAS_SANDBOX

#include <stdatomic.h>

// The deepest a thread's levels go: an enforce block, a trusted call's
// barrier and a host bridge's run_sandboxed call each take one. A push past
// it panics, so a level is never silently left out.
#define AETHER_SANDBOX_MAX_LEVELS 64

// Adds a level holding `grants` (a grant list: (category, pattern) pairs;
// borrowed, the pusher keeps it alive until the matching pop). An empty
// list denies everything; NULL allows everything.
void aether_sandbox_push(void* grants);
// Takes the innermost level off.
void aether_sandbox_pop(void);
// This thread's levels, and back to `depth` of them (only ever fewer).
int  aether_sandbox_depth(void);
void aether_sandbox_restore(int depth);

// Trusted names of `sandbox.enforce(perms, name) { ... }` (#1751): the
// enforce call tags the push it is about to make with its site, `image`
// being the address of a static of the generated code that wrote it (site
// numbers are per program or library), and a trusted call pushes a
// barrier that hides the levels from that enforce's up. Returns the depth
// to restore after the call.
void aether_sandbox_tag_site(const void* image, int site);
int  aether_sandbox_trust_enter(const void* image, int site);

// A panic unwinding to the try frame at depth `frames` (aether_try_depth):
// the levels pushed inside that frame are left, as their pops are skipped.
// aether_panic calls this; nothing else needs to.
void aether_sandbox_unwind_to_frame(int frames);

// The check proper, for aether_sandbox_check below: an fs path is resolved
// to where it leads first (".." and symlinks), so a grant for "/box/*"
// cannot be left through "/box/../x" or a symlink in /box; an fs path that
// cannot be resolved is refused.
int aether_sandbox_check_slow(const char* category, const char* resource);

// For the LD_PRELOAD library, which resolves fs paths itself: 1 allowed,
// 0 refused, -1 when no level is in force on this thread (its file-based
// grants then apply).
int aether_sandbox_check_resolved(const char* category, const char* resource);
// The levels in force on this thread, in this runtime image and any other
// it shares the count with (0: none).
int aether_sandbox_levels_here(void);

// A grant pattern as the checks compare it: an fs pattern's directory
// resolved the same way; malloc'd.
char* aether_sandbox_grant_pattern(const char* category, const char* pattern);

// fs.write_atomic writes `path` by creating a temporary beside it,
// `<path>.tmp.<pid>.<n>`, and renaming that over it. The temporary is a name
// of its own, which a grant on `path` does not cover, so around the open that
// creates it, the rename of it over `path` and its removal on a failure, std
// says which file it stands for: aether_sandbox_stage(tmp, path) before,
// aether_sandbox_stage(NULL, NULL) after. Only the LD_PRELOAD library reads it
// (aether_sandbox_staged_for: `path` when `tmp` is the name staged on this
// thread, else NULL), and it then checks `path`'s fs_write under all its
// policies instead of the temporary's, for a name of exactly that shape: an
// open that creates it new (O_CREAT|O_EXCL), its rename onto `path`, and its
// unlink or remove.
// The in-process check is std's own, of `path`.
void aether_sandbox_stage(const char* tmp, const char* path);
const char* aether_sandbox_staged_for(const char* tmp);

// Threads with a level in force, in this runtime image and every other one
// it found in the process (aether_sandbox.c); NULL until the first check or
// push looks for them. Zero is the common case and costs two loads.
extern atomic_int* _Atomic aether_sandbox_in_force;

// Check if an operation is allowed (returns 1 if no level is in force on
// this thread, or if every level in force grants it)
static inline int aether_sandbox_check(const char* category, const char* resource) {
    atomic_int* in_force = atomic_load_explicit(&aether_sandbox_in_force, memory_order_acquire);
    if (in_force && atomic_load_explicit(in_force, memory_order_relaxed) == 0) return 1;
    return aether_sandbox_check_slow(category, resource);
}

// A command string run through the shell (os.system, os.exec): the exec
// grants must cover the shell (aether_sandbox_shell) as well as the command
// string, because the shell runs whatever follows the part a grant matched.
// The command is checked first, so a refused command is what the audit
// records (the thing the code asked to run, the same on every platform);
// the shell is checked, and recorded, only for a command that was granted.
static inline int aether_sandbox_check_shell(const char* cmd) {
    return aether_sandbox_check("exec", cmd) &&
           aether_sandbox_check("exec", aether_sandbox_shell());
}

// The shell's name as a string the grant builders can store
// (std.sandbox's grant_shell).
const char* aether_sandbox_shell_path(void);

// The check for a call that acts on the last name of `path` itself, not on
// where a symlink there leads (unlink, rmdir, rename, mkdir, lstat, readlink,
// the new name of link and symlink): its directory is resolved and the last
// name kept (aether_sandbox_canon_entry). 1 allowed, 0 refused.
int aether_sandbox_check_entry(const char* category, const char* resource);

// The check for a hard link to `target` named `linkpath`: fs_write on the new
// name, and the new name may not reach the file more than the old one may
// (fs_read and fs_write on `target` for whichever the new name has). 1
// allowed, 0 refused. aether_sandbox_link makes it too.
int aether_sandbox_check_link(const char* target, const char* linkpath);

// Calls made on the path the sandbox checked (#3751). Each resolves its path
// once, checks the grants on that resolution, and makes the call on what was
// checked. On POSIX the call is made on the resolution following no symlink
// (aether_sandbox_walk and aether_sandbox_anchor, aether_sandbox_path.h), so
// nothing changed between the check and the call is followed. On Windows
// what the call acts on (or the directory it acts in) is opened without
// changing it, the final path of the handle is checked, and the call is
// made through the handle (a truncation by SetEndOfFile, a removal by its
// disposition, a rename, link or new name relative to the verified
// directory). With no level in force on the calling thread each is the
// plain call. A refusal is -1 (NULL) with errno EACCES. Under the LD_PRELOAD
// library a call is passed to its wrapper, which does the same with the
// functions behind it. An open is checked for what its flags or mode ask
// (fs_read, fs_write or both, as the preload's open is); the calls on a
// name, and an open with O_NOFOLLOW or O_CREAT|O_EXCL, check that name
// (aether_sandbox_check_entry).
void* aether_sandbox_fopen(const char* path, const char* mode);   // FILE*
int   aether_sandbox_open(const char* path, int flags, int mode);
int   aether_sandbox_unlink(const char* path);
int   aether_sandbox_rmdir(const char* path);
int   aether_sandbox_remove(const char* path);
int   aether_sandbox_mkdir(const char* path, int mode);
// mkdir -p: the whole path checked once, as std's fs.mkdir_p always has,
// then each missing directory made from the one before it, none followed.
int   aether_sandbox_mkdir_p(const char* path, int mode);
int   aether_sandbox_rename(const char* from, const char* to);
// A hard link is a second name for `target`'s file: it needs fs_write on
// the new name, and the new name may not reach the file more than the old
// one may (fs_read and fs_write on `target` for what the new name has).
int   aether_sandbox_link(const char* target, const char* linkpath);

// A lookup (stat, lstat with `nofollow`): fs_read on the resolution, then
// fstatat of the name from its anchor without following it (POSIX), or a
// handle opened and verified (Windows).
int aether_sandbox_stat(const char* path, int nofollow, AetherSandboxStat* out);
// fs.realpath: the resolution the check matched, once a lookup finds
// something there. 0 with it in `out`.
int aether_sandbox_realpath(const char* path, char* out, size_t cap);
// Whether the grants would allow `category` on `path`, asked without being
// recorded. 1 with no level in force.
int aether_sandbox_granted(const char* category, const char* path);

// fs.write_atomic: a temporary beside `path`, made in the directory `path`
// resolves to (fs_write on `path` itself), and renamed onto it from there.
// begin gives the descriptor to write; commit renames, abort removes; each
// releases what begin held. -2 from begin when this is not the way here (no
// level in force, or the LD_PRELOAD library's staged open and rename do it):
// the caller then makes its own.
int  aether_sandbox_stage_begin(const char* path, AetherSandboxStaged* s);
int  aether_sandbox_stage_commit(AetherSandboxStaged* s);
void aether_sandbox_stage_abort(AetherSandboxStaged* s);

// fs.make_temp_file and fs.make_temp_dir: fs_write on `dir`, and a fresh name
// made in the directory that was checked (a file comes back closed). The
// path, `dir` as given and the name, goes to `out`. 0; -1; -2 when this is
// not the way here (as for stage_begin).
int aether_sandbox_temp_in(const char* dir, const char* prefix, int is_dir, char* out, size_t cap);

#ifndef _WIN32
void* aether_sandbox_opendir(const char* path);                   // DIR*
int   aether_sandbox_symlink(const char* target, const char* linkpath);
int   aether_sandbox_chmod(const char* path, int mode);
long  aether_sandbox_readlink(const char* path, char* buf, size_t cap);
// A descriptor of what `path` names, to ask about it (fstatvfs, a watch
// through /proc/self/fd): fs_read, opened by the walk as a path only.
int   aether_sandbox_open_lookup(const char* path);
// utimensat of `path` (`ts` a struct timespec[2]): fs_write on the
// resolution (on the name itself with `nofollow`), set from its anchor.
int   aether_sandbox_utimens(const char* path, const void* ts, int nofollow);
#else
// What `path` names (the name itself with `nofollow`), opened with
// `desired` access and `flags` (FILE_FLAG_*; FILE_FLAG_BACKUP_SEMANTICS is
// always added) and verified for `access` (AETHER_ACCESS_READ, _WRITE): a
// HANDLE, INVALID_HANDLE_VALUE on a failure (aether_sandbox_refused says
// whether the grants refused it). With no level in force it is opened as
// given. For calls that act through a handle (SetSecurityInfo,
// SetFileInformationByHandle, SetFileTime) instead of by name.
void* aether_sandbox_win_open(const char* path, unsigned long desired, unsigned long flags,
                              int nofollow, int access);
// A directory listing through a verified handle (NtQueryDirectoryFile),
// for fs.list_dir, fs.walk and fs.glob inside an enforce block. opendir
// checks fs_read on the directory and verifies the handle; `pattern`
// (FindFirstFile's wildcards, NULL for every entry) filters as
// FindFirstFile does. opendir_at opens the subdirectory `name` of `parent`
// relative to its handle, a link (a reparse point that redirects names)
// refused (NULL, errno ELOOP), as a walk on POSIX opens one with
// O_NOFOLLOW. readdir gives the next name (UTF-8, "." and ".." skipped) and
// its FILE_ATTRIBUTE_* bits: 1, 0 at the end, -1 on a failure.
void* aether_sandbox_win_opendir(const char* path, const char* pattern);
void* aether_sandbox_win_opendir_at(void* parent, const char* name);
int   aether_sandbox_win_readdir(void* dir, char* name, size_t cap, unsigned long* attrs);
void  aether_sandbox_win_closedir(void* dir);
// mkdir with a security descriptor (NULL: the folder's), for a private
// directory.
int   aether_sandbox_win_mkdir(const char* path, void* sd);
// A name std made by name (CreateSymbolicLinkW): verified, and removed again
// through its handle when a swap sent it elsewhere. 0, or -1 refused.
int   aether_sandbox_win_verify_entry(const char* path);
// What `path` names, opened for ReadDirectoryChangesW and verified: a
// HANDLE, INVALID_HANDLE_VALUE on a failure.
void* aether_sandbox_win_open_watched(const char* path);
// Is the handle std opened on `path` itself what the check saw? 1 yes (or
// no level in force), 0 no.
int   aether_sandbox_win_handle_ok(void* handle, const char* path, int nofollow, int access);
#endif
// 1 when this thread's last call above was refused by the grants, so a
// caller can tell that EACCES from the system's own.
int aether_sandbox_refused(void);

#else

// No sandbox compiled in: always allow. The arguments are evaluated for
// their side effects only, so a resource built in a local buffer
// (aether_net_resource) does not leave that buffer unused.
#define aether_sandbox_check(category, resource) ((void)(category), (void)(resource), 1)
#define aether_sandbox_stage(tmp, path) ((void)(tmp), (void)(path))
#define aether_sandbox_check_shell(cmd) ((void)(cmd), 1)
#define aether_sandbox_check_entry(category, resource) ((void)(category), (void)(resource), 1)
#define aether_sandbox_check_link(target, linkpath) ((void)(target), (void)(linkpath), 1)
#define aether_sandbox_fopen(path, mode) ((void*)fopen((path), (mode)))
#define aether_sandbox_refused() 0
#define aether_sandbox_levels_here() 0
#define aether_sandbox_granted(category, path) ((void)(category), (void)(path), 1)
#define aether_sandbox_stage_begin(path, s) ((void)(path), (void)(s), -2)
#define aether_sandbox_stage_commit(s) ((void)(s), -1)
#define aether_sandbox_stage_abort(s) ((void)(s))
#define aether_sandbox_temp_in(dir, prefix, is_dir, out, cap) \
    ((void)(dir), (void)(prefix), (void)(is_dir), (void)(out), (void)(cap), -2)
#define aether_sandbox_stat(path, nofollow, out) ((void)(path), (void)(nofollow), (void)(out), -1)
#define aether_sandbox_realpath(path, out, cap) ((void)(path), (void)(out), (void)(cap), -1)
#define aether_sandbox_mkdir_p(path, mode) ((void)(path), (void)(mode), -1)
#ifdef _WIN32
// What std calls on Windows whether or not a level is in force: as the
// sandbox's own with none in force, in aether_sandbox.c (a source every
// build compiles). The handle-relative ones exist only inside a level.
void* aether_sandbox_win_open(const char* path, unsigned long desired, unsigned long flags,
                              int nofollow, int access);
#define aether_sandbox_win_handle_ok(handle, path, nofollow, access) \
    ((void)(handle), (void)(path), (void)(nofollow), (void)(access), 1)
#define aether_sandbox_win_verify_entry(path) ((void)(path), 0)
#define aether_sandbox_win_opendir(path, pattern) ((void)(path), (void)(pattern), (void*)0)
#define aether_sandbox_win_opendir_at(parent, name) ((void)(parent), (void)(name), (void*)0)
#define aether_sandbox_win_readdir(dir, name, cap, attrs) \
    ((void)(dir), (void)(name), (void)(cap), (void)(attrs), -1)
#define aether_sandbox_win_closedir(dir) ((void)(dir))
#define aether_sandbox_win_mkdir(path, sd) ((void)(path), (void)(sd), -1)
#define aether_sandbox_win_open_watched(path) ((void)(path), (void*)0)
// Called on Windows only inside a level (std falls back to its own calls
// otherwise), which a build with no sandbox never has.
#define aether_sandbox_open(path, flags, mode) ((void)(path), (void)(flags), (void)(mode), errno = ENOSYS, -1)
#define aether_sandbox_unlink(path) ((void)(path), errno = ENOSYS, -1)
#define aether_sandbox_rmdir(path) ((void)(path), errno = ENOSYS, -1)
#define aether_sandbox_remove(path) ((void)(path), errno = ENOSYS, -1)
#define aether_sandbox_mkdir(path, mode) ((void)(path), (void)(mode), errno = ENOSYS, -1)
#define aether_sandbox_rename(from, to) ((void)(from), (void)(to), errno = ENOSYS, -1)
#define aether_sandbox_link(target, linkpath) ((void)(target), (void)(linkpath), errno = ENOSYS, -1)
#else
#define aether_sandbox_readlink(path, buf, cap) ((long)readlink((path), (buf), (cap)))
#define aether_sandbox_open_lookup(path) ((void)(path), -1)
#endif
#ifndef _WIN32
#define aether_sandbox_open(path, flags, mode) open((path), (flags), (mode))
#define aether_sandbox_opendir(path) ((void*)opendir(path))
#define aether_sandbox_unlink(path) unlink(path)
#define aether_sandbox_rmdir(path) rmdir(path)
#define aether_sandbox_remove(path) remove(path)
#define aether_sandbox_mkdir(path, mode) mkdir((path), (mode_t)(mode))
#define aether_sandbox_rename(from, to) rename((from), (to))
#define aether_sandbox_symlink(target, linkpath) symlink((target), (linkpath))
#define aether_sandbox_link(target, linkpath) link((target), (linkpath))
#define aether_sandbox_chmod(path, mode) chmod((path), (mode_t)(mode))
#define aether_sandbox_utimens(path, ts, nofollow) \
    utimensat(AT_FDCWD, (path), (const struct timespec*)(ts), (nofollow) ? AT_SYMLINK_NOFOLLOW : 0)
#endif

#endif // AETHER_HAS_SANDBOX

#endif // AETHER_SANDBOX_H
