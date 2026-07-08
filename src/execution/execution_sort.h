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
#include <algorithm>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> desc_flags_;
    size_t tuple_num;
    std::vector<size_t> used_tuple;
    std::unique_ptr<RmRecord> current_tuple;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    std::vector<ColMeta> out_cols_;
    size_t len_;
    size_t cursor_;
    int limit_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<SortKey> &sort_keys, int limit) {
        prev_ = std::move(prev);
        for (const auto &sort_key : sort_keys) {
            sort_cols_.push_back(prev_->get_col_offset(sort_key.col));
            desc_flags_.push_back(sort_key.desc);
        }
        out_cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        tuple_num = 0;
        cursor_ = 0;
        limit_ = limit;
        used_tuple.clear();
    }

    void beginTuple() override { 
        tuples_.clear();
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec != nullptr) {
                tuples_.push_back(std::move(rec));
            }
        }
        if (!sort_cols_.empty()) {
            std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
                for (size_t i = 0; i < sort_cols_.size(); ++i) {
                    const auto &col = sort_cols_[i];
                    int cmp = compare_raw_value(lhs->data + col.offset, rhs->data + col.offset, col.type, col.len);
                    if (cmp == 0) {
                        continue;
                    }
                    return desc_flags_[i] ? cmp > 0 : cmp < 0;
                }
                return false;
            });
        }
        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(limit_);
        }
        tuple_num = tuples_.size();
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return out_cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(out_cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
