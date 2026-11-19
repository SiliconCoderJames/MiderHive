#pragma once
// SQLite 封装：所有外部输入一律通过 sqlite3_bind_* 参数绑定，
// 禁止任何字符串拼接 / format 组装 SQL。
#include <sqlite3.h>

#include <functional>
#include <string>

namespace ah {

class Stmt {
public:
    explicit Stmt(sqlite3_stmt* s = nullptr) : s_(s) {}
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind(int idx, const std::string& v);
    void bind(int idx, int64_t v);
    void bind(int idx, double v);
    void bindBlob(int idx, const void* data, size_t n);
    // 显式绑定 NULL（如"广播消息"的 recipient：schema 约定 NULL = 广播给所有人）
    void bindNull(int idx) { sqlite3_bind_null(s_, idx); }
    int step();                    // SQLITE_ROW / SQLITE_DONE / 错误码
    void reset();
    std::string text(int col) const;
    int64_t i64(int col) const;
    double dbl(int col) const;
    bool isNull(int col) const;
    int lastError() const;

private:
    sqlite3_stmt* s_;
};

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开（或创建）数据库并启用 WAL。
    bool open(const std::string& path, std::string& err);
    void close();

    // 单条语句：bind 回调填充参数（参数绑定），row 回调处理每行结果。
    // const：SELECT 是逻辑只读（last_insert_id_ 仅 INSERT 路径写入，mutable）——
    // 接入开关的鉴权拒接检查发生在 const 鉴权路径上，读原语必须可达
    bool query(const std::string& sql, const std::function<void(Stmt&)>& bind,
               const std::function<void(Stmt&)>& row, std::string& err) const;
    // 执行脚本（建表语句；不含任何外部输入）。
    bool execScript(const std::string& sql, std::string& err);
    // 迁移用：执行语句，"duplicate column" 类错误静默吞掉并返回 true。
    bool tryExec(const std::string& sql);
    // 显式事务（互斥锁之外的多语句原子性，如用量扣减）
    bool beginImmediate(std::string& err);
    bool commit(std::string& err);
    bool rollback();

    int64_t lastInsertId() const { return last_insert_id_; }
    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
    mutable int64_t last_insert_id_ = 0;  // query(const) 内的 INSERT 副产物
};

}  // namespace ah
