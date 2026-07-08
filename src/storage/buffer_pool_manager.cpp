/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    PageId old_page_id = page->id_;
    if (old_page_id.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->data_, PAGE_SIZE);
        }
        page_table_.erase(old_page_id);
    }

    page->reset_memory();
    page->id_ = new_page_id;
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    if (new_page_id.page_no != INVALID_PAGE_ID) {
        page_table_[new_page_id] = new_frame_id;
    }
}

/**
 * @description: 从buffer pool获取需要的页。
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Page *page = &pages_[it->second];
        page->pin_count_++;
        replacer_->pin(it->second);
        return page;
    }

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    Page *page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);
    return page;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ <= 0) {
        return false;
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second);
    }
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
    Page *page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);
    return page;
}

/**
 * @description: 从buffer_pool删除目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        disk_manager_->deallocate_page(page_id.page_no);
        return true;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ != 0) {
        return false;
    }
    replacer_->pin(it->second);
    page_table_.erase(it);
    page->reset_memory();
    page->id_ = PageId{page_id.fd, INVALID_PAGE_ID};
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    free_list_.push_back(it->second);
    disk_manager_->deallocate_page(page_id.page_no);
    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (auto &entry : page_table_) {
        const PageId &page_id = entry.first;
        if (page_id.fd != fd) {
            continue;
        }
        Page *page = &pages_[entry.second];
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}
