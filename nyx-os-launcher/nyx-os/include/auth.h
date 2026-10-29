// include/auth.h
#ifndef AUTH_H
#define AUTH_H

#define MAX_USERS     4
#define MAX_USERNAME  16
#define MAX_PASSWORD  16

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    int  active;
} User;

extern User users[MAX_USERS];
extern int  user_count;
extern int  logged_in_user;

void auth_init(void);
int  auth_login(const char* username, const char* password);
int  auth_add_user(const char* username, const char* password);
int  auth_change_password(int user_idx, const char* old_pass, const char* new_pass);
int  auth_delete_user(int user_idx);

#endif

