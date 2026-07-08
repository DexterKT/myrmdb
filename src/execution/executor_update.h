/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        for (const auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);
            auto old_rec = std::make_unique<RmRecord>(rec->size);
            memcpy(old_rec->data, rec->data, rec->size);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (!set_clause.rhs.coerce_to(col->type)) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                }
                if (set_clause.rhs.raw == nullptr) {
                    set_clause.rhs.init_raw(col->len);
                }
                memcpy(rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            if (context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
                Rid log_rid = rid;
                UpdateLogRecord log_record(context_->txn_->get_transaction_id(), *old_rec, *rec, log_rid, tab_name_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
                context_->log_mgr_->flush_log_to_disk();
            }
            if (context_->txn_ != nullptr) {
                context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec));
            }
            sm_manager_->update_index_entries(tab_name_, old_rec.get(), rec.get(), rid);
            fh_->update_record(rid, rec->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
