// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "supersonic/testing/repeating_block.h"

#include <algorithm>
using std::copy;
using std::max;
using std::min;
using std::reverse;
using std::sort;
using std::swap;
#include <string>
using std::string;

#include <glog/logging.h>
#include "supersonic/utils/logging-inl.h"
#include "supersonic/utils/scoped_ptr.h"
#include "supersonic/utils/exception/failureor.h"
#include "supersonic/base/infrastructure/copy_column.h"
#include "supersonic/base/infrastructure/types.h"
#include "supersonic/base/infrastructure/view_copier.h"
#include "supersonic/cursor/base/cursor.h"
#include "supersonic/cursor/proto/cursors.pb.h"
#include "supersonic/cursor/infrastructure/basic_cursor.h"
#include "supersonic/cursor/infrastructure/iterators.h"
#include "supersonic/utils/strings/join.h"

namespace supersonic {

class RepeatingBlockCursor : public BasicCursor {
 public:
  RepeatingBlockCursor(const Block& input_block, size_t total_num_rows)
      : BasicCursor(input_block.schema()),
        input_block_(input_block),
        num_rows_remaining_(total_num_rows),
        current_offset_(0),
        iterator_(input_block.schema()) {
  }

  virtual ~RepeatingBlockCursor() {}

  ResultView Next(rowcount_t max_row_count) {
    if (num_rows_remaining_ == 0) return ResultView::EOS();
    max_row_count = std::min(max_row_count, num_rows_remaining_);
    if (!iterator_.next(max_row_count)) {
      iterator_.reset(input_block_.view());
      CHECK(iterator_.next(max_row_count));
    }
    num_rows_remaining_ -= iterator_.row_count();
    return ResultView::Success(&iterator_.view());
  }

  virtual CursorId GetCursorId() const { return REPEATING_BLOCK; }

  void AppendDebugDescription(string* target) const {
    StrAppend(target,
              "RepeatingBlockCursor with ",
              num_rows_remaining_, " rows left");
  }

 private:
  const Block& input_block_;
  rowcount_t num_rows_remaining_;
  rowcount_t current_offset_;
  ViewIterator iterator_;
  DISALLOW_COPY_AND_ASSIGN(RepeatingBlockCursor);
};

RepeatingBlockOperation::RepeatingBlockOperation(Block* block,
                                                 size_t total_num_rows)
    : BasicOperation(),
      resized_block_(CreateResizedBlock(block, Cursor::kDefaultRowCount)),
      total_num_rows_(total_num_rows) {
  CHECK_NOTNULL(resized_block_.get());
}

FailureOrOwned<Cursor> RepeatingBlockOperation::CreateCursor() const {
  CHECK_NOTNULL(resized_block_.get());
  return Success(new RepeatingBlockCursor(*resized_block_, total_num_rows_));
}

Block* RepeatingBlockOperation::CreateResizedBlock(Block* original_block,
                                                   size_t min_num_rows) {
  CHECK_NOTNULL(original_block);
  size_t num_input_rows = original_block->row_capacity();
  if (num_input_rows >= min_num_rows) {
    // No need to create new block, this one has enough rows.
    return original_block;
  }
  scoped_ptr<Block> original_block_deleter(original_block);

  size_t num_copies = (min_num_rows + num_input_rows - 1) / num_input_rows;
  CHECK_GT(num_copies, 1);
  scoped_ptr<Block> new_block(new Block(original_block->schema(),
                                        buffer_allocator()));
  if (!new_block->Reallocate(num_copies * num_input_rows)) {
    return NULL;
  }

  ViewCopier view_copier(original_block->schema(), new_block->schema(),
                         NO_SELECTOR, true);
  for (size_t i = 0; i < num_copies; ++i) {
    size_t rows_copied = view_copier.Copy(
        num_input_rows, original_block->view(), NULL,
        i * num_input_rows, new_block.get());
    CHECK_EQ(num_input_rows, rows_copied);
  }
  return new_block.release();
}

}  // namespace supersonic
