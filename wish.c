#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define ERROR_MESSAGE "An error has occurred\n"
#define MAX_TOKENS 512

char **paths = NULL;
int num_paths = 0;

void error() {
    write(STDERR_FILENO, ERROR_MESSAGE, strlen(ERROR_MESSAGE));
}

void init_default_path() {
    num_paths = 1;
    paths = malloc(sizeof(char*) * num_paths);
    if (!paths) {
        error();
        exit(1);
    }
    paths[0] = strdup("/bin");
}

void free_paths() {
    if (!paths) return;
    for (int i = 0; i < num_paths; ++i) free(paths[i]);
    free(paths);
    paths = NULL;
    num_paths = 0;
}

void set_path(char **newpaths, int n) {
    free_paths();
    if (n == 0) {
        paths = NULL;
        num_paths = 0;
        return;
    }
    paths = malloc(sizeof(char*) * n);
    if (!paths) { error(); exit(1); }
    for (int i = 0; i < n; ++i) {
        paths[i] = strdup(newpaths[i]);
        if (!paths[i]) { error(); exit(1); }
    }
    num_paths = n;
}

char *trim(char *s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

char **tokenize_whitespace(char *cmd, int *out_argc) {
    char **argv = malloc(sizeof(char*) * (MAX_TOKENS + 1));
    if (!argv) { error(); return NULL; }
    int argc = 0;
    char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        int len = p - start;
        char *tok = malloc(len + 1);
        if (!tok) { error(); /* free */
            for (int i=0;i<argc;i++) free(argv[i]); free(argv); return NULL; }
        strncpy(tok, start, len);
        tok[len] = '\0';
        argv[argc++] = tok;
        if (argc >= MAX_TOKENS) break;
    }
    argv[argc] = NULL;
    if (out_argc) *out_argc = argc;
    return argv;
}

void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i] != NULL; ++i) free(argv[i]);
    free(argv);
}

char *resolve_executable(const char *cmd) {
    if (!cmd) return NULL;
    if (num_paths == 0) return NULL; 
    for (int i = 0; i < num_paths; ++i) {
        int needed = strlen(paths[i]) + 1 + strlen(cmd) + 1;
        char *full = malloc(needed);
        if (!full) { error(); return NULL; }
        snprintf(full, needed, "%s/%s", paths[i], cmd);
        if (access(full, X_OK) == 0) return full;
        free(full);
    }
    return NULL;
}

pid_t launch_command(char **argv, int argc, const char *outfile) {
    if (argc == 0) return -1;
    char *cmd = argv[0];
    char *fullpath = resolve_executable(cmd);
    if (!fullpath) {
        error();
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        error();
        free(fullpath);
        return -1;
    }
    if (pid == 0) {
        if (outfile) {
            int fd = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0666);
            if (fd < 0) {
                error();
                _exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0) { error(); _exit(1); }
            if (dup2(fd, STDERR_FILENO) < 0) { error(); _exit(1); }
            close(fd);
        }
        execv(fullpath, argv);
        error();
        _exit(1);
    }

    free(fullpath);
    return pid;
}

/* Handle a single command string (no ampersand). Returns pid if external launched, 0 if built-in handled, -1 if error/no-op. If launched in background, returns pid>0 and caller should wait later. */
int handle_single_command(char *command, pid_t *out_pid) {
    *out_pid = -1;
    if (!command) return -1;
    command = trim(command);
    if (strlen(command) == 0) return -1; // empty

    /* check redirection: count '>' occurrences */
    /* NEW: check if left side before '>' is empty */
    char *p = command;
    while (*p == ' ' || *p == ' ') p++;
    if (*p == '>') {
    error();
    return -1;
    }
    /* NEW: ensure left command (before '>') is not empty */
    char *redir_ptr = strchr(command, '>');
    /* NEW: if '>' is the first non-whitespace character, left side is empty */
    if (redir_ptr != NULL && redir_ptr == command) {
        error();
        return -1;
    }

    /* check redirection: count '>' occurrences */
    char *outfile = NULL;
    if (redir_ptr) {
        /* ensure only one '>' */
        char *second = strchr(redir_ptr + 1, '>');
        if (second) { error(); return -1; }
        /* split */
        *redir_ptr = '\0';
        char *right = redir_ptr + 1;
        right = trim(right);
        if (strlen(right) == 0) { error(); return -1; }
        /* there should be exactly one token on right (filename) */
        int rc = 0;
        char **right_argv = tokenize_whitespace(right, &rc);
        if (!right_argv) { error(); return -1; }
        if (rc != 1) { error(); free_argv(right_argv); return -1; }
        outfile = strdup(right_argv[0]);
        free_argv(right_argv);
    }

    /* tokenize left part */
    command = trim(command);
    int argc = 0;
    char **argv = tokenize_whitespace(command, &argc);
    if (!argv) { if (outfile) free(outfile); return -1; }
    if (argc == 0) { free_argv(argv); if (outfile) free(outfile); return -1; }

    /* built-ins: exit, cd, path */
    if (strcmp(argv[0], "exit") == 0) {
        if (argc != 1) {
            error();
            free_argv(argv);
            if (outfile) free(outfile);
            return -1;
        }
        free_argv(argv);
        if (outfile) free(outfile);
        exit(0);
    }
    if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            error();
            free_argv(argv);
            if (outfile) free(outfile);
            return -1;
        }
        if (chdir(argv[1]) != 0) {
            error();
            free_argv(argv);
            if (outfile) free(outfile);
            return -1;
        }
        free_argv(argv);
        if (outfile) free(outfile);
        return 0;
    }
    if (strcmp(argv[0], "path") == 0) {
        /* argv[1..argc-1] are new paths */
        if (argc - 1 == 0) {
            set_path(NULL, 0);
        } else {
            /* build array of char* */
            char **newp = malloc(sizeof(char*) * (argc - 1));
            if (!newp) { error(); free_argv(argv); if (outfile) free(outfile); return -1; }
            for (int i = 1; i < argc; ++i) newp[i-1] = argv[i];
            /* note: set_path copies strings, so it's safe */
            set_path(newp, argc - 1);
            free(newp);
        }
        free_argv(argv);
        if (outfile) free(outfile);
        return 0;
    }

    /* external command */
    pid_t pid = launch_command(argv, argc, outfile);
    if (pid > 0) {
        *out_pid = pid;
        free_argv(argv);
        if (outfile) free(outfile);
        return 1;
    } else {
        free_argv(argv);
        if (outfile) free(outfile);
        return -1;
    }
}

int main(int argc, char *argv[]) {
    FILE *input = stdin;
    int interactive = 1;

    if (argc == 1) {
        interactive = 1;
    } else if (argc == 2) {
        interactive = 0;
        input = fopen(argv[1], "r");
        if (!input) {
            error();
            exit(1);
        }
    } else {
        error();
        exit(1);
    }

    init_default_path();

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;

    while (1) {
        if (interactive) {
            printf("wish> ");
            fflush(stdout);
        }
        nread = getline(&line, &len, input);
        if (nread == -1) {
            /* EOF */
            free(line);
            if (!interactive) fclose(input);
            exit(0);
        }
        /* remove trailing newline */
        if (nread > 0 && line[nread-1] == '\n') line[nread-1] = '\0';

        /* Skip empty lines */
        char *tline = trim(line);
        if (strlen(tline) == 0) continue;

        /* Split by '&' for parallel commands. We must not lose empty tokens; treat consecutive & as errors (empty command) */
        /* We'll use a manual scan to extract substrings between & */
        char *cursor = tline;
        char *start = cursor;
        pid_t pids[MAX_TOKENS];
        int pcount = 0;
        int error_in_line = 0;

        while (1) {
            char *amp = strchr(start, '&');
            if (!amp) {
                /* last command */
                char *cmd = strdup(start);
                char *cmd_trim = trim(cmd);
                if (strlen(cmd_trim) == 0) {
                    /* empty command at end */
                    free(cmd);
                    error_in_line = 1;
                } else {
                    pid_t child = -1;
                    int rc = handle_single_command(cmd_trim, &child);
                    if (rc == -1) {
                        /* if built-in error or other parse error, mark but continue */
                        error_in_line = 1;
                    }
                    if (child > 0) pids[pcount++] = child;
                    free(cmd);
                }
                break;
            } else {
                /* command from start to amp-1 */
                *amp = '\0';
                char *cmd = strdup(start);
                char *cmd_trim = trim(cmd);
                if (strlen(cmd_trim) == 0) {
                    /* empty command between ampersands is error */
                    free(cmd);
                    error_in_line = 1;
                } else {
                    pid_t child = -1;
                    int rc = handle_single_command(cmd_trim, &child);
                    if (rc == -1) {
                        error_in_line = 1;
                    }
                    if (child > 0) pids[pcount++] = child;
                    free(cmd);
                }
                start = amp + 1;
                /* continue loop */
            }
        }

        /* After launching all children for this line, wait for them */
        for (int i = 0; i < pcount; ++i) {
            int status;
            waitpid(pids[i], &status, 0);
        }
        /* continue to next line */
    }

    /* never reached */
    return 0;
}
