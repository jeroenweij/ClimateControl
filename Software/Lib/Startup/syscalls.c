/*************************************************************
 * Created by J. Weij
 *
 * Minimal newlib syscall stubs. These firmware images do no file I/O and
 * (with _Min_Heap_Size == 0 in the linker script) no heap allocation --
 * the stubs exist only so newlib links without the "_x is not implemented"
 * warnings. _sbrk still supports a heap if a future image needs one.
 *************************************************************/

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

extern char _end; /* set by the linker script */

void* _sbrk(ptrdiff_t incr)
{
    static char* heap = &_end;
    char*        prev = heap;
    heap += incr;
    return prev;
}

int _close(int fd)
{
    (void)fd;
    return -1;
}

int _lseek(int fd, int offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    return 0;
}

int _read(int fd, char* buf, int len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return 0;
}

int _write(int fd, const char* buf, int len)
{
    (void)fd;
    (void)buf;
    return len;
}

int _fstat(int fd, struct stat* st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int code)
{
    (void)code;
    while (1)
    {
    }
}
