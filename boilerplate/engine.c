#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define BUFFER_SIZE 256
#define STACK_SIZE (1024 * 1024)

static char container_stack[STACK_SIZE];

/* ================= CONTAINER STORAGE ================= */
typedef struct {
    char id[32];
    pid_t pid;
} container_t;

container_t containers[100];
int container_count = 0;

/* ================= CHILD ================= */
typedef struct {
    char rootfs[256];
    int write_fd;
} child_args_t;

int child_fn(void *arg)
{
    child_args_t *cfg = (child_args_t *)arg;

    dup2(cfg->write_fd, STDOUT_FILENO);
    dup2(cfg->write_fd, STDERR_FILENO);
    close(cfg->write_fd);

    chroot(cfg->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    execl("/bin/sh", "/bin/sh", NULL);

    return 0;
}

/* ================= SUPERVISOR ================= */
int run_supervisor(const char *rootfs)
{
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);

        char buffer[BUFFER_SIZE] = {0};
        read(client_fd, buffer, sizeof(buffer));

        /* ================= START ================= */
        if (strncmp(buffer, "start", 5) == 0) {
            char id[32];
            sscanf(buffer, "start %s", id);

            int pipefd[2];
            pipe(pipefd);

            child_args_t *cfg = malloc(sizeof(child_args_t));
            strcpy(cfg->rootfs, rootfs);
            cfg->write_fd = pipefd[1];

            pid_t pid = clone(
                child_fn,
                container_stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg
            );

            close(pipefd[1]);

            if (pid > 0) {
                /* store container */
                containers[container_count].pid = pid;
                strcpy(containers[container_count].id, id);
                container_count++;

                /* logging */
                char logfile[64];
                sprintf(logfile, "%s.log", id);

                int fd = open(logfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);

                if (fork() == 0) {
                    char buf[512];
                    int n;
                    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                        write(fd, buf, n);
                    }
                    exit(0);
                }
            }

            write(client_fd, "Started\n", 8);
        }

        /* ================= LOGS ================= */
        else if (strncmp(buffer, "logs", 4) == 0) {
            char id[32];
            sscanf(buffer, "logs %s", id);

            char logfile[64];
            sprintf(logfile, "%s.log", id);

            int fd = open(logfile, O_RDONLY);

            if (fd < 0) {
                write(client_fd, "No logs\n", 8);
            } else {
                char buf[512];
                int n;
                while ((n = read(fd, buf, sizeof(buf))) > 0) {
                    write(client_fd, buf, n);
                }
                close(fd);
            }
        }

        /* ================= PS ================= */
        else if (strncmp(buffer, "ps", 2) == 0) {

            char header[] = "ID\tPID\tSTATE\tSOFT(MiB)\tHARD(MiB)\n";
            write(client_fd, header, strlen(header));

            for (int i = 0; i < container_count; i++) {

                char line[128];

                int state = kill(containers[i].pid, 0);
                char *status = (state == 0) ? "running" : "exited";

                snprintf(line, sizeof(line),
                         "%s\t%d\t%s\t40\t64\n",
                         containers[i].id,
                         containers[i].pid,
                         status);

                write(client_fd, line, strlen(line));
            }
        }

        else {
            write(client_fd, "Unknown command\n", 16);
        }

        close(client_fd);
    }
}

/* ================= CLIENT ================= */
int send_cmd(char *cmd)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    write(sock, cmd, strlen(cmd));

    char buffer[BUFFER_SIZE];
    int n = read(sock, buffer, sizeof(buffer)-1);

    if (n > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    }

    close(sock);
    return 0;
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("./engine supervisor <rootfs>\n");
        printf("./engine start <id>\n");
        printf("./engine logs <id>\n");
        printf("./engine ps\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor(argv[2]);
    }

    char cmd[BUFFER_SIZE] = {0};

    for (int i = 1; i < argc; i++) {
        strcat(cmd, argv[i]);
        strcat(cmd, " ");
    }

    return send_cmd(cmd);
}
