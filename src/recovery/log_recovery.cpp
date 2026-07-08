/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <vector>

#include "errors.h"

namespace {

std::string logged_table_name(const char *table_name, size_t table_name_size) {
    return std::string(table_name, table_name_size);
}

bool record_exists(RmFileHandle *fh, const Rid &rid) {
    try {
        return fh->is_record(rid);
    } catch (RMDBError &) {
        return false;
    }
}

std::unique_ptr<LogRecord> build_log_record(const char *data) {
    auto type = *reinterpret_cast<const LogType *>(data + OFFSET_LOG_TYPE);
    std::unique_ptr<LogRecord> record;
    switch (type) {
        case LogType::UPDATE:
            record = std::make_unique<UpdateLogRecord>();
            break;
        case LogType::INSERT:
            record = std::make_unique<InsertLogRecord>();
            break;
        case LogType::DELETE:
            record = std::make_unique<DeleteLogRecord>();
            break;
        case LogType::begin:
            record = std::make_unique<BeginLogRecord>();
            break;
        case LogType::commit:
            record = std::make_unique<CommitLogRecord>();
            break;
        case LogType::ABORT:
            record = std::make_unique<AbortLogRecord>();
            break;
        default:
            return nullptr;
    }
    record->deserialize(data);
    return record;
}

}

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    logs_.clear();
    committed_txns_.clear();
    active_txns_.clear();

    int offset = 0;
    while (true) {
        char header[LOG_HEADER_SIZE];
        int header_size = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (header_size <= 0) {
            break;
        }
        if (header_size < LOG_HEADER_SIZE) {
            break;
        }
        uint32_t log_len = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (log_len < LOG_HEADER_SIZE || log_len > LOG_BUFFER_SIZE) {
            break;
        }

        std::vector<char> log_data(log_len);
        int read_size = disk_manager_->read_log(log_data.data(), log_len, offset);
        if (read_size != static_cast<int>(log_len)) {
            break;
        }
        auto record = build_log_record(log_data.data());
        if (record == nullptr) {
            break;
        }

        txn_id_t tid = record->log_tid_;
        if (record->log_type_ == LogType::begin) {
            active_txns_.insert(tid);
        } else if (record->log_type_ == LogType::commit) {
            committed_txns_.insert(tid);
            active_txns_.erase(tid);
        } else if (record->log_type_ == LogType::ABORT) {
            active_txns_.insert(tid);
        } else {
            active_txns_.insert(tid);
        }

        logs_.push_back(std::move(record));
        offset += log_len;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for (const auto &log : logs_) {
        if (committed_txns_.find(log->log_tid_) == committed_txns_.end()) {
            continue;
        }

        if (log->log_type_ == LogType::INSERT) {
            auto *insert_log = dynamic_cast<InsertLogRecord *>(log.get());
            auto table_name = logged_table_name(insert_log->table_name_, insert_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, insert_log->rid_)) {
                fh->update_record(insert_log->rid_, insert_log->insert_value_.data, nullptr);
            } else {
                fh->insert_record(insert_log->rid_, insert_log->insert_value_.data);
            }
        } else if (log->log_type_ == LogType::DELETE) {
            auto *delete_log = dynamic_cast<DeleteLogRecord *>(log.get());
            auto table_name = logged_table_name(delete_log->table_name_, delete_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, delete_log->rid_)) {
                fh->delete_record(delete_log->rid_, nullptr);
            }
        } else if (log->log_type_ == LogType::UPDATE) {
            auto *update_log = dynamic_cast<UpdateLogRecord *>(log.get());
            auto table_name = logged_table_name(update_log->table_name_, update_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, update_log->rid_)) {
                fh->update_record(update_log->rid_, update_log->new_value_.data, nullptr);
            } else {
                fh->insert_record(update_log->rid_, update_log->new_value_.data);
            }
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        LogRecord *log = it->get();
        if (active_txns_.find(log->log_tid_) == active_txns_.end()) {
            continue;
        }

        if (log->log_type_ == LogType::INSERT) {
            auto *insert_log = dynamic_cast<InsertLogRecord *>(log);
            auto table_name = logged_table_name(insert_log->table_name_, insert_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, insert_log->rid_)) {
                fh->delete_record(insert_log->rid_, nullptr);
            }
        } else if (log->log_type_ == LogType::DELETE) {
            auto *delete_log = dynamic_cast<DeleteLogRecord *>(log);
            auto table_name = logged_table_name(delete_log->table_name_, delete_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, delete_log->rid_)) {
                fh->update_record(delete_log->rid_, delete_log->delete_value_.data, nullptr);
            } else {
                fh->insert_record(delete_log->rid_, delete_log->delete_value_.data);
            }
        } else if (log->log_type_ == LogType::UPDATE) {
            auto *update_log = dynamic_cast<UpdateLogRecord *>(log);
            auto table_name = logged_table_name(update_log->table_name_, update_log->table_name_size_);
            auto *fh = sm_manager_->fhs_.at(table_name).get();
            if (record_exists(fh, update_log->rid_)) {
                fh->update_record(update_log->rid_, update_log->old_value_.data, nullptr);
            } else {
                fh->insert_record(update_log->rid_, update_log->old_value_.data);
            }
        }
    }

    sm_manager_->rebuild_runtime_indexes(nullptr);
    sm_manager_->flush_all_tables();
}
