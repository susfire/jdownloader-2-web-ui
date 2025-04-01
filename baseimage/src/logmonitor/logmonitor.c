/* vi: set sw=4 ts=4 sts=4 et: */

/*
 * Simple log file monitor.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <linux/limits.h>
#include <assert.h>
#include <time.h>
#include <getopt.h>
#include <inttypes.h>

#define LM_SUCCESS 0
#define LM_ERROR 1

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define DIM(a) (sizeof(a)/sizeof(a[0]))

#define DEFAULT_CONFIG_DIR "/etc/logmonitor"

#define MAIN_LOOP_SLEEP_PERIOD 1

#define MAX_NUM_NOTIFICATIONS 16
#define MAX_NUM_MONITORED_FILES_PER_NOTIFICATION 4
#define MAX_NUM_MONITORED_FILES (MAX_NUM_NOTIFICATIONS * MAX_NUM_MONITORED_FILES_PER_NOTIFICATION)
#define MAX_NUM_TARGETS 16

#define MAX_READ_FILE_SIZE (100 * 1024) /* 100KB */

#define STATUS_FILE_READ_INTERVAL 5 /* seconds */

#define DEBUG(...) do { if (debug_enabled) { printf(__VA_ARGS__); puts(""); } } while (0)
#define INFO(...)  do { printf(__VA_ARGS__); puts(""); } while (0)
#define ERROR(...) do { printf(__VA_ARGS__); puts(""); } while (0)

#define IS_SUCCESS(ret) ((ret) == LM_SUCCESS)
#define IS_ERROR(ret) (!IS_SUCCESS(ret))
#define SET_ERROR(ret, ...) do { (ret) = LM_ERROR; ERROR(__VA_ARGS__); } while (0)

#define FOR_EACH_MONITORED_FILE(c, f, idx) \
    lm_monitored_file_t *f = &(c)->monitored_files[0]; \
    for (int idx = 0; idx < (c)->num_monitored_files; idx++, (f) = &(c)->monitored_files[idx])

#define FOR_EACH_NOTIFICATION(c, n, idx) \
    lm_notification_t *n = (c)->notifications[0]; \
    for (int idx = 0; idx < (c)->num_notifications; idx++, (n) = (c)->notifications[idx])

#define FOR_EACH_TARGET(c, t, idx) \
    lm_target_t *t = (c)->targets[0]; \
    for (int idx = 0; idx < (c)->num_targets; idx++, (t) = (c)->targets[idx])

static const char* const short_options = "c:dh";
static struct option long_options[] = {
    { "configdir", no_argument, NULL, 'c' },
    { "debug", no_argument, NULL, 'd' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

static bool debug_enabled = false;

typedef struct {
    char *name;

    char *filter;

    char *title;
    bool is_title_exe;

    char *desc;
    bool is_desc_exe;

    char *level;
    bool is_level_exe;

    unsigned char num_monitored_files;
    struct {
        char *path;
        bool is_status;
    } monitored_files[MAX_NUM_MONITORED_FILES_PER_NOTIFICATION];
} lm_notification_t;

typedef struct {
    char *name;
    char *send;
    int debouncing;

    time_t last_notif_sent[MAX_NUM_NOTIFICATIONS];
} lm_target_t;

typedef struct {
    char *path;
    int fd;
    bool is_status;
    time_t last_read;
    struct stat last_stat;

    char *pending_buf;
} lm_monitored_file_t;

typedef struct {
    char *config_dir;

    unsigned char num_monitored_files;
    lm_monitored_file_t monitored_files[MAX_NUM_MONITORED_FILES];

    unsigned char num_notifications;
    lm_notification_t *notifications[MAX_NUM_NOTIFICATIONS];

    unsigned char num_targets;
    lm_target_t *targets[MAX_NUM_TARGETS];
} lm_context_t;

static void handle_sigchld(int sig) {
    /* Do nothing here.*/
}

static void reap_children() {
    int saved_errno = errno;
    while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) { }
    errno = saved_errno;
}

static time_t get_time()
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        return (now.tv_sec) + (now.tv_nsec / 1000000000);
    }
    else {
        // Check if we failed to get time because operation is not permitted.
        // This could happen for example on Raspberry Pi running with an old
        // version of libseccomp2 (e.g. with distros based on Debian 10).
        if (errno != EPERM) {
            ERROR("FATAL: Could not get time: %s.",
                    strerror(errno));
            abort();
        }
    }

    // Get time via /proc/uptime as a fallback.
    {
        double uptime;
        FILE *f = fopen("/proc/uptime", "r");
        if (f == NULL) {
            ERROR("FATAL: Could not get time: %s.", strerror(errno));
            abort();
        }

        if (fscanf(f, "%lf", &uptime) != 1) {
            ERROR("FATAL: Could not get time: parse error.");
            abort();
        }
        fclose(f);

        return uptime;
    }
}

static bool str_ends_with(const char *str, const char c)
{
    return (str && strlen(str) > 0 && str[strlen(str) - 1] == c);
}

static bool str_starts_with(const char *str, const char c)
{
    return (str && strlen(str) > 0 && str[0] == c);
}

static char *join_path(const char *dir, const char *file)
{
    int size = strlen(dir) + strlen(file) + 2;
    char *buf = malloc(size * sizeof(char));

    if (buf) {
        strcpy(buf, dir);

        // Add '/' if necessary.
        if (!str_ends_with(dir, '/') && !str_starts_with(file, '/')) {
            strcat(buf, "/");
        }

        strcat(buf, file);
    }

    return buf;
}

static void terminate_str_at_first_eol(char *str)
{
    for (char *ptr = str; *ptr != '\0'; ptr++) {
        if (*ptr == '\n' || *ptr == '\r') {
            *ptr = '\0';
            break;
        }
    }
}

static ssize_t safe_read(int fd, void *buf, size_t count)
{
    ssize_t n;

    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);

    if (n < 0 && errno == EAGAIN) {
        n = 0;
    }

    return n;
}

/*
 * Read all of the supplied buffer from a file.
 * This does multiple reads as necessary.
 * Returns the amount read, or -1 on an error.
 * A short read is returned on an end of file.
 */
static ssize_t full_read(int fd, void *buf, size_t len)
{
    ssize_t total = 0;

    while (len) {
        ssize_t cc = safe_read(fd, buf, len);

        if (cc < 0) {
            if (total) {
                /* we already have some! */
                /* user can do another read to know the error code */
                return total;
            }
            return cc; /* read() returns -1 on failure. */
        }
        if (cc == 0)
            break;
        buf = ((char *)buf) + cc;
        total += cc;
        len -= cc;
    }

    return total;
}

static ssize_t tail_read(int fd, char *buf, size_t count)
{
    ssize_t r;

    r = full_read(fd, buf, count);
    if (r < 0) {
        ERROR("read error: %s", strerror(errno));
    }

    return r;
}

static char *file2str(const char *filepath)
{
    int retval = LM_SUCCESS;
    char *buf = NULL;

    int fd = -1;
    struct stat st;

    // Get file stats.
    if (IS_SUCCESS(retval)) {
        if (stat(filepath, &st) < 0) {
            SET_ERROR(retval, "Failed to get stats of '%s'.", filepath);
        }
        else if (st.st_size > MAX_READ_FILE_SIZE) {
            SET_ERROR(retval, "File too big: '%s'.", filepath);
        }
    }

    // Allocate memory.
    if (IS_SUCCESS(retval)) {
        buf = malloc((st.st_size + 1) * sizeof(char));
        if (!buf) {
            SET_ERROR(retval, "Failed to allocated %jd bytes of memory to read file '%s'.",
                      (intmax_t)(st.st_size + 1),
                      filepath);
        }
    }

    // Open the file.
    if (IS_SUCCESS(retval)) {
        fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            SET_ERROR(retval, "Failed to open '%s'.", filepath);
        }
    }

    // Read the file.
    if (IS_SUCCESS(retval)) {
        if (safe_read(fd, buf, st.st_size) < 0) {
            SET_ERROR(retval, "Failed to read '%s'.", filepath);
        }
        buf[st.st_size] = '\0';
    }

    // Close the file.
    if (fd >= 0) {
        close(fd);
    }

    // Free buffer if error occurred.
    if (retval != LM_SUCCESS && buf) {
        free(buf);
        buf = NULL;
    }

    return buf;
}

static int invoke_exec(const char *exec, const char *args[], unsigned int num_args, int *exit_code, char *output, size_t outputsize)
{
    int retval = LM_SUCCESS;
    pid_t pid;
    int pipefds[2] = { -1, -1 };
    bool redirect_stdout = (output && outputsize > 0);

    // Create a pipe.
    if (IS_SUCCESS(retval) && redirect_stdout) {
        if (pipe(pipefds) == -1) {
            SET_ERROR(retval, "Pipe creation failed: %s.", strerror(errno));
        }
    }

    // Fork.
    if (IS_SUCCESS(retval)) {
        pid = fork();
        if (pid == -1) {
            SET_ERROR(retval, "Fork failed: %s.", strerror(errno));
        }
    }

    // Handle child.
    if (IS_SUCCESS(retval)) {
        if (pid == 0) {
            const char *all_args[num_args + 2];

            all_args[0] = exec;
            for (int i = 1; i <= num_args; i++) {
                all_args[i] = args[i-1];
            }
            all_args[num_args + 1] = NULL;

            // Redirect stdout to our pipe.
            if (redirect_stdout) {
                while ((dup2(pipefds[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
                close(pipefds[1]);
                close(pipefds[0]);
            }

            if (execv(exec, (char *const *)all_args) == -1) {
                exit(127);
            }
        }
    }

    if (pipefds[1] >= 0) {
        close(pipefds[1]);
    }

    // Handle output from child and wait for its termination.
    if (IS_SUCCESS(retval)) {
        pid_t ret;
        int status;

        assert(pid != 0);

        // Handle output from the child.
        if (redirect_stdout) {
            // Make sure to keep one byte for the null terminating character.
            ssize_t count = full_read(pipefds[0], output, outputsize - 1);
            if (count < 0) {
                output[0] = '\0';
                SET_ERROR(retval, "Failed to read output of child.");
            }
            else {
                output[count] = '\0';
            }
        }

        // Wait for the child to terminate.
        while ((ret = waitpid(pid, &status, 0)) == -1) {
            if (errno != EINTR) {
                SET_ERROR(retval, "wait_pid failure");
                break;
            }
        }

        if (ret == -1 || !WIFEXITED(status)) {
            retval = LM_ERROR;
        }
        else {
            *exit_code = WEXITSTATUS(status);
        }
    }

    if (pipefds[0] >= 0) {
        close(pipefds[0]);
    }

    return retval;
}

static int invoke_exec_no_wait(const char *exec, const char *args[], unsigned int num_args)
{
    int retval = LM_SUCCESS;
    pid_t pid;

    // Fork.
    if (IS_SUCCESS(retval)) {
        pid = fork();
        if (pid == -1) {
            SET_ERROR(retval, "Fork failed: %s.", strerror(errno));
        }
    }

    // Handle child.
    if (IS_SUCCESS(retval)) {
        if (pid == 0) {
            const char *all_args[num_args + 2];

            all_args[0] = exec;
            for (int i = 1; i <= num_args; i++) {
                all_args[i] = args[i-1];
            }
            all_args[num_args + 1] = NULL;

            if (execv(exec, (char *const *)all_args) == -1) {
                exit(127);
            }
        }
    }

    return retval;
}

static int invoke_filter(const char *filter_exe, const char *line)
{
    int retval = LM_SUCCESS;
    const char *args[] = { line };
    int exit_code;

    retval = invoke_exec(filter_exe, args, DIM(args), &exit_code, NULL, 0);
    if (IS_SUCCESS(retval)) {
        return (exit_code == 0) ? LM_SUCCESS : LM_ERROR;
    }

    return retval;
}

static int invoke_target(const char *send_exe, const char *title, const char *desc, const char *level)
{
    int retval = LM_SUCCESS;
    const char *args[] = { title, desc, level };

    retval = invoke_exec_no_wait(send_exe, args, DIM(args));

    return retval;
}

static void handle_line(lm_context_t *ctx, unsigned int mfid, char *buf)
{
#define BUFFER_SIZE 512

    FOR_EACH_NOTIFICATION(ctx, notif, nidx) {
        // Skip this notification if not for the monitored file.
        bool skip = true;
        for (int i = 0; i < notif->num_monitored_files; i++) {
            if (strcmp(notif->monitored_files[i].path, ctx->monitored_files[mfid].path) == 0) {
                skip = false;
                break;
            }
        }
        if (skip) {
            continue;
        }

        DEBUG("Invoking filter for notification '%s'...", notif->name);
        if (invoke_filter(notif->filter, buf) == LM_SUCCESS) {
            char *title = NULL;
            char *desc = NULL;
            char *level = NULL;

            // Filter indicated a match.
            DEBUG("Filter result: match.");

            // Get the notification title.
            if (notif->is_title_exe) {
                title = calloc(1, BUFFER_SIZE);
                if (title) {
                    int retval = LM_SUCCESS;
                    const char *args[] = { buf };
                    int exit_code;

                    retval = invoke_exec(notif->title, args, DIM(args), &exit_code, title, BUFFER_SIZE);
                    if (IS_SUCCESS(retval)) {
                        if (exit_code == 0) {
                            terminate_str_at_first_eol(title);
                        }
                        else {
                            ERROR("Notification title execution exited with code %d: %s.",
                                    exit_code,
                                    notif->title);
                            free(title);
                            title = NULL;
                        }
                    }
                    else{
                        ERROR("Notification title execution failure: %s.",
                                notif->title);
                        free(title);
                        title = NULL;
                    }
                }
                else {
                    ERROR("Notification title memory allocation failure: %s.",
                            notif->title);
                }

                if (!title) {
                    title = "EXECERROR";
                }
            }
            else {
                title = notif->title;
            }

            // Get the notification description.
            if (notif->is_desc_exe) {
                desc = calloc(1, BUFFER_SIZE);
                if (desc) {
                    int retval = LM_SUCCESS;
                    const char *args[] = { buf };
                    int exit_code;

                    retval = invoke_exec(notif->desc, args, DIM(args), &exit_code, desc, BUFFER_SIZE);
                    if (IS_SUCCESS(retval)) {
                        if (exit_code == 0) {
                            terminate_str_at_first_eol(desc);
                        }
                        else {
                            ERROR("Notification description execution exited with code %d: %s.",
                                    exit_code,
                                    notif->desc);
                            free(desc);
                            desc = NULL;
                        }
                    }
                    else{
                        ERROR("Notification description execution failure: %s.",
                                notif->desc);
                        free(desc);
                        desc = NULL;
                    }
                }
                else {
                    ERROR("Notification description memory allocation failure: %s.",
                            notif->desc);
                }

                if (!desc) {
                    desc = "EXECERROR";
                }
            }
            else {
                desc = notif->desc;
            }

            // Get the notification level.
            if (notif->is_level_exe) {
                level = calloc(1, BUFFER_SIZE);
                if (level) {
                    int retval = LM_SUCCESS;
                    const char *args[] = { buf };
                    int exit_code;

                    retval = invoke_exec(notif->level, args, DIM(args), &exit_code, level, BUFFER_SIZE);
                    if (IS_SUCCESS(retval)) {
                        if (exit_code == 0) {
                            terminate_str_at_first_eol(level);
                            if (strcmp(level, "ERROR") &&
                                strcmp(level, "WARNING") &&
                                strcmp(level, "INFO")) {
                                ERROR("Notification level level '%s' invalid: %s.",
                                        level,
                                        notif->level);
                                free(level);
                                level = NULL;
                            }
                        }
                        else {
                            ERROR("Notification level execution exited with code %d: %s.",
                                    exit_code,
                                    notif->level);
                            free(level);
                            level = NULL;
                        }
                    }
                    else{
                        ERROR("Notification level execution failure: %s.",
                                notif->level);
                        free(level);
                        level = NULL;
                    }
                }
                else {
                    ERROR("Notification level memory allocation failure: %s.",
                            notif->level);
                }

                if (!level) {
                    title = "EXECERROR";
                }
            }
            else {
                level = notif->level;
            }

            FOR_EACH_TARGET(ctx, target, tidx) {
                if (target->last_notif_sent[nidx] > 0) {
                    if (target->debouncing == 0) {
                        DEBUG("Ignoring target '%s': debouncing.", target->name);
                        continue;
                    }
                    else if ((get_time() - target->last_notif_sent[nidx]) < target->debouncing) {
                        DEBUG("Ignoring target '%s': debouncing.", target->name);
                        continue;
                    }
                }

                // Send the target.
                DEBUG("Invoking target '%s'...", target->name);
                invoke_target(target->send, title, desc, level);
                target->last_notif_sent[nidx] = get_time();
            }

            if (notif->is_title_exe && title) {
                free(title);
            }
            if (notif->is_desc_exe && desc) {
                free(desc);
            }
            if (notif->is_level_exe && level) {
                free(level);
            }
        }
        else {
            DEBUG("Filter result: no match.");
        }
    }
}

static void handle_read(lm_context_t *ctx, unsigned int mfid, char *buf)
{
    lm_monitored_file_t *mf = &ctx->monitored_files[mfid];
    char *work_buf = NULL;
    char *eol_ptr = NULL;

    // Create the work buffer.
    if (mf->pending_buf) {
        int new_buf_size = strlen(mf->pending_buf) + strlen(buf) + 1;
        if (new_buf_size > (500 * 1024)) {
            ERROR("line too long");
            free(mf->pending_buf);
            mf->pending_buf = NULL;
            return;
        }

        work_buf = realloc(mf->pending_buf, new_buf_size);
        if (work_buf) {
            strcat(work_buf, buf);
            mf->pending_buf = NULL;
        }
        else {
            ERROR("memory reallocation error");
            free(mf->pending_buf);
            mf->pending_buf = NULL;
            return;
        }
    }
    else {
        work_buf = strdup(buf);
        if (!work_buf) {
            ERROR("string duplication error");
            return;
        }
    }

    eol_ptr = strchr(work_buf, '\n');

    if (eol_ptr) {
        char *next_line = eol_ptr + 1;

        // Remove the end of line characters.
        *eol_ptr = '\0';
        if (eol_ptr != work_buf && *(eol_ptr - 1) == '\r') {
            *(eol_ptr - 1) = '\0';
        }

        // Handle the line.
        if (strlen(work_buf) > 0) {
            handle_line(ctx, mfid, work_buf);
        }

        // Handle the next potential line.
        if (strlen(next_line) > 0) {
            handle_read(ctx, mfid, next_line);
        }

        free(work_buf);
        work_buf = NULL;
    }
    else {
        mf->pending_buf = work_buf;
    }
}

static void free_notification(lm_notification_t *notif) {
    if (notif) {
        if (notif->name) free(notif->name);
        if (notif->filter) free(notif->filter);
        if (notif->title) free(notif->title);
        if (notif->desc) free(notif->desc);
        if (notif->level) free(notif->level);

        for (int i = 0; i < notif->num_monitored_files; i++) {
            if (notif->monitored_files[i].path) {
                free(notif->monitored_files[i].path);
            }
        }

        memset(notif, 0, sizeof(*notif));
        free(notif);
    }
}

static lm_notification_t *alloc_notification(const char *notifications_dir, const char *name)
{
    int retval = LM_SUCCESS;
    lm_notification_t *notif = NULL;
    char *notification_dir = NULL;
    struct dirent *de = NULL;
    DIR *dr = NULL;

    // Build the notification directory path.
    if (IS_SUCCESS(retval)) {
        notification_dir = join_path(notifications_dir, name);
        if (!notification_dir) {
            SET_ERROR(retval, "Failed to build notification directory path.");
        }
    }

    // Alloc memory for notification structure.
    if (IS_SUCCESS(retval)) {
        notif = calloc(1, sizeof(*notif));
        if (!notif) {
            SET_ERROR(retval, "Failed to alloc memory for new notification.");
        }
    }

    // Set notification's name.
    if (IS_SUCCESS(retval)) {
        notif->name = strdup(name);
        if (!notif->name) {
            SET_ERROR(retval, "Failed to alloc memory for notification name.");
        }
    }

    // Open the directory.
    if (IS_SUCCESS(retval)) {
        dr = opendir(notification_dir);
        if (dr == NULL) {
            SET_ERROR(retval, "Notification config '%s' directory not found.", notification_dir);
        }
    }

    // Loop through all files of the directory.
    while (IS_SUCCESS(retval) && (de = readdir(dr)) != NULL) {
        // Handle regular files only.
        if (de->d_type != DT_REG) {
            continue;
        }

        char *filepath = join_path(notification_dir, de->d_name);
        if (!filepath) {
            SET_ERROR(retval, "Failed to build path.");
        }
        else if (strcmp(de->d_name, "filter") == 0) {
            notif->filter = filepath;
            filepath = NULL;
            if (access(notif->filter, X_OK) != 0) {
                SET_ERROR(retval, "Notification filter '%s' not executable.", notif->filter);
            }
        }
        else if (strcmp(de->d_name, "title") == 0) {
            if (access(filepath, X_OK) == 0) {
                notif->title = filepath;
                notif->is_title_exe = true;
                filepath = NULL;
            }
            else {
                notif->title = file2str(filepath);
                if (notif->title) {
                    terminate_str_at_first_eol(notif->title);
                }
                else {
                    SET_ERROR(retval, "Failed to read notification title.");
                }
            }
        }
        else if (strcmp(de->d_name, "desc") == 0) {
            if (access(filepath, X_OK) == 0) {
                notif->desc = filepath;
                notif->is_desc_exe = true;
                filepath = NULL;
            }
            else {
                notif->desc = file2str(filepath);
                if (notif->desc) {
                    terminate_str_at_first_eol(notif->desc);
                }
                else {
                    SET_ERROR(retval, "Failed to read notification description.");
                }
            }
        }
        else if (strcmp(de->d_name, "level") == 0) {
            if (access(filepath, X_OK) == 0) {
                notif->level = filepath;
                notif->is_level_exe = true;
                filepath = NULL;
            }
            else {
                notif->level = file2str(filepath);
                if (notif->level) {
                    terminate_str_at_first_eol(notif->level);
                    if (strcmp(notif->level, "ERROR") &&
                        strcmp(notif->level, "WARNING") &&
                        strcmp(notif->level, "INFO")) {
                        SET_ERROR(retval, "Invalid level '%s'.", notif->level);
                    }
                }
                else {
                    SET_ERROR(retval, "Failed to read notification level.");
                }
            }
        }
        else if (strcmp(de->d_name, "source") == 0) {
            char *mfp = file2str(filepath);
            if (mfp) {
                char *token = strtok(mfp, "\n");
                while (token != NULL) {
                    bool is_status = false;

                    if (strlen(token) == 0) {
                        continue;
                    }

                    if (strncmp(token, "log:", strlen("log:")) == 0) {
                        is_status = false;
                        token += strlen("log:");
                    }
                    else if (strncmp(token, "status:", strlen("status:")) == 0) {
                        is_status = true;
                        token += strlen("status:");
                    }

                    if (strlen(token) == 0) {
                        SET_ERROR(retval, "Source file path is empty.");
                        break;
                    }
                    else if (token[0] != '/') {
                        SET_ERROR(retval, "Source file path is not absolute.");
                        break;
                    }

                    if (notif->num_monitored_files < DIM(notif->monitored_files)) {
                        notif->monitored_files[notif->num_monitored_files].path = strdup(token);
                        notif->monitored_files[notif->num_monitored_files].is_status = is_status;

                        if (!notif->monitored_files[notif->num_monitored_files].path) {
                            SET_ERROR(retval, "Failed to alloc memory for monitored file path.");
                            break;
                        }

                        notif->num_monitored_files++;
                    }
                    else {
                        SET_ERROR(retval, "Maximum number of monitored files reached.");
                        break;
                    }

                    token = strtok(NULL, "\n");
                }

                free(mfp);
                mfp = NULL;
            }
            else {
                SET_ERROR(retval, "Failed to read notification monitored file path.");
            }
        }

        if (filepath) {
            free(filepath);
            filepath = NULL;
        }
    }

    if (dr) {
        closedir(dr);
        dr = NULL;
    }

    // Validate config.
    if (IS_SUCCESS(retval)) {
        if (strlen(notif->filter) == 0) {
            SET_ERROR(retval, "Filter executable missing for notification defined at '%s'", notification_dir);
        }
        else if (strlen(notif->title) == 0) {
            SET_ERROR(retval, "Title missing for notification defined at '%s'", notification_dir);
        }
        else if (strlen(notif->desc) == 0) {
            SET_ERROR(retval, "Description missing for notification defined at '%s'", notification_dir);
        }
        else if (strlen(notif->level) == 0) {
            SET_ERROR(retval, "Level missing for notification defined at '%s'", notification_dir);
        }
        else if (notif->num_monitored_files == 0) {
            SET_ERROR(retval, "At least one file to monitor must be specified.");
        }
    }

    if (notification_dir) {
        free(notification_dir);
        notification_dir = NULL;
    }

    if (IS_ERROR(retval) && notif) {
        free_notification(notif);
        notif = NULL;
    }

    return notif;
}

static int load_config_notifications(lm_context_t *ctx, const char *notifications_dir)
{
    int retval = LM_SUCCESS;

    DIR *dr = opendir(notifications_dir);
    if (dr) {
        struct dirent *de = NULL;

        while (IS_SUCCESS(retval) && (de = readdir(dr)) != NULL) {
            // Handle directories only.
            if (de->d_type != DT_DIR) {
                continue;
            }

            // Skip '.' and '..' directories.
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }

            // Load the current notification config.
            if (ctx->num_notifications < DIM(ctx->notifications)) {
                ctx->notifications[ctx->num_notifications] = 
                    alloc_notification(notifications_dir, de->d_name);

                if (ctx->notifications[ctx->num_notifications]) {
                    ctx->num_notifications++;
                }
                else {
                    SET_ERROR(retval, "Failed to load notification '%s'.", de->d_name);
                }
            }
            else {
                SET_ERROR(retval, "Too many notifications defined.");
            }
        }

        closedir(dr);
    }
    else {
        SET_ERROR(retval, "Config directory '%s' not found.", notifications_dir);
    }

    return retval;
}

static void free_target(lm_target_t *target) {
    if (target) {
        if (target->name) free(target->name);
        if (target->send) free(target->send);

        memset(target, 0, sizeof(*target));
        free(target);
    }
}

static lm_target_t *alloc_target(const char *targets_dir, const char *name)
{
    int retval = LM_SUCCESS;
    lm_target_t *target = NULL;
    char *target_dir = NULL;
    struct dirent *de = NULL;
    DIR *dr = NULL;

    // Build the target directory path.
    if (IS_SUCCESS(retval)) {
        target_dir = join_path(targets_dir, name);
        if (!target_dir) {
            SET_ERROR(retval, "Failed to build target directory path.");
        }
    }

    // Alloc memory for target structure.
    if (IS_SUCCESS(retval)) {
        target = calloc(1, sizeof(*target));
        if (!target) {
            SET_ERROR(retval, "Failed to alloc memory for new target.");
        }
    }

    // Set target's name.
    if (IS_SUCCESS(retval)) {
        target->name = strdup(name);
        if (!target->name) {
            SET_ERROR(retval, "Failed to alloc memory for target name.");
        }
    }

    // Open the directory.
    if (IS_SUCCESS(retval)) {
        dr = opendir(target_dir);
        if (dr == NULL) {
            SET_ERROR(retval, "Config directory '%s' not found.", target_dir);
        }
    }

    // Loop through all files of the directory.
    while (IS_SUCCESS(retval) && (de = readdir(dr)) != NULL) {
        // Handle regular files only.
        if (de->d_type != DT_REG) {
            continue;
        }

        char *filepath = join_path(target_dir, de->d_name);
        if (!filepath) {
            SET_ERROR(retval, "Failed to build path.");
        }
        // Handle the send script.
        else if (strcmp(de->d_name, "send") == 0) {
            target->send = filepath;
            filepath = NULL;
            if (access(target->send, X_OK) != 0) {
                SET_ERROR(retval, "Target send '%s' not executable.", target->send);
            }
        }
        else if (strcmp(de->d_name, "debouncing") == 0) {
            char *debouncing_str = NULL;

            // Get the debouncing value.
            debouncing_str = file2str(filepath);
            if (debouncing_str) {
                terminate_str_at_first_eol(debouncing_str);
            }
            else {
                SET_ERROR(retval, "Failed to read target debouncing.");
            }

            // Convert the debouncing value.
            if (IS_SUCCESS(retval)) {
                char *end;
                long int val = strtol(debouncing_str, &end, 10);
                if (end == debouncing_str
                    || *end != '\0'
                    || ((LONG_MIN == val || LONG_MAX == val) && ERANGE == errno)
                    || val > INT_MAX
                    || val < INT_MIN) {
                    SET_ERROR(retval, "Invalid debouncing value '%s' defined in %s.", debouncing_str, filepath);
                }
                else {
                    target->debouncing = val;
                }
            }

            if (debouncing_str) {
                free(debouncing_str);
                debouncing_str = NULL;
            }
        }

        if (filepath) {
            free(filepath);
            filepath = NULL;
        }
    }

    if (dr) {
        closedir(dr);
    }

    // Validate config.
    if (IS_SUCCESS(retval)) {
        if (strlen(target->send) == 0) {
            SET_ERROR(retval, "Missing send executable for target defined at '%s'.", target_dir);
        }
    }

    if (target_dir) {
        free(target_dir);
        target_dir = NULL;
    }

    if (IS_ERROR(retval) && target) {
        free_target(target);
        target = NULL;
    }

    return target;
}

static int load_config_targets(lm_context_t *ctx, const char *targets_dir)
{
    int retval = LM_SUCCESS;

    DIR *dr = opendir(targets_dir);
    if (dr) {
        struct dirent *de = NULL;

        while (IS_SUCCESS(retval) && (de = readdir(dr)) != NULL) {
            // Handle directories only.
            if (de->d_type != DT_DIR) {
                continue;
            }

            // Skip '.' and '..' directories.
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }

            // Load the current target config.
            if (ctx->num_targets < DIM(ctx->targets)) {
                ctx->targets[ctx->num_targets] = alloc_target(targets_dir, de->d_name);

                if (ctx->targets[ctx->num_targets]) {
                    ctx->num_targets++;
                }
                else {
                    SET_ERROR(retval, "Failed to load target '%s'.", de->d_name);
                }
            }
            else {
                SET_ERROR(retval, "Too many targets defined.");
            }
        }

        closedir(dr);
    }
    else {
        SET_ERROR(retval, "Config directory not found: %s.", targets_dir);
    }

    return retval;
}

static void destroy_context(lm_context_t *ctx)
{
    if (ctx) {
        if (ctx->config_dir) {
            free(ctx->config_dir);
            ctx->config_dir = NULL;
        }

        // Free notifications.
        FOR_EACH_NOTIFICATION(ctx, notif, nidx) {
            free_notification(notif);
            ctx->notifications[nidx] = NULL;
        }
        ctx->num_notifications = 0;

        // Free targets.
        FOR_EACH_TARGET(ctx, target, tidx) {
            free_target(target);
            ctx->targets[tidx] = NULL;
        }
        ctx->num_targets = 0;

        // Free monitored files.
        FOR_EACH_MONITORED_FILE(ctx, mf, mfidx) {
            if (mf->path) {
                free(mf->path);
                mf->path = NULL;
            }
            if (mf->pending_buf) {
                free(mf->pending_buf);
                mf->pending_buf = NULL;
            }
        }

        memset(ctx, 0, sizeof(*ctx));
        free(ctx);
        ctx = NULL;
    }
}

static lm_context_t *create_context(const char *cfgdir)
{
    int retval = LM_SUCCESS;
    lm_context_t *ctx = NULL;

    // Alloc memory for context structure.
    if (IS_SUCCESS(retval)) {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            SET_ERROR(retval, "Failed to alloc memory for context.");
        }
    }

    // Set configuration directory.
    if (IS_SUCCESS(retval)) {
        ctx->config_dir = strdup(cfgdir);
        if (!ctx->config_dir) {
            SET_ERROR(retval, "Failed to set context configuration directory.");
        }
    }

    // Load notifications.
    if (IS_SUCCESS(retval)) {
        char *path = join_path(ctx->config_dir, "notifications.d");
        if (path) {
            retval = load_config_notifications(ctx, path);
            free(path);
        }
        else {
            SET_ERROR(retval, "Failed to build notifications directory path.");
        }
    }

    // Load targets.
    if (IS_SUCCESS(retval)) {
        char *path = join_path(ctx->config_dir, "targets.d");
        if (path) {
            retval = load_config_targets(ctx, path);
            free(path);
        }
        else {
            SET_ERROR(retval, "Failed to build targets directory path.");
        }
    }

    // Set monitored files.
    if (IS_SUCCESS(retval)) {
        FOR_EACH_NOTIFICATION(ctx, notif, nidx) {
            for (int i = 0; i < notif->num_monitored_files; i++) {
                bool exists = false;

                FOR_EACH_MONITORED_FILE(ctx, mf, mfidx) {
                    if (strcmp(mf->path, notif->monitored_files[i].path) == 0) {
                        if (mf->is_status == notif->monitored_files[i].is_status) {
                            exists = true;
                        }
                        else {
                            SET_ERROR(retval, "Monitored file defined multiple times with different types: %s.", mf->path);
                        }
                        break;
                    }
                }

                if (IS_SUCCESS(retval) && !exists) {
                    ctx->monitored_files[ctx->num_monitored_files].fd = -1;
                    ctx->monitored_files[ctx->num_monitored_files].is_status = notif->monitored_files[i].is_status;
                    ctx->monitored_files[ctx->num_monitored_files].path = strdup(notif->monitored_files[i].path);

                    if (!ctx->monitored_files[ctx->num_monitored_files].path) {
                        SET_ERROR(retval, "Failed to alloc memory for monitored file path.");
                        break;
                    }

                    ctx->num_monitored_files++;
                }
                else if (IS_ERROR(retval)) {
                    break;
                }
            }

            if (IS_ERROR(retval)) {
                break;
            }
        }
    }

    if (IS_ERROR(retval)) {
        destroy_context(ctx);
        ctx = NULL;
    }

    return ctx;
}

static void usage(void)
{
    fprintf(stderr, "Usage: logmonitor [OPTIONS...] FILE [FILE...]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --configdir         Directory where configuration is stored (default: "DEFAULT_CONFIG_DIR").\n");
    fprintf(stderr, "  -d, --debug             Enable debug logging.\n");
    fprintf(stderr, "  -h, --help              Display this help and exit.\n");
}

int main(int argc, char **argv)
{
    int retval = LM_SUCCESS;
    lm_context_t *ctx = NULL;
    const char *cfgdir = DEFAULT_CONFIG_DIR;

    char *tailbuf = NULL;

    // Parse options.
    if (IS_SUCCESS(retval)) {
        while (true) {
            int n = getopt_long(argc, argv, short_options, long_options, NULL);
            if (n < 0) {
                 break;
            }
            switch (n) {
                case 'c':
                    cfgdir = optarg;
                    break;
                case 'd':
                    debug_enabled = true;
                    break;
                case 'h':
                case '?':
                    retval = LM_ERROR;
                    usage();
            }
        }
    }

    // Set SIGCHLD handler.
    if (IS_SUCCESS(retval)) {
        struct sigaction sa;
        sa.sa_handler = &handle_sigchld;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, 0) == -1) {
            SET_ERROR(retval, "Failed to set signal handler: %s.", strerror(errno));
        }
    }

    // Create context.
    if (IS_SUCCESS(retval)) {
        ctx = create_context(cfgdir);
        if (!ctx) {
            SET_ERROR(retval, "Context creation failed.");
        }
    }

    // Validate config.
    if (IS_SUCCESS(retval)) {
        if (ctx->num_notifications == 0) {
            SET_ERROR(retval, "No notification configured.");
        }
        else if (ctx->num_targets == 0) {
            SET_ERROR(retval, "No target configured.");
        }
    }

    // Open all files to be monitored.
    if (IS_SUCCESS(retval)) {
        FOR_EACH_MONITORED_FILE(ctx, mf, mfidx) {
            const char *filename = mf->path;
            int fd = open(filename, O_RDONLY|O_NONBLOCK);

            if (fd >= 0 && !mf->is_status) {
                lseek(fd, 0, SEEK_END);
            }

            mf->fd = fd;
        }
    }

    // Allocate the memory for the main buffer.
    if (IS_SUCCESS(retval)) {
        tailbuf = malloc(BUFSIZ + 1);
        if (tailbuf == NULL) {
            SET_ERROR(retval, "Buffer memory allocation failed.");
        }
    }

    // Display monitored files.
    if (IS_SUCCESS(retval)) {
        FOR_EACH_MONITORED_FILE(ctx, mf, mfidx) {
            INFO("Monitoring %s file: %s",
                    mf->is_status ? "status" : "log",
                    mf->path);
        }
    }

    // Tail the files.
    while (IS_SUCCESS(retval)) {
        for (int i = 0; i < ctx->num_monitored_files; i++) {
            int stat_rc;
            struct stat sbuf;
            const char *filename = ctx->monitored_files[i].path;
            int fd = ctx->monitored_files[i].fd;

            // Skip status file if we read it recently.
            if (ctx->monitored_files[i].is_status) {
                if (get_time() - ctx->monitored_files[i].last_read < STATUS_FILE_READ_INTERVAL) {
                    continue;
                }
            }

            // Get file status.
            stat_rc = stat(filename, &sbuf);

            // Re-open the file if needed.
            {
                struct stat fsbuf;

                if (fd < 0
                 || fstat(fd, &fsbuf) < 0
                 || stat_rc < 0
                 || fsbuf.st_dev != sbuf.st_dev
                 || fsbuf.st_ino != sbuf.st_ino
                ) {
                    int new_fd;

                    if (fd >= 0) {
                        close(fd);
                    }
                    new_fd = open(filename, O_RDONLY|O_NONBLOCK);
                    if (new_fd >= 0) {
                        DEBUG("%s has %s; following end of new file.",
                            filename, (fd < 0) ? "appeared" : "been replaced");
                    }
                    else if (fd >= 0) {
                        DEBUG("%s has become inaccessible.", filename);
                    }
                    ctx->monitored_files[i].fd = fd = new_fd;
                }
            }
            if (fd < 0) {
                 continue;
            }

            if (ctx->monitored_files[i].is_status) {
                // Check if the file changed.
                if (stat_rc == 0) {
                    if (sbuf.st_size != ctx->monitored_files[i].last_stat.st_size) {
                        // Size of file changed.
                    }
                    else if (sbuf.st_mtime != ctx->monitored_files[i].last_stat.st_mtime) {
                        // Last modification time changed.
                    }
                    else if (sbuf.st_dev != ctx->monitored_files[i].last_stat.st_dev) {
                        // ID of device containing file changed.
                    }
                    else if (sbuf.st_ino != ctx->monitored_files[i].last_stat.st_ino) {
                        // Inode number changed.
                    }
                    else {
                        // Status file is the same.  Skip it.
                        ctx->monitored_files[i].last_read = get_time();
                        continue;
                    }

                    // Update file status.
                    ctx->monitored_files[i].last_stat = sbuf;
                }

                // Status files are always read from the beginning.
                lseek(fd, 0, SEEK_SET);
            }

            // Read the file until its end.
            for (;;) {
                int nread;
                struct stat sbuf;

                // Check if the file has been truncated.
                if (fstat(fd, &sbuf) == 0) {
                    off_t current = lseek(fd, 0, SEEK_CUR);
                    if (sbuf.st_size < current) {
                        lseek(fd, 0, SEEK_SET);
                    }
                }

                nread = tail_read(fd, tailbuf, BUFSIZ);
                if (nread <= 0) {
                    break;
                }
                tailbuf[nread] = '\0';
                handle_read(ctx, i, tailbuf);
            }

            ctx->monitored_files[i].last_read = get_time();
        }
        reap_children();
        sleep(MAIN_LOOP_SLEEP_PERIOD);
    }

    if (tailbuf) {
        free(tailbuf);
    }

    destroy_context(ctx);
    ctx = NULL;

    reap_children();

    return (IS_SUCCESS(retval)) ? EXIT_SUCCESS : EXIT_FAILURE;
}
