/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <algorithm>

bool LockManager::compatible(LockMode requested, LockMode granted) {
    if (requested == LockMode::INTENTION_SHARED) {
        return granted != LockMode::EXLUCSIVE;
    }
    if (requested == LockMode::INTENTION_EXCLUSIVE) {
        return granted == LockMode::INTENTION_SHARED || granted == LockMode::INTENTION_EXCLUSIVE;
    }
    if (requested == LockMode::SHARED) {
        return granted == LockMode::INTENTION_SHARED || granted == LockMode::SHARED;
    }
    return false;
}

bool LockManager::is_stronger_or_equal(LockMode held, LockMode requested) {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held == LockMode::S_IX) {
        return requested != LockMode::EXLUCSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

LockManager::GroupLockMode LockManager::compute_group_mode(const LockRequestQueue &queue) {
    bool has_is = false, has_ix = false, has_s = false, has_x = false, has_six = false;
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        has_is |= request.lock_mode_ == LockMode::INTENTION_SHARED;
        has_ix |= request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE;
        has_s |= request.lock_mode_ == LockMode::SHARED;
        has_x |= request.lock_mode_ == LockMode::EXLUCSIVE;
        has_six |= request.lock_mode_ == LockMode::S_IX;
    }
    if (has_x) return GroupLockMode::X;
    if (has_six || (has_s && has_ix)) return GroupLockMode::SIX;
    if (has_s) return GroupLockMode::S;
    if (has_ix) return GroupLockMode::IX;
    if (has_is) return GroupLockMode::IS;
    return GroupLockMode::NON_LOCK;
}

bool LockManager::lock(Transaction* txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    std::scoped_lock lock_guard{latch_};
    auto &queue = lock_table_[lock_data_id];
    auto self = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(), [&](const LockRequest &request) {
        return request.txn_id_ == txn->get_transaction_id() && request.granted_;
    });
    if (self != queue.request_queue_.end() && is_stronger_or_equal(self->lock_mode_, lock_mode)) {
        txn->get_lock_set()->insert(lock_data_id);
        return true;
    }

    for (const auto &request : queue.request_queue_) {
        if (!request.granted_ || request.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (!compatible(lock_mode, request.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    if (self != queue.request_queue_.end()) {
        self->lock_mode_ = lock_mode;
    } else {
        queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
        queue.request_queue_.back().granted_ = true;
    }
    queue.group_lock_mode_ = compute_group_mode(queue);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }
    std::scoped_lock lock_guard{latch_};
    auto queue_it = lock_table_.find(lock_data_id);
    if (queue_it == lock_table_.end()) {
        return false;
    }
    auto &queue = queue_it->second;
    auto request_it = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(), [&](const LockRequest &request) {
        return request.txn_id_ == txn->get_transaction_id();
    });
    if (request_it == queue.request_queue_.end()) {
        return false;
    }
    queue.request_queue_.erase(request_it);
    queue.group_lock_mode_ = compute_group_mode(queue);
    if (queue.request_queue_.empty()) {
        lock_table_.erase(queue_it);
    }
    txn->get_lock_set()->erase(lock_data_id);
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }
    return true;
}
