#include "dbms.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    dbms* db = NULL;
    dbms_stmt* stmt = NULL;
    int rc;

    printf("==================================================================\n");
    printf("     NATIVE C CRUD DEMONSTRATION USING OUR CUSTOM ENGINE         \n");
    printf("==================================================================\n");

    // 1. Open Connection
    rc = dbms_open("my_app.db", &db);
    if (rc != DBMS_OK) {
        fprintf(stderr, "Failed to open database: %s\n", dbms_errmsg(db));
        return 1;
    }
    printf("✓ Opened database 'my_app.db'\n");

    // 2. CREATE TABLE
    dbms_exec(db, "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);", NULL, NULL, NULL);
    printf("✓ Created table 'users'\n");

    // 3. CREATE (INSERT Rows)
    dbms_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    rc = dbms_prepare_v2(db, "INSERT INTO users VALUES (?, ?, ?);", -1, &stmt, NULL);
    if (rc == DBMS_OK) {
        // Insert User 1
        dbms_bind_int(stmt, 1, 101);
        dbms_bind_text(stmt, 2, "Alice", -1);
        dbms_bind_int(stmt, 3, 28);
        dbms_step(stmt);
        dbms_reset(stmt);

        // Insert User 2
        dbms_bind_int(stmt, 1, 102);
        dbms_bind_text(stmt, 2, "Bob", -1);
        dbms_bind_int(stmt, 3, 34);
        dbms_step(stmt);
        dbms_reset(stmt);

        dbms_finalize(stmt);
    }
    dbms_exec(db, "COMMIT;", NULL, NULL, NULL);
    printf("✓ Inserted 2 rows (Alice, Bob)\n");

    // 4. READ (SELECT Rows)
    printf("\n--- Querying Table Contents (READ) ---\n");
    rc = dbms_prepare_v2(db, "SELECT * FROM users;", -1, &stmt, NULL);
    if (rc == DBMS_OK) {
        while (dbms_step(stmt) == DBMS_ROW) {
            int id = dbms_column_int(stmt, 0);
            const char* name = dbms_column_text(stmt, 1);
            int age = dbms_column_int(stmt, 2);
            printf("  Row -> ID: %d | Name: %-10s | Age: %d\n", id, name, age);
        }
        dbms_finalize(stmt);
    }

    // 5. UPDATE
    dbms_exec(db, "UPDATE users SET age = 29 WHERE id = 101;", NULL, NULL, NULL);
    printf("\n✓ Updated Alice's age to 29\n");

    // 6. DELETE
    dbms_exec(db, "DELETE FROM users WHERE id = 102;", NULL, NULL, NULL);
    printf("✓ Deleted Bob from users table\n");

    // 7. READ AGAIN to verify UPDATE & DELETE
    printf("\n--- Querying Table Contents After UPDATE & DELETE ---\n");
    rc = dbms_prepare_v2(db, "SELECT * FROM users;", -1, &stmt, NULL);
    if (rc == DBMS_OK) {
        while (dbms_step(stmt) == DBMS_ROW) {
            int id = dbms_column_int(stmt, 0);
            const char* name = dbms_column_text(stmt, 1);
            int age = dbms_column_int(stmt, 2);
            printf("  Row -> ID: %d | Name: %-10s | Age: %d\n", id, name, age);
        }
        dbms_finalize(stmt);
    }

    // 8. Close Connection
    dbms_close(db);
    printf("\n✓ Closed database connection successfully!\n");
    printf("==================================================================\n");
    return 0;
}
