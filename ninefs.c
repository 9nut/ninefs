/*
 * ninefs.c
 *  Dokan-based 9p filesystem for windows.
 *
 * TODO:
 *  - error reporting when dokan fails will make it easier for users
 *    with dokan incorrectly installed
 *  - better 8.3 support?  eliminate?
 *  - make good win32 error codes
 *  - investigate user security.  right now all files appear to be
 *    owned by the user mounting the filesystem.  Is this a dokan limitation?
 *  - ssl support
 *  - support attach name as an argument
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "npfs.h"
#include "npclient.h"
#include "npauth.h"
#include "dokan.h"

#define ARRSZ(a)    (sizeof(a) / sizeof((a)[0]))

static Npuser *user = NULL;
static Npcfsys *fs = NULL;
static int debug = 0;
static int transPath = 1;

static int optind = 1;
static int optpos = 0;
static char *optarg = NULL;

static int
getopt(int argc, char **argv, const char *opts)
{
    char *p, ch;

    if(optind >= argc || !argv[optind])
        return -1;
    if(optpos && !argv[optind][optpos]) {
        optind ++;
        optpos = 0;
    }
    if(optind >= argc || !argv[optind])
        return -1;
    if(optpos == 0 && argv[optind][optpos++] != '-')
        return -1;
    ch = argv[optind][optpos++];
    p = strchr(opts, ch);
    if(!p)
        return '?';
    if(p[1] != ':')
        return ch;

    optarg = argv[optind++] + optpos;
    optpos = 0;
    if(*optarg)
        return ch;
    if(optind >= argc || !argv[optind])
        return '?';
    optarg = argv[optind++];
    return ch;
}

static int
notyet(char *msg) {
    fprintf(stderr, "notyet: %s\n", msg);
    fflush(stderr);
    return -(int)ERROR_CALL_NOT_IMPLEMENTED;
}

// Return a dynamically allocated utf8 string.
static char *
utf8(LPCWSTR ws)
{
    char *s;
    int l, e;

    if(!ws)
        ws = L"";

    l = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    e = GetLastError();
    if((!transPath && e == ERROR_NO_UNICODE_TRANSLATION)
    || l == 0) {
        if(debug)
            fprintf(stderr, "utf8 bad conversion: %d\n", e);
        return NULL;
    }
    s = malloc(l);
    if(!s) {
        if(debug)
            fprintf(stderr, "utf8 malloc failed\n");
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, l, NULL, NULL);
    return s;
}

// Return a dynamically allocated path in utf8 format with p9 conversions.
static char*
p9path(LPCWSTR ws)
{
    char *fn = utf8(ws);
    int i;

    if(fn) {
        for(i = 0; fn[i]; i++) {
            switch(fn[i]) {
            case '\\':
                fn[i] = '/';
                break;
            case ' ':
                if(transPath)
                    fn[i] = '?';
                break;
            }
        }
    }
    return fn;
}

// Return a dynamically allocated wide string.
static LPWSTR
wstr(char *s)
{
    LPWSTR ws;
    int l, e;

    if(!s)
        s = "";
    l = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    e = GetLastError();
    if((!transPath && e == ERROR_NO_UNICODE_TRANSLATION)
    || l == 0) {
        if(debug)
            fprintf(stderr, "wstr bad conversion: %d\n", e);
        return NULL;
    }
    ws = malloc(l * sizeof *ws);
    if(!s) {
        if(debug)
            fprintf(stderr, "wstr malloc failed\n");
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, l);
    return ws;
}

// Return a dynamically allocated path in wstr format with win conversions.
static LPWSTR
winpath(char *s)
{
    LPWSTR fn = wstr(s);
    int i;

    if(fn) {
        for(i = 0; fn[i]; i++) {
            switch(fn[i]) {
            case L'/':
                fn[i] = L'\\';
                break;
            case L'?':
                if(transPath)
                    fn[i] = L' ';
                break;
            }
        }
    }
    return fn;
}

static void
maybeOpen(LPCWSTR fname, int omode, int *opened, Npcfid **fidp)
{
    char *fn;

    *opened = 0;
    if(*fidp)
        return;
    fn = p9path(fname);
    *fidp = npc_open(fs, fn, omode);
    if(*fidp)
        *opened = 1;
    free(fn);
}

static void
maybeClose(int *opened, Npcfid **fidp)
{
    if(*opened)
        npc_close(*fidp);
}

static u32
fromFT(const FILETIME *f)
{
    LONGLONG dt = f->dwLowDateTime | ((LONGLONG)f->dwHighDateTime << 32);
    return (u32)((dt - 116444736000000000) / 10000000);
}

static FILETIME
toFT(u32 ut)
{
    FILETIME f;
    LONGLONG dt;

    dt = Int32x32To64(ut, 10000000) + 116444736000000000;
    f.dwLowDateTime = (DWORD)dt;
    f.dwHighDateTime = (DWORD)(dt >> 32);
    return f;
}

static FILETIME zt = { 0, 0 };

static void
toFileInfo(Npwstat *st, LPBY_HANDLE_FILE_INFORMATION fi)
{
    fi->dwFileAttributes = 0;
    if(st->qid.type & Qtdir)
        fi->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    else
        fi->dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
    fi->ftCreationTime = zt;
    fi->ftLastAccessTime = toFT(st->atime);
    fi->ftLastWriteTime = toFT(st->mtime);
    fi->dwVolumeSerialNumber = st->dev;
    fi->nFileSizeHigh = (DWORD)(st->length >> 32);
    fi->nFileSizeLow = (DWORD)st->length;
    fi->nNumberOfLinks = 1;
    fi->nFileIndexHigh = (DWORD)(st->qid.path >> 32);
    fi->nFileIndexLow = (DWORD)st->qid.path;
}

static int
toFindData(Npwstat *st, WIN32_FIND_DATA *fd)
{
    LPWSTR fn = winpath(st->name);
    int i, j;

    if(!fn)
        return -(int)ERROR_FILE_NOT_FOUND;
    fd->dwFileAttributes = 0;
    if(st->qid.type & Qtdir)
        fd->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    else
        fd->dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
    fd->ftCreationTime = zt;
    fd->ftLastAccessTime = toFT(st->atime);
    fd->ftLastWriteTime = toFT(st->mtime);
    fd->nFileSizeHigh = (DWORD)(st->length >> 32);
    fd->nFileSizeLow = (DWORD)st->length;
    fd->dwReserved0 = 0;
    fd->dwReserved1 = 0;
    wcsncpy(fd->cFileName, fn, ARRSZ(fd->cFileName) - 1);
    free(fn);

    // XXX this is a really bad hack:
    for(j = 0, i = 0 ; j < 13 && fd->cFileName[i]; i++) {
        if(j == 8)
            fd->cAlternateFileName[j++] = '.';
        if(st->name[i] != '.')
            fd->cAlternateFileName[j++] = fd->cFileName[i];
    }
    fd->cAlternateFileName[j] = 0;
    return 0;
}

static int
cvtError(void)
{
    char *err;
    int num;

    np_rerror(&err, &num);
    switch(num) {
    case ENOENT: return -(int)ERROR_FILE_NOT_FOUND;
    default: return -(int)ERROR_INVALID_PARAMETER; // XXX bogus
    }
}


static int
_CreateFile(
    LPCWSTR                 FileName,
    DWORD                   AccessMode,
    DWORD                   ShareMode,
    DWORD                   CreationDisposition,
    DWORD                   FlagsAndAttributes,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    Npcfid *fid = NULL;
    char *fn;
    int omode, rd, wr;

    if(debug)
        fprintf(stderr, "createfile '%ws' create %d access %x flags %x\n", FileName, CreationDisposition, AccessMode, FlagsAndAttributes);
    rd = (AccessMode & (GENERIC_READ | FILE_READ_DATA)) != 0;
    wr = (AccessMode & (GENERIC_WRITE | FILE_WRITE_DATA)) != 0;
    if(rd && wr)
        omode = Ordwr;
    else if(wr)
        omode = Owrite;
    else
        omode = Oread;
    if(CreationDisposition == TRUNCATE_EXISTING)
        omode |= Otrunc;

    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    fid = npc_open(fs, fn, omode);
    if(!fid && (CreationDisposition == CREATE_ALWAYS || 
                CreationDisposition == CREATE_NEW ||
                CreationDisposition == OPEN_ALWAYS)) {
        fid = npc_create(fs, fn, 0666, omode);
    }
    free(fn);
    if(!fid) {
        if(debug)
            fprintf(stderr, "open %ws failed\n", FileName);
        return cvtError();
    }
    DokanFileInfo->Context = (ULONG64)fid;
    return 0;
}

static int
_CreateDirectory(
    LPCWSTR                 FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    Npcfid *fid;
    char *fn;
    int perm;

    if(debug)
        fprintf(stderr, "create directory '%ws'\n", FileName);
    fn = p9path(FileName);
    perm = Dmdir | 0777; // XXX figure out perm
    fid = npc_create(fs, fn, perm, Oread);
    if(fid)
        npc_close(fid);
    free(fn);
    if(!fid) {
        if(debug)
            fprintf(stderr, "create directory %ws failed\n", FileName);
        return cvtError();
    }
    return 0;
}

static int
_OpenDirectory(
    LPCWSTR                 FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    Npcfid *fid = NULL;
    char *fn;
    int e;

    if(debug)
        fprintf(stderr, "open directory '%ws'\n", FileName);
    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;

    e = 0;
    fid = npc_open(fs, fn, Oread);
    if(fid && !(fid->qid.type & Qtdir)) {
        npc_close(fid);
        fid = NULL;
        e = -(int)ERROR_DIRECTORY; // XXX?
    } else if(!fid) {
        e = cvtError();
    }
    free(fn);

    if(e) {
        if(debug)
            fprintf(stderr, "diropen %ws failed\n", FileName);
        return e;
    }
    DokanFileInfo->Context = (ULONG64)fid;
    return 0;
}

static int
_CloseFile(
    LPCWSTR                 FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    Npcfid *fid = (Npcfid *)DokanFileInfo->Context;

    if(fid) {
        DokanFileInfo->Context = 0;
        npc_close(fid);
    }
    return 0;
}

static int
_Cleanup(
    LPCWSTR                 FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    // XXX handle delete-on-close?
    return _CloseFile(FileName, DokanFileInfo);
}

static int
_ReadFile(
    LPCWSTR             FileName,
    LPVOID              Buffer,
    DWORD               BufferLength,
    LPDWORD             ReadLength,
    LONGLONG            Offset,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npcfid *fid = (Npcfid *)DokanFileInfo->Context;
    int e, r, opened;

    if(debug)
        fprintf(stderr, "readfile\n");
    maybeOpen(FileName, Oread, &opened, &fid);
    if(!fid)
        return cvtError();
    e = 0;
    r = npc_read(fid, Buffer, BufferLength, Offset);
    if(r < 0)
        e = cvtError();
    maybeClose(&opened, &fid);
    if(e) {
        if(debug)
            fprintf(stderr, "readfile error\n");
        return e;
    }
    *ReadLength = r;
    return 0;
}

static int
_WriteFile(
    LPCWSTR     FileName,
    LPCVOID     Buffer,
    DWORD       NumberOfBytesToWrite,
    LPDWORD     NumberOfBytesWritten,
    LONGLONG            Offset,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npcfid *fid = (Npcfid *)DokanFileInfo->Context;
    int e, r, opened;

    if(debug)
        fprintf(stderr, "writefile\n");
    maybeOpen(FileName, Owrite, &opened, &fid);
    if(!fid)
        return cvtError();
    e = 0;
    r = npc_write(fid, (u8*)Buffer, NumberOfBytesToWrite, Offset);
    if(r < 0)
        e = cvtError();
    maybeClose(&opened, &fid);
    if(e) {
        if(debug)
            fprintf(stderr, "writefile error\n");
        return e;
    }
    *NumberOfBytesWritten = r;
    return 0;
}

static int
_FlushFileBuffers(
    LPCWSTR     FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npwstat st;
    char *fn;
    int e, r;

    if(debug)
        fprintf(stderr, "flushfilebuffers '%ws'\n", FileName);
    fn = p9path(FileName);
    npc_emptystat(&st);
    e = 0;
    r = npc_wstat(fs, fn, &st);
    if(r < 0)
        e = cvtError();
    free(fn);
    if(e) {
        if(debug)
            fprintf(stderr, "flushfilebuffers error\n");
        return e;
    }
    return 0;
}

static int
_GetFileInformation(
    LPCWSTR                         FileName,
    LPBY_HANDLE_FILE_INFORMATION    fi,
    PDOKAN_FILE_INFO                DokanFileInfo)
{
    Npwstat *st = NULL;
    char *fn;
    int e;

    if(debug)
        fprintf(stderr, "getfileinfo '%ws'\n", FileName);
    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    e = 0;
    st = npc_stat(fs, fn);
    if(st) {
        toFileInfo(st, fi);
        free(st);
    } else {
        e = cvtError();
    }
    free(fn);
    if(e) {
        if(debug)
            fprintf(stderr, "getfileinfo error\n");
        return e;
    }
    return 0;
}

static int
_FindFiles(
    LPCWSTR             FileName,
    PFillFindData       FillFindData, // function pointer
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npwstat *st;
    WIN32_FIND_DATA findData;
    Npcfid *fid = NULL;
    char *fn;
    int cnt, i, e;

    if(debug)
        fprintf(stderr, "findfiles '%ws'\n", FileName);
    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    e = 0;
    fid = npc_open(fs, fn, Oread);
    if(!fid)
        e = cvtError();
    while(fid) {
        cnt = npc_dirread(fid, &st);
        if(cnt == 0)
            break;
        if(cnt < 0) {
            e = cvtError();
            break;
        }
        for(i = 0; i < cnt; i++) {
            if(!st[i].name[0])
                continue;
            e = toFindData(&st[i], &findData);
            if(e) {
                if(debug)
                    fprintf(stderr, "findfiles error converting '%s'... eliding.\n", st[i].name);
                continue;
            }
            FillFindData(&findData, DokanFileInfo);
        }
        free(st);
    }
    if(fid)
        npc_close(fid);
    free(fn);
    if(e) {
        if(debug)
            fprintf(stderr, "findfiles failed\n");
        return e;
    }
    return 0;
}

static int
_DeleteFile(
    LPCWSTR             FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    char *fn;
    int r;

    if(debug)
        fprintf(stderr, "deletefile %ws\n", FileName);
    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    r = npc_remove(fs, fn);
    free(fn);
    if(r < 0) {
        if(debug)
            fprintf(stderr, "deletefile %ws failed\n", FileName);
        return cvtError();
    }
    return 0;
}

static int
_DeleteDirectory(
    LPCWSTR             FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    return _DeleteFile(FileName, DokanFileInfo);
}

static int
_MoveFile(
    LPCWSTR             FileName, // existing file name
    LPCWSTR             NewFileName,
    BOOL                ReplaceIfExisting,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npwstat st;
    char *fn, *fn2, *p, *newname;
    int dirlen, r, e;

    e = 0;
    if(debug)
        fprintf(stderr, "move %ws to %ws\n", FileName, NewFileName);
    fn = p9path(FileName);
    fn2 = p9path(NewFileName);
    if(!fn || !fn2) {
        e = -(int)ERROR_NOT_ENOUGH_MEMORY;
        goto err;
    }

    p = strrchr(fn, '/');
    if(p) {
        dirlen = p - fn;
        newname = fn2 + dirlen + 1;
    } else {
        dirlen = 0;
        newname = fn2;
    }
    // same directory?
    if(strncmp(fn, fn2, dirlen) != 0 || strchr(newname, '/') != NULL) {
        // XXX better error?  cant move files between directories...
        e = -(int)ERROR_NOT_SAME_DEVICE;
        goto err;
    }

    npc_emptystat(&st);
    st.name = newname;
    r = npc_wstat(fs, fn, &st);
    if(r < 0) {
        e = cvtError();
        goto err; 
    }
    /* fall through */

err:
    if(fn)
        free(fn);
    if(fn2)
        free(fn2);
    if(e) {
        if(debug)
            fprintf(stderr, "move failed\n");
        return e;
    }
    return 0;
}

static int
_LockFile(
    LPCWSTR             FileName,
    LONGLONG            ByteOffset,
    LONGLONG            Length,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    // always fail; we dont do locking.
    return -(int)ERROR_NOT_SUPPORTED;
}

static int
_SetEndOfFile(
    LPCWSTR             FileName,
    LONGLONG            ByteOffset,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npwstat st;
    char *fn;
    int r;

    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    npc_emptystat(&st);
    st.length = ByteOffset;
    r = npc_wstat(fs, fn, &st);
    free(fn);
    if(r < 0)
        return cvtError();
    return 0;
}

static int
_SetAllocationSize(
    LPCWSTR             FileName,
    LONGLONG            AllocSize,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    // XXX what exactly is this?
    return notyet("SetAllocationSize");
}

static int
_SetFileAttributes(
    LPCWSTR             FileName,
    DWORD               FileAttributes,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    if(debug)
        fprintf(stderr, "setfileattributes '%ws' %x\n", FileName, FileAttributes);
    FileAttributes &= ~FILE_ATTRIBUTE_NORMAL;
    if(FileAttributes) {
        if(debug)
            fprintf(stderr, "setfileattributes error (unsupported bits)\n");
        return -(int)ERROR_NOT_SUPPORTED;
    }
    return 0;
}


static int
_SetFileTime(
    LPCWSTR             FileName,
    CONST FILETIME*     CreationTime,
    CONST FILETIME*     LastAccessTime,
    CONST FILETIME*     LastWriteTime,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    Npwstat st;
    char *fn;
    int r;

    if(!LastAccessTime && !LastWriteTime)
        return 0;
    fn = p9path(FileName);
    if(!fn)
        return -(int)ERROR_NOT_ENOUGH_MEMORY;
    npc_emptystat(&st);
    if(LastAccessTime)
        st.atime = fromFT(LastAccessTime);
    if(LastWriteTime)
        st.mtime = fromFT(LastWriteTime);
    r = npc_wstat(fs, fn, &st);
    free(fn);
    if(r < 0)
        return cvtError();
    return 0;
}

static int
_UnlockFile(
    LPCWSTR             FileName,
    LONGLONG            ByteOffset,
    LONGLONG            Length,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    // always fail; we dont do locking.
    return -(int)ERROR_NOT_SUPPORTED;
}

static int
_Unmount(
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    if(debug)
        fprintf(stderr, "unmount\n");
    npc_umount(fs);
    fs = NULL;
    return 0;
}

static void
usage(char *prog)
{
    fprintf(stderr, "usage:  %s [-cdDtU] [-a authserv] [-p passwd] [-u user] addr driveletter\n", prog);
    fprintf(stderr, "\taddr and authserv must be of the form tcp!hostname!port\n");
    fprintf(stderr, "\t-c\tchatty npfs messages\n");
    fprintf(stderr, "\t-d\tninefs debug messages\n");
    fprintf(stderr, "\t-D\tDokan debug mesages\n");
    fprintf(stderr, "\t-t\tdo not perform path character translations\n");
    fprintf(stderr, "\t-U\tdisable 9p2000.u support\n");
    _exit(1);
}

int __cdecl
main(int argc, char **argv)
{
    extern int npc_chatty;
    DOKAN_OPERATIONS ops;
    DOKAN_OPTIONS opt;
    WSADATA wsData;
    char *serv, *authserv, *passwd, *uname, *prog;
    int x, ch, dotu;
    char letter;

    WSAStartup(MAKEWORD(2,2), &wsData);
    memset(&opt, 0, sizeof opt);

    uname = "nobody";
    prog = argv[0];
    dotu = 1;
    authserv = NULL;
    passwd = NULL;
    while((ch = getopt(argc, argv, "a:cdDp:tu:U")) != -1) {
        switch(ch) {
        case 'a':
            authserv = optarg;
            break;
        case 'c':
            npc_chatty = 1;
            break;
        case 'd':
            debug = 1;
            break;
        case 'D':
            opt.Options |= DOKAN_OPTION_DEBUG | DOKAN_OPTION_STDERR;
            break;
        case 'p':
            passwd = optarg;
            break;
        case 't':
            transPath = 0;
            break;
        case 'u':
            uname = optarg;
            break;
        case 'U':
            dotu = 0;
            break;
            
        default:
            usage(prog);
        }
    
    }
    argc -= optind;
    argv += optind;
    if(argc != 2 || !argv[1][0])
        usage(prog);
    serv = argv[0];
    letter = argv[1][0];

    user = np_default_users->uname2user(np_default_users, uname);
    if(passwd) {
        struct npcauth auth;

        if(!authserv)
            authserv = serv;
        memset(&auth, 0, sizeof auth);
        makeKey(passwd, auth.key);
        auth.srv = npc_netaddr(authserv, 567);
        fs = npc_netmount(npc_netaddr(serv, 564), dotu, user, 564, authp9any, &auth);
    } else {
        fs = npc_netmount(npc_netaddr(serv, 564), dotu, user, 564, NULL, NULL);
    }
    if(!fs) {
        char *emsg;
        int eno;

        np_rerror(&emsg, &eno);
        fprintf(stderr, "failed to mount %s: (%d) %s\n", serv, eno, emsg);
        return 1;
    }

    opt.ThreadCount = 0;
    opt.DriveLetter = letter;
    //opt.Options |= DOKAN_OPTION_KEEP_ALIVE;

    memset(&ops, 0, sizeof ops);
    ops.CreateFile = _CreateFile;
    ops.OpenDirectory = _OpenDirectory;
    ops.CreateDirectory = _CreateDirectory;
    ops.Cleanup = _Cleanup;
    ops.CloseFile = _CloseFile;
    ops.ReadFile = _ReadFile;
    ops.WriteFile = _WriteFile;
    ops.FlushFileBuffers = _FlushFileBuffers;
    ops.GetFileInformation = _GetFileInformation;
    ops.FindFiles = _FindFiles;
    ops.FindFilesWithPattern = NULL;
    ops.SetFileAttributes = _SetFileAttributes;
    ops.SetFileTime = _SetFileTime;
    ops.DeleteFile = _DeleteFile;
    ops.DeleteDirectory = _DeleteDirectory;
    ops.MoveFile = _MoveFile;
    ops.SetEndOfFile = _SetEndOfFile;
    ops.SetAllocationSize = _SetAllocationSize;
    ops.LockFile = _LockFile;
    ops.UnlockFile = _UnlockFile;
    ops.GetDiskFreeSpace = NULL;
    ops.GetVolumeInformation = NULL;
    ops.Unmount = _Unmount;
    x = DokanMain(&opt, &ops);
    if(x)
        fprintf(stderr, "error: %x\n", x);
    return 0;
}

