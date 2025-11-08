
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_LINE 1024
#include "common.h"

static int g_hist_header_needed = 1; 
static int g_hist_boxw = 0;          

static void print_border(int width, char ch) {
    if (width < 4) width = 4;
    for (int i = 0; i < width; i++) putchar(ch);
    putchar('\n');
}

static void print_centered(const char *text, int width) {
    if (width < 4) width = 4;
    int len = (int)strlen(text);
    if (len > width - 2) len = width - 2;
    int pad_total = width - 2 - len;
    int pad_left = pad_total / 2;
    int pad_right = pad_total - pad_left;

    putchar('|');
    for (int i = 0; i < pad_left; i++) putchar(' ');
    fwrite(text, 1, (size_t)len, stdout);
    for (int i = 0; i < pad_right; i++) putchar(' ');
    putchar('|');
    putchar('\n');
}

static void print_row_left(const char *text, int width) {
    if (width < 4) width = 4;
    int inner = width - 2;
    if (inner < 1) inner = 1;
    int len = (int)strlen(text);
    int avail = inner - 1;
    if (avail < 0) avail = 0;

    putchar('|');
    putchar(' ');
    int to_copy = len > avail ? avail : len;
    fwrite(text, 1, (size_t)to_copy, stdout);
    for (int i = 0; i < (avail - to_copy); i++) putchar(' ');
    putchar('|');
    putchar('\n');
}

static void print_box_menu(const char *title, const char *items[], int count) {
    int width = (int)strlen(title);
    for (int i = 0; i < count; i++) {
        int l = (int)strlen(items[i]) + 2;
        if (l > width) width = l;
    }
    width += 4;
    if (width < 40) width = 40;

    print_border(width, '=');
    print_centered(title, width);
    print_border(width, '-');
    for (int i = 0; i < count; i++) print_row_left(items[i], width);
    print_border(width, '=');
}

static void print_history_line(const char *line) {
    char buf[MAX_LINE + 1];
    strncpy(buf, line, MAX_LINE);
    buf[MAX_LINE] = '\0';

    size_t L = strlen(buf);
    if (L && buf[L-1] == '\n') buf[L-1] = '\0';

    char *parts[6] = {0};
    int pcount = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, "|", &saveptr); tok && pcount < 6; tok = strtok_r(NULL, "|", &saveptr)) {
        parts[pcount++] = tok;
    }

    if (pcount < 5) { 
        printf("%s\n", line);
        return;
    }

    const char *ts   = parts[0];
    const char *acct = parts[1];
    const char *type = parts[2];
    const char *amt  = parts[3];
    const char *bal  = parts[4];
    const char *note = (pcount >= 6) ? parts[5] : "-";

    int w_ts   = 19; 
    int w_type = 14; 
    int w_amt  = 14;
    int w_bal  = 14;

    if (g_hist_header_needed) {
        int boxw = 2 + w_ts + 3 + 6 + 3 + w_type + 3 + w_amt + 3 + w_bal + 3 + 20 + 2;
        print_border(boxw, '=');
        printf("| %-*s | %-6s | %-*s | %-*s | %-*s | %-20s |\n",
               w_ts, "Timestamp", "Acct", w_type, "Type", w_amt, "Amount", w_bal, "Balance", "Note");
        print_border(boxw, '-');
        g_hist_header_needed = 0;
        g_hist_boxw = boxw;
    }

    const char *acctnum = acct;
    const char *eq = strchr(acct, '=');
    if (eq && *(eq+1)) acctnum = eq + 1;

    printf("| %-19.19s | %-6.6s | %-*.*s | %-*.*s | %-*.*s | %-20.20s |\n",
           ts,
           acctnum,
           w_type, w_type, type,
           w_amt,  w_amt,  amt,
           w_bal,  w_bal,  bal,
           note ? note : "-");
}

static void print_kv_table(const char *title, const char *keys[], const char *vals[], int n) {
    int wkey = 0, wval = 0;
    for (int i = 0; i < n; i++) {
        int lk = (int)strlen(keys[i] ? keys[i] : "");
        int lv = (int)strlen(vals[i] ? vals[i] : "");
        if (lk > wkey) wkey = lk;
        if (lv > wval) wval = lv;
    }
    if (wkey < 6) wkey = 6; if (wval < 4) wval = 4;
    int width = 2 + wkey + 3 + wval + 2;
    print_border(width, '=');
    print_centered(title ? title : "Info", width);
    print_border(width, '-');
    for (int i = 0; i < n; i++) {
        char row[MAX_LINE];
        snprintf(row, sizeof(row), "%-*s : %-*s", wkey, keys[i] ? keys[i] : "", wval, vals[i] ? vals[i] : "");
        print_row_left(row, width);
    }
    print_border(width, '=');
}

static int render_response_table(const char *line) {
    if (!line) return 0;
    // CUSTOMER_ADDED <uname> ID <uid> ACCT <acct>
    {
        char uname[USERNAME_MAX]; int uid, acct;
        if (sscanf(line, "CUSTOMER_ADDED %63s ID %d ACCT %d", uname, &uid, &acct) == 3) {
            const char *keys[] = { "Action", "Username", "Customer ID", "Account No" };
            char uidbuf[32], acctbuf[32];
            snprintf(uidbuf, sizeof(uidbuf), "%d", uid);
            snprintf(acctbuf, sizeof(acctbuf), "%d", acct);
            const char *vals[] = { "Customer Added", uname, uidbuf, acctbuf };
            print_kv_table("Result", keys, vals, 4);
            return 1;
        }
    }
    // EMPLOYEE_ADDED <uname> ID <uid>
    {
        char uname[USERNAME_MAX]; int uid;
        if (sscanf(line, "EMPLOYEE_ADDED %63s ID %d", uname, &uid) == 2) {
            const char *keys[] = { "Action", "Username", "Employee ID" };
            char uidbuf[32]; snprintf(uidbuf, sizeof(uidbuf), "%d", uid);
            const char *vals[] = { "Employee Added", uname, uidbuf };
            print_kv_table("Result", keys, vals, 3);
            return 1;
        }
    }
    // BALANCE acct=%d %lld
    {
        int acct; long long bal;
        if (sscanf(line, "BALANCE acct=%d %lld", &acct, &bal) == 2) {
            const char *keys[] = { "Account No", "Balance" };
            char abuf[32], bbuf[32]; snprintf(abuf, sizeof(abuf), "%d", acct); snprintf(bbuf, sizeof(bbuf), "%lld", bal);
            const char *vals[] = { abuf, bbuf };
            print_kv_table("Balance", keys, vals, 2);
            return 1;
        }
    }
    // DEPOSITED acct=%d amt NEW_BAL %lld or DEPOSITED acct=%d <amt> NEW_BAL <nb>
    {
        int acct; long long amt, nb;
        if (sscanf(line, "DEPOSITED acct=%d %lld NEW_BAL %lld", &acct, &amt, &nb) == 3) {
            const char *keys[] = { "Account No", "Deposited", "New Balance" };
            char abuf[32], ambuf[32], nbbuf[32];
            snprintf(abuf, sizeof(abuf), "%d", acct);
            snprintf(ambuf, sizeof(ambuf), "%lld", amt);
            snprintf(nbbuf, sizeof(nbbuf), "%lld", nb);
            const char *vals[] = { abuf, ambuf, nbbuf };
            print_kv_table("Deposit", keys, vals, 3);
            return 1;
        }
    }
    // WITHDREW acct=%d <amt> NEW_BAL <nb>
    {
        int acct; long long amt, nb;
        if (sscanf(line, "WITHDREW acct=%d %lld NEW_BAL %lld", &acct, &amt, &nb) == 3) {
            const char *keys[] = { "Account No", "Withdrawn", "New Balance" };
            char abuf[32], ambuf[32], nbbuf[32];
            snprintf(abuf, sizeof(abuf), "%d", acct);
            snprintf(ambuf, sizeof(ambuf), "%lld", amt);
            snprintf(nbbuf, sizeof(nbbuf), "%lld", nb);
            const char *vals[] = { abuf, ambuf, nbbuf };
            print_kv_table("Withdraw", keys, vals, 3);
            return 1;
        }
    }
    // TRANSFER OK to acct=%d <amt>
    {
        int acct; long long amt;
        if (sscanf(line, "TRANSFER OK to acct=%d %lld", &acct, &amt) == 2) {
            const char *keys[] = { "Status", "To Account", "Amount" };
            char abuf[32], ambuf[32]; snprintf(abuf, sizeof(abuf), "%d", acct); snprintf(ambuf, sizeof(ambuf), "%lld", amt);
            const char *vals[] = { "OK", abuf, ambuf };
            print_kv_table("Transfer", keys, vals, 3);
            return 1;
        }
    }
    // LOAN_APPLIED <id> AMOUNT <amt>
    {
        int id; long long amt;
        if (sscanf(line, "LOAN_APPLIED %d AMOUNT %lld", &id, &amt) == 2) {
            const char *keys[] = { "Loan ID", "Amount", "Status" };
            char idb[32], amb[32]; snprintf(idb, sizeof(idb), "%d", id); snprintf(amb, sizeof(amb), "%lld", amt);
            const char *vals[] = { idb, amb, "PENDING" };
            print_kv_table("Loan", keys, vals, 3);
            return 1;
        }
    }
    // LOAN_ASSIGNED <id> emp_id=<eid>
    {
        int id, eid;
        if (sscanf(line, "LOAN_ASSIGNED %d emp_id=%d", &id, &eid) == 2 || sscanf(line, "LOAN_ASSIGNED %d %d", &id, &eid) == 2) {
            const char *keys[] = { "Loan ID", "Employee ID", "Status" };
            char idb[32], ebuf[32]; snprintf(idb, sizeof(idb), "%d", id); snprintf(ebuf, sizeof(ebuf), "%d", eid);
            const char *vals[] = { idb, ebuf, "ASSIGNED" };
            print_kv_table("Loan", keys, vals, 3);
            return 1;
        }
    }
    // LOAN_APPROVED <id>
    {
        int id; if (sscanf(line, "LOAN_APPROVED %d", &id) == 1) {
            const char *keys[] = { "Loan ID", "Status" };
            char idb[32]; snprintf(idb, sizeof(idb), "%d", id);
            const char *vals[] = { idb, "APPROVED" };
            print_kv_table("Loan", keys, vals, 2);
            return 1;
        }
    }
    // LOAN_REJECTED <id>
    {
        int id; if (sscanf(line, "LOAN_REJECTED %d", &id) == 1) {
            const char *keys[] = { "Loan ID", "Status" };
            char idb[32]; snprintf(idb, sizeof(idb), "%d", id);
            const char *vals[] = { idb, "REJECTED" };
            print_kv_table("Loan", keys, vals, 2);
            return 1;
        }
    }
    // ACTIVATED / DEACTIVATED acct and uid
    {
        int acct, uid;
        if (sscanf(line, "ACTIVATED acct=%d uid=%d", &acct, &uid) == 2) {
            const char *keys[] = { "Account No", "User ID", "Active" };
            char ab[32], ub[32]; snprintf(ab, sizeof(ab), "%d", acct); snprintf(ub, sizeof(ub), "%d", uid);
            const char *vals[] = { ab, ub, "Yes" };
            print_kv_table("Account", keys, vals, 3); return 1;
        }
        if (sscanf(line, "DEACTIVATED acct=%d uid=%d", &acct, &uid) == 2) {
            const char *keys[] = { "Account No", "User ID", "Active" };
            char ab[32], ub[32]; snprintf(ab, sizeof(ab), "%d", acct); snprintf(ub, sizeof(ub), "%d", uid);
            const char *vals[] = { ab, ub, "No" };
            print_kv_table("Account", keys, vals, 3); return 1;
        }
    }
    // PASSWORD_CHANGED
    if (!strncmp(line, "PASSWORD_CHANGED", 16)) {
        const char *keys[] = { "Result" };
        const char *vals[] = { "Password changed" };
        print_kv_table("Success", keys, vals, 1);
        return 1;
    }
    // FEEDBACK_OK
    if (!strncmp(line, "FEEDBACK_OK", 11)) {
        const char *keys[] = { "Result" };
        const char *vals[] = { "Feedback saved" };
        print_kv_table("Success", keys, vals, 1);
        return 1;
    }
    // ERR ...
    if (!strncmp(line, "ERR ", 4)) {
        const char *keys[] = { "Error" };
        const char *vals[] = { line + 4 };
        print_kv_table("Error", keys, vals, 1);
        return 1;
    }
    return 0;
}

static void print_message_box(const char *title, const char *text) {
    const char *items[] = { text ? text : "" };
    print_box_menu(title ? title : "Message", items, 1);
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

static void send_line(int fd, const char *s) {
    send(fd, s, strlen(s), 0);
    send(fd, "\n", 1, 0);
}

static void read_password_masked(const char *prompt, char *out, size_t cap) {
    struct termios oldt, raw;
    if (!out || cap == 0) return;
    out[0] = '\0';
    fputs(prompt, stdout);
    fflush(stdout);
    if (tcgetattr(STDIN_FILENO, &oldt) == -1) return;
    raw = oldt;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return;
    size_t len = 0;
    for (;;) {
        int ch = getchar();
        if (ch == '\n' || ch == '\r' || ch == EOF) break;
        if ((ch == 127 || ch == '\b') && len > 0) {
            
            len--;
            out[len] = '\0';
            fputs("\b \b", stdout);
            fflush(stdout);
            continue;
        }
        if (len + 1 < cap) {
            out[len++] = (char)ch;
            out[len] = '\0';
            fputc('*', stdout);
            fflush(stdout);
        }
    }
    fputc('\n', stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    // prepare server address
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { fprintf(stderr, "bad ip\n"); return 1; }

    // connect to server
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }

    char line[MAX_LINE];

    // receive greeting messages from server (welcome + login prompt)
    char greet1[MAX_LINE] = {0}, greet2[MAX_LINE] = {0};
    if (recv_line(fd, greet1, sizeof(greet1)) > 0) { /* store */ }
    if (recv_line(fd, greet2, sizeof(greet2)) > 0) { /* store */ }

    // print greet message in box menue.
    const char *banner_items[] = { greet2 };
    print_box_menu(greet1[0] ? greet1 : "Banking Management System", banner_items, 1);

    // Take username and passwords from user and send that data to server.
    printf("Enter username: ");
    char uname[256]; if (!fgets(uname, sizeof(uname), stdin)) return 0;
    uname[strcspn(uname, "\r\n")] = 0;

    char pw[256];
    read_password_masked("Enter password: ", pw, sizeof(pw));

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "LOGIN %s %s", uname, pw);
    send_line(fd, cmd);

    // Check if login failed
    if (recv_line(fd, line, sizeof(line)) <= 0) { printf("Disconnected\n"); return 0; }
    printf("%s\n", line);
    if (strncmp(line, "LOGIN_OK", 8) != 0) { printf("Login failed\n"); return 0; }

    
    char *menu_lines[128]; int mcount = 0;
    for (;;) {
        if (recv_line(fd, line, sizeof(line)) <= 0) break;
        if (!strncmp(line, "OK Awaiting", 11)) break;
        if (!strncmp(line, "BYE", 3)) goto out;
        if (mcount < 128) menu_lines[mcount++] = strndup(line, MAX_LINE);
    }
    if (mcount > 0) {
       
        const char *title = menu_lines[0];
        const char **items = (const char**)&menu_lines[0];

        if (!strncmp(title, "MENU", 4)) {
            title += 5;
            if (mcount > 1) { items = (const char**)&menu_lines[1]; mcount -= 1; }
        } else {
            title = "Menu";
        }
        print_box_menu(title && *title ? title : "Menu", items, mcount);
        for (int i = 0; i < mcount + 1; i++) free(menu_lines[i]);
    }

    for (;;) {
        printf("> ");
        char in[1024];
        if (!fgets(in, sizeof(in), stdin)) break;
        in[strcspn(in, "\r\n")] = 0;
        if (strlen(in) == 0) continue;

        char tmp[64]; memset(tmp, 0, sizeof(tmp));
        sscanf(in, "%63s", tmp);
        for (char *p = tmp; *p; ++p) *p = (char)toupper((unsigned char)*p);

        if (!strcmp(tmp, "HISTORY") || !strcmp(tmp, "VIEW_TXNS")) {
            g_hist_header_needed = 1;
            g_hist_boxw = 0;
        }

        if (!strcmp(tmp, "ADD_CUSTOMER")) {
            
            char u[64] = {0}, pw2[256] = {0};
            long long initb = -1;
            int tokens = sscanf(in, "ADD_CUSTOMER %63s %255s %lld", u, pw2, &initb);

            if (tokens == 2) {
                
                char *end = NULL; long long maybe_bal = strtoll(pw2, &end, 10);
                if (end && *end == '\0') { 
                    char npw[256]; read_password_masked("Set password: ", npw, sizeof(npw));
                    snprintf(in, sizeof(in), "ADD_CUSTOMER %s %s %lld", u, npw, maybe_bal);
                }
            } else if (tokens == 1) {
                char npw[256]; read_password_masked("Set password: ", npw, sizeof(npw));
                printf("Initial balance: "); char balbuf[64] = {0};
                if (!fgets(balbuf, sizeof(balbuf), stdin)) balbuf[0] = '0';
                long long b = atoll(balbuf);
                snprintf(in, sizeof(in), "ADD_CUSTOMER %s %s %lld", u, npw, b);
            }
        }

        if (!strcasecmp(tmp, "CHANGE_PASSWORD")) {
            char *sp = in;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            while (*sp && isspace((unsigned char)*sp)) sp++;
            if (*sp == '\0') {
                char npw[256];
                read_password_masked("New password: ", npw, sizeof(npw));
                snprintf(in, sizeof(in), "CHANGE_PASSWORD %s", npw);
            }
        }

        send_line(fd, in);

        for (;;) {
            int rr = recv_line(fd, line, sizeof(line));
            if (rr <= 0) goto out;

            if (!strncmp(line, "OK Awaiting", 11)) { break; }
            if (!strncmp(line, "BYE", 3)) {
                printf("%s\n", line);
                goto out;
            }

            
            if (!strncmp(line, "HISTORY_END", 11)) {
                if (g_hist_boxw > 0) {
                    print_border(g_hist_boxw, '=');
                }
                g_hist_header_needed = 1;
                g_hist_boxw = 0;
                print_message_box("History", line);
                continue;
            }

            if (strstr(line, "acct=") && (strstr(line, "amt=") || strstr(line, "bal="))) {
                print_history_line(line);
                continue;
            }

            if (render_response_table(line)) continue;
            print_message_box("Info", line);
        }
    }

out:
    // show BYE or final message in a box if any previously captured
    print_message_box("Session", "BYE");
    close(fd);
    return 0;
}

