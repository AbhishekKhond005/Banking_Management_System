
#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "db.h"

#define BACKLOG 64
#define MAX_LINE 1024

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void send_line(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    send(fd, buf, strlen(buf), 0);
}

static void send_plain_menu(int fd, const char *title, const char *items[], int count) {
    send_line(fd, "MENU %s", title ? title : "Menu");
    for (int i = 0; i < count; i++) {
        send_line(fd, "%s", items[i] ? items[i] : "");
    }
}


static int recv_line(int fd, char *out, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        out[pos++] = c;
    }
    out[pos] = '\0';
    return 1;
}

typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_ctx_t;

static void show_customer_menu(int fd) {
    const char *items[] = {
        "1) VIEW_BALANCE",
        "2) DEPOSIT <amount>",
        "3) WITHDRAW <amount>",
        "4) TRANSFER <to_acct_no> <amount>",
        "5) APPLY_LOAN <amount>",
        "6) CHANGE_PASSWORD <new_password>",
        "7) HISTORY",
        "8) FEEDBACK <text>",
        "9) LOGOUT"
    };
    send_plain_menu(fd, "Customer Menu", items, (int)(sizeof(items) / sizeof(items[0])));
}

static void show_employee_menu(int fd) {
    const char *items[] = {
        "1) ADD_CUSTOMER <username> <password> <initial_balance>",
        "2) VIEW_TXNS <acct_no>",
        "3) APPROVE_LOAN <loan_id> | REJECT_LOAN <loan_id>",
        "4) CHANGE_PASSWORD <new_password>",
        "5) LOGOUT"
    };
    send_plain_menu(fd, "Employee Menu", items, (int)(sizeof(items) / sizeof(items[0])));
}

static void show_manager_menu(int fd) {
    const char *items[] = {
        "1) ACTIVATE <acct_no>",
        "2) DEACTIVATE <acct_no>",
        "3) REVIEW_FEEDBACK",
        "4) ASSIGN_LOAN <loan_id> <employee_user_id>",
        "5) CHANGE_PASSWORD <new_password>",
        "6) LOGOUT"
    };
    send_plain_menu(fd, "Manager Menu", items, (int)(sizeof(items) / sizeof(items[0])));
}

static void show_admin_menu(int fd) {
    const char *items[] = {
        "1) ADD_EMPLOYEE <username> <password>",
        "2) SET_ROLE <username> <role_int>",
        "3) CHANGE_PASSWORD <new_password>",
        "4) LOGOUT"
    };
    send_plain_menu(fd, "Admin Menu", items, (int)(sizeof(items) / sizeof(items[0])));
}


static void handle_customer(int fd, user_record *u) {
    show_customer_menu(fd);
    char line[MAX_LINE];

    for (;;) {
        send_line(fd, "OK Awaiting command");
        int rr = recv_line(fd, line, sizeof(line));
        if (rr <= 0) break;

        char cmd[MAX_LINE]; memset(cmd, 0, sizeof(cmd));
        sscanf(line, "%1023s", cmd);

        if (!strcasecmp(cmd, "VIEW_BALANCE")) {
            long long bal; int acct_no = -1;
            if (db_get_account_number(u->id, &acct_no) == 0 && db_get_balance(u->id, &bal) == 0)
                send_line(fd, "BALANCE acct=%d %lld", acct_no, bal);
            else
                send_line(fd, "ERR Could not read balance");
        } else if (!strcasecmp(cmd, "DEPOSIT")) {
            long long amt;
            if (sscanf(line, "%*s %lld", &amt) != 1 || amt <= 0) { send_line(fd, "ERR Invalid amount"); continue; }
            long long nb; int acct_no = -1; db_get_account_number(u->id, &acct_no);
            int rc = db_deposit(u->id, amt, &nb);
            if (rc == 0) send_line(fd, "DEPOSITED acct=%d %lld NEW_BAL %lld", acct_no, amt, nb);
            else send_line(fd, "ERR Deposit failed");
        } else if (!strcasecmp(cmd, "WITHDRAW")) {
            long long amt;
            if (sscanf(line, "%*s %lld", &amt) != 1 || amt <= 0) { send_line(fd, "ERR Invalid amount"); continue; }
            long long nb; int acct_no = -1; db_get_account_number(u->id, &acct_no);
            int rc = db_withdraw(u->id, amt, &nb);
            if (rc == 0) send_line(fd, "WITHDREW acct=%d %lld NEW_BAL %lld", acct_no, amt, nb);
            else send_line(fd, "ERR Withdraw failed");
        } else if (!strcasecmp(cmd, "TRANSFER")) {
            int to_acct; long long amt;
            if (sscanf(line, "%*s %d %lld", &to_acct, &amt) != 2 || amt <= 0) {
                send_line(fd, "ERR Usage: TRANSFER <to_acct_no> <amount>");
                continue;
            }
            int rc = db_transfer_to_account(u->id, to_acct, amt);
            if (rc == 0) send_line(fd, "TRANSFER OK to acct=%d %lld", to_acct, amt);
            else send_line(fd, "ERR Transfer failed");
        } else if (!strcasecmp(cmd, "APPLY_LOAN")) {
            long long amt;
            if (sscanf(line, "%*s %lld", &amt) != 1 || amt <= 0) { send_line(fd, "ERR Invalid amount"); continue; }
            int loan_id;
            int rc = db_apply_loan(u->id, amt, &loan_id);
            if (rc == 0) send_line(fd, "LOAN_APPLIED %d AMOUNT %lld", loan_id, amt);
            else send_line(fd, "ERR Loan application failed");
        } else if (!strcasecmp(cmd, "CHANGE_PASSWORD")) {
            char npw[PASSWORD_MAX];
            if (sscanf(line, "%*s %127s", npw) != 1) { send_line(fd, "ERR Usage: CHANGE_PASSWORD <new_password>"); continue; }
            int rc = db_change_password(u->id, npw);
            if (rc == 0) send_line(fd, "PASSWORD_CHANGED");
            else send_line(fd, "ERR Change password failed");
        } else if (!strcasecmp(cmd, "HISTORY")) {
            int rc = db_send_history(fd, u->id);
            if (rc == 0) send_line(fd, "HISTORY_END");
            else send_line(fd, "ERR History read failed");
        } else if (!strcasecmp(cmd, "FEEDBACK")) {
            const char *p = strchr(line, ' ');
            if (!p || !*(p + 1)) { send_line(fd, "ERR Provide feedback text"); continue; }
            int rc = db_append_feedback(u->id, p + 1);
            if (rc == 0) send_line(fd, "FEEDBACK_OK");
            else send_line(fd, "ERR Feedback failed");
        } else if (!strcasecmp(cmd, "LOGOUT")) {
            send_line(fd, "BYE");
            break;
        } else {
            send_line(fd, "ERR Unknown command");
        }
    }
}

static void handle_employee(int fd, user_record *u) {
    (void)u;
    show_employee_menu(fd);
    char line[MAX_LINE];

    for (;;) {
        send_line(fd, "OK Awaiting command");
        int rr = recv_line(fd, line, sizeof(line));
        if (rr <= 0) break;

        char cmd[MAX_LINE]; memset(cmd, 0, sizeof(cmd));
        sscanf(line, "%1023s", cmd);

        if (!strcasecmp(cmd, "ADD_CUSTOMER")) {
            char uname[USERNAME_MAX], pw[PASSWORD_MAX]; long long initb;
            if (sscanf(line, "%*s %63s %127s %lld", uname, pw, &initb) != 3 || initb < 0) {
                send_line(fd, "ERR Usage: ADD_CUSTOMER <username> <password> <initial_balance>");
                continue;
            }
            int uid, acct_no;
            int rc = db_add_user_with_account(uname, pw, ROLE_CUSTOMER, 1, initb, &uid, &acct_no);
            if (rc == 0) send_line(fd, "CUSTOMER_ADDED %s ID %d ACCT %d", uname, uid, acct_no);
            else send_line(fd, "ERR Add customer failed");
        } else if (!strcasecmp(cmd, "VIEW_TXNS")) {
            int acct_no;
            if (sscanf(line, "%*s %d", &acct_no) != 1) { send_line(fd, "ERR Usage: VIEW_TXNS <acct_no>"); continue; }
            int rc = db_send_history_by_account(fd, acct_no);
            if (rc == 0) send_line(fd, "HISTORY_END");
            else send_line(fd, "ERR History failed");
        } else if (!strcasecmp(cmd, "APPROVE_LOAN")) {
            int id;
            if (sscanf(line, "%*s %d", &id) != 1) { send_line(fd, "ERR Usage: APPROVE_LOAN <loan_id>"); continue; }
            int rc = db_set_loan_status_owned(id, u->id, LOAN_APPROVED);
            if (rc == 0) send_line(fd, "LOAN_APPROVED %d", id);
            else if (rc == -3) send_line(fd, "ERR Not assigned to you");
            else if (rc == -4) send_line(fd, "ERR Loan not found");
            else if (rc == -5) send_line(fd, "ERR Invalid state");
            else send_line(fd, "ERR Approve failed");
        } else if (!strcasecmp(cmd, "REJECT_LOAN")) {
            int id;
            if (sscanf(line, "%*s %d", &id) != 1) { send_line(fd, "ERR Usage: REJECT_LOAN <loan_id>"); continue; }
            int rc = db_set_loan_status_owned(id, u->id, LOAN_REJECTED);
            if (rc == 0) send_line(fd, "LOAN_REJECTED %d", id);
            else if (rc == -3) send_line(fd, "ERR Not assigned to you");
            else if (rc == -4) send_line(fd, "ERR Loan not found");
            else if (rc == -5) send_line(fd, "ERR Invalid state");
            else send_line(fd, "ERR Reject failed");
        } else if (!strcasecmp(cmd, "CHANGE_PASSWORD")) {
            char npw[PASSWORD_MAX];
            if (sscanf(line, "%*s %127s", npw) != 1) { send_line(fd, "ERR Usage: CHANGE_PASSWORD <new_password>"); continue; }
            int rc = db_change_password(u->id, npw);
            if (rc == 0) send_line(fd, "PASSWORD_CHANGED");
            else send_line(fd, "ERR Change password failed");
        } else if (!strcasecmp(cmd, "LOGOUT")) {
            send_line(fd, "BYE");
            break;
        } else {
            send_line(fd, "ERR Unknown command");
        }
    }
}

static void handle_manager(int fd, user_record *u) {
    (void)u;
    show_manager_menu(fd);
    char line[MAX_LINE];

    for (;;) {
        send_line(fd, "OK Awaiting command");
        int rr = recv_line(fd, line, sizeof(line));
        if (rr <= 0) break;

        char cmd[MAX_LINE]; memset(cmd, 0, sizeof(cmd));
        sscanf(line, "%1023s", cmd);

        if (!strcasecmp(cmd, "ACTIVATE")) {
            int acct_no;
            if (sscanf(line, "%*s %d", &acct_no) != 1) { send_line(fd, "ERR Usage: ACTIVATE <acct_no>"); continue; }
            int uid;
            if (db_get_user_id_by_account_number(acct_no, &uid) != 0) { send_line(fd, "ERR Account not found"); continue; }
            int rc = db_set_user_active_by_id(uid, 1);
            if (rc == 0) send_line(fd, "ACTIVATED acct=%d uid=%d", acct_no, uid);
            else send_line(fd, "ERR Activate failed");
        } else if (!strcasecmp(cmd, "DEACTIVATE")) {
            int acct_no;
            if (sscanf(line, "%*s %d", &acct_no) != 1) { send_line(fd, "ERR Usage: DEACTIVATE <acct_no>"); continue; }
            int uid;
            if (db_get_user_id_by_account_number(acct_no, &uid) != 0) { send_line(fd, "ERR Account not found"); continue; }
            int rc = db_set_user_active_by_id(uid, 0);
            if (rc == 0) send_line(fd, "DEACTIVATED acct=%d uid=%d", acct_no, uid);
            else send_line(fd, "ERR Deactivate failed");
        } else if (!strcasecmp(cmd, "REVIEW_FEEDBACK")) {
            int rc = db_send_feedback(fd);
            if (rc == 0) send_line(fd, "FEEDBACK_END");
            else send_line(fd, "ERR Feedback read failed");
        } else if (!strcasecmp(cmd, "ASSIGN_LOAN")) {
            int id; int emp_id;
            if (sscanf(line, "%*s %d %d", &id, &emp_id) != 2) { send_line(fd, "ERR Usage: ASSIGN_LOAN <loan_id> <employee_user_id>"); continue; }
            int rc = db_assign_loan_by_employee_id(id, emp_id);
            if (rc == 0) send_line(fd, "LOAN_ASSIGNED %d emp_id=%d", id, emp_id);
            else if (rc == -2) send_line(fd, "ERR Loan already assigned");
            else if (rc == -4) send_line(fd, "ERR Loan not found");
            else send_line(fd, "ERR Assign loan failed");
        } else if (!strcasecmp(cmd, "CHANGE_PASSWORD")) {
            char npw[PASSWORD_MAX];
            if (sscanf(line, "%*s %127s", npw) != 1) { send_line(fd, "ERR Usage: CHANGE_PASSWORD <new_password>"); continue; }
            int rc = db_change_password(u->id, npw);
            if (rc == 0) send_line(fd, "PASSWORD_CHANGED");
            else send_line(fd, "ERR Change password failed");
        } else if (!strcasecmp(cmd, "LOGOUT")) {
            send_line(fd, "BYE");
            break;
        } else {
            send_line(fd, "ERR Unknown command");
        }
    }
}

static void handle_admin(int fd, user_record *u) {
    (void)u;
    show_admin_menu(fd);
    char line[MAX_LINE];

    for (;;) {
        send_line(fd, "OK Awaiting command");
        int rr = recv_line(fd, line, sizeof(line));
        if (rr <= 0) break;

        char cmd[MAX_LINE]; memset(cmd, 0, sizeof(cmd));
        sscanf(line, "%1023s", cmd);

        if (!strcasecmp(cmd, "ADD_EMPLOYEE")) {
            char uname[USERNAME_MAX], pw[PASSWORD_MAX];
            if (sscanf(line, "%*s %63s %127s", uname, pw) != 2) { send_line(fd, "ERR Usage: ADD_EMPLOYEE <username> <password>"); continue; }
            int uid, acct_no;
            int rc = db_add_user_with_account(uname, pw, ROLE_EMPLOYEE, 1, 0, &uid, &acct_no);
            if (rc == 0) send_line(fd, "EMPLOYEE_ADDED %s ID %d", uname, uid);
            else send_line(fd, "ERR Add employee failed");
        } else if (!strcasecmp(cmd, "SET_ROLE")) {
            char uname[USERNAME_MAX]; int role;
            if (sscanf(line, "%*s %63s %d", uname, &role) != 2 || role < ROLE_CUSTOMER || role > ROLE_ADMIN) {
                send_line(fd, "ERR Usage: SET_ROLE <username> <role_int>");
                continue;
            }
            int rc = db_set_user_role(uname, role);
            if (rc == 0) send_line(fd, "ROLE_SET %s %d", uname, role);
            else send_line(fd, "ERR Set role failed");
        } else if (!strcasecmp(cmd, "CHANGE_PASSWORD")) {
            char npw[PASSWORD_MAX];
            if (sscanf(line, "%*s %127s", npw) != 1) { send_line(fd, "ERR Usage: CHANGE_PASSWORD <new_password>"); continue; }
            int rc = db_change_password(u->id, npw);
            if (rc == 0) send_line(fd, "PASSWORD_CHANGED");
            else send_line(fd, "ERR Change password failed");
        } else if (!strcasecmp(cmd, "LOGOUT")) {
            send_line(fd, "BYE");
            break;
        } else {
            send_line(fd, "ERR Unknown command");
        }
    }
}


static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t*)arg;
    int fd = ctx->fd;
    free(ctx);

    send_line(fd, "WELCOME Banking Management System");
    send_line(fd, "LOGIN <username> <password>");

    char line[MAX_LINE];
    user_record u;
    bool authed = false;

    for (;;) {
        int rr = recv_line(fd, line, sizeof(line));
        if (rr <= 0) goto out;

        char cmd[MAX_LINE]; memset(cmd, 0, sizeof(cmd));
        sscanf(line, "%1023s", cmd);

        if (!authed) {
            if (!strcasecmp(cmd, "LOGIN")) {
                char uname[USERNAME_MAX], pw[PASSWORD_MAX];
                if (sscanf(line, "%*s %63s %127s", uname, pw) != 2) {
                    send_line(fd, "ERR Usage: LOGIN <username> <password>");
                    continue;
                }
                int rc = db_login(uname, pw, &u);
                if (rc == 0) {
                    authed = true;
                    send_line(fd, "LOGIN_OK ROLE %d", u.role);
                    break;
                } else {
                    send_line(fd, "ERR Login failed");
                }
            } else {
                send_line(fd, "ERR Please LOGIN first");
            }
        }
    }

    if (u.role == ROLE_CUSTOMER) handle_customer(fd, &u);
    else if (u.role == ROLE_EMPLOYEE) handle_employee(fd, &u);
    else if (u.role == ROLE_MANAGER) handle_manager(fd, &u);
    else if (u.role == ROLE_ADMIN) handle_admin(fd, &u);
    else send_line(fd, "ERR Unknown role");

out:
    if (authed) db_logout(u.id);
    close(fd);
    return NULL;
}


int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, on_sigint);

    if (db_init() != 0) {
        fprintf(stderr, "Database init failed\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(sfd, BACKLOG) < 0) { perror("listen"); return 1; }

    printf("Server listening on port %d\n", port);

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }

        client_ctx_t *ctx = (client_ctx_t*)malloc(sizeof(*ctx));
        if (!ctx) { close(cfd); continue; }
        ctx->fd = cfd;
        ctx->addr = caddr;

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ctx) == 0) {
            pthread_detach(th);
        } else {
            close(cfd);
            free(ctx);
        }
    }

    close(sfd);
    return 0;
}
