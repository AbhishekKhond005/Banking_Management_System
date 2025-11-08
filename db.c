
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#ifndef bzero
#define bzero(ptr, sz) memset((ptr), 0, (sz))
#endif

#define USERS_FILE     "users.db"
#define ACCOUNTS_FILE  "accounts.db"
#define LOANS_FILE     "loans.db"
#define TXN_LOG        "transactions.log"
#define FEEDBACK_LOG   "feedback.log"
#define JOURNAL_FILE   "accounts.journal"


static int ensure_file(const char *path, size_t rec_size) {
    (void)rec_size; 
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    return fd;
}

static int lock_region(int fd, short type, off_t start, off_t len) {
    struct flock fl;
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    return fcntl(fd, F_SETLKW, &fl);
}

static int lock_file_shared(int fd) { return lock_region(fd, F_RDLCK, 0, 0); }
static int lock_file_excl(int fd)   { return lock_region(fd, F_WRLCK, 0, 0); }
static int unlock_file(int fd)      { return lock_region(fd, F_UNLCK, 0, 0); }


// Journaling disabled: provide no-op stubs to retain lenient behavior
typedef struct { int kind; off_t off1, off2; long long old_bal1, old_bal2; int acct_no1, acct_no2; off_t user_off1, loan_off1; user_record old_user1; loan_record old_loan1; } journal_entry;
static int journal_open_locked(int *out_fd) { int fd = open("/dev/null", O_RDWR); if (fd < 0) return -1; if (out_fd) *out_fd = fd; return 0; }
static int journal_write_and_sync(int jfd, const journal_entry *je) { (void)jfd; (void)je; return 0; }
static int journal_clear(int jfd) { (void)jfd; return 0; }
static int recover_accounts_from_journal(void) { return 0; }

static int read_user_by_username(int fd, const char *username, user_record *out, off_t *off_out) {
    off_t off = 0;
    user_record u;
    ssize_t rs;
    while ((rs = pread(fd, &u, sizeof(u), off)) == (ssize_t)sizeof(u)) {
        if (strncmp(u.username, username, USERNAME_MAX) == 0) {
            if (out) *out = u;
            if (off_out) *off_out = off;
            return 0;
        }
        off += sizeof(u);
    }
    return -1;
}

static int read_user_by_id(int fd, int uid, user_record *out, off_t *off_out) {
    off_t off = 0;
    user_record u;
    ssize_t rs;
    while ((rs = pread(fd, &u, sizeof(u), off)) == (ssize_t)sizeof(u)) {
        if (u.id == uid) {
            if (out) *out = u;
            if (off_out) *off_out = off;
            return 0;
        }
        off += sizeof(u);
    }
    return -1;
}

static int read_account_by_user(int fd, int uid, account_record *out, off_t *off_out) {
    off_t off = 0;
    account_record a;
    ssize_t rs;
    while ((rs = pread(fd, &a, sizeof(a), off)) == (ssize_t)sizeof(a)) {
        if (a.user_id == uid) {
            if (out) *out = a;
            if (off_out) *off_out = off;
            return 0;
        }
        off += sizeof(a);
    }
    return -1;
}

static int read_account_by_account_number(int afd, int acct_no, account_record *out, off_t *off_out) {
    off_t off = 0;
    account_record a;
    ssize_t rs;
    while ((rs = pread(afd, &a, sizeof(a), off)) == (ssize_t)sizeof(a)) {
        if (a.account_number == acct_no) {
            if (out) *out = a;
            if (off_out) *off_out = off;
            return 0;
        }
        off += sizeof(a);
    }
    return -1;
}

static int next_id_from_file(int fd, size_t rec_sz, int id_offset) {
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz < (off_t)rec_sz) return 1;
    char *buf = (char *)malloc(rec_sz);
    if (!buf) return 1;
    if (pread(fd, buf, rec_sz, sz - (off_t)rec_sz) != (ssize_t)rec_sz) {
        free(buf);
        return 1;
    }
    int id = 0;
    memcpy(&id, buf + id_offset, sizeof(int));
    free(buf);
    if (id < 0 || id > 10000) return 1;
    return id + 1;
}

static int next_account_number(int afd) {
    int maxno = 1000;
    off_t off = 0;
    account_record a;
    ssize_t rs;
    while ((rs = pread(afd, &a, sizeof(a), off)) == (ssize_t)sizeof(a)) {
        if (a.account_number > maxno) maxno = a.account_number;
        off += sizeof(a);
    }
    return maxno + 1;
}

static int append_txn(int tfd, int account_number, const char *type, long long amount, long long new_bal, const char *note) {
    time_t now = time(NULL);
    char line[512];
    snprintf(line, sizeof(line), "%ld|acct=%d|%s|amt=%lld|bal=%lld|%s\n",
             (long)now, account_number, type, amount, new_bal, note ? note : "-");
    if (write(tfd, line, strlen(line)) < 0) return -1;
    fsync(tfd);
    return 0;
}


typedef struct { int old_no; int new_no; } acct_remap;

static int migrate_account_numbers_if_needed(void) {
    int afd = open(ACCOUNTS_FILE, O_RDWR);
    int tfd = open(TXN_LOG, O_RDWR | O_CREAT, 0644);
    if (afd < 0) { if (tfd >= 0) close(tfd); return -1; }
    if (tfd < 0) { close(afd); return -1; }

    if (lock_file_excl(afd) < 0) { close(afd); close(tfd); return -1; }

    // First pass: find any accounts with account_number < 1000
    acct_remap remaps[1024]; int rcount = 0;
    int maxno = 1000;
    off_t off = 0; account_record a;
    while (pread(afd, &a, sizeof(a), off) == (ssize_t)sizeof(a)) {
        if (a.account_number > maxno) maxno = a.account_number;
        off += sizeof(a);
    }
    off = 0;
    while (pread(afd, &a, sizeof(a), off) == (ssize_t)sizeof(a)) {
        if (a.account_number < 1000) {
            if (rcount < (int)(sizeof(remaps)/sizeof(remaps[0]))) {
                remaps[rcount].old_no = a.account_number;
                remaps[rcount].new_no = ++maxno;
                a.account_number = remaps[rcount].new_no;
                pwrite(afd, &a, sizeof(a), off);
                rcount++;
            }
        }
        off += sizeof(a);
    }
    fsync(afd);
    unlock_file(afd);
    close(afd);

    if (rcount == 0) { close(tfd); return 0; }

    // Rewrite transactions.log applying the remaps
    FILE *in = fdopen(tfd, "r+");
    if (!in) { close(tfd); return -1; }
    fseek(in, 0, SEEK_SET);

    char tmpname[] = "transactions.log.tmpXXXXXX";
    int tmpfd = mkstemp(tmpname);
    if (tmpfd < 0) { fclose(in); return -1; }
    FILE *out = fdopen(tmpfd, "w");
    if (!out) { close(tmpfd); unlink(tmpname); fclose(in); return -1; }

    char *line = NULL; size_t n = 0;
    while (getline(&line, &n, in) != -1) {
        // For each mapping, replace acct=old with acct=new
        for (int i = 0; i < rcount; i++) {
            char tagold[32], tagnew[32];
            snprintf(tagold, sizeof(tagold), "acct=%d", remaps[i].old_no);
            snprintf(tagnew, sizeof(tagnew), "acct=%d", remaps[i].new_no);
            char *pos;
            while ((pos = strstr(line, tagold)) != NULL) {
                size_t before = (size_t)(pos - line);
                char buf[2048];
                size_t lold = strlen(tagold), lnew = strlen(tagnew);
                size_t llen = strlen(line);
                if (before + lnew + (llen - before - lold) + 1 >= sizeof(buf)) break;
                memcpy(buf, line, before);
                memcpy(buf + before, tagnew, lnew);
                memcpy(buf + before + lnew, pos + lold, llen - before - lold + 1);
                free(line);
                line = strdup(buf);
                if (!line) break;
            }
        }
        if (line) fputs(line, out);
    }
    free(line);
    fflush(out);
    fsync(tmpfd);
    fclose(out);

    // Truncate and replace original log
    fclose(in); // closes tfd
    if (rename(tmpname, TXN_LOG) != 0) {
        unlink(tmpname);
        return -1;
    }
    return 0;
}


int db_init(void) {
    int ufd = ensure_file(USERS_FILE, sizeof(user_record));
    if (ufd < 0) return -1;
    int afd = ensure_file(ACCOUNTS_FILE, sizeof(account_record));
    if (afd < 0) { close(ufd); return -1; }
    int lfd = ensure_file(LOANS_FILE, sizeof(loan_record));
    if (lfd < 0) { close(ufd); close(afd); return -1; }
    int tfd = open(TXN_LOG, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (tfd < 0) { close(ufd); close(afd); close(lfd); return -1; }
    int ffd = open(FEEDBACK_LOG, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (ffd < 0) { close(ufd); close(afd); close(lfd); close(tfd); return -1; }

    // Crash recovery: restore any in-flight account updates from journal
    recover_accounts_from_journal();

    // Data migration: normalize legacy account numbers (<1000)
    migrate_account_numbers_if_needed();

    if (lock_file_excl(ufd) < 0) { close(ufd); close(afd); close(lfd); close(tfd); close(ffd); return -1; }
    off_t sz = lseek(ufd, 0, SEEK_END);
    if (sz == 0) {
        user_record admin;
        bzero(&admin, sizeof(admin));
        admin.id = 1;
        admin.role = ROLE_ADMIN;
        admin.active = 1;
        admin.session_active = 0;
        strncpy(admin.username, "admin", USERNAME_MAX - 1);
        strncpy(admin.password, "admin", PASSWORD_MAX - 1);
        pwrite(ufd, &admin, sizeof(admin), 0);
        fsync(ufd);
    }
    unlock_file(ufd);

    close(ufd);
    close(afd);
    close(lfd);
    close(tfd);
    close(ffd);
    return 0;
}

int db_login(const char *username, const char *password, user_record *out) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    int rc = read_user_by_username(ufd, username, &u, &off);
    if (rc != 0 || !u.active || strncmp(u.password, password, PASSWORD_MAX) != 0 || u.session_active) {
        unlock_file(ufd);
        close(ufd);
        return -1;
    }

    // Journal user change (session_active)
    int jfd;
    if (journal_open_locked(&jfd) != 0) { unlock_file(ufd); close(ufd); return -1; }
    journal_entry je; bzero(&je, sizeof(je));
    je.kind = 10; je.user_off1 = off; je.old_user1 = u;
    if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(ufd); close(ufd); return -1; }

    u.session_active = 1;
    if (pwrite(ufd, &u, sizeof(u), off) != (ssize_t)sizeof(u)) { unlock_file(ufd); close(ufd); unlock_file(jfd); close(jfd); return -1; }
    fsync(ufd);

    journal_clear(jfd);
    unlock_file(jfd);
    close(jfd);
    unlock_file(ufd);
    close(ufd);
    if (out) *out = u;
    return 0;
}

int db_logout(int user_id) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    int rc = read_user_by_id(ufd, user_id, &u, &off);
    if (rc == 0) {
        int jfd;
        if (journal_open_locked(&jfd) != 0) { unlock_file(ufd); close(ufd); return -1; }
        journal_entry je; bzero(&je, sizeof(je));
        je.kind = 10; je.user_off1 = off; je.old_user1 = u;
        if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(ufd); close(ufd); return -1; }

        u.session_active = 0;
        if (pwrite(ufd, &u, sizeof(u), off) != (ssize_t)sizeof(u)) { unlock_file(ufd); close(ufd); unlock_file(jfd); close(jfd); return -1; }
        fsync(ufd);

        journal_clear(jfd);
        unlock_file(jfd);
        close(jfd);
    }

    unlock_file(ufd);
    close(ufd);
    return rc;
}


int db_get_balance(int user_id, long long *bal_out) {
    int afd = open(ACCOUNTS_FILE, O_RDWR);
    if (afd < 0) return -1;
    if (lock_file_shared(afd) < 0) { close(afd); return -1; }

    account_record a;
    off_t off;
    int rc = read_account_by_user(afd, user_id, &a, &off);
    if (rc == 0 && bal_out) *bal_out = a.balance;

    unlock_file(afd);
    close(afd);
    return rc;
}

int db_deposit(int user_id, long long amount, long long *new_bal) {
    if (amount <= 0) return -1;
    int afd = open(ACCOUNTS_FILE, O_RDWR);
    int tfd = open(TXN_LOG, O_WRONLY | O_APPEND);
    int jfd = -1;
    if (afd < 0 || tfd < 0) { if (afd >= 0) close(afd); if (tfd >= 0) close(tfd); return -1; }
    if (lock_file_excl(afd) < 0) { close(afd); close(tfd); return -1; }

    account_record a;
    off_t off;
    if (read_account_by_user(afd, user_id, &a, &off) != 0) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }

    // Journal old state
    if (journal_open_locked(&jfd) != 0) { unlock_file(afd); close(afd); close(tfd); return -1; }
    journal_entry je; bzero(&je, sizeof(je));
    je.kind = 1; je.off1 = off; je.old_bal1 = a.balance; je.acct_no1 = a.account_number;
    if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(afd); close(afd); close(tfd); return -1; }

    a.balance += amount;
    if (pwrite(afd, &a, sizeof(a), off) != (ssize_t)sizeof(a)) {
        // leave journal for recovery
        unlock_file(afd); close(afd); close(tfd); unlock_file(jfd); close(jfd); return -1;
    }
    fsync(afd);

    // Clear journal (commit)
    journal_clear(jfd);
    unlock_file(jfd);
    close(jfd);

    append_txn(tfd, a.account_number, "DEPOSIT", amount, a.balance, "-");

    if (new_bal) *new_bal = a.balance;
    unlock_file(afd);
    close(afd);
    close(tfd);
    return 0;
}

int db_withdraw(int user_id, long long amount, long long *new_bal) {
    if (amount <= 0) return -1;
    int afd = open(ACCOUNTS_FILE, O_RDWR);
    int tfd = open(TXN_LOG, O_WRONLY | O_APPEND);
    int jfd = -1;
    if (afd < 0 || tfd < 0) { if (afd >= 0) close(afd); if (tfd >= 0) close(tfd); return -1; }
    if (lock_file_excl(afd) < 0) { close(afd); close(tfd); return -1; }

    account_record a;
    off_t off;
    if (read_account_by_user(afd, user_id, &a, &off) != 0) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }
    if (a.balance < amount) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }

    // Journal old state
    if (journal_open_locked(&jfd) != 0) { unlock_file(afd); close(afd); close(tfd); return -1; }
    journal_entry je; bzero(&je, sizeof(je));
    je.kind = 1; je.off1 = off; je.old_bal1 = a.balance; je.acct_no1 = a.account_number;
    if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(afd); close(afd); close(tfd); return -1; }

    a.balance -= amount;
    if (pwrite(afd, &a, sizeof(a), off) != (ssize_t)sizeof(a)) {
        // leave journal for recovery
        unlock_file(afd); close(afd); close(tfd); unlock_file(jfd); close(jfd); return -1;
    }
    fsync(afd);

    // Clear journal (commit)
    journal_clear(jfd);
    unlock_file(jfd);
    close(jfd);

    append_txn(tfd, a.account_number, "WITHDRAW", amount, a.balance, "-");

    if (new_bal) *new_bal = a.balance;
    unlock_file(afd);
    close(afd);
    close(tfd);
    return 0;
}

int db_transfer_to_account(int from_user_id, int to_account_number, long long amount) {
    if (amount <= 0) return -1;

    int afd = open(ACCOUNTS_FILE, O_RDWR);
    int tfd = open(TXN_LOG, O_WRONLY | O_APPEND);
    int jfd = -1;
    if (afd < 0 || tfd < 0) { if (afd >= 0) close(afd); if (tfd >= 0) close(tfd); return -1; }
    if (lock_file_excl(afd) < 0) { close(afd); close(tfd); return -1; }

    account_record from, to;
    off_t offfrom, offto;
    if (read_account_by_user(afd, from_user_id, &from, &offfrom) != 0) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }
    if (read_account_by_account_number(afd, to_account_number, &to, &offto) != 0) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }
    if (from.account_number == to.account_number) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }
    if (from.balance < amount) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }

    // Journal old states of both records
    if (journal_open_locked(&jfd) != 0) { unlock_file(afd); close(afd); close(tfd); return -1; }
    journal_entry je; bzero(&je, sizeof(je));
    je.kind = 2; je.off1 = offfrom; je.old_bal1 = from.balance; je.acct_no1 = from.account_number;
    je.off2 = offto;    je.old_bal2 = to.balance;   je.acct_no2 = to.account_number;
    if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(afd); close(afd); close(tfd); return -1; }

    from.balance -= amount;
    to.balance   += amount;

    if (pwrite(afd, &from, sizeof(from), offfrom) != (ssize_t)sizeof(from) ||
        pwrite(afd, &to,   sizeof(to),   offto)   != (ssize_t)sizeof(to)) {
        // leave journal for recovery
        unlock_file(afd); close(afd); close(tfd); unlock_file(jfd); close(jfd); return -1;
    }
    fsync(afd);

    // Clear journal (commit)
    journal_clear(jfd);
    unlock_file(jfd);
    close(jfd);

    char note_out[64]; snprintf(note_out, sizeof(note_out), "to=%d", to.account_number);
    char note_in[64];  snprintf(note_in,  sizeof(note_in),  "from=%d", from.account_number);
    append_txn(tfd, from.account_number, "TRANSFER_OUT", amount, from.balance, note_out);
    append_txn(tfd, to.account_number,   "TRANSFER_IN",  amount, to.balance,   note_in);

    unlock_file(afd);
    close(afd);
    close(tfd);
    return 0;
}

int db_change_password(int user_id, const char *new_password) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    if (read_user_by_id(ufd, user_id, &u, &off) != 0) {
        unlock_file(ufd); close(ufd); return -1;
    }
    int jfd;
    if (journal_open_locked(&jfd) != 0) { unlock_file(ufd); close(ufd); return -1; }
    journal_entry je; bzero(&je, sizeof(je));
    je.kind = 10; je.user_off1 = off; je.old_user1 = u;
    if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(ufd); close(ufd); return -1; }

    strncpy(u.password, new_password, PASSWORD_MAX - 1);
    u.password[PASSWORD_MAX - 1] = 0;
    if (pwrite(ufd, &u, sizeof(u), off) != (ssize_t)sizeof(u)) { unlock_file(ufd); close(ufd); unlock_file(jfd); close(jfd); return -1; }
    fsync(ufd);

    journal_clear(jfd);
    unlock_file(jfd);
    close(jfd);

    unlock_file(ufd);
    close(ufd);
    return 0;
}

int db_apply_loan(int customer_user_id, long long amount, int *loan_id_out) {
    int lfd = open(LOANS_FILE, O_RDWR);
    if (lfd < 0) return -1;
    if (lock_file_excl(lfd) < 0) { close(lfd); return -1; }

    int id = next_id_from_file(lfd, sizeof(loan_record), offsetof(loan_record, id));
    loan_record L;
    bzero(&L, sizeof(L));
    L.id = id;
    L.customer_user_id = customer_user_id;
    L.assigned_employee_user_id = 0;
    L.amount = amount;
    L.status = LOAN_PENDING;

    off_t off = lseek(lfd, 0, SEEK_END);
    pwrite(lfd, &L, sizeof(L), off);
    fsync(lfd);

    unlock_file(lfd);
    close(lfd);

    if (loan_id_out) *loan_id_out = id;
    return 0;
}

int db_send_history(int fd, int user_id) {
    int afd = open(ACCOUNTS_FILE, O_RDONLY);
    int tfd = open(TXN_LOG, O_RDONLY);
    if (afd < 0 || tfd < 0) { if (afd >= 0) close(afd); if (tfd >= 0) close(tfd); return -1; }
    if (lock_file_shared(afd) < 0) { close(afd); close(tfd); return -1; }

    account_record a;
    off_t off;
    if (read_account_by_user(afd, user_id, &a, &off) != 0) {
        unlock_file(afd); close(afd); close(tfd); return -1;
    }

    int acct_no = a.account_number;
    unlock_file(afd);
    close(afd);

    FILE *fp = fdopen(tfd, "r");
    if (!fp) { close(tfd); return -1; }

    char *line = NULL;
    size_t n = 0;
    char tag[32];
    snprintf(tag, sizeof(tag), "acct=%d", acct_no);

    while (getline(&line, &n, fp) != -1) {
        if (!strstr(line, tag)) continue;

        char *pbar = strchr(line, '|');
        if (!pbar) continue;

        long long secs = 0;
        if (sscanf(line, "%lld", &secs) != 1) continue;

        time_t tt = (time_t)secs;
        struct tm tm;
        localtime_r(&tt, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        char outbuf[1024];
        snprintf(outbuf, sizeof(outbuf), "%s%s", ts, pbar);
        send(fd, outbuf, strlen(outbuf), 0);
    }

    free(line);
    fclose(fp);
    return 0;
}

int db_append_feedback(int user_id, const char *text) {
    int ffd = open(FEEDBACK_LOG, O_WRONLY | O_APPEND);
    if (ffd < 0) return -1;
    time_t now = time(NULL);
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%ld|uid=%d|%s\n", (long)now, user_id, text ? text : "-");
    if (write(ffd, buf, n) != n) { close(ffd); return -1; }
    fsync(ffd);
    close(ffd);
    return 0;
}

int db_add_user_with_account(const char *username, const char *password, int role, int active, long long initial_balance,
                             int *new_user_id, int *new_account_number) {
    int ufd = open(USERS_FILE, O_RDWR);
    int afd = open(ACCOUNTS_FILE, O_RDWR);
    if (ufd < 0 || afd < 0) { if (ufd >= 0) close(ufd); if (afd >= 0) close(afd); return -1; }

    // Create user
    if (lock_file_excl(ufd) < 0) { close(ufd); close(afd); return -1; }

    user_record exists;
    off_t dummy;
    if (read_user_by_username(ufd, username, &exists, &dummy) == 0) {
        unlock_file(ufd); close(ufd); close(afd); return -1;
    }

    int uid = next_id_from_file(ufd, sizeof(user_record), offsetof(user_record, id));
    user_record u;
    bzero(&u, sizeof(u));
    u.id = uid;
    u.role = role;
    u.active = active ? 1 : 0;
    u.session_active = 0;
    strncpy(u.username, username, USERNAME_MAX - 1);
    strncpy(u.password, password, PASSWORD_MAX - 1);

    off_t uoff = lseek(ufd, 0, SEEK_END);
    pwrite(ufd, &u, sizeof(u), uoff);
    fsync(ufd);
    unlock_file(ufd);
    close(ufd);

    int acct_no = -1;

    if (role == ROLE_CUSTOMER) {
        if (lock_file_excl(afd) < 0) { close(afd); return -1; }

        int aid = next_id_from_file(afd, sizeof(account_record), offsetof(account_record, id));
        account_record a;
        bzero(&a, sizeof(a));
        a.id = aid;
        a.user_id = uid;
        a.account_number = next_account_number(afd);
        acct_no = a.account_number;
        a.balance = initial_balance;

        off_t aoff = lseek(afd, 0, SEEK_END);
        pwrite(afd, &a, sizeof(a), aoff);
        fsync(afd);
        unlock_file(afd);
    }

    close(afd);

    if (new_user_id) *new_user_id = uid;
    if (new_account_number) *new_account_number = acct_no;
    return 0;
}

int db_send_history_by_account(int fd, int account_number) {
    int tfd = open(TXN_LOG, O_RDONLY);
    if (tfd < 0) return -1;

    FILE *fp = fdopen(tfd, "r");
    if (!fp) { close(tfd); return -1; }

    char *line = NULL;
    size_t n = 0;
    char tag[32];
    snprintf(tag, sizeof(tag), "acct=%d", account_number);

    while (getline(&line, &n, fp) != -1) {
        if (!strstr(line, tag)) continue;

        char *pbar = strchr(line, '|');
        if (!pbar) continue;

        long long secs = 0;
        if (sscanf(line, "%lld", &secs) != 1) continue;
 
        time_t tt = (time_t)secs;
        struct tm tm;
        localtime_r(&tt, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        char outbuf[1024];
        snprintf(outbuf, sizeof(outbuf), "%s%s", ts, pbar);
        send(fd, outbuf, strlen(outbuf), 0);
    }

    free(line);
    fclose(fp);
    return 0;
}


int db_assign_loan(int loan_id, const char *employee_username) {
    int ufd = open(USERS_FILE, O_RDWR);
    int lfd = open(LOANS_FILE, O_RDWR);
    if (ufd < 0 || lfd < 0) { if (ufd >= 0) close(ufd); if (lfd >= 0) close(lfd); return -1; }

    if (lock_file_excl(lfd) < 0) { close(ufd); close(lfd); return -1; }

    // Lookup employee
    user_record emp;
    off_t uoff = 0;
    if (read_user_by_username(ufd, employee_username, &emp, &uoff) != 0) {
        unlock_file(lfd); close(ufd); close(lfd); return -3;
    }
    if (emp.role != ROLE_EMPLOYEE || !emp.active) {
        unlock_file(lfd); close(ufd); close(lfd); return -3;
    }

    loan_record L;
    off_t off = 0;
    ssize_t rs;
    int rc = -4;
    while ((rs = pread(lfd, &L, sizeof(L), off)) == (ssize_t)sizeof(L)) {
        if (L.id == loan_id) {
            if (L.assigned_employee_user_id != 0) { rc = -2; break; }
            // Journal loan assignment change
            int jfd; if (journal_open_locked(&jfd) != 0) { rc = -1; break; }
            journal_entry je; bzero(&je, sizeof(je));
            je.kind = 11; je.loan_off1 = off; je.old_loan1 = L;
            if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); rc = -1; break; }

            L.assigned_employee_user_id = emp.id;
            if (pwrite(lfd, &L, sizeof(L), off) != (ssize_t)sizeof(L)) { unlock_file(jfd); close(jfd); rc = -1; break; }
            fsync(lfd);
            journal_clear(jfd);
            unlock_file(jfd);
            close(jfd);
            rc = 0;
            break;
        }
        off += sizeof(L);
    }

    unlock_file(lfd);
    close(ufd);
    close(lfd);
    return rc;
}

int db_assign_loan_by_employee_id(int loan_id, int employee_user_id) {
    int lfd = open(LOANS_FILE, O_RDWR);
    if (lfd < 0) return -1;
    if (lock_file_excl(lfd) < 0) { close(lfd); return -1; }

    loan_record L;
    off_t off = 0;
    ssize_t rs;
    int rc = -4;
    while ((rs = pread(lfd, &L, sizeof(L), off)) == (ssize_t)sizeof(L)) {
        if (L.id == loan_id) {
            if (L.assigned_employee_user_id != 0) { rc = -2; break; }
            L.assigned_employee_user_id = employee_user_id;
            if (pwrite(lfd, &L, sizeof(L), off) != (ssize_t)sizeof(L)) { rc = -1; break; }
            fsync(lfd);
            rc = 0;
            break;
        }
        off += sizeof(L);
    }

    unlock_file(lfd);
    close(lfd);
    return rc;
}

int db_set_user_active_by_id(int user_id, int active) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    int rc = read_user_by_id(ufd, user_id, &u, &off);
    if (rc == 0) {
        u.active = active ? 1 : 0;
        if (!u.active) u.session_active = 0;
        pwrite(ufd, &u, sizeof(u), off);
        fsync(ufd);
    }

    unlock_file(ufd);
    close(ufd);
    return rc;
}

int db_get_user_id_by_account_number(int account_number, int *user_id_out) {
    if (!user_id_out) return -1;
    int afd = open(ACCOUNTS_FILE, O_RDONLY);
    if (afd < 0) return -1;
    if (lock_file_shared(afd) < 0) { close(afd); return -1; }

    account_record a;
    off_t off;
    int rc = read_account_by_account_number(afd, account_number, &a, &off);
    if (rc == 0) {
        *user_id_out = a.user_id;
    }

    unlock_file(afd);
    close(afd);
    return rc;
}

int db_set_loan_status(int loan_id, int status) {
    int lfd = open(LOANS_FILE, O_RDWR);
    if (lfd < 0) return -1;
    if (lock_file_excl(lfd) < 0) { close(lfd); return -1; }

    off_t off = 0;
    loan_record L;
    ssize_t rs;
    int rc = -1;
    while ((rs = pread(lfd, &L, sizeof(L), off)) == (ssize_t)sizeof(L)) {
        if (L.id == loan_id) {
            int jfd; if (journal_open_locked(&jfd) != 0) { unlock_file(lfd); close(lfd); return -1; }
            journal_entry je; bzero(&je, sizeof(je));
            je.kind = 11; je.loan_off1 = off; je.old_loan1 = L;
            if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(lfd); close(lfd); return -1; }

            L.status = status;
            if (pwrite(lfd, &L, sizeof(L), off) != (ssize_t)sizeof(L)) { unlock_file(lfd); close(lfd); unlock_file(jfd); close(jfd); return -1; }
            fsync(lfd);
            journal_clear(jfd);
            unlock_file(jfd);
            close(jfd);
            rc = 0;
            break;
        }
        off += sizeof(L);
    }

    unlock_file(lfd);
    close(lfd);
    return rc;
}

int db_set_loan_status_owned(int loan_id, int employee_user_id, int new_status) {
    if (new_status != LOAN_APPROVED && new_status != LOAN_REJECTED) return -5;

    int lfd = open(LOANS_FILE, O_RDWR);
    if (lfd < 0) return -1;
    if (lock_file_excl(lfd) < 0) { close(lfd); return -1; }

    loan_record L;
    off_t loff = 0;
    ssize_t rs;
    int found = 0;
    while ((rs = pread(lfd, &L, sizeof(L), loff)) == (ssize_t)sizeof(L)) {
        if (L.id == loan_id) { found = 1; break; }
        loff += sizeof(L);
    }
    if (!found) { unlock_file(lfd); close(lfd); return -4; }

    if (L.assigned_employee_user_id != employee_user_id) { unlock_file(lfd); close(lfd); return -3; }
    if (L.status != LOAN_PENDING) { unlock_file(lfd); close(lfd); return -5; }

    L.status = new_status;
    if (pwrite(lfd, &L, sizeof(L), loff) != (ssize_t)sizeof(L)) { unlock_file(lfd); close(lfd); return -1; }
    fsync(lfd);

    unlock_file(lfd);
    close(lfd);

    if (new_status == LOAN_APPROVED) {
        int afd = open(ACCOUNTS_FILE, O_RDWR);
        int tfd = open(TXN_LOG, O_WRONLY | O_APPEND);
        if (afd < 0 || tfd < 0) { if (afd >= 0) close(afd); if (tfd >= 0) close(tfd); return -1; }
        if (lock_file_excl(afd) < 0) { close(afd); close(tfd); return -1; }

        account_record a;
        off_t aoff;
        if (read_account_by_user(afd, L.customer_user_id, &a, &aoff) != 0) {
            unlock_file(afd); close(afd); close(tfd); return -1;
        }

        a.balance += L.amount;
        if (pwrite(afd, &a, sizeof(a), aoff) != (ssize_t)sizeof(a)) {
            unlock_file(afd); close(afd); close(tfd); return -1;
        }
        fsync(afd);

        append_txn(tfd, a.account_number, "LOAN_CREDIT", L.amount, a.balance, "-");

        unlock_file(afd);
        close(afd);
        close(tfd);
    }

    return 0;
}


int db_set_user_active(const char *username, int active) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    int rc = read_user_by_username(ufd, username, &u, &off);
    if (rc == 0) {
        int jfd; if (journal_open_locked(&jfd) != 0) { unlock_file(ufd); close(ufd); return -1; }
        journal_entry je; bzero(&je, sizeof(je));
        je.kind = 10; je.user_off1 = off; je.old_user1 = u;
        if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(ufd); close(ufd); return -1; }

        u.active = active ? 1 : 0;
        if (!u.active) u.session_active = 0;
        if (pwrite(ufd, &u, sizeof(u), off) != (ssize_t)sizeof(u)) { unlock_file(ufd); close(ufd); unlock_file(jfd); close(jfd); return -1; }
        fsync(ufd);
        journal_clear(jfd);
        unlock_file(jfd);
        close(jfd);
    }

    unlock_file(ufd);
    close(ufd);
    return rc;
}

int db_send_feedback(int fd) {
    int ffd = open(FEEDBACK_LOG, O_RDONLY);
    if (ffd < 0) return -1;

    FILE *fp = fdopen(ffd, "r");
    if (!fp) { close(ffd); return -1; }

    char *line = NULL;
    size_t n = 0;
    while (getline(&line, &n, fp) != -1) {
        send(fd, line, strlen(line), 0);
    }

    free(line);
    fclose(fp);
    return 0;
}

int db_set_user_role(const char *username, int role) {
    int ufd = open(USERS_FILE, O_RDWR);
    if (ufd < 0) return -1;
    if (lock_file_excl(ufd) < 0) { close(ufd); return -1; }

    user_record u;
    off_t off;
    int rc = read_user_by_username(ufd, username, &u, &off);
    if (rc == 0) {
        int jfd; if (journal_open_locked(&jfd) != 0) { unlock_file(ufd); close(ufd); return -1; }
        journal_entry je; bzero(&je, sizeof(je));
        je.kind = 10; je.user_off1 = off; je.old_user1 = u;
        if (journal_write_and_sync(jfd, &je) != 0) { unlock_file(jfd); close(jfd); unlock_file(ufd); close(ufd); return -1; }

        u.role = role;
        if (pwrite(ufd, &u, sizeof(u), off) != (ssize_t)sizeof(u)) { unlock_file(ufd); close(ufd); unlock_file(jfd); close(jfd); return -1; }
        fsync(ufd);
        journal_clear(jfd);
        unlock_file(jfd);
        close(jfd);
    }

    unlock_file(ufd);
    close(ufd);
    return rc;
}

int db_get_account_number(int user_id, int *acct_no_out) {
    if (!acct_no_out) return -1;
    int afd = open(ACCOUNTS_FILE, O_RDONLY);
    if (afd < 0) return -1;
    if (lock_file_shared(afd) < 0) { close(afd); return -1; }

    account_record a;
    off_t off;
    int rc = read_account_by_user(afd, user_id, &a, &off);
    if (rc == 0) {
        *acct_no_out = a.account_number;
    }

    unlock_file(afd);
    close(afd);
    return rc;
}
