#include "buffer/buffer_pool_manager.h"

namespace scudb {

/*
 * BufferPoolManager Constructor
 * When log_manager is nullptr, logging is disabled (for test purpose)
 * WARNING: Do Not Edit This Function
 */
//创建BufferPool
BufferPoolManager::BufferPoolManager(size_t pool_size,
                                                 DiskManager *disk_manager,
                                                 LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager),
      log_manager_(log_manager) {
  // a consecutive memory space for buffer pool
  //在缓冲池中创建pages指针指向缓冲池Page
  //创建数量为pool_size_的Page
  pages_ = new Page[pool_size_];
  //创建一个hashTable,map<page_id,*Page>(table装的是page_id映射的Page)
  //HashTable装的是page_id和Page的地址（父指针指向子对象）
  page_table_ = new ExtendibleHash<page_id_t, Page *>(BUCKET_SIZE);
  //替换界面LRU  map<*Page,Node(*Page)>
  replacer_ = new LRUReplacer<Page *>;
  //空闲指针列表
  free_list_ = new std::list<Page *>;

  // put all the pages into free list
  for (size_t i = 0; i < pool_size_; ++i) {
    //所有的初始界面指针空闲列表中
    free_list_->push_back(&pages_[i]);
  }
}

/*
 * BufferPoolManager Deconstructor
 * WARNING: Do Not Edit This Function
 */
BufferPoolManager::~BufferPoolManager() {
  delete[] pages_;
  delete page_table_;
  delete replacer_;
  delete free_list_;
}

/**
 * 1. search hash table.
 *  1.1 if exist, pin the page and return immediately
 *  1.2 if no exist, find a replacement entry from either free list or lru
 *      replacer. (NOTE: always find from free list first)
 * 2. If the entry chosen for replacement is dirty, write it back to disk.
 * 3. Delete the entry for the old page from the hash table and insert an
 * entry for the new page.
 * 4. Update page metadata, read page content from disk file and return page
 * pointer
 *
 * This function must mark the Page as pinned and remove its entry from LRUReplacer before it is returned to the caller.
 */

Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  lock_guard<mutex> lck(latch_);
  Page *tar = nullptr;
  //如果找到该Page，直接返回该Page
  if (page_table_->Find(page_id,tar)) { //1.1
    tar->pin_count_++;
    //从替换列表中移除
    replacer_->Erase(tar);
    return tar;
  }
  //1.2
  //先在freelist找是否有空闲页面，再找lru是否能替换
  tar = GetVictimPage();
  if (tar == nullptr) return tar;
  //2
  //如果找到的页面是脏页，则迅速写回外存
  if (tar->is_dirty_) {
    disk_manager_->WritePage(tar->GetPageId(),tar->data_);
  }
  //3
  //删除旧的Page，插入新的Page
  page_table_->Remove(tar->GetPageId());
  page_table_->Insert(page_id,tar);
  //4
  //读取该Page
  disk_manager_->ReadPage(page_id,tar->data_);
  //将新页面pin置1，修改位置为false，赋page_id的值
  tar->pin_count_ = 1;
  tar->is_dirty_ = false;
  tar->page_id_= page_id;

  return tar;
}
//Page *BufferPoolManager::find

/*
 * Implementation of unpin page
 * if pin_count>0, decrement it and if it becomes zero, put it back to
 * replacer if pin_count<=0 before this call, return false. is_dirty: set the
 * dirty flag of this page
 */
//使用完页面后，解除Page的Pin
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  lock_guard<mutex> lck(latch_);
  Page *tar = nullptr;
  page_table_->Find(page_id,tar);
  if (tar == nullptr) {
    return false;
  }
  tar->is_dirty_ = is_dirty;
  //如果pinCount==0，那么并没有使用此页面
  if (tar->GetPinCount() <= 0) {
    return false;
  }
   //pin_count减一后如果等于零，将其插入待替换队列
  if (--tar->pin_count_ == 0) {
    replacer_->Insert(tar);
  }
  return true;
}

/*
 * Used to flush a particular page of the buffer pool to disk. Should call the
 * write_page method of the disk manager
 * if page is not found in page table, return false
 * NOTE: make sure page_id != INVALID_PAGE_ID
 */
//将页面写回外存
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  lock_guard<mutex> lck(latch_);
  Page *tar = nullptr;
  page_table_->Find(page_id,tar);
  if (tar == nullptr || tar->page_id_ == INVALID_PAGE_ID) {
    return false;
  }
  //如果是脏页，写回外存
  if (tar->is_dirty_) {
    disk_manager_->WritePage(page_id,tar->GetData());
    tar->is_dirty_ = false;
  }

  return true;
}

/**
 * User should call this method for deleting a page. This routine will call
 * disk manager to deallocate the page. First, if page is found within page
 * table, buffer pool manager should be reponsible for removing this entry out
 * of page table, reseting page metadata and adding back to free list. Second,
 * call disk manager's DeallocatePage() method to delete from disk file. If
 * the page is found within page table, but pin_count != 0, return false
 */
//删除Page
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  lock_guard<mutex> lck(latch_);
  Page *tar = nullptr;
  page_table_->Find(page_id,tar);
  if (tar != nullptr) {
   // 若pin大于零表示仍有进程在使用此页面，返回false
        if (tar->GetPinCount() > 0) {
            return false;
        }
        //将此页从代替换页面中删除
        replacer_->Erase(tar);
        //将此页面从pagetable中删除
        page_table_->Remove(page_id);
        //脏页位置位false
        tar->is_dirty_ = false;
        //将此页面数据清空
        tar->ResetMemory();
        //将此页面加入freelist中
        free_list_->push_back(tar);
  }
  disk_manager_->DeallocatePage(page_id);
  return true;
}

/**
 * User should call this method if needs to create a new page. This routine
 * will call disk manager to allocate a page.
 * Buffer pool manager should be responsible to choose a victim page either
 * from free list or lru replacer(NOTE: always choose from free list first),
 * update new page's metadata, zero out memory and add corresponding entry
 * into page table. return nullptr if all the pages in pool are pinned
 */
//创建新的Page
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  lock_guard<mutex> lck(latch_);
  Page *tar = nullptr;
  //找page可以放的位置
  tar = GetVictimPage();
  if (tar == nullptr) return tar;
  //创建页面
  page_id = disk_manager_->AllocatePage();
  //2
  if (tar->is_dirty_) {
    disk_manager_->WritePage(tar->GetPageId(),tar->data_);
  }
  //3
  page_table_->Remove(tar->GetPageId());
  page_table_->Insert(page_id,tar);
  //4
  tar->page_id_ = page_id;
  //清空Page的数据
  tar->ResetMemory();
  tar->is_dirty_ = false;
  tar->pin_count_ = 1;

  return tar;
}
//寻找要换出的Page
Page *BufferPoolManager::GetVictimPage() {
  //先在freelist中寻找，再在replace中寻找
  Page *tar = nullptr;
  if (free_list_->empty()) {
    if (replacer_->Size() == 0) {
      return nullptr;
    }
    replacer_->Victim(tar);
  } else {
    tar = free_list_->front();
    free_list_->pop_front();
    assert(tar->GetPageId() == INVALID_PAGE_ID);
  }
  assert(tar->GetPinCount() == 0);
  return tar;
}

} // namespace cmudb
