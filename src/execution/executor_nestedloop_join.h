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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t JOIN_BUFFER_BYTES = 4 * 1024 * 1024;

    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    std::unique_ptr<RmRecord> right_tuple_;
    std::unique_ptr<RmRecord> current_tuple_;
    size_t block_cursor_;

    std::unique_ptr<RmRecord> join_tuple(const RmRecord *left_rec, const RmRecord *right_rec) {
        if (left_rec == nullptr || right_rec == nullptr) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    bool load_left_block() {
        left_block_.clear();
        size_t used_bytes = 0;
        while (!left_->is_end()) {
            auto rec = left_->Next();
            if (rec == nullptr) {
                break;
            }
            used_bytes += rec->size;
            left_block_.push_back(std::move(rec));
            left_->nextTuple();
            if (used_bytes >= JOIN_BUFFER_BYTES) {
                break;
            }
        }
        block_cursor_ = 0;
        return !left_block_.empty();
    }

    void advance_to_match() {
        current_tuple_.reset();
        while (true) {
            if (left_block_.empty()) {
                if (!load_left_block()) {
                    isend = true;
                    return;
                }
                right_->beginTuple();
                right_tuple_.reset();
            }
            while (!right_->is_end()) {
                if (right_tuple_ == nullptr) {
                    right_tuple_ = right_->Next();
                }
                while (block_cursor_ < left_block_.size()) {
                    auto rec = join_tuple(left_block_[block_cursor_].get(), right_tuple_.get());
                    block_cursor_++;
                    if (rec != nullptr && eval_conditions(cols_, fed_conds_, rec.get())) {
                        current_tuple_ = std::move(rec);
                        isend = false;
                        return;
                    }
                }
                right_->nextTuple();
                right_tuple_.reset();
                block_cursor_ = 0;
            }
            left_block_.clear();
        }
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        block_cursor_ = 0;

    }

    void beginTuple() override {
        left_block_.clear();
        right_tuple_.reset();
        current_tuple_.reset();
        block_cursor_ = 0;
        left_->beginTuple();
        advance_to_match();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        advance_to_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_tuple_);
    }

    bool is_end() const override { return isend; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
