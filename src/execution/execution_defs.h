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
#include <cstring>
#include <vector>

#include "common/common.h"
#include "defs.h"
#include "errors.h"
#include "system/sm_meta.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int lhs_val = *reinterpret_cast<const int *>(lhs);
        int rhs_val = *reinterpret_cast<const int *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    if (type == TYPE_FLOAT) {
        float lhs_val = *reinterpret_cast<const float *>(lhs);
        float rhs_val = *reinterpret_cast<const float *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    if (type == TYPE_BIGINT) {
        int64_t lhs_val = *reinterpret_cast<const int64_t *>(lhs);
        int64_t rhs_val = *reinterpret_cast<const int64_t *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    return std::strncmp(lhs, rhs, len);
}

inline bool eval_compare(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

inline std::vector<ColMeta>::const_iterator find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
    auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (pos == cols.end()) {
        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }
    return pos;
}

inline bool eval_condition(const std::vector<ColMeta> &cols, const Condition &cond, const RmRecord *rec) {
    auto lhs_col = find_col_meta(cols, cond.lhs_col);
    const char *lhs = rec->data + lhs_col->offset;
    const char *rhs = nullptr;

    if (cond.is_rhs_val) {
        if (cond.rhs_val.raw == nullptr) {
            throw InternalError("Condition RHS value is not initialized");
        }
        rhs = cond.rhs_val.raw->data;
    } else {
        auto rhs_col = find_col_meta(cols, cond.rhs_col);
        rhs = rec->data + rhs_col->offset;
    }

    int cmp = compare_raw_value(lhs, rhs, lhs_col->type, lhs_col->len);
    return eval_compare(cmp, cond.op);
}

inline bool eval_conditions(const std::vector<ColMeta> &cols, const std::vector<Condition> &conds,
                            const RmRecord *rec) {
    for (const auto &cond : conds) {
        if (!eval_condition(cols, cond, rec)) {
            return false;
        }
    }
    return true;
}
