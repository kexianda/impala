// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_EXEC_TEXT_CONVERTER_INLINE_H
#define IMPALA_EXEC_TEXT_CONVERTER_INLINE_H

#include "text-converter.h"

#include <boost/algorithm/string.hpp>

#include "runtime/runtime-state.h"
#include "runtime/descriptors.h"
#include "runtime/tuple.h"
#include "util/string-parser.h"
#include "runtime/string-value.h"
#include "runtime/timestamp-value.h"
#include "runtime/mem-pool.h"
#include "runtime/string-value.inline.h"
#include "exprs/string-functions.h"

namespace impala {

// Note: this function has a codegen'd version.  Changing this function requires
// corresponding changes to CodegenWriteSlot.
inline bool TextConverter::WriteSlot(const SlotDescriptor* slot_desc, Tuple* tuple,
    const char* data, int len, bool copy_string, bool need_escape, MemPool* pool) {
  if ((len == 0 && !slot_desc->type().IsStringType()) || data == NULL) {
    tuple->SetNull(slot_desc->null_indicator_offset());
    return true;
  } else if (check_null_ && len == null_col_val_.size() &&
      StringCompare(data, len, null_col_val_.data(), null_col_val_.size(), len) == 0) {
    // We matched the special NULL indicator.
    tuple->SetNull(slot_desc->null_indicator_offset());
    return true;
  }

  StringParser::ParseResult parse_result = StringParser::PARSE_SUCCESS;
  void* slot = tuple->GetSlot(slot_desc->tuple_offset());

  // Parse the raw-text data. Translate the text string to internal format.
  const ColumnType& type = slot_desc->type();
  switch (type.type) {
    case TYPE_STRING:
    case TYPE_VARCHAR:
    case TYPE_CHAR: {
      int buffer_len = len;
      if (type.type == TYPE_VARCHAR || type.type == TYPE_CHAR) buffer_len = type.len;

      bool reuse_data = type.IsVarLen() && !(len != 0 && (copy_string || need_escape));
      if (type.type == TYPE_CHAR) reuse_data &= (buffer_len <= len);

      StringValue str;
      str.len = std::min(buffer_len, len);
      if (reuse_data) {
        str.ptr = const_cast<char*>(data);
      } else {
        str.ptr = type.IsVarLen() ? reinterpret_cast<char*>(pool->Allocate(buffer_len)) :
            reinterpret_cast<char*>(slot);
        if (need_escape) {
          UnescapeString(data, str.ptr, &str.len, buffer_len);
        } else {
          memcpy(str.ptr, data, str.len);
        }
      }

      if (type.type == TYPE_CHAR) {
        StringValue::PadWithSpaces(str.ptr, buffer_len, str.len);
        str.len = type.len;
      }
      // write back to the slot, if !IsVarLen() we already wrote to the slot
      if (type.IsVarLen()) {
        StringValue* str_slot = reinterpret_cast<StringValue*>(slot);
        *str_slot = str;
      }
      break;
    }
    case TYPE_BOOLEAN:
      *reinterpret_cast<bool*>(slot) =
        StringParser::StringToBool(data, len, &parse_result);
      break;
    case TYPE_TINYINT:
      *reinterpret_cast<int8_t*>(slot) =
        StringParser::StringToInt<int8_t>(data, len, &parse_result);
      break;
    case TYPE_SMALLINT:
      *reinterpret_cast<int16_t*>(slot) =
        StringParser::StringToInt<int16_t>(data, len, &parse_result);
      break;
    case TYPE_INT:
      *reinterpret_cast<int32_t*>(slot) =
        StringParser::StringToInt<int32_t>(data, len, &parse_result);
      break;
    case TYPE_BIGINT:
      *reinterpret_cast<int64_t*>(slot) =
        StringParser::StringToInt<int64_t>(data, len, &parse_result);
      break;
    case TYPE_FLOAT:
      *reinterpret_cast<float*>(slot) =
        StringParser::StringToFloat<float>(data, len, &parse_result);
      break;
    case TYPE_DOUBLE:
      *reinterpret_cast<double*>(slot) =
        StringParser::StringToFloat<double>(data, len, &parse_result);
      break;
    case TYPE_TIMESTAMP: {
      TimestampValue* ts_slot = reinterpret_cast<TimestampValue*>(slot);
      *ts_slot = TimestampValue(data, len);
      if (!ts_slot->HasDateOrTime()) {
        parse_result = StringParser::PARSE_FAILURE;
      }
      break;
    }
    case TYPE_DECIMAL: {
      switch (slot_desc->slot_size()) {
        case 4:
          *reinterpret_cast<Decimal4Value*>(slot) =
              StringParser::StringToDecimal<int32_t>(
                  data, len, slot_desc->type(), &parse_result);
          break;
        case 8:
          *reinterpret_cast<Decimal8Value*>(slot) =
              StringParser::StringToDecimal<int64_t>(
                  data, len, slot_desc->type(), &parse_result);
          break;
        case 12:
          DCHECK(false) << "Planner should not generate this.";
          break;
        case 16:
          *reinterpret_cast<Decimal16Value*>(slot) =
              StringParser::StringToDecimal<int128_t>(
                  data, len, slot_desc->type(), &parse_result);
          break;
        default:
          DCHECK(false) << "Decimal slots can't be this size.";
      }
      if (parse_result != StringParser::PARSE_SUCCESS) {
        // Don't accept underflow and overflow for decimals.
        parse_result = StringParser::PARSE_FAILURE;
      }
      break;
    }
    default:
      DCHECK(false) << "bad slot type: " << slot_desc->type();
      break;
  }

  // TODO: add warning for overflow case
  if (parse_result == StringParser::PARSE_FAILURE) {
    tuple->SetNull(slot_desc->null_indicator_offset());
    return false;
  }

  return true;
}

}

#endif