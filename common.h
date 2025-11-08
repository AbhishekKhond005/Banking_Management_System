
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

#define USERNAME_MAX 64
#define PASSWORD_MAX 128

typedef enum {
    ROLE_CUSTOMER = 1,
    ROLE_EMPLOYEE = 2,
    ROLE_MANAGER  = 3,
    ROLE_ADMIN    = 4
} user_role;

typedef struct {
    int id;
    int role;             
    int active;            
    int session_active;    
    char username[USERNAME_MAX];
    char password[PASSWORD_MAX]; 
} user_record;

typedef struct {
  int id;
  int user_id;
  int account_number;   
  long long balance;
} account_record;

typedef enum {
    LOAN_PENDING  = 0,
    LOAN_APPROVED = 1,
    LOAN_REJECTED = 2
} loan_status;

typedef struct {
    int id;
    int customer_user_id;
    int assigned_employee_user_id;
    long long amount;
    int status; 
} loan_record;

#endif
