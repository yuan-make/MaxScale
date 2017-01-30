/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * Loading MySQL users from a MySQL backend server
 */

#include "mysql_auth.h"

#include <stdio.h>
#include <ctype.h>
#include <mysql.h>
#include <netdb.h>

#include <maxscale/dcb.h>
#include <maxscale/service.h>
#include <maxscale/users.h>
#include <maxscale/log_manager.h>
#include <maxscale/secrets.h>
#include <maxscale/protocol/mysql.h>
#include <mysqld_error.h>
#include <maxscale/mysql_utils.h>
#include <maxscale/alloc.h>

/** Don't include the root user */
#define USERS_QUERY_NO_ROOT " AND user.user NOT IN ('root')"

/** Normal password column name */
#define MYSQL_PASSWORD "password"

/** MySQL 5.7 password column name */
#define MYSQL57_PASSWORD "authentication_string"

#define NEW_LOAD_DBUSERS_QUERY "SELECT u.user, u.host, d.db, u.select_priv, u.%s \
    FROM mysql.user AS u LEFT JOIN mysql.db AS d \
    ON (u.user = d.user AND u.host = d.host) %s \
    UNION \
    SELECT u.user, u.host, t.db, u.select_priv, u.%s \
    FROM mysql.user AS u LEFT JOIN mysql.tables_priv AS t \
    ON (u.user = t.user AND u.host = t.host) %s"

static int add_databases(SERV_LISTENER *listener, MYSQL *con);
static int add_wildcard_users(USERS *users, char* name, char* host,
                              char* password, char* anydb, char* db, HASHTABLE* hash);
static void *dbusers_keyread(int fd);
static int dbusers_keywrite(int fd, void *key);
static void *dbusers_valueread(int fd);
static int dbusers_valuewrite(int fd, void *value);
static int get_all_users(SERV_LISTENER *listener, USERS *users);
static int get_databases(SERV_LISTENER *listener, MYSQL *users);
static int get_users(SERV_LISTENER *listener, USERS *users);
static MYSQL *gw_mysql_init(void);
static int gw_mysql_set_timeouts(MYSQL* handle);
static bool host_has_singlechar_wildcard(const char *host);
static bool host_matches_singlechar_wildcard(const char* user, const char* wild);
static bool is_ipaddress(const char* host);
static char *mysql_format_user_entry(void *data);
static char *mysql_format_user_entry(void *data);
static int normalize_hostname(const char *input_host, char *output_host);
static int resource_add(HASHTABLE *, char *, char *);
static HASHTABLE *resource_alloc();
static void *resource_fetch(HASHTABLE *, char *);
static void resource_free(HASHTABLE *resource);
static int uh_cmpfun(const void* v1, const void* v2);
static int uh_hfun(const void* key);
static MYSQL_USER_HOST *uh_keydup(const MYSQL_USER_HOST* key);
static void uh_keyfree(MYSQL_USER_HOST* key);
static int wildcard_db_grant(char* str);
static void merge_netmask(char *host);

static char* get_new_users_query(const char *server_version, bool include_root)
{
    const char* password = strstr(server_version, "5.7.") ? MYSQL57_PASSWORD : MYSQL_PASSWORD;
    const char *with_root = include_root ? "WHERE u.user NOT IN ('root')" : "";

    size_t n_bytes = snprintf(NULL, 0, NEW_LOAD_DBUSERS_QUERY, password, with_root, password, with_root);
    char *rval = MXS_MALLOC(n_bytes + 1);

    if (rval)
    {
        snprintf(rval, n_bytes + 1, NEW_LOAD_DBUSERS_QUERY, password, with_root, password, with_root);
    }

    return rval;
}

/**
 * Check if the IP address of the user matches the one in the grant. This assumes
 * that the grant has one or more single-character wildcards in it.
 * @param userhost User host address
 * @param wildcardhost Host address in the grant
 * @return True if the host address matches
 */
static bool host_matches_singlechar_wildcard(const char* user, const char* wild)
{
    while (*user != '\0' && *wild != '\0')
    {
        if (*user != *wild && *wild != '_')
        {
            return false;
        }
        user++;
        wild++;
    }
    return true;
}

/**
 * Replace the user/passwd form mysql.user table into the service users' hashtable
 * environment.
 * The replacement is succesful only if the users' table checksums differ
 *
 * @param service   The current service
 * @return      -1 on any error or the number of users inserted (0 means no users at all)
 */
int
replace_mysql_users(SERV_LISTENER *listener)
{
    USERS *newusers = mysql_users_alloc();

    if (newusers == NULL)
    {
        return -1;
    }

    spinlock_acquire(&listener->lock);

    /* load users and grants from the backend database */
    int i = get_users(listener, newusers);

    if (i <= 0)
    {
        /** Failed to load users */
        if (listener->users)
        {
            /* Restore old users and resources */
            users_free(newusers);
        }
        else
        {
            /* No users allocated, use the empty new one */
            listener->users = newusers;
        }
        spinlock_release(&listener->lock);
        return i;
    }

    /** TODO: Figure out a way to create a checksum function in the backend server
     * so that we can avoid querying the complete list of users every time we
     * need to refresh the users */
    MXS_DEBUG("%lu [replace_mysql_users] users' tables replaced", pthread_self());
    USERS *oldusers = listener->users;
    listener->users = newusers;

    spinlock_release(&listener->lock);

    if (oldusers)
    {
        /* free the old table */
        users_free(oldusers);
    }

    return i;
}

/**
 * Check if the IP address is a valid MySQL IP address. The IP address can contain
 * single or multi-character wildcards as used by MySQL.
 * @param host IP address to check
 * @return True if the address is a valid, MySQL type IP address
 */
static bool is_ipaddress(const char* host)
{
    while (*host != '\0')
    {
        if (!isdigit(*host) && *host != '.' && *host != '_' && *host != '%')
        {
            return false;
        }
        host++;
    }
    return true;
}

/**
 * Check if an IP address has single-character wildcards. A single-character
 * wildcard is represented by an underscore in the MySQL hostnames.
 * @param host Hostname to check
 * @return True if the hostname is a valid IP address with a single character wildcard
 */
static bool host_has_singlechar_wildcard(const char *host)
{
    const char* chrptr = host;
    bool retval = false;

    while (*chrptr != '\0')
    {
        if (!isdigit(*chrptr) && *chrptr != '.')
        {
            if (*chrptr == '_')
            {
                retval = true;
            }
            else
            {
                return false;
            }
        }
        chrptr++;
    }
    return retval;
}

/**
 * Add a new MySQL user with host, password and netmask into the service users table
 *
 * The netmask values are:
 * 0 for any, 32 for single IPv4
 * 24 for a class C from a.b.c.%, 16 for a Class B from a.b.%.% and 8 for a Class A from a.%.%.%
 *
 * @param users         The users table
 * @param user          The user name
 * @param host          The host to add, with possible wildcards
 * @param passwd        The sha1(sha1(passoword)) to add
 * @return              1 on success, 0 on failure and -1 on duplicate user
 */

int add_mysql_users_with_host_ipv4(USERS *users, const char *user, const char *host,
                                   char *passwd, const char *anydb, const char *db)
{
    struct sockaddr_in serv_addr;
    MYSQL_USER_HOST key;
    char ret_ip[400] = "";
    int ret = 0;

    if (users == NULL || user == NULL || host == NULL)
    {
        return ret;
    }

    /* prepare the user@host data struct */
    memset(&serv_addr, 0, sizeof(serv_addr));
    memset(&key, 0, sizeof(key));

    /* set user */
    key.user = MXS_STRDUP(user);

    if (key.user == NULL)
    {
        return ret;
    }

    /* for anydb == Y key.resource is '\0' as set by memset */
    if (anydb == NULL)
    {
        key.resource = NULL;
    }
    else
    {
        if (strcmp(anydb, "N") == 0)
        {
            if (db != NULL)
            {
                key.resource = MXS_STRDUP(db);
                MXS_ABORT_IF_NULL(key.resource);
            }
            else
            {
                key.resource = NULL;
            }
        }
        else
        {
            key.resource = MXS_STRDUP("");
            MXS_ABORT_IF_NULL(key.resource);
        }
    }

    /* handle ANY, Class C,B,A */

    /* ANY */
    if (strcmp(host, "%") == 0)
    {
        strcpy(ret_ip, "0.0.0.0");
        key.netmask = 0;
    }
    else if ((strnlen(host, MYSQL_HOST_MAXLEN + 1) <= MYSQL_HOST_MAXLEN) &&
             /** The host is an ip-address and has a '_'-wildcard but not '%'
              * (combination of both is invalid). */
             (is_ipaddress(host) && host_has_singlechar_wildcard(host)))
    {
        strcpy(key.hostname, host);
        strcpy(ret_ip, "0.0.0.0");
        key.netmask = 0;
    }
    else
    {
        /* hostname without % wildcards has netmask = 32 */
        key.netmask = normalize_hostname(host, ret_ip);

        if (key.netmask == -1)
        {
            MXS_ERROR("strdup() failed in normalize_hostname for %s@%s", user, host);
        }
    }

    /* fill IPv4 data struct */
    if (setipaddress(&serv_addr.sin_addr, ret_ip) && strlen(ret_ip))
    {

        /* copy IPv4 data into key.ipv4 */
        memcpy(&key.ipv4, &serv_addr, sizeof(serv_addr));

        /* if netmask < 32 there are % wildcards */
        if (key.netmask < 32)
        {
            /* let's zero the last IP byte: a.b.c.0 we may have set above to 1*/
            key.ipv4.sin_addr.s_addr &= 0x00FFFFFF;
        }

        /* add user@host as key and passwd as value in the MySQL users hash table */
        if (mysql_users_add(users, &key, passwd))
        {
            ret = 1;
        }
        else if (key.user)
        {
            ret = -1;
        }
    }

    MXS_FREE(key.user);
    MXS_FREE(key.resource);

    return ret;
}

static bool check_password(const char *output, uint8_t *token, size_t token_len,
                           uint8_t *scramble, size_t scramble_len, uint8_t *phase2_scramble)
{
    uint8_t stored_token[SHA_DIGEST_LENGTH] = {};
    size_t stored_token_len = sizeof(stored_token);

    if (*output)
    {
        /** Convert the hexadecimal string to binary */
        gw_hex2bin(stored_token, output, strlen(output));
    }

    /**
     * The client authentication token is made up of:
     *
     * XOR( SHA1(real_password), SHA1( CONCAT( scramble, <value of mysql.user.password> ) ) )
     *
     * Since we know the scramble and the value stored in mysql.user.password,
     * we can extract the SHA1 of the real password by doing a XOR of the client
     * authentication token with the SHA1 of the scramble concatenated with the
     * value of mysql.user.password.
     *
     * Once we have the SHA1 of the original password,  we can create the SHA1
     * of this hash and compare the value with the one stored in the backend
     * database. If the values match, the user has sent the right password.
     */

    /** First, calculate the SHA1 of the scramble and the hash stored in the database */
    uint8_t step1[SHA_DIGEST_LENGTH];
    gw_sha1_2_str(scramble, scramble_len, stored_token, stored_token_len, step1);

    /** Next, extract the SHA1 of the real password by XOR'ing it with
     * the output of the previous calculation */
    uint8_t step2[SHA_DIGEST_LENGTH];
    gw_str_xor(step2, token, step1, token_len);

    /** The phase 2 scramble needs to be copied to the shared data structure as it
     * is required when the backend authentication is done. */
    memcpy(phase2_scramble, step2, SHA_DIGEST_LENGTH);

    /** Finally, calculate the SHA1 of the hashed real password */
    uint8_t final_step[SHA_DIGEST_LENGTH];
    gw_sha1_str(step2, SHA_DIGEST_LENGTH, final_step);

    /** If the two values match, the client has sent the correct password */
    return memcmp(final_step, stored_token, stored_token_len) == 0;
}

/** Callback for check_database() */
static int database_cb(void *data, int columns, char** rows, char** row_names)
{
    bool *rval = (bool*)data;
    *rval = true;
    return 0;
}

static bool check_database(sqlite3 *handle, const char *database)
{
    bool rval = true;

    if (*database)
    {
        rval = false;
        size_t len = sizeof(mysqlauth_validate_database_query) + strlen(database) + 1;
        char sql[len];

        sprintf(sql, mysqlauth_validate_database_query, database);

        char *err;

        if (sqlite3_exec(handle, sql, database_cb, &rval, &err) != SQLITE_OK)
        {
            MXS_ERROR("Failed to execute auth query: %s", err);
            sqlite3_free(err);
            rval = false;
        }
    }

    return rval;
}

/** Used to detect empty result sets */
struct user_query_result
{
    bool ok;
    char output[SHA_DIGEST_LENGTH * 2 + 1];
};

/** @brief Callback for sqlite3_exec() */
static int auth_cb(void *data, int columns, char** rows, char** row_names)
{
    struct user_query_result *res = (struct user_query_result*)data;
    strcpy(res->output, rows[0] ? rows[0] : "");
    res->ok = true;
    return 0;
}

/**
 * @brief Verify the user has access to the database
 *
 * @param handle       SQLite handle to MySQLAuth user database
 * @param dcb          Client DCB
 * @param session      Shared MySQL session
 * @param scramble     The scramble sent to the client in the initial handshake
 * @param scramble_len Length of @c scramble
 *
 * @return True if the user has access to the database
 */
bool validate_mysql_user(sqlite3 *handle, DCB *dcb, MYSQL_session *session,
                         uint8_t *scramble, size_t scramble_len)
{
    size_t len = sizeof(mysqlauth_validate_user_query) + strlen(session->user) * 2 +
                 strlen(session->db) * 2 + MYSQL_HOST_MAXLEN + session->auth_token_len * 4 + 1;
    char sql[len + 1];
    bool rval = false;
    char *err;

    sprintf(sql, mysqlauth_validate_user_query, session->user, dcb->remote,
            session->db, session->db);

    struct user_query_result res = {};

    if (sqlite3_exec(handle, sql, auth_cb, &res, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to execute auth query: %s", err);
        sqlite3_free(err);
    }

    if (!res.ok)
    {
        /**
         * Try authentication with the hostname instead of the IP. We do this only
         * as a last resort so we avoid the high cost of the DNS lookup.
         */
        char client_hostname[MYSQL_HOST_MAXLEN];
        wildcard_domain_match(dcb->remote, client_hostname);
        sprintf(sql, mysqlauth_validate_user_query, session->user, client_hostname,
                session->db, session->db);

        if (sqlite3_exec(handle, sql, auth_cb, &res, &err) != SQLITE_OK)
        {
            MXS_ERROR("Failed to execute auth query: %s", err);
            sqlite3_free(err);
        }
    }

    if (res.ok)
    {
        /** Found a matching row */
        if (session->auth_token_len)
        {
            /** If authentication fails, this will trigger the right
             * error message with `Using password : YES` */
            session->client_sha1[0] = '_';
        }

        if (check_password(res.output, session->auth_token, session->auth_token_len,
                           scramble, scramble_len, session->client_sha1))
        {
            /** Password is OK, check that the database exists */
            rval = check_database(handle, session->db);
        }
    }

    return rval;
}

/**
 * @brief Delete all users
 *
 * @param handle SQLite handle
 */
static void delete_mysql_users(sqlite3 *handle)
{
    char *err;

    if (sqlite3_exec(handle, delete_users_query, NULL, NULL, &err) != SQLITE_OK ||
        sqlite3_exec(handle, delete_databases_query, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to delete old users: %s", err);
        sqlite3_free(err);
    }
}

/**
 * @brief Add new MySQL user to the internal user database
 *
 * @param handle Database handle
 * @param user   Username
 * @param host   Host
 * @param db     Database
 * @param anydb  Global access to databases
 */
void add_mysql_user(sqlite3 *handle, const char *user, const char *host,
                    const char *db, bool anydb, const char *pw)
{
    size_t dblen = db && *db ? strlen(db) + 2 : sizeof(null_token); /** +2 for single quotes */
    char dbstr[dblen + 1];

    if (db && *db)
    {
        sprintf(dbstr, "'%s'", db);
    }
    else
    {
        strcpy(dbstr, null_token);
    }

    size_t pwlen = pw && *pw ? strlen(pw) + 2 : sizeof(null_token); /** +2 for single quotes */
    char pwstr[pwlen + 1];

    if (pw && *pw)
    {
        if (*pw == '*')
        {
            pw++;
        }
        sprintf(pwstr, "'%s'", pw);
    }
    else
    {
        strcpy(pwstr, null_token);
    }

    size_t len = sizeof(insert_user_query) + strlen(user) + strlen(host) + dblen + pwlen + 1;

    char insert_sql[len + 1];
    sprintf(insert_sql, insert_user_query, user, host, dbstr, anydb ? "1" : "0", pwstr);

    char *err;
    if (sqlite3_exec(handle, insert_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to insert user: %s", err);
        sqlite3_free(err);
    }

    MXS_INFO("Added user: %s", insert_sql);
}

static void add_database(sqlite3 *handle, const char *db)
{
    size_t len = sizeof(insert_database_query) + strlen(db) + 1;
    char insert_sql[len + 1];

    sprintf(insert_sql, insert_database_query, db);

    char *err;
    if (sqlite3_exec(handle, insert_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to insert database: %s", err);
        sqlite3_free(err);
    }
}

/**
 * Allocate a new MySQL users table for mysql specific users@host as key
 *
 *  @return The users table
 */
USERS *
mysql_users_alloc()
{
    USERS *rval;

    if ((rval = MXS_CALLOC(1, sizeof(USERS))) == NULL)
    {
        return NULL;
    }

    if ((rval->data = hashtable_alloc(USERS_HASHTABLE_DEFAULT_SIZE, uh_hfun,
                                      uh_cmpfun)) == NULL)
    {
        MXS_FREE(rval);
        return NULL;
    }

    /* set the MySQL user@host print routine for the debug interface */
    rval->usersCustomUserFormat = mysql_format_user_entry;

    /* the key is handled by uh_keydup/uh_keyfree.
     * the value is a (char *): it's handled by strdup/free
     */
    hashtable_memory_fns(rval->data,
                         (HASHCOPYFN) uh_keydup, hashtable_item_strdup,
                         (HASHFREEFN) uh_keyfree, hashtable_item_free);

    return rval;
}

/**
 * Add a new MySQL user to the user table. The user name must be unique
 *
 * @param users     The users table
 * @param user      The user name
 * @param auth      The authentication data
 * @return          The number of users added to the table
 */
int
mysql_users_add(USERS *users, MYSQL_USER_HOST *key, char *auth)
{
    int add;

    if (key == NULL || key->user == NULL)
    {
        return 0;
    }

    atomic_add(&users->stats.n_adds, 1);
    add = hashtable_add(users->data, key, auth);
    atomic_add(&users->stats.n_entries, add);

    return add;
}

/**
 * Fetch the authentication data for a particular user from the users table
 *
 * @param users The MySQL users table
 * @param key   The key with user@host
 * @return  The authentication data or NULL on error
 */
char *mysql_users_fetch(USERS *users, MYSQL_USER_HOST *key)
{
    if (key == NULL)
    {
        return NULL;
    }
    atomic_add(&users->stats.n_fetches, 1);
    return hashtable_fetch(users->data, key);
}

/**
 * The hash function we use for storing MySQL users as: users@hosts.
 * Currently only IPv4 addresses are supported
 *
 * @param key   The key value, i.e. username@host (IPv4)
 * @return      The hash key
 */

static int uh_hfun(const void* key)
{
    const MYSQL_USER_HOST *hu = (const MYSQL_USER_HOST *) key;

    if (key == NULL || hu == NULL || hu->user == NULL)
    {
        return 0;
    }
    else
    {
        return (*hu->user + * (hu->user + 1) +
                (unsigned int) (hu->ipv4.sin_addr.s_addr & 0xFF000000 / (256 * 256 * 256)));
    }
}

/**
 * The compare function we use for compare MySQL users as: users@hosts.
 * Currently only IPv4 addresses are supported
 *
 * @param key1  The key value, i.e. username@host (IPv4)
 * @param key2  The key value, i.e. username@host (IPv4)
 * @return      The compare value
 */

static int uh_cmpfun(const void* v1, const void* v2)
{
    const MYSQL_USER_HOST *hu1 = (const MYSQL_USER_HOST *) v1;
    const MYSQL_USER_HOST *hu2 = (const MYSQL_USER_HOST *) v2;

    if (v1 == NULL || v2 == NULL)
    {
        return 0;
    }

    if (hu1->user == NULL || hu2->user == NULL)
    {
        return 0;
    }

    /** If the stored user has the unmodified address stored, that means we were not able
     * to resolve it at the time we loaded the users. We need to check if the
     * address contains wildcards and if the user's address matches that. */

    const bool wildcard_host = strlen(hu2->hostname) > 0 && strlen(hu1->hostname) > 0;

    if ((strcmp(hu1->user, hu2->user) == 0) &&
        /** Check for wildcard hostnames */
        ((wildcard_host && host_matches_singlechar_wildcard(hu1->hostname, hu2->hostname)) ||
         /** If no wildcard hostname is stored, check for network address. */
         (!wildcard_host && (hu1->ipv4.sin_addr.s_addr == hu2->ipv4.sin_addr.s_addr) &&
          (hu1->netmask >= hu2->netmask))))
    {
        /* if no database name was passed, auth is ok */
        if (hu1->resource == NULL || (hu1->resource && !strlen(hu1->resource)))
        {
            return 0;
        }
        else
        {
            /* (1) check for no database grants at all and deny auth */
            if (hu2->resource == NULL)
            {
                return 1;
            }
            /* (2) check for ANY database grant and allow auth */
            if (!strlen(hu2->resource))
            {
                return 0;
            }
            /* (3) check for database name specific grant and allow auth */
            if (hu1->resource && hu2->resource && strcmp(hu1->resource,
                                                         hu2->resource) == 0)
            {
                return 0;
            }

            if (hu2->resource && strlen(hu2->resource) && strchr(hu2->resource, '%') != NULL)
            {
                regex_t re;
                char db[MYSQL_DATABASE_MAXLEN * 2 + 1];
                strcpy(db, hu2->resource);
                int len = strlen(db);
                char* ptr = strrchr(db, '%');

                if (ptr == NULL)
                {
                    return 1;
                }

                while (ptr)
                {
                    memmove(ptr + 1, ptr, (len - (ptr - db)) + 1);
                    *ptr = '.';
                    *(ptr + 1) = '*';
                    len = strlen(db);
                    ptr = strrchr(db, '%');
                }

                if ((regcomp(&re, db, REG_ICASE | REG_NOSUB)))
                {
                    return 1;
                }

                if (regexec(&re, hu1->resource, 0, NULL, 0) == 0)
                {
                    regfree(&re);
                    return 0;
                }
                regfree(&re);
            }

            /* no matches, deny auth */
            return 1;
        }
    }
    else
    {
        return 1;
    }
}

/**
 *The key dup function we use for duplicate the users@hosts.
 *
 * @param key   The key value, i.e. username@host ip4/ip6 data
 */

static MYSQL_USER_HOST *uh_keydup(const MYSQL_USER_HOST* key)
{
    if ((key == NULL) || (key->user == NULL))
    {
        return NULL;
    }

    MYSQL_USER_HOST *rval = (MYSQL_USER_HOST *) MXS_CALLOC(1, sizeof(MYSQL_USER_HOST));
    char* user = MXS_STRDUP(key->user);
    char* resource = key->resource ? MXS_STRDUP(key->resource) : NULL;

    if (!user || !rval || (key->resource && !resource))
    {
        MXS_FREE(rval);
        MXS_FREE(user);
        MXS_FREE(resource);
        return NULL;
    }

    rval->user = user;
    rval->ipv4 = key->ipv4;
    rval->netmask = key->netmask;
    rval->resource = resource;
    strcpy(rval->hostname, key->hostname);

    return (void *) rval;
}

/**
 * The key free function we use for freeing the users@hosts data
 *
 * @param key   The key value, i.e. username@host ip4 data
 */
static void uh_keyfree(MYSQL_USER_HOST* key)
{
    if (key)
    {
        MXS_FREE(key->user);
        MXS_FREE(key->resource);
        MXS_FREE(key);
    }
}

/**
 * Format the mysql user as user@host
 * The returned memory must be freed by the caller
 *
 *  @param data     Input data
 *  @return         the MySQL user@host
 */
static char *mysql_format_user_entry(void *data)
{
    MYSQL_USER_HOST *entry;
    char *mysql_user;
    /* the returned user string is "USER" + "@" + "HOST" + '\0' */
    int mysql_user_len = MYSQL_USER_MAXLEN + 1 + INET_ADDRSTRLEN + 10 +
                         MYSQL_USER_MAXLEN + 1;

    if (data == NULL)
    {
        return NULL;
    }

    entry = (MYSQL_USER_HOST *) data;

    mysql_user = (char *) MXS_CALLOC(mysql_user_len, sizeof(char));

    if (mysql_user == NULL)
    {
        return NULL;
    }

    /* format user@host based on wildcards */

    if (entry->ipv4.sin_addr.s_addr == INADDR_ANY && entry->netmask == 0)
    {
        snprintf(mysql_user, mysql_user_len - 1, "%s@%%", entry->user);
    }
    else if ((entry->ipv4.sin_addr.s_addr & 0xFF000000) == 0 && entry->netmask == 24)
    {
        snprintf(mysql_user, mysql_user_len - 1, "%s@%i.%i.%i.%%", entry->user,
                 entry->ipv4.sin_addr.s_addr & 0x000000FF,
                 (entry->ipv4.sin_addr.s_addr & 0x0000FF00) / (256),
                 (entry->ipv4.sin_addr.s_addr & 0x00FF0000) / (256 * 256));
    }
    else if ((entry->ipv4.sin_addr.s_addr & 0xFFFF0000) == 0 && entry->netmask == 16)
    {
        snprintf(mysql_user, mysql_user_len - 1, "%s@%i.%i.%%.%%", entry->user,
                 entry->ipv4.sin_addr.s_addr & 0x000000FF,
                 (entry->ipv4.sin_addr.s_addr & 0x0000FF00) / (256));
    }
    else if ((entry->ipv4.sin_addr.s_addr & 0xFFFFFF00) == 0 && entry->netmask == 8)
    {
        snprintf(mysql_user, mysql_user_len - 1, "%s@%i.%%.%%.%%", entry->user,
                 entry->ipv4.sin_addr.s_addr & 0x000000FF);
    }
    else if (entry->netmask == 32)
    {
        strcpy(mysql_user, entry->user);
        strcat(mysql_user, "@");
        inet_ntop(AF_INET, &(entry->ipv4).sin_addr, mysql_user + strlen(mysql_user),
                  INET_ADDRSTRLEN);
    }
    else
    {
        snprintf(mysql_user, MYSQL_USER_MAXLEN - 5, "Err: %s", entry->user);
        strcat(mysql_user, "@");
        inet_ntop(AF_INET, &(entry->ipv4).sin_addr, mysql_user + strlen(mysql_user),
                  INET_ADDRSTRLEN);
    }

    return mysql_user;
}

/**
 * Remove the resources table
 *
 * @param resources The resources table to remove
 */
static void
resource_free(HASHTABLE *resources)
{
    if (resources)
    {
        hashtable_free(resources);
    }
}

/**
 * Allocate a MySQL database names table
 *
 * @return  The database names table
 */
static HASHTABLE *
resource_alloc()
{
    HASHTABLE *resources;

    if ((resources = hashtable_alloc(10, hashtable_item_strhash, hashtable_item_strcmp)) == NULL)
    {
        return NULL;
    }

    hashtable_memory_fns(resources,
                         hashtable_item_strdup, hashtable_item_strdup,
                         hashtable_item_free, hashtable_item_free);

    return resources;
}

/**
 * Add a new MySQL database name to the resources table. The resource name must
 * be unique.
 * @param resources The resources table
 * @param key       The resource name
 * @param value     The value for resource (not used)
 * @return          The number of resources dded to the table
 */
static int
resource_add(HASHTABLE *resources, char *key, char *value)
{
    return hashtable_add(resources, key, value);
}

/**
 * Fetch a particular database name from the resources table
 *
 * @param resources The MySQL database names table
 * @param key       The database name to fetch
 * @return          The database esists or NULL if not found
 */
static void *
resource_fetch(HASHTABLE *resources, char *key)
{
    return hashtable_fetch(resources, key);
}

/**
 * Normalize hostname with % wildcards to a valid IP string.
 *
 * Valid input values:
 * a.b.c.d, a.b.c.%, a.b.%.%, a.%.%.%
 * Short formats a.% and a.%.% are both converted to a.%.%.%
 * Short format a.b.% is converted to a.b.%.%
 *
 * Last host byte is set to 1, avoiding setipadress() failure
 *
 * @param input_host    The hostname with possible % wildcards
 * @param output_host   The normalized hostname (buffer must be preallocated)
 * @return              The calculated netmask or -1 on failure
 */
static int normalize_hostname(const char *input_host, char *output_host)
{
    int netmask, bytes, bits = 0, found_wildcard = 0;
    char *p, *lasts, *tmp;
    int useorig = 0;

    output_host[0] = 0;
    bytes = 0;

    tmp = MXS_STRDUP(input_host);

    if (tmp == NULL)
    {
        return -1;
    }
    /* Handle hosts with netmasks (e.g. "123.321.123.0/255.255.255.0") by
     * replacing the zeros with '%'.
     */
    merge_netmask(tmp);

    p = strtok_r(tmp, ".", &lasts);
    while (p != NULL)
    {

        if (strcmp(p, "%"))
        {
            if (!isdigit(*p))
            {
                useorig = 1;
            }

            strcat(output_host, p);
            bits += 8;
        }
        else if (bytes == 3)
        {
            found_wildcard = 1;
            strcat(output_host, "1");
        }
        else
        {
            found_wildcard = 1;
            strcat(output_host, "0");
        }
        bytes++;
        p = strtok_r(NULL, ".", &lasts);
        if (p)
        {
            strcat(output_host, ".");
        }
    }
    if (found_wildcard)
    {
        netmask = bits;
        while (bytes++ < 4)
        {
            if (bytes == 4)
            {
                strcat(output_host, ".1");
            }
            else
            {
                strcat(output_host, ".0");
            }
        }
    }
    else
    {
        netmask = 32;
    }

    if (useorig == 1)
    {
        netmask = 32;
        strcpy(output_host, input_host);
    }

    MXS_FREE(tmp);

    return netmask;
}

/**
 * Returns a MYSQL object suitably configured.
 *
 * @return An object or NULL if something fails.
 */
MYSQL *gw_mysql_init()
{
    MYSQL* con = mysql_init(NULL);

    if (con)
    {
        if (gw_mysql_set_timeouts(con) == 0)
        {
            // MYSQL_OPT_USE_REMOTE_CONNECTION must be set if the embedded
            // libary is used. With Connector-C (at least 2.2.1) the call
            // fails.
#if !defined(LIBMARIADB)
            if (mysql_options(con, MYSQL_OPT_USE_REMOTE_CONNECTION, NULL) != 0)
            {
                MXS_ERROR("Failed to set external connection. "
                          "It is needed for backend server connections.");
                mysql_close(con);
                con = NULL;
            }
#endif
        }
        else
        {
            MXS_ERROR("Failed to set timeout values for backend connection.");
            mysql_close(con);
            con = NULL;
        }
    }
    else
    {
        MXS_ERROR("mysql_init: %s", mysql_error(NULL));
    }

    return con;
}

/**
 * Set read, write and connect timeout values for MySQL database connection.
 *
 * @param handle            MySQL handle
 * @param read_timeout      Read timeout value in seconds
 * @param write_timeout     Write timeout value in seconds
 * @param connect_timeout   Connect timeout value in seconds
 *
 * @return 0 if succeed, 1 if failed
 */
static int gw_mysql_set_timeouts(MYSQL* handle)
{
    int rc;

    MXS_CONFIG* cnf = config_get_global_options();

    if ((rc = mysql_options(handle, MYSQL_OPT_READ_TIMEOUT,
                            (void *) &cnf->auth_read_timeout)))
    {
        MXS_ERROR("Failed to set read timeout for backend connection.");
        goto retblock;
    }

    if ((rc = mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT,
                            (void *) &cnf->auth_conn_timeout)))
    {
        MXS_ERROR("Failed to set connect timeout for backend connection.");
        goto retblock;
    }

    if ((rc = mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT,
                            (void *) &cnf->auth_write_timeout)))
    {
        MXS_ERROR("Failed to set write timeout for backend connection.");
        goto retblock;
    }

retblock:
    return rc;
}

/*
 * Serialise a key for the dbusers hashtable to a file
 *
 * @param fd    File descriptor to write to
 * @param key   The key to write
 * @return      0 on error, 1 if the key was written
 */
static int
dbusers_keywrite(int fd, void *key)
{
    MYSQL_USER_HOST *dbkey = (MYSQL_USER_HOST *) key;
    int tmp;

    tmp = strlen(dbkey->user);
    if (write(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
    {
        return 0;
    }
    if (write(fd, dbkey->user, tmp) != tmp)
    {
        return 0;
    }
    if (write(fd, &dbkey->ipv4, sizeof(dbkey->ipv4)) != sizeof(dbkey->ipv4))
    {
        return 0;
    }
    if (write(fd, &dbkey->netmask, sizeof(dbkey->netmask)) != sizeof(dbkey->netmask))
    {
        return 0;
    }
    if (dbkey->resource)
    {
        tmp = strlen(dbkey->resource);
        if (write(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
        {
            return 0;
        }
        if (write(fd, dbkey->resource, tmp) != tmp)
        {
            return 0;
        }
    }
    else // NULL is valid, so represent with a length of -1
    {
        tmp = -1;
        if (write(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * Serialise a value for the dbusers hashtable to a file
 *
 * @param fd    File descriptor to write to
 * @param value The value to write
 * @return      0 on error, 1 if the value was written
 */
static int
dbusers_valuewrite(int fd, void *value)
{
    int tmp;

    tmp = strlen(value);
    if (write(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
    {
        return 0;
    }
    if (write(fd, value, tmp) != tmp)
    {
        return 0;
    }
    return 1;
}

/**
 * Unserialise a key for the dbusers hashtable from a file
 *
 * @param fd    File descriptor to read from
 * @return      Pointer to the new key or NULL on error
 */
static void *
dbusers_keyread(int fd)
{
    MYSQL_USER_HOST *dbkey;

    if ((dbkey = (MYSQL_USER_HOST *) MXS_MALLOC(sizeof(MYSQL_USER_HOST))) == NULL)
    {
        return NULL;
    }

    *dbkey->hostname = '\0';

    int user_size;
    if (read(fd, &user_size, sizeof(user_size)) != sizeof(user_size))
    {
        MXS_FREE(dbkey);
        return NULL;
    }
    if ((dbkey->user = (char *) MXS_MALLOC(user_size + 1)) == NULL)
    {
        MXS_FREE(dbkey);
        return NULL;
    }
    if (read(fd, dbkey->user, user_size) != user_size)
    {
        MXS_FREE(dbkey->user);
        MXS_FREE(dbkey);
        return NULL;
    }
    dbkey->user[user_size] = 0; // NULL Terminate
    if (read(fd, &dbkey->ipv4, sizeof(dbkey->ipv4)) != sizeof(dbkey->ipv4))
    {
        MXS_FREE(dbkey->user);
        MXS_FREE(dbkey);
        return NULL;
    }
    if (read(fd, &dbkey->netmask, sizeof(dbkey->netmask)) != sizeof(dbkey->netmask))
    {
        MXS_FREE(dbkey->user);
        MXS_FREE(dbkey);
        return NULL;
    }

    int res_size;
    if (read(fd, &res_size, sizeof(res_size)) != sizeof(res_size))
    {
        MXS_FREE(dbkey->user);
        MXS_FREE(dbkey);
        return NULL;
    }
    else if (res_size != -1)
    {
        if ((dbkey->resource = (char *) MXS_MALLOC(res_size + 1)) == NULL)
        {
            MXS_FREE(dbkey->user);
            MXS_FREE(dbkey);
            return NULL;
        }
        if (read(fd, dbkey->resource, res_size) != res_size)
        {
            MXS_FREE(dbkey->resource);
            MXS_FREE(dbkey->user);
            MXS_FREE(dbkey);
            return NULL;
        }
        dbkey->resource[res_size] = 0; // NULL Terminate
    }
    else // NULL is valid, so represent with a length of -1
    {
        dbkey->resource = NULL;
    }
    return (void *) dbkey;
}

/**
 * Unserialise a value for the dbusers hashtable from a file
 *
 * @param fd    File descriptor to read from
 * @return      Return the new value data or NULL on error
 */
static void *
dbusers_valueread(int fd)
{
    char *value;
    int tmp;

    if (read(fd, &tmp, sizeof(tmp)) != sizeof(tmp))
    {
        return NULL;
    }
    if ((value = (char *) MXS_MALLOC(tmp + 1)) == NULL)
    {
        return NULL;
    }
    if (read(fd, value, tmp) != tmp)
    {
        MXS_FREE(value);
        return NULL;
    }
    value[tmp] = 0;
    return (void *) value;
}

int dump_user_cb(void *data, int fields, char **row, char **field_names)
{
    sqlite3 *handle = (sqlite3*)data;
    add_mysql_user(handle, row[0], row[1], row[2], row[3] && strcmp(row[3], "1"), row[4]);
    return 0;
}

int dump_database_cb(void *data, int fields, char **row, char **field_names)
{
    sqlite3 *handle = (sqlite3*)data;
    add_database(handle, row[0]);
    return 0;
}

static bool transfer_table_contents(sqlite3 *src, sqlite3 *dest)
{
    bool rval = true;
    char *err;

    /** Make sure the tables exist in both databases */
    if (sqlite3_exec(src, users_create_sql, NULL, NULL, &err) != SQLITE_OK ||
        sqlite3_exec(dest, users_create_sql, NULL, NULL, &err) != SQLITE_OK ||
        sqlite3_exec(src, databases_create_sql, NULL, NULL, &err) != SQLITE_OK ||
        sqlite3_exec(dest, databases_create_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to create tables: %s", err);
        sqlite3_free(err);
        rval = false;
    }

    if (sqlite3_exec(dest, "BEGIN", NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to start transaction: %s", err);
        sqlite3_free(err);
        rval = false;
    }

    /** Replace the data */
    if (sqlite3_exec(src, dump_users_query, dump_user_cb, dest, &err) != SQLITE_OK ||
        sqlite3_exec(src, dump_databases_query, dump_database_cb, dest, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to load database contents: %s", err);
        sqlite3_free(err);
        rval = false;
    }

    if (sqlite3_exec(dest, "COMMIT", NULL, NULL, &err) != SQLITE_OK)
    {
        MXS_ERROR("Failed to commit transaction: %s", err);
        sqlite3_free(err);
        rval = false;
    }

    return rval;
}

/**
 * Load users from persisted database
 *
 * @param dest Open SQLite handle where contents are loaded
 *
 * @return True on success
 */
bool dbusers_load(sqlite3 *dest, const char *filename)
{
    sqlite3 *src;

    if (sqlite3_open_v2(filename, &src, db_flags, NULL) != SQLITE_OK)
    {
        MXS_ERROR("Failed to open persisted SQLite3 database.");
        return false;
    }

    bool rval = transfer_table_contents(src, dest);
    sqlite3_close_v2(src);

    return rval;
}

/**
 * Save users to persisted database
 *
 * @param dest Open SQLite handle where contents are stored
 *
 * @return True on success
 */
bool dbusers_save(sqlite3 *src, const char *filename)
{
    sqlite3 *dest;

    if (sqlite3_open_v2(filename, &dest, db_flags, NULL) != SQLITE_OK)
    {
        MXS_ERROR("Failed to open persisted SQLite3 database.");
        return -1;
    }

    bool rval = transfer_table_contents(src, dest);
    sqlite3_close_v2(dest);

    return rval;
}

/**
 * Check if the database name contains a wildcard character
 * @param str Database grant
 * @return 1 if the name contains the '%' wildcard character, 0 if it does not
 */
static int wildcard_db_grant(char* str)
{
    char* ptr = str;

    while (ptr && *ptr != '\0')
    {
        if (*ptr == '%')
        {
            return 1;
        }
        ptr++;
    }

    return 0;
}

/**
 *
 * @param users Pointer to USERS struct
 * @param name Username of the client
 * @param host Host address of the client
 * @param password Client password
 * @param anydb If the user has access to all databases
 * @param db Database, in wildcard form
 * @param hash Hashtable with all database names
 * @return number of unique grants generated from wildcard database name
 */
static int add_wildcard_users(USERS *users, char* name, char* host, char* password,
                              char* anydb, char* db, HASHTABLE* hash)
{
    HASHITERATOR* iter;
    HASHTABLE* ht = hash;
    char *restr, *ptr, *value;
    int len, err, rval = 0;
    char errbuf[1024];
    regex_t re;

    if (db == NULL || hash == NULL)
    {
        return 0;
    }

    if ((restr = MXS_MALLOC(sizeof(char) * strlen(db) * 2)) == NULL)
    {
        return 0;
    }

    strcpy(restr, db);

    len = strlen(restr);
    ptr = strchr(restr, '%');

    if (ptr == NULL)
    {
        MXS_FREE(restr);
        return 0;
    }

    while (ptr)
    {
        memmove(ptr + 1, ptr, (len - (ptr - restr)) + 1);
        *ptr++ = '.';
        *ptr = '*';
        len = strlen(restr);
        ptr = strchr(restr, '%');
    }

    if ((err = regcomp(&re, restr, REG_ICASE | REG_NOSUB)))
    {
        regerror(err, &re, errbuf, 1024);
        MXS_ERROR("Failed to compile regex when resolving wildcard database grants: %s",
                  errbuf);
        MXS_FREE(restr);
        return 0;
    }

    iter = hashtable_iterator(ht);

    while (iter && (value = hashtable_next(iter)))
    {
        if (regexec(&re, value, 0, NULL, 0) == 0)
        {
            rval += add_mysql_users_with_host_ipv4(users, name, host, password,
                                                   anydb, value);
        }
    }

    hashtable_iterator_free(iter);
    regfree(&re);
    MXS_FREE(restr);

    return rval;
}

/**
 * @brief Check service permissions on one server
 *
 * @param server Server to check
 * @param user Username
 * @param password Password
 * @return True if the service permissions are OK, false if one or more permissions
 * are missing.
 */
static bool check_server_permissions(SERVICE *service, SERVER* server,
                                     const char* user, const char* password)
{
    MYSQL *mysql = gw_mysql_init();

    if (mysql == NULL)
    {
        return false;
    }

    MXS_CONFIG* cnf = config_get_global_options();
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &cnf->auth_read_timeout);
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &cnf->auth_conn_timeout);
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &cnf->auth_write_timeout);

    if (mxs_mysql_real_connect(mysql, server, user, password) == NULL)
    {
        int my_errno = mysql_errno(mysql);

        MXS_ERROR("[%s] Failed to connect to server '%s' (%s:%d) when"
                  " checking authentication user credentials and permissions: %d %s",
                  service->name, server->unique_name, server->name, server->port,
                  my_errno, mysql_error(mysql));

        mysql_close(mysql);
        return my_errno != ER_ACCESS_DENIED_ERROR;
    }

    /** Copy the server charset */
    MY_CHARSET_INFO cs_info;
    mysql_get_character_set_info(mysql, &cs_info);
    server->charset = cs_info.number;

    if (server->server_string == NULL)
    {
        const char *server_string = mysql_get_server_info(mysql);
        server_set_version_string(server, server_string);
    }

    const char *template = "SELECT user, host, %s, Select_priv FROM mysql.user limit 1";
    const char* query_pw = strstr(server->server_string, "5.7.") ?
    MYSQL57_PASSWORD : MYSQL_PASSWORD;
    char query[strlen(template) + strlen(query_pw) + 1];
    bool rval = true;
    sprintf(query, template, query_pw);

    if (mysql_query(mysql, query) != 0)
    {
        if (mysql_errno(mysql) == ER_TABLEACCESS_DENIED_ERROR)
        {
            MXS_ERROR("[%s] User '%s' is missing SELECT privileges"
                      " on mysql.user table. MySQL error message: %s",
                      service->name, user, mysql_error(mysql));
            rval = false;
        }
        else
        {
            MXS_ERROR("[%s] Failed to query from mysql.user table."
                      " MySQL error message: %s", service->name, mysql_error(mysql));
        }
    }
    else
    {

        MYSQL_RES* res = mysql_use_result(mysql);
        if (res == NULL)
        {
            MXS_ERROR("[%s] Result retrieval failed when checking for permissions to "
                      "the mysql.user table: %s", service->name, mysql_error(mysql));
        }
        else
        {
            mysql_free_result(res);
        }
    }

    if (mysql_query(mysql, "SELECT user, host, db FROM mysql.db limit 1") != 0)
    {
        if (mysql_errno(mysql) == ER_TABLEACCESS_DENIED_ERROR)
        {
            MXS_WARNING("[%s] User '%s' is missing SELECT privileges on mysql.db table. "
                        "Database name will be ignored in authentication. "
                        "MySQL error message: %s", service->name, user, mysql_error(mysql));
        }
        else
        {
            MXS_ERROR("[%s] Failed to query from mysql.db table. MySQL error message: %s",
                      service->name, mysql_error(mysql));
        }
    }
    else
    {
        MYSQL_RES* res = mysql_use_result(mysql);
        if (res == NULL)
        {
            MXS_ERROR("[%s] Result retrieval failed when checking for permissions "
                      "to the mysql.db table: %s", service->name, mysql_error(mysql));
        }
        else
        {
            mysql_free_result(res);
        }
    }

    if (mysql_query(mysql, "SELECT user, host, db FROM mysql.tables_priv limit 1") != 0)
    {
        if (mysql_errno(mysql) == ER_TABLEACCESS_DENIED_ERROR)
        {
            MXS_WARNING("[%s] User '%s' is missing SELECT privileges on mysql.tables_priv table. "
                        "Database name will be ignored in authentication. "
                        "MySQL error message: %s", service->name, user, mysql_error(mysql));
        }
        else
        {
            MXS_ERROR("[%s] Failed to query from mysql.tables_priv table. "
                      "MySQL error message: %s", service->name, mysql_error(mysql));
        }
    }
    else
    {
        MYSQL_RES* res = mysql_use_result(mysql);
        if (res == NULL)
        {
            MXS_ERROR("[%s] Result retrieval failed when checking for permissions "
                      "to the mysql.tables_priv table: %s", service->name, mysql_error(mysql));
        }
        else
        {
            mysql_free_result(res);
        }
    }

    mysql_close(mysql);

    return rval;
}

/**
 * @brief Check if the service user has all required permissions to operate properly.
 *
 * This checks for SELECT permissions on mysql.user, mysql.db and mysql.tables_priv
 * tables and for SHOW DATABASES permissions. If permissions are not adequate,
 * an error message is logged and the service is not started.
 *
 * @param service Service to inspect
 * @return True if service permissions are correct on at least one server, false
 * if permissions are missing or if an error occurred.
 */
bool check_service_permissions(SERVICE* service)
{
    if (is_internal_service(service->routerModule) ||
        config_get_global_options()->skip_permission_checks ||
        service->dbref == NULL) // No servers to check
    {
        return true;
    }

    char *user, *password;

    if (serviceGetUser(service, &user, &password) == 0)
    {
        MXS_ERROR("[%s] Service is missing the user credentials for authentication.",
                  service->name);
        return false;
    }

    char *dpasswd = decrypt_password(password);
    bool rval = false;

    for (SERVER_REF *server = service->dbref; server; server = server->next)
    {
        if (check_server_permissions(service, server->server, user, dpasswd))
        {
            rval = true;
        }
    }

    free(dpasswd);

    return rval;
}

/**
 * If the hostname is of form a.b.c.d/e.f.g.h where e-h is 255 or 0, replace
 * the zeros in the first part with '%' and remove the second part. This does
 * not yet support netmasks completely, but should be sufficient for most
 * situations. In case of error, the hostname may end in an invalid state, which
 * will cause an error later on.
 *
 * @param host  The hostname, which is modified in-place. If merging is unsuccessful,
 *              it may end up garbled.
 */
static void merge_netmask(char *host)
{
    char *delimiter_loc = strchr(host, '/');
    if (delimiter_loc == NULL)
    {
        return; // Nothing to do
    }
    /* If anything goes wrong, we put the '/' back in to ensure the hostname
     * cannot be used.
     */
    *delimiter_loc = '\0';

    char *ip_token_loc = host;
    char *mask_token_loc = delimiter_loc + 1; // This is at minimum a \0

    while (ip_token_loc && mask_token_loc)
    {
        if (strncmp(mask_token_loc, "255", 3) == 0)
        {
            // Skip
        }
        else if (*mask_token_loc == '0' && *ip_token_loc == '0')
        {
            *ip_token_loc = '%';
        }
        else
        {
            /* Any other combination is considered invalid. This may leave the
             * hostname in a partially modified state.
             * TODO: handle more cases
             */
            *delimiter_loc = '/';
            MXS_ERROR("Unrecognized IP-bytes in host/mask-combination. "
                      "Merge incomplete: %s", host);
            return;
        }

        ip_token_loc = strchr(ip_token_loc, '.');
        mask_token_loc = strchr(mask_token_loc, '.');
        if (ip_token_loc && mask_token_loc)
        {
            ip_token_loc++;
            mask_token_loc++;
        }
    }
    if (ip_token_loc || mask_token_loc)
    {
        *delimiter_loc = '/';
        MXS_ERROR("Unequal number of IP-bytes in host/mask-combination. "
                  "Merge incomplete: %s", host);
    }
}

/**
 * @brief Check if an ip matches a wildcard hostname.
 *
 * One of the parameters should be an IP-address without wildcards, the other a
 * hostname with wildcards. The hostname corresponding to the ip-address will be
 * looked up and compared to the hostname with wildcard(s). Any error in the
 * parameters or looking up the hostname will result in a false match.
 *
 * @param ip-address or a hostname with wildcard(s)
 * @param ip-address or a hostname with wildcard(s)
 * @return True if the host represented by the IP matches the wildcard string
 */
static bool wildcard_domain_match(const char *ip_address, char *client_hostname)
{
    /* Looks like the parameters are valid. First, convert the client IP string
     * to binary form. This is somewhat silly, since just a while ago we had the
     * binary address but had to zero it. dbusers.c should be refactored to fix this.
     */
    struct sockaddr_in bin_address;
    bin_address.sin_family = AF_INET;
    if (inet_pton(bin_address.sin_family, ip_address, &(bin_address.sin_addr)) != 1)
    {
        MXS_ERROR("Could not convert to binary ip-address: '%s'.", ip_address);
        return false;
    }

    /* Try to lookup the domain name of the given IP-address. This is a slow
     * i/o-operation, which will stall the entire thread. TODO: cache results
     * if this feature is used often.
     */
    MXS_DEBUG("Resolving '%s'", ip_address);
    int lookup_result = getnameinfo((struct sockaddr*)&bin_address,
                                    sizeof(struct sockaddr_in),
                                    client_hostname, sizeof(client_hostname),
                                    NULL, 0, // No need for the port
                                    NI_NAMEREQD); // Text address only

    if (lookup_result != 0)
    {
        MXS_ERROR("Client hostname lookup failed, getnameinfo() returned: '%s'.",
                  gai_strerror(lookup_result));
    }
    else
    {
        MXS_DEBUG("IP-lookup success, hostname is: '%s'", client_hostname);
    }

    return false;
}

int get_users_from_server(MYSQL *con, SERVER_REF *server, SERVICE *service, SERV_LISTENER *listener)
{

    if (server->server->server_string == NULL)
    {
        const char *server_string = mysql_get_server_info(con);
        if (!server_set_version_string(server->server, server_string))
        {
            return -1;
        }
    }

    /** Testing new users query */
    char *query = get_new_users_query(server->server->server_string, service->enable_root);
    MYSQL_AUTH *instance = (MYSQL_AUTH*)listener->auth_instance;
    bool anon_user = false;
    int users = 0;

    if (query)
    {
        if (mysql_query(con, query) == 0)
        {
            MYSQL_RES *result = mysql_store_result(con);

            if (result)
            {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(result)))
                {
                    if (service->strip_db_esc)
                    {
                        strip_escape_chars(row[2]);
                    }

                    add_mysql_user(instance->handle, row[0], row[1], row[2],
                                   row[3] && strcmp(row[3], "Y") == 0, row[4]);
                    users++;

                    if (row[0] && *row[0] == '\0')
                    {
                        /** Empty username is used for the anonymous user. This means
                         that localhost does not match wildcard host. */
                        anon_user = true;
                    }
                }

                mysql_free_result(result);
            }
        }
        else
        {
            MXS_ERROR("Failed to load users: %s", mysql_error(con));
        }

        MXS_FREE(query);
    }

    /** Set the parameter if it is not configured by the user */
    if (service->localhost_match_wildcard_host == SERVICE_PARAM_UNINIT)
    {
        service->localhost_match_wildcard_host = anon_user ? 0 : 1;
    }

    /** Load the list of databases */
    if (mysql_query(con, "SHOW DATABASES") == 0)
    {
        MYSQL_RES *result = mysql_store_result(con);
        if (result)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result)))
            {
                add_database(instance->handle, row[0]);
            }

            mysql_free_result(result);
        }
    }
    else
    {
        MXS_ERROR("Failed to load list of databases: %s", mysql_error(con));
    }

    return users;
}

/**
 * Load the user/passwd form mysql.user table into the service users' hashtable
 * environment.
 *
 * @param service   The current service
 * @param users     The users table into which to load the users
 * @return          -1 on any error or the number of users inserted
 */
static int get_users(SERV_LISTENER *listener, USERS *users)
{
    char *service_user = NULL;
    char *service_passwd = NULL;
    SERVICE *service = listener->service;

    if (serviceGetUser(service, &service_user, &service_passwd) == 0)
    {
        return -1;
    }

    char *dpwd = decrypt_password(service_passwd);

    if (dpwd == NULL)
    {
        return -1;
    }

    SERVER_REF *server = service->dbref;
    int total_users = -1;

    for (server = service->dbref; !service->svc_do_shutdown && server; server = server->next)
    {
        MYSQL *con = gw_mysql_init();
        if (con)
        {
            if (mxs_mysql_real_connect(con, server->server, service_user, dpwd) == NULL)
            {
                MXS_ERROR("Failure loading users data from backend "
                          "[%s:%i] for service [%s]. MySQL error %i, %s",
                          server->server->name, server->server->port,
                          service->name, mysql_errno(con), mysql_error(con));
                mysql_close(con);
            }
            else
            {
                /** Successfully connected to a server */
                int users = get_users_from_server(con, server, service, listener);

                if (users > total_users)
                {
                    total_users = users;
                }

                mysql_close(con);

                if (!service->users_from_all)
                {
                    break;
                }
            }
        }
    }

    MXS_FREE(dpwd);

    if (server == NULL)
    {
        MXS_ERROR("Unable to get user data from backend database for service [%s]."
                  " Failed to connect to any of the backend databases.", service->name);
    }

    return total_users;
}
