
#ifndef DB_H
#define DB_H
#include "common.h"

int db_init(void);
int db_login(const char *username, const char *password, user_record *out);
int db_logout(int user_id);

int db_get_balance(int user_id, long long *bal_out);
int db_deposit(int user_id, long long amount, long long *new_bal);
int db_withdraw(int user_id, long long amount, long long *new_bal);

int db_transfer_to_account(int from_user_id, int to_account_number, long long amount);

int db_send_history(int fd, int user_id);

int db_change_password(int user_id, const char *new_password);
int db_apply_loan(int customer_user_id, long long amount, int *loan_id_out);
int db_append_feedback(int user_id, const char *text);
int db_assign_loan(int loan_id, const char *employee_username);
int db_assign_loan_by_employee_id(int loan_id, int employee_user_id);
int db_set_loan_status_owned(int loan_id, int employee_user_id, int new_status);

int db_add_user_with_account(const char *username, const char *password, int role, int active, long long initial_balance,
                             int *new_user_id, int *new_account_number);

int db_send_history_by_account(int fd, int account_number);

int db_set_loan_status(int loan_id, int status);
int db_set_user_active(const char *username, int active);
int db_set_user_active_by_id(int user_id, int active);
int db_send_feedback(int fd);
int db_set_user_role(const char *username, int role);

int db_get_account_number(int user_id, int *acct_no_out);
int db_get_user_id_by_account_number(int account_number, int *user_id_out);

#endif
