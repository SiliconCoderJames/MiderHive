#include "core/db/database.h"

#include <cstring>

namespace ah {

void Stmt::bind(int idx, const std::string& v) {
    sqlite3_bind_text(s_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}
void Stmt::bind(int idx, int64_t v) { sqlite3_bind_int64(s_, idx, v); }
void Stmt::bind(int idx, double v) { sqlite3_bind_double(s_, idx, v); }
void Stmt::bindBlob(int idx, const void* data, size_t n) {
    sqlite3_bind_blob(s_, idx, data, static_cast<int>(n), SQLITE_TRANSIENT);
}
int Stmt::step() { return sqlite3_step(s_); }
void Stmt::reset() { sqlite3_reset(s_); sqlite3_clear_bindings(s_); }
std::string Stmt::text(int col) const {
    const unsigned char* t = sqlite3_column_text(s_, col);
    int n = sqlite3_column_bytes(s_, col);
    return t ? std::string(reinterpret_cast<const char*>(t), n) : std::string();
}
int64_t Stmt::i64(int col) const { return sqlite3_column_int64(s_, col); }
double Stmt::dbl(int col) const { return sqlite3_column_double(s_, col); }
bool Stmt::isNull(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
int Stmt::lastError() const { return sqlite3_errcode(sqlite3_db_handle(s_)); }

Database::~Database() { close(); }

bool Database::open(const std::string& path, std::string& err) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    return true;
}

void Database::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool Database::query(const std::string& sql, const std::function<void(Stmt&)>& bind,
                     const std::function<void(Stmt&)>& row, std::string& err) const {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    Stmt st(raw);
    if (bind) bind(st);
    int rc;
    while ((rc = st.step()) == SQLITE_ROW) {
        if (row) row(st);
    }
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    last_insert_id_ = sqlite3_last_insert_rowid(db_);
    return true;
}

bool Database::execScript(const std::string& sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "exec failed";
        sqlite3_free(msg);
        return false;
    }
    last_insert_id_ = sqlite3_last_insert_rowid(db_);
    return true;
}

bool Database::tryExec(const std::string& sql) {
    char* msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
    bool dup = rc != SQLITE_OK && msg && std::strstr(msg, "duplicate column") != nullptr;
    sqlite3_free(msg);
    return rc == SQLITE_OK || dup;
}

bool Database::beginImmediate(std::string& err) {
    return execScript("BEGIN IMMEDIATE;", err);
}
bool Database::commit(std::string& err) { return execScript("COMMIT;", err); }
bool Database::rollback() {
    char* msg = nullptr;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &msg);
    sqlite3_free(msg);
    return true;
}

}  // namespace ah
