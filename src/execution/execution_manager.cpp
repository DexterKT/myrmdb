/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <iomanip>
#include <sstream>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  SHOW INDEX FROM table_name\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...] | aggregate(column) AS alias [, ...]}\n";

namespace {

std::string format_column_value(const ColMeta &col, const char *rec_buf) {
    if (col.type == TYPE_INT) {
        return std::to_string(*(int *)rec_buf);
    }
    if (col.type == TYPE_FLOAT) {
        return std::to_string(*(float *)rec_buf);
    }
    if (col.type == TYPE_BIGINT) {
        return std::to_string(*(int64_t *)rec_buf);
    }
    if (col.type == TYPE_DATETIME) {
        return format_datetime_value(*(int64_t *)rec_buf);
    }
    std::string col_str((char *)rec_buf, col.len);
    col_str.resize(strlen(col_str.c_str()));
    return col_str;
}

std::string format_float_fixed(double value) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(6) << value;
    return os.str();
}

struct AggregateState {
    AggregateCall call;
    ColMeta col;
    bool initialized = false;
    int64_t count = 0;
    int64_t int_sum = 0;
    double float_sum = 0;
    std::string best_raw;
};

std::vector<std::string> finalize_aggregates(const std::vector<AggregateState> &states) {
    std::vector<std::string> row;
    row.reserve(states.size());
    for (const auto &state : states) {
        switch (state.call.type) {
            case AGG_COUNT:
                row.push_back(std::to_string(state.count));
                break;
            case AGG_SUM:
                if (state.col.type == TYPE_FLOAT) {
                    row.push_back(format_float_fixed(state.float_sum));
                } else {
                    row.push_back(std::to_string(state.int_sum));
                }
                break;
            case AGG_MAX:
            case AGG_MIN:
                if (!state.initialized) {
                    row.emplace_back("");
                } else if (state.col.type == TYPE_FLOAT) {
                    row.push_back(format_float_fixed(*(float *)state.best_raw.data()));
                } else {
                    row.push_back(format_column_value(state.col, state.best_raw.data()));
                }
                break;
        }
    }
    return row;
}

}

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }  
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }     
            default:
                throw InternalError("Unexpected field type");
                break;                        
        }

    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols,
                            std::vector<AggregateCall> aggregates, Context *context) {
    if (!aggregates.empty()) {
        std::vector<std::string> captions;
        captions.reserve(aggregates.size());
        std::vector<AggregateState> states;
        states.reserve(aggregates.size());
        for (const auto &agg : aggregates) {
            captions.push_back(agg.alias);
            AggregateState state;
            state.call = agg;
            if (!agg.count_star) {
                state.col = executorTreeRoot->get_col_offset(agg.col);
            }
            states.push_back(std::move(state));
        }

        RecordPrinter rec_printer(captions.size());
        rec_printer.print_separator(context);
        rec_printer.print_record(captions, context);
        rec_printer.print_separator(context);

        std::fstream outfile;
        outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << "|";
        for (const auto &caption : captions) {
            outfile << " " << caption << " |";
        }
        outfile << "\n";

        for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
            auto tuple = executorTreeRoot->Next();
            for (auto &state : states) {
                if (state.call.type == AGG_COUNT) {
                    state.count++;
                    continue;
                }

                char *rec_buf = tuple->data + state.col.offset;
                if (state.call.type == AGG_SUM) {
                    if (state.col.type == TYPE_FLOAT) {
                        state.float_sum += *(float *)rec_buf;
                    } else if (state.col.type == TYPE_BIGINT) {
                        state.int_sum += *(int64_t *)rec_buf;
                    } else {
                        state.int_sum += *(int *)rec_buf;
                    }
                    continue;
                }

                if (!state.initialized) {
                    state.best_raw.assign(rec_buf, state.col.len);
                    state.initialized = true;
                    continue;
                }
                int cmp = compare_raw_value(rec_buf, state.best_raw.data(), state.col.type, state.col.len);
                if ((state.call.type == AGG_MAX && cmp > 0) || (state.call.type == AGG_MIN && cmp < 0)) {
                    state.best_raw.assign(rec_buf, state.col.len);
                }
            }
        }

        auto row = finalize_aggregates(states);
        rec_printer.print_record(row, context);
        outfile << "|";
        for (const auto &col : row) {
            outfile << " " << col << " |";
        }
        outfile << "\n";
        outfile.close();
        rec_printer.print_separator(context);
        RecordPrinter::print_record_count(1, context);
        return;
    }

    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.col_name);
    }

    // Print header into buffer
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for(int i = 0; i < captions.size(); ++i) {
        outfile << " " << captions[i] << " |";
    }
    outfile << "\n";

    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            char *rec_buf = Tuple->data + col.offset;
            if (col.type == TYPE_INT) {
                col_str = std::to_string(*(int *)rec_buf);
            } else if (col.type == TYPE_FLOAT) {
                col_str = std::to_string(*(float *)rec_buf);
            } else if (col.type == TYPE_BIGINT) {
                col_str = std::to_string(*(int64_t *)rec_buf);
            } else if (col.type == TYPE_DATETIME) {
                col_str = format_datetime_value(*(int64_t *)rec_buf);
            } else if (col.type == TYPE_STRING) {
                col_str = std::string((char *)rec_buf, col.len);
                col_str.resize(strlen(col_str.c_str()));
            }
            columns.push_back(col_str);
        }
        // print record into buffer
        rec_printer.print_record(columns, context);
        // print record into file
        outfile << "|";
        for(int i = 0; i < columns.size(); ++i) {
            outfile << " " << columns[i] << " |";
        }
        outfile << "\n";
        num_rec++;
    }
    outfile.close();
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
