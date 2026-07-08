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

#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "errors.h"
#include "defs.h"
#include "record/rm_defs.h"

inline bool is_leap_year(int year) {
    return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

inline int days_in_month(int year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month];
}

inline int parse_fixed_int(const std::string &text, int pos, int len) {
    int value = 0;
    for (int i = 0; i < len; ++i) {
        char ch = text[pos + i];
        if (ch < '0' || ch > '9') {
            throw InternalError("Invalid DATETIME value");
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

inline int64_t parse_datetime_literal(const std::string &text) {
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':') {
        throw InternalError("Invalid DATETIME value");
    }

    int year = parse_fixed_int(text, 0, 4);
    int month = parse_fixed_int(text, 5, 2);
    int day = parse_fixed_int(text, 8, 2);
    int hour = parse_fixed_int(text, 11, 2);
    int minute = parse_fixed_int(text, 14, 2);
    int second = parse_fixed_int(text, 17, 2);

    if (year < 1000 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        throw InternalError("Invalid DATETIME value");
    }

    return static_cast<int64_t>(year) * 10000000000LL +
           static_cast<int64_t>(month) * 100000000LL +
           static_cast<int64_t>(day) * 1000000LL +
           static_cast<int64_t>(hour) * 10000LL +
           static_cast<int64_t>(minute) * 100LL +
           second;
}

inline std::string format_datetime_value(int64_t value) {
    int second = value % 100;
    value /= 100;
    int minute = value % 100;
    value /= 100;
    int hour = value % 100;
    value /= 100;
    int day = value % 100;
    value /= 100;
    int month = value % 100;
    value /= 100;
    int year = static_cast<int>(value);

    std::ostringstream os;
    os.fill('0');
    os.width(4);
    os << year << '-';
    os.width(2);
    os << month << '-';
    os.width(2);
    os << day << ' ';
    os.width(2);
    os << hour << ':';
    os.width(2);
    os << minute << ':';
    os.width(2);
    os << second;
    return os.str();
}


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

enum AggType { AGG_COUNT, AGG_SUM, AGG_MAX, AGG_MIN };

struct AggregateCall {
    AggType type;
    TabCol col;
    std::string alias;
    bool count_star;
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
        int64_t bigint_val;  // bigint value
        int64_t datetime_val;  // datetime encoded as YYYYMMDDHHMMSS
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        datetime_val = datetime_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    bool coerce_to(ColType target_type) {
        if (type == target_type) {
            return true;
        }
        if (target_type == TYPE_FLOAT && type == TYPE_INT) {
            set_float(static_cast<float>(int_val));
            return true;
        }
        if (target_type == TYPE_BIGINT && type == TYPE_INT) {
            set_bigint(static_cast<int64_t>(int_val));
            return true;
        }
        if (target_type == TYPE_INT && type == TYPE_BIGINT &&
            bigint_val >= std::numeric_limits<int>::min() &&
            bigint_val <= std::numeric_limits<int>::max()) {
            set_int(static_cast<int>(bigint_val));
            return true;
        }
        if (target_type == TYPE_DATETIME && type == TYPE_STRING) {
            set_datetime(parse_datetime_literal(str_val));
            return true;
        }
        return false;
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = datetime_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};
