/* Started with no argument, the program starts copies of itself by posix_spawn; a copy's errand is
 * its second argument. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <openkal/fs.h>

extern char** environ;

static int failures;
static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static const char* self;

static int start(char* const* argv, const posix_spawn_file_actions_t* fa, int* status) {
    pid_t pid;
    const int rc = posix_spawn(&pid, self, fa, NULL, argv, environ);
    if (rc != 0) return rc;
    if (waitpid(pid, status, 0) != pid) return errno;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "vector") == 0) {
        const char* v = getenv("OKSTARTS");
        const int ok = argc == 6 && strcmp(argv[0], "kid") == 0 && strcmp(argv[2], "a b") == 0
                    && strcmp(argv[3], "") == 0 && strcmp(argv[4], "q\"uote") == 0
                    && strcmp(argv[5], "C:\\A B\\") == 0 && v && strcmp(v, "set") == 0;
        return ok ? 3 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "handles") == 0) {
        /* No handle of its starter's but the three streams and DOS's AUX and PRN: of the DOS
         * handles from 5, one answers a seek, the program's own file, which its stub holds. */
        int open = 0;
        for (unsigned long h = 5; h < 20; ++h) {
            kal_u64 at;
            if (kal_fs_seek((struct kal_file){ h }, 0, KAL_SEEK_CURRENT, &at) == kal_ok) ++open;
        }
        return open == 1 ? 5 : 10 + open;
    }
    if (argc >= 2 && strcmp(argv[1], "write") == 0) {
        printf("written by the copy\n");
        return 4;
    }
    self = argv[0];
    printf("starts, as %s\n", self);
    int status = -1;

    setenv("OKSTARTS", "set", 1);
    char* vector[] = { "kid", "vector", "a b", "", "q\"uote", "C:\\A B\\", NULL };
    check(start(vector, NULL, &status) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 3,
          "a copy receives its first element, an argument with a blank, an empty one, one with a quote "
          "and one ending in a backslash, and the variable set; its status comes back");

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "SPAWNED.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char* write[] = { "kid", "write", NULL };
    const int placed = start(write, &fa, &status);
    posix_spawn_file_actions_destroy(&fa);
    char text[64] = {0};
    FILE* f = fopen("SPAWNED.TXT", "r");
    if (f) { fgets(text, sizeof text, f); fclose(f); }
    check(placed == 0 && WEXITSTATUS(status) == 4 && strncmp(text, "written by the copy", 19) == 0,
          "a copy's standard output placed on a file is written there");

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    const int nulled = start(write, &fa, &status);
    posix_spawn_file_actions_destroy(&fa);
    check(nulled == 0 && WEXITSTATUS(status) == 4, "a copy's standard output on the null device");

    FILE* held = fopen("HELD.TXT", "w");
    char* handles[] = { "kid", "handles", NULL };
    check(held && start(handles, NULL, &status) == 0 && WEXITSTATUS(status) == 5,
          "a copy receives no handle of its starter's beyond its streams, a file held open included");
    if (held) fclose(held);

    pid_t pid;
    char* absent[] = { "absent", NULL };
    check(posix_spawn(&pid, "C:\\ABSENT.EXE", NULL, NULL, absent, environ) == ENOENT,
          "a name that is not there is ENOENT");
    static char longer[200];
    memset(longer, 'x', sizeof longer - 1);
    char* toolong[] = { "kid", "write", longer, NULL };
    check(posix_spawn(&pid, self, NULL, NULL, toolong, environ) == E2BIG,
          "a command tail beyond DOS's 126 bytes is E2BIG, not shortened");
    puts(failures ? "FAILED" : "all held");
    return failures ? 1 : 42;
}
