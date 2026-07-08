/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "execution/execution_defs.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record/rm_scan.h"
#include "record_printer.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw FileNotFoundError(DB_META_NAME);
    }
    db_ = DbMeta();
    ifs >> db_;
    ifs.close();

    fhs_.clear();
    ihs_.clear();
    for (auto &entry : db_.tabs_) {
        fhs_.emplace(entry.first, rm_manager_->open_file(entry.first));
    }
    rebuild_runtime_indexes(nullptr);
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    flush_meta();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    ihs_.clear();
    runtime_indexes_.clear();
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    auto fh = fhs_.find(tab_name);
    if (fh != fhs_.end()) {
        rm_manager_->close_file(fh->second.get());
        fhs_.erase(fh);
    }
    rm_manager_->destroy_file(tab_name);
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    IndexMeta index;
    index.tab_name = tab_name;
    index.col_tot_len = 0;
    index.col_num = static_cast<int>(col_names.size());
    for (const auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        col->index = true;
        index.cols.push_back(*col);
        index.col_tot_len += col->len;
    }

    RuntimeIndex runtime_index;
    runtime_index.meta = index;
    RmFileHandle *fh = fhs_.at(tab_name).get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        auto rec = fh->get_record(rid, context);
        runtime_index.entries.push_back({make_index_key(index, rec.get()), rid});
    }
    sort_runtime_index(runtime_index);
    for (size_t i = 1; i < runtime_index.entries.size(); ++i) {
        if (key_equal(index, runtime_index.entries[i - 1].key, runtime_index.entries[i].key)) {
            throw InternalError("Duplicate value violates unique index");
        }
    }

    tab.indexes.push_back(index);
    runtime_indexes_[runtime_index_name(index)] = std::move(runtime_index);
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    auto index = tab.get_index_meta(col_names);
    tab.indexes.erase(index);
    runtime_indexes_.erase(runtime_index_name(tab_name, col_names));

    for (auto &col : tab.cols) {
        bool still_indexed = false;
        for (const auto &idx : tab.indexes) {
            for (const auto &idx_col : idx.cols) {
                if (idx_col.name == col.name) {
                    still_indexed = true;
                    break;
                }
            }
            if (still_indexed) {
                break;
            }
        }
        col.index = still_indexed;
    }
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    for (const auto &index : tab.indexes) {
        std::ostringstream cols;
        cols << "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i > 0) {
                cols << ",";
            }
            cols << index.cols[i].name;
        }
        cols << ")";
        std::string line = "| " + tab_name + " | unique | " + cols.str() + " |\n";
        outfile << line;
        if (context != nullptr && context->data_send_ != nullptr && context->offset_ != nullptr) {
            memcpy(context->data_send_ + *(context->offset_), line.c_str(), line.size());
            *(context->offset_) += line.size();
        }
    }
    outfile.close();
}

std::vector<Rid> SmManager::scan_index(const std::string& tab_name, const std::vector<std::string>& col_names,
                                       const std::vector<Condition>& conds, Context* context) {
    const RuntimeIndex &runtime_index = get_runtime_index(tab_name, col_names);
    const IndexMeta &index = runtime_index.meta;

    std::string eq_prefix;
    int eq_cols = 0;
    std::string lower_key;
    std::string upper_key;
    int lower_cols = 0;
    int upper_cols = 0;
    bool lower_inclusive = true;
    bool upper_inclusive = true;

    auto better_lower = [&](const std::string &lhs, bool lhs_inclusive, const std::string &rhs,
                            bool rhs_inclusive, int cols) {
        int cmp = compare_index_key(index, lhs, rhs, cols);
        return cmp > 0 || (cmp == 0 && !lhs_inclusive && rhs_inclusive);
    };
    auto better_upper = [&](const std::string &lhs, bool lhs_inclusive, const std::string &rhs,
                            bool rhs_inclusive, int cols) {
        int cmp = compare_index_key(index, lhs, rhs, cols);
        return cmp < 0 || (cmp == 0 && !lhs_inclusive && rhs_inclusive);
    };

    for (int i = 0; i < index.col_num; ++i) {
        const auto &idx_col = index.cols[i];
        const Condition *eq_cond = nullptr;
        for (const auto &cond : conds) {
            if (cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name == tab_name &&
                cond.lhs_col.col_name == idx_col.name) {
                eq_cond = &cond;
                break;
            }
        }
        if (eq_cond != nullptr) {
            eq_prefix.append(eq_cond->rhs_val.raw->data, idx_col.len);
            eq_cols++;
            continue;
        }

        for (const auto &cond : conds) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name || cond.lhs_col.col_name != idx_col.name) {
                continue;
            }
            if (cond.op != OP_LT && cond.op != OP_LE && cond.op != OP_GT && cond.op != OP_GE) {
                continue;
            }
            std::string bound = eq_prefix;
            bound.append(cond.rhs_val.raw->data, idx_col.len);
            int bound_cols = i + 1;
            if (cond.op == OP_GT || cond.op == OP_GE) {
                bool inclusive = cond.op == OP_GE;
                if (lower_cols == 0 || better_lower(bound, inclusive, lower_key, lower_inclusive, bound_cols)) {
                    lower_key = std::move(bound);
                    lower_cols = bound_cols;
                    lower_inclusive = inclusive;
                }
            } else {
                bool inclusive = cond.op == OP_LE;
                if (upper_cols == 0 || better_upper(bound, inclusive, upper_key, upper_inclusive, bound_cols)) {
                    upper_key = std::move(bound);
                    upper_cols = bound_cols;
                    upper_inclusive = inclusive;
                }
            }
        }
        break;
    }

    if (eq_cols > 0 && lower_cols == 0) {
        lower_key = eq_prefix;
        lower_cols = eq_cols;
        lower_inclusive = true;
    }
    if (eq_cols > 0 && upper_cols == 0) {
        upper_key = eq_prefix;
        upper_cols = eq_cols;
        upper_inclusive = true;
    }

    auto begin = runtime_index.entries.begin();
    auto end = runtime_index.entries.end();
    if (lower_cols > 0) {
        if (lower_inclusive) {
            begin = std::lower_bound(runtime_index.entries.begin(), runtime_index.entries.end(), lower_key,
                                     [&](const RuntimeIndexEntry &entry, const std::string &key) {
                                         return compare_index_key(index, entry.key, key, lower_cols) < 0;
                                     });
        } else {
            begin = std::upper_bound(runtime_index.entries.begin(), runtime_index.entries.end(), lower_key,
                                     [&](const std::string &key, const RuntimeIndexEntry &entry) {
                                         return compare_index_key(index, key, entry.key, lower_cols) < 0;
                                     });
        }
    }
    if (upper_cols > 0) {
        if (upper_inclusive) {
            end = std::upper_bound(runtime_index.entries.begin(), runtime_index.entries.end(), upper_key,
                                   [&](const std::string &key, const RuntimeIndexEntry &entry) {
                                       return compare_index_key(index, key, entry.key, upper_cols) < 0;
                                   });
        } else {
            end = std::lower_bound(runtime_index.entries.begin(), runtime_index.entries.end(), upper_key,
                                   [&](const RuntimeIndexEntry &entry, const std::string &key) {
                                       return compare_index_key(index, entry.key, key, upper_cols) < 0;
                                   });
        }
    }

    std::vector<Rid> rids;
    for (auto it = begin; it < end; ++it) {
        rids.push_back(it->rid);
    }
    return rids;
}

void SmManager::check_index_insert(const std::string& tab_name, const RmRecord* rec, const Rid* self) {
    TabMeta &tab = db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        const RuntimeIndex &runtime_index = get_runtime_index(tab_name, [&]() {
            std::vector<std::string> names;
            for (const auto &col : index.cols) names.push_back(col.name);
            return names;
        }());
        std::string key = make_index_key(index, rec);
        auto it = std::lower_bound(runtime_index.entries.begin(), runtime_index.entries.end(), key,
                                   [&](const RuntimeIndexEntry &entry, const std::string &target) {
                                       return compare_index_key(index, entry.key, target) < 0;
                                   });
        if (it != runtime_index.entries.end() && key_equal(index, it->key, key) &&
            (self == nullptr || it->rid != *self)) {
            throw InternalError("Duplicate value violates unique index");
        }
    }
}

void SmManager::insert_index_entries(const std::string& tab_name, const RmRecord* rec, const Rid& rid) {
    TabMeta &tab = db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        RuntimeIndex &runtime_index = runtime_indexes_.at(runtime_index_name(index));
        RuntimeIndexEntry entry{make_index_key(index, rec), rid};
        auto it = std::lower_bound(runtime_index.entries.begin(), runtime_index.entries.end(), entry.key,
                                   [&](const RuntimeIndexEntry &lhs, const std::string &rhs) {
                                       return compare_index_key(index, lhs.key, rhs) < 0;
                                   });
        runtime_index.entries.insert(it, std::move(entry));
    }
}

void SmManager::delete_index_entries(const std::string& tab_name, const RmRecord* rec, const Rid& rid) {
    TabMeta &tab = db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        RuntimeIndex &runtime_index = runtime_indexes_.at(runtime_index_name(index));
        std::string key = make_index_key(index, rec);
        auto it = std::lower_bound(runtime_index.entries.begin(), runtime_index.entries.end(), key,
                                   [&](const RuntimeIndexEntry &entry, const std::string &target) {
                                       return compare_index_key(index, entry.key, target) < 0;
                                   });
        while (it != runtime_index.entries.end() && key_equal(index, it->key, key)) {
            if (it->rid == rid) {
                runtime_index.entries.erase(it);
                break;
            }
            ++it;
        }
    }
}

void SmManager::update_index_entries(const std::string& tab_name, const RmRecord* old_rec, const RmRecord* new_rec,
                                     const Rid& rid) {
    check_index_insert(tab_name, new_rec, &rid);
    delete_index_entries(tab_name, old_rec, rid);
    insert_index_entries(tab_name, new_rec, rid);
}

std::string SmManager::runtime_index_name(const std::string& tab_name, const std::vector<std::string>& col_names) const {
    std::string name = tab_name;
    for (const auto &col_name : col_names) {
        name += "#";
        name += col_name;
    }
    return name;
}

std::string SmManager::runtime_index_name(const IndexMeta& index) const {
    std::vector<std::string> col_names;
    col_names.reserve(index.cols.size());
    for (const auto &col : index.cols) {
        col_names.push_back(col.name);
    }
    return runtime_index_name(index.tab_name, col_names);
}

std::string SmManager::make_index_key(const IndexMeta& index, const RmRecord* rec) const {
    std::string key;
    key.reserve(index.col_tot_len);
    for (const auto &col : index.cols) {
        key.append(rec->data + col.offset, col.len);
    }
    return key;
}

int SmManager::compare_index_key(const IndexMeta& index, const std::string& lhs, const std::string& rhs,
                                 int col_num) const {
    if (col_num < 0) {
        col_num = index.col_num;
    }
    int offset = 0;
    for (int i = 0; i < col_num; ++i) {
        const auto &col = index.cols[i];
        int cmp = compare_raw_value(lhs.data() + offset, rhs.data() + offset, col.type, col.len);
        if (cmp != 0) {
            return cmp;
        }
        offset += col.len;
    }
    return 0;
}

bool SmManager::key_equal(const IndexMeta& index, const std::string& lhs, const std::string& rhs) const {
    return compare_index_key(index, lhs, rhs) == 0;
}

void SmManager::sort_runtime_index(RuntimeIndex& runtime_index) {
    const IndexMeta &index = runtime_index.meta;
    std::sort(runtime_index.entries.begin(), runtime_index.entries.end(),
              [&](const RuntimeIndexEntry &lhs, const RuntimeIndexEntry &rhs) {
                  int cmp = compare_index_key(index, lhs.key, rhs.key);
                  if (cmp != 0) {
                      return cmp < 0;
                  }
                  return std::make_pair(lhs.rid.page_no, lhs.rid.slot_no) <
                         std::make_pair(rhs.rid.page_no, rhs.rid.slot_no);
              });
}

void SmManager::rebuild_runtime_index(const IndexMeta& index, Context* context) {
    RuntimeIndex runtime_index;
    runtime_index.meta = index;
    RmFileHandle *fh = fhs_.at(index.tab_name).get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        auto rec = fh->get_record(rid, context);
        runtime_index.entries.push_back({make_index_key(index, rec.get()), rid});
    }
    sort_runtime_index(runtime_index);
    runtime_indexes_[runtime_index_name(index)] = std::move(runtime_index);
}

void SmManager::rebuild_runtime_indexes(Context* context) {
    runtime_indexes_.clear();
    for (const auto &tab_entry : db_.tabs_) {
        for (const auto &index : tab_entry.second.indexes) {
            rebuild_runtime_index(index, context);
        }
    }
}

void SmManager::flush_all_tables() {
    for (auto &entry : fhs_) {
        entry.second->flush();
    }
}

SmManager::RuntimeIndex& SmManager::get_runtime_index(const std::string& tab_name,
                                                      const std::vector<std::string>& col_names) {
    auto pos = runtime_indexes_.find(runtime_index_name(tab_name, col_names));
    if (pos == runtime_indexes_.end()) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    return pos->second;
}

const SmManager::RuntimeIndex& SmManager::get_runtime_index(const std::string& tab_name,
                                                            const std::vector<std::string>& col_names) const {
    auto pos = runtime_indexes_.find(runtime_index_name(tab_name, col_names));
    if (pos == runtime_indexes_.end()) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    return pos->second;
}
