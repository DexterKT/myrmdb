/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages ||
        rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    auto rec = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return rec;
}

Rid RmFileHandle::insert_record(char* buf, Context* context) {
    RmPageHandle page_handle = create_page_handle();
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("RmFileHandle::insert_record no free slot");
    }

    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }

    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.slot_no < 0 ||
        rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    while (rid.page_no >= file_hdr_.num_pages) {
        RmPageHandle new_page = create_new_page_handle();
        buffer_pool_manager_->unpin_page(new_page.page->get_page_id(), true);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("RmFileHandle::insert_record duplicate rid");
    }

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        }
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages ||
        rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    if (was_full) {
        release_page_handle(page_handle);
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages ||
        rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }

    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw InternalError("RmFileHandle::fetch_page_handle failed");
    }
    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("RmFileHandle::create_new_page_handle failed");
    }

    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    page_handle.page_hdr->num_records = 0;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);

    file_hdr_.num_pages++;
    file_hdr_.first_free_page_no = page_id.page_no;
    return page_handle;
}

RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    int page_no = page_handle.page->get_page_id().page_no;
    if (file_hdr_.first_free_page_no == page_no) {
        return;
    }
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_no;
}
